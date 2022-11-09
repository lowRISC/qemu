/*
 * QEMU OpenTitan Big Number device
 *
 * Copyright (c) 2022-2023 Rivos, Inc.
 *
 * Author(s):
 *  Emmanuel Blot <eblot@rivosinc.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * OTBN emulation backend is a derivative work of the Rust RISC-V simulator,
 * and is released under the Apache2 license. See README.md file in the
 * hw/opentitan/otbn/otbn directory.
 */

#include "qemu/osdep.h"
#include <zlib.h> /* for CRC-32 */
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_clkmgr.h"
#include "hw/opentitan/ot_edn.h"
#include "hw/opentitan/ot_fifo32.h"
#include "hw/opentitan/ot_otbn.h"
#include "hw/opentitan/otbn/otbnproxy.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "trace.h"

/* clang-format off */
REG32(INTR_STATE, 0x00u)
    SHARED_FIELD(INTR_DONE, 0u, 1u)
REG32(INTR_ENABLE, 0x04u)
REG32(INTR_TEST, 0x08u)
REG32(ALERT_TEST, 0x0cu)
    FIELD(ALERT_TEST, FATAL, 0u, 1u)
    FIELD(ALERT_TEST, RECOVERY, 1u, 1u)
REG32(CMD, 0x10u)
    FIELD(CMD, CMD, 0u, 8u)
REG32(CTRL, 0x14u)
    FIELD(CTRL, SW_ERRS_FATAL, 0u, 1u)
REG32(STATUS, 0x18u)
    FIELD(STATUS, STATUS, 0u, 8u)
REG32(ERR_BITS, 0x1cu)
    FIELD(ERR_BITS, BAD_DATA_ADDR, 0u, 1u)
    FIELD(ERR_BITS, BAD_INSN_ADDR, 1u, 1u)
    FIELD(ERR_BITS, CALL_STACK, 2u, 1u)
    FIELD(ERR_BITS, ILLEGAL_INSN, 3u, 1u)
    FIELD(ERR_BITS, LOOP, 4u, 1u)
    FIELD(ERR_BITS, KEY_INVALID, 5u, 1u)
    FIELD(ERR_BITS, RND_REP_CHK_FAIL, 6u, 1u)
    FIELD(ERR_BITS, RND_FIPS_CHK_FAIL, 7u, 1u)
    FIELD(ERR_BITS, IMEM_INTG_VIOLATION, 16u, 1u)
    FIELD(ERR_BITS, DMEM_INTG_VIOLATION, 17u, 1u)
    FIELD(ERR_BITS, REG_INTG_VIOLATION, 18u, 1u)
    FIELD(ERR_BITS, BUS_INTG_VIOLATION, 19u, 1u)
    FIELD(ERR_BITS, BAD_INTERNAL_STATE, 20u, 1u)
    FIELD(ERR_BITS, ILLEGAL_BUS_ACCESS, 21u, 1u)
    FIELD(ERR_BITS, LIFECYCLE_ESCALATION, 22u, 1u)
    FIELD(ERR_BITS, FATAL_SOFTWARE, 23u, 1u)
REG32(FATAL_ALERT_CAUSE, 0x20u)
    FIELD(FATAL_ALERT_CAUSE, IMEM_INTG_VIOLATION, 0u, 1u)
    FIELD(FATAL_ALERT_CAUSE, DMEM_INTG_VIOLATION, 1u, 1u)
    FIELD(FATAL_ALERT_CAUSE, REG_INTG_VIOLATION, 2u, 1u)
    FIELD(FATAL_ALERT_CAUSE, BUS_INTG_VIOLATION, 3u, 1u)
    FIELD(FATAL_ALERT_CAUSE, BAD_INTERNAL_STATE, 4u, 1u)
    FIELD(FATAL_ALERT_CAUSE, ILLEGAL_BUS_ACCESS, 5u, 1u)
    FIELD(FATAL_ALERT_CAUSE, LIFECYCLE_ESCALATION, 6u, 1u)
    FIELD(FATAL_ALERT_CAUSE, FATAL_SOFTWARE, 7u, 1u)
REG32(INSN_CNT, 0x24u)
REG32(LOAD_CHECKSUM, 0x28u)
/* clang-format on */

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define OT_OTBN_REGS_BASE 0x0u
#define OT_OTBN_IMEM_BASE 0x4000u
#define OT_OTBN_DMEM_BASE 0x8000u

#define R_LAST_REG (R_LOAD_CHECKSUM)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    REG_NAME_ENTRY(INTR_STATE),
    REG_NAME_ENTRY(INTR_ENABLE),
    REG_NAME_ENTRY(INTR_TEST),
    REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(CMD),
    REG_NAME_ENTRY(CTRL),
    REG_NAME_ENTRY(STATUS),
    REG_NAME_ENTRY(ERR_BITS),
    REG_NAME_ENTRY(FATAL_ALERT_CAUSE),
    REG_NAME_ENTRY(INSN_CNT),
    REG_NAME_ENTRY(LOAD_CHECKSUM),
};

typedef struct {
    OtFifo32 packer; /* 32-bit to 256-bit packer */
    OtOTBNState *otbn; /* parent */
    OtEDNState *device; /* EDN instance property */
    QEMUBH *proxy_entropy_req_bh;
    uint8_t ep; /* EDN client endpoint property */
    bool connected; /* EDN has been connected */
    bool no_fips; /* Non FIPS-compliant data */
    bool entropy_requested; /* EDN request on-going */
} OtOTBNRandom;

struct OtOTBNState {
    /* <private> */
    SysBusDevice parent_obj;

    /* <public> */
    MemoryRegion mmio;
    MemoryRegion regs;
    MemoryRegion imem;
    MemoryRegion dmem;

    IbexIRQ irq_done;
    IbexIRQ alert;
    IbexIRQ clkmgr;

    QEMUBH *proxy_completion_bh;
    QEMUTimer *proxy_defer;
    OTBNProxy proxy;

    uint32_t intr_state;
    uint32_t intr_enable;
    uint32_t intr_test;
    uint32_t alert_test;
    uint32_t fatal_alert_cause;
    uint32_t load_checksum;

    enum OtOTBNCommand last_cmd;

    OtOTBNRandom rnds[OT_OTBN_RND_COUNT];
    char *logfile;
};

static void ot_otbn_request_entropy(OtOTBNRandom *rnd);

static bool ot_otbn_is_idle(OtOTBNState *s)
{
    return ot_otbn_proxy_get_status(s->proxy) == OT_OTBN_STATUS_IDLE;
}

static bool ot_otbn_is_locked(OtOTBNState *s)
{
    return ot_otbn_proxy_get_status(s->proxy) == OT_OTBN_STATUS_LOCKED;
}

static void ot_otbn_update_irq(OtOTBNState *s)
{
    bool level = s->intr_state & s->intr_enable & INTR_DONE_MASK;
    trace_ot_otbn_irq(s->intr_state, s->intr_enable, level);
    ibex_irq_set(&s->irq_done, level);
}

static void ot_otbn_update_alert(OtOTBNState *s)
{
    bool level = s->alert_test || s->fatal_alert_cause;
    ibex_irq_set(&s->alert, level);
}

static void ot_otbn_post_execute(void *opaque)
{
    OtOTBNState *s = OT_OTBN(opaque);

    uint32_t errbits = ot_otbn_proxy_get_err_bits(s->proxy);
    uint32_t insncount = ot_otbn_proxy_get_instruction_count(s->proxy);
    trace_ot_otbn_post_execute(errbits, insncount);
    s->fatal_alert_cause |= errbits >> 16U;
    s->intr_state |= INTR_DONE_MASK;
    ot_otbn_proxy_acknowledge_execution(s->proxy);
    ot_otbn_update_alert(s);
    ot_otbn_update_irq(s);
    ibex_irq_set(&s->clkmgr, false);
}

static void ot_otbn_signal_on_completion(void *opaque)
{
    OtOTBNState *s = OT_OTBN(opaque);

    qemu_bh_schedule(s->proxy_completion_bh);
}

static void ot_otbn_trigger_entropy_req(void *opaque)
{
    OtOTBNRandom *r = (OtOTBNRandom *)opaque;

    /* sanity check */
    unsigned slot = (unsigned)(uintptr_t)(r - &r->otbn->rnds[0]);
    trace_ot_otbn_proxy_entropy_request(slot);

    switch (slot) {
    case OT_OTBN_URND:
    case OT_OTBN_RND:
        break;
    default:
        g_assert_not_reached();
        break;
    }

    qemu_bh_schedule(r->proxy_entropy_req_bh);
}

static void ot_otbn_proxy_completion_bh(void *opaque)
{
    OtOTBNState *s = opaque;

    enum OtOTBNCommand last_cmd = s->last_cmd;
    s->last_cmd = OT_OTBN_CMD_NONE;

    trace_ot_otbn_proxy_completion_bh(last_cmd);

    switch (last_cmd) {
    case OT_OTBN_CMD_EXECUTE:
    case OT_OTBN_CMD_SEC_WIPE_DMEM:
    case OT_OTBN_CMD_SEC_WIDE_IMEM:
        if (s->proxy_defer) {
            /*
             * timer is used to simulate a delayed processing, which maybe
             * useful to pass some test suites such as OT smoketest wait 100
             * microsecs so that the virtual hart can be scheduled and may poll
             * the status register before the actual completion is signalled
             * from the OTBN working thread.
             */
            timer_del(s->proxy_defer);
            timer_mod(s->proxy_defer,
                      qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + 100u);
        } else {
            ot_otbn_post_execute(s);
        }
        break;
    case OT_OTBN_CMD_NONE:
    default:
        break;
    }
}

static void ot_otbn_fill_entropy(void *opaque, uint32_t bits, bool fips)
{
    OtOTBNRandom *rnd = opaque;

    if (!rnd->entropy_requested) {
        /* entropy not expected, may occur on reset */
        trace_ot_otbn_error("received unexpected entropy");
        return;
    }

    if (ot_fifo32_is_full(&rnd->packer)) {
        /* too many entropy bits, internal error */
        trace_ot_otbn_error("received too many entropy");
        return;
    }

    ot_fifo32_push(&rnd->packer, bits);
    rnd->no_fips |= !fips;
    rnd->entropy_requested = false;

    if (!ot_fifo32_is_full(&rnd->packer)) {
        /* need more entropy to fill in the packer */
        ot_otbn_request_entropy(rnd);
        return;
    }

    /* packer is ready to inject data into the OTBN */
    uint32_t num = 0;
    const uint32_t *buf;
    buf = ot_fifo32_pop_buf(&rnd->packer, OT_OTBN_RANDOM_WORD_COUNT, &num);
    const uint8_t *buf8 = (const uint8_t *)buf;
    assert(num == OT_OTBN_RANDOM_WORD_COUNT);
    num *= sizeof(uint32_t);
    OtOTBNState *s = rnd->otbn;
    assert(s != NULL);
    unsigned rnd_ix = (unsigned)(rnd - &s->rnds[0]);
    int res;
    switch (rnd_ix) {
    case OT_OTBN_URND:
        trace_ot_otbn_proxy_push_entropy("urnd", !rnd->no_fips);
        res = ot_otbn_proxy_push_entropy(s->proxy, rnd_ix, buf8, num,
                                         !rnd->no_fips);
        break;
    case OT_OTBN_RND:
        trace_ot_otbn_proxy_push_entropy("rnd", !rnd->no_fips);
        res = ot_otbn_proxy_push_entropy(s->proxy, rnd_ix, buf8, num,
                                         !rnd->no_fips);
        break;
    default:
        g_assert_not_reached();
        break;
    }
    ot_fifo32_reset(&rnd->packer);
    if (res) {
        trace_ot_otbn_error("cannot push entropy");
    }
}

static void ot_otbn_request_entropy(OtOTBNRandom *rnd)
{
    if (!rnd->connected) {
        ot_edn_connect_endpoint(rnd->device, rnd->ep, &ot_otbn_fill_entropy,
                                rnd);
        rnd->connected = true;
    }

    if (rnd->entropy_requested) {
        /* another request is already ongoing */
        return;
    }

    rnd->entropy_requested = true;
    trace_ot_otbn_request_entropy(rnd->ep);
    if (ot_edn_request_entropy(rnd->device, rnd->ep)) {
        trace_ot_otbn_error("failed to request entropy");
        rnd->entropy_requested = false;
    }
}

static void ot_otbn_proxy_entropy_req_bh(void *opaque)
{
    /* BH triggered from the proxy */
    ot_otbn_request_entropy((OtOTBNRandom *)opaque);
}

static void ot_otbn_handle_command(OtOTBNState *s, unsigned command)
{
    /* "Writes are ignored if OTBN is not idle" */
    if (!ot_otbn_is_idle(s)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Cannot execute command %02X from a not IDLE state\n",
                      command);
        return;
    }

    if (s->last_cmd != OT_OTBN_CMD_NONE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Previous command %02X did not complete\n", s->last_cmd);
        return;
    }

    ibex_irq_set(&s->clkmgr, true);

    switch (command) {
    case (unsigned)OT_OTBN_CMD_EXECUTE:
        s->last_cmd = command;
        ot_otbn_proxy_execute(s->proxy, false);
        break;
    case (unsigned)OT_OTBN_CMD_SEC_WIPE_DMEM:
        s->last_cmd = command;
        ot_otbn_proxy_wipe_memory(s->proxy, false);
        break;
    case (unsigned)OT_OTBN_CMD_SEC_WIDE_IMEM:
        s->last_cmd = command;
        ot_otbn_proxy_wipe_memory(s->proxy, true);
        break;
    default:
        ibex_irq_set(&s->clkmgr, false);
        qemu_log_mask(LOG_GUEST_ERROR, "Invalid command %02X\n", s->last_cmd);
        break;
    }
}

static uint64_t ot_otbn_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtOTBNState *s = opaque;
    uint32_t val32;

    uint64_t pc = ibex_get_current_pc();

    hwaddr reg = R32_OFF(addr);
    switch (reg) {
    case R_INTR_STATE:
        val32 = s->intr_state;
        break;
    case R_INTR_ENABLE:
        val32 = s->intr_enable;
        break;
    case R_CTRL:
        val32 = (uint32_t)ot_otbn_proxy_get_ctrl(s->proxy);
        break;
    case R_STATUS:
        val32 = ot_otbn_proxy_get_status(s->proxy);
        break;
    case R_ERR_BITS:
        val32 = ot_otbn_proxy_get_err_bits(s->proxy);
        break;
    case R_FATAL_ALERT_CAUSE:
        val32 = s->fatal_alert_cause;
        break;
    case R_INSN_CNT:
        val32 = ot_otbn_proxy_get_instruction_count(s->proxy);
        break;
    case R_LOAD_CHECKSUM:
        val32 = s->load_checksum;
        break;
    case R_INTR_TEST:
    case R_ALERT_TEST:
    case R_CMD:
        val32 = 0;
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s is write only\n", __func__,
                      REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        val32 = 0;
        break;
    }

    trace_ot_otbn_io_read_out((unsigned)addr, REG_NAME(reg), (uint64_t)val32,
                              pc);

    return (uint64_t)val32;
}

static void ot_otbn_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                               unsigned size)
{
    OtOTBNState *s = opaque;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint64_t pc = ibex_get_current_pc();
    trace_ot_otbn_io_write((unsigned)addr, REG_NAME(reg), val64, pc);

    if (ot_otbn_is_locked(s)) {
        trace_ot_otbn_deny(pc, "write denied: locked");
        return;
    }

    switch (reg) {
    case R_INTR_STATE:
        val32 &= INTR_DONE_MASK;
        s->intr_state &= ~val32; /* RW1C */
        ot_otbn_update_irq(s);
        break;
    case R_INTR_ENABLE:
        s->intr_enable = val32 & INTR_DONE_MASK;
        ot_otbn_update_irq(s);
        break;
    case R_INTR_TEST:
        if (val32 & INTR_DONE_MASK) {
            s->intr_state |= val32 & INTR_DONE_MASK;
            ot_otbn_update_irq(s);
        }
        break;
    case R_ALERT_TEST:
        val32 &= R_ALERT_TEST_FATAL_MASK | R_ALERT_TEST_RECOVERY_MASK;
        s->alert_test |= val32;
        ot_otbn_update_alert(s);
        break;
    case R_CMD:
        val32 &= R_CMD_CMD_MASK;
        ot_otbn_handle_command(s, (unsigned)val32);
        break;
    case R_CTRL:
        val32 &= R_CTRL_SW_ERRS_FATAL_MASK;
        ot_otbn_proxy_set_ctrl(s->proxy, (bool)val32);
        ot_otbn_update_alert(s);
        break;
    case R_ERR_BITS:
        ot_otbn_proxy_set_err_bits(s->proxy, val32);
        break;
    case R_INSN_CNT:
        ot_otbn_proxy_set_instruction_count(s->proxy, val32);
        break;
    case R_LOAD_CHECKSUM:
        val32 = s->load_checksum;
        break;
    case R_STATUS:
    case R_FATAL_ALERT_CAUSE:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s is read only\n", __func__,
                      REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static void ot_otbn_update_checksum(OtOTBNState *s, bool doi, uint32_t addr,
                                    uint32_t value)
{
    uint8_t buf[6];

    /* BE or LE? */
    stw_be_p(&buf[0], addr >> 2U);
    buf[0] |= doi ? 0x80u : 0x00u;
    stl_be_p(&buf[2], value);

    s->load_checksum = crc32(s->load_checksum, buf, sizeof(buf));
}

static uint32_t ot_otbn_mem_read(OtOTBNState *s, bool doi, hwaddr addr)
{
    uint32_t value = ot_otbn_proxy_read_memory(s->proxy, doi, addr);
    trace_ot_otbn_mem_read(doi ? 'I' : 'D', addr, value);
    return value;
}

static void ot_otbn_mem_write(OtOTBNState *s, bool doi, hwaddr addr,
                              uint32_t value)
{
    bool written = ot_otbn_proxy_write_memory(s->proxy, doi, addr, value);
    trace_ot_otbn_mem_write(doi ? 'I' : 'D', addr, value,
                            written ? "" : " FAILED");
    if (written) {
        ot_otbn_update_checksum(s, doi, (uint32_t)addr, value);
    }
}

static inline uint64_t
ot_otbn_imem_read(void *opaque, hwaddr addr, unsigned size)
{
    (void)size;

    return (uint64_t)ot_otbn_mem_read((OtOTBNState *)opaque, true, addr);
}

static inline void ot_otbn_imem_write(void *opaque, hwaddr addr, uint64_t val64,
                                      unsigned size)
{
    (void)size;

    return ot_otbn_mem_write((OtOTBNState *)opaque, true, addr,
                             (uint32_t)val64);
}

static inline uint64_t
ot_otbn_dmem_read(void *opaque, hwaddr addr, unsigned size)
{
    (void)size;

    return (uint64_t)ot_otbn_mem_read((OtOTBNState *)opaque, false, addr);
}

static inline void ot_otbn_dmem_write(void *opaque, hwaddr addr, uint64_t val64,
                                      unsigned size)
{
    (void)size;

    return ot_otbn_mem_write((OtOTBNState *)opaque, false, addr,
                             (uint32_t)val64);
}

static Property ot_otbn_properties[] = {
    DEFINE_PROP_LINK("edn-u", OtOTBNState, rnds[OT_OTBN_URND].device,
                     TYPE_OT_EDN, OtEDNState *),
    DEFINE_PROP_LINK("edn-r", OtOTBNState, rnds[OT_OTBN_RND].device,
                     TYPE_OT_EDN, OtEDNState *),
    DEFINE_PROP_UINT8("edn-u-ep", OtOTBNState, rnds[OT_OTBN_URND].ep,
                      UINT8_MAX),
    DEFINE_PROP_UINT8("edn-r-ep", OtOTBNState, rnds[OT_OTBN_RND].ep, UINT8_MAX),
    DEFINE_PROP_STRING("logfile", OtOTBNState, logfile),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_otbn_regs_ops = {
    .read = &ot_otbn_regs_read,
    .write = &ot_otbn_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static const MemoryRegionOps ot_otbn_imem_ops = {
    .read = &ot_otbn_imem_read,
    .write = &ot_otbn_imem_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static const MemoryRegionOps ot_otbn_dmem_ops = {
    .read = &ot_otbn_dmem_read,
    .write = &ot_otbn_dmem_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_otbn_reset(DeviceState *dev)
{
    OtOTBNState *s = OT_OTBN(dev);

    timer_del(s->proxy_defer);

    s->intr_state = 0;
    s->intr_enable = 0;
    s->intr_test = 0;
    s->alert_test = 0;
    s->fatal_alert_cause = 0;
    s->load_checksum = 0;

    s->last_cmd = OT_OTBN_CMD_NONE;
    ibex_irq_set(&s->irq_done, 0);
    ibex_irq_set(&s->alert, 0);

    for (unsigned rix = 0; rix < (unsigned)OT_OTBN_RND_COUNT; rix++) {
        OtOTBNRandom *rnd = &s->rnds[rix];
        rnd->otbn = s;
        rnd->no_fips = false;
        rnd->entropy_requested = false;
        ot_fifo32_reset(&rnd->packer);
    }

    ot_otbn_proxy_start(s->proxy, false, s->logfile);
}


static void ot_otbn_init(Object *obj)
{
    OtOTBNState *s = OT_OTBN(obj);

    memory_region_init(&s->mmio, obj, TYPE_OT_OTBN, 0x10000u);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    memory_region_init_io(&s->regs, obj, &ot_otbn_regs_ops, s,
                          TYPE_OT_OTBN "-regs", REGS_SIZE);
    memory_region_add_subregion(&s->mmio, OT_OTBN_REGS_BASE, &s->regs);

    /*
     * IMEM cannot be defined as a RAM region since accesses need to be
     * controlled and checksum to be computed in-order
     */
    memory_region_init_io(&s->imem, obj, &ot_otbn_imem_ops, s,
                          TYPE_OT_OTBN "-imem", OT_OTBN_IMEM_SIZE);
    memory_region_add_subregion(&s->mmio, OT_OTBN_IMEM_BASE, &s->imem);

    /*
     * DMEM cannot be defined as a RAM region since accesses need to be
     * controlled and checksum to be computed in-order
     */
    memory_region_init_io(&s->dmem, obj, &ot_otbn_dmem_ops, s,
                          TYPE_OT_OTBN "-dmem", OT_OTBN_DMEM_SIZE);
    memory_region_add_subregion(&s->mmio, OT_OTBN_DMEM_BASE, &s->dmem);

    ibex_sysbus_init_irq(obj, &s->irq_done);
    ibex_qdev_init_irq(obj, &s->alert, OPENTITAN_DEVICE_ALERT);
    ibex_qdev_init_irq(obj, &s->clkmgr, OPENTITAN_CLOCK_ACTIVE);

    for (unsigned rix = 0; rix < (unsigned)OT_OTBN_RND_COUNT; rix++) {
        OtOTBNRandom *r = &s->rnds[rix];
        r->proxy_entropy_req_bh = qemu_bh_new(&ot_otbn_proxy_entropy_req_bh, r);
        ot_fifo32_create(&r->packer, OT_OTBN_RANDOM_WORD_COUNT);
    }

    s->proxy_completion_bh = qemu_bh_new(&ot_otbn_proxy_completion_bh, s);
    s->proxy_defer = timer_new_us(QEMU_CLOCK_VIRTUAL, &ot_otbn_post_execute, s);
    s->proxy =
        ot_otbn_proxy_new(&ot_otbn_trigger_entropy_req, &s->rnds[OT_OTBN_URND],
                          &ot_otbn_trigger_entropy_req, &s->rnds[OT_OTBN_RND],
                          &ot_otbn_signal_on_completion, s);
}

static void ot_otbn_realize(DeviceState *dev, Error **errp)
{
    (void)dev;
    (void)errp;
}

static void ot_otbn_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = &ot_otbn_reset;
    dc->realize = &ot_otbn_realize;
    device_class_set_props(dc, ot_otbn_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_otbn_info = {
    .name = TYPE_OT_OTBN,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtOTBNState),
    .instance_init = &ot_otbn_init,
    .class_init = &ot_otbn_class_init,
};

static void ot_otbn_register_types(void)
{
    type_register_static(&ot_otbn_info);
}

type_init(ot_otbn_register_types)
