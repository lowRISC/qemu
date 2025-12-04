/*
 * QEMU OpenTitan Big Number device
 *
 * Copyright (c) 2022-2025 Rivos, Inc.
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
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_edn.h"
#include "hw/opentitan/ot_fifo32.h"
#include "hw/opentitan/ot_key_sink.h"
#include "hw/opentitan/ot_otbn.h"
#include "hw/opentitan/otbn/otbnproxy.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_clock_src.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "trace.h"

#undef OT_OTBN_DEBUG

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

#define OT_OTBN_CLOCK_ACTIVE "clock-active"
#define OT_OTBN_CLOCK_INPUT  "clock-in"

#define R_LAST_REG (R_LOAD_CHECKSUM)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) < REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

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

enum {
    ALERT_FATAL,
    ALERT_RECOVERABLE,
    ALERT_COUNT,
};

struct OtOTBNState {
    /* <private> */
    SysBusDevice parent_obj;

    /* <public> */
    MemoryRegion mmio;
    MemoryRegion regs;
    MemoryRegion imem;
    MemoryRegion dmem;

    IbexIRQ irq_done;
    IbexIRQ alerts[ALERT_COUNT];
    IbexIRQ clock_active;

    QEMUBH *proxy_completion_bh;
    QEMUTimer *proxy_defer;
    OTBNProxy proxy;
    unsigned pclk; /* Current input clock */
    const char *clock_src_name; /* IRQ name once connected */
    char *hexstr;

    uint32_t intr_state;
    uint32_t intr_enable;
    uint32_t intr_test;
    uint32_t alert_test;
    uint32_t errbits;
    uint32_t load_checksum;

    enum OtOTBNCommand last_cmd;

    char *ot_id;
    char *clock_name;
    DeviceState *clock_src;
    char *log_file;
    OtOTBNRandom rnds[OT_OTBN_RND_COUNT];
    bool log_asm;
};

struct OtOTBNClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

#ifdef OT_OTBN_DEBUG
#define OT_OTBN_HEXSTR_SIZE  (OT_OTBN_KEY_SIZE * 2u + 2u)
#define TRACE_OTBN(msg, ...) qemu_log("%s: " msg "\n", __func__, ##__VA_ARGS__);
#define ot_otbn_hexdump(_s_, _b_, _l_) \
    ot_common_lhexdump((const uint8_t *)_b_, _l_, false, (_s_)->hexstr, \
                       OT_OTBN_HEXSTR_SIZE)
#else
#define TRACE_OTBN(msg, ...)
#define ot_otbn_hexdump(_s_, _b_, _l_)
#endif

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
    trace_ot_otbn_irq(s->ot_id, s->intr_state, s->intr_enable, level);
    ibex_irq_set(&s->irq_done, level);
}

static void ot_otbn_update_alert(OtOTBNState *s)
{
    uint32_t levels = s->alert_test;

    uint16_t recov_err_bits = (uint16_t)s->errbits;
    if (recov_err_bits) {
        levels |= 1u << ALERT_RECOVERABLE;
    }

    uint16_t fatal_err_bits = (uint16_t)(s->errbits >> 16u);
    if (fatal_err_bits) {
        levels |= 1u << ALERT_FATAL;
    }

    for (unsigned ix = 0u; ix < ALERT_COUNT; ix++) {
        int level = (int)((levels >> ix) & 0x1u);
        if (level != ibex_irq_get_level(&s->alerts[ix])) {
            trace_ot_otbn_update_alert(s->ot_id,
                                       ibex_irq_get_level(&s->alerts[ix]),
                                       level);
        }
        ibex_irq_set(&s->alerts[ix], level);
    }

    if (!s->alert_test && !recov_err_bits) {
        return;
    }
    /* ALERT_TEST and recoverable error alerts are transient */
    s->alert_test = 0u;
    s->errbits &= ~UINT16_MAX;
    levels = fatal_err_bits ? (1u << ALERT_FATAL) : 0u;

    for (unsigned ix = 0u; ix < ALERT_COUNT; ix++) {
        int level = (int)((levels >> ix) & 0x1u);
        if (level != ibex_irq_get_level(&s->alerts[ix])) {
            trace_ot_otbn_update_alert(s->ot_id,
                                       ibex_irq_get_level(&s->alerts[ix]),
                                       level);
        }
        ibex_irq_set(&s->alerts[ix], level);
    }
}

static void ot_otbn_post_execute(void *opaque)
{
    OtOTBNState *s = OT_OTBN(opaque);

    s->errbits = ot_otbn_proxy_get_err_bits(s->proxy);
    uint32_t insncount = ot_otbn_proxy_get_instruction_count(s->proxy);
    trace_ot_otbn_post_execute(s->ot_id, s->errbits, insncount);
    s->intr_state |= INTR_DONE_MASK;
    ot_otbn_proxy_acknowledge_execution(s->proxy);
    ot_otbn_update_alert(s);
    ot_otbn_update_irq(s);
    ibex_irq_set(&s->clock_active, false);
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
    trace_ot_otbn_proxy_entropy_request(r->otbn->ot_id, slot);

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

    trace_ot_otbn_proxy_completion_bh(s->ot_id, last_cmd);

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
                      qemu_clock_get_us(OT_VIRTUAL_CLOCK) + 100u);
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
    OtOTBNState *s = rnd->otbn;

    if (!rnd->entropy_requested) {
        /* entropy not expected, may occur on reset */
        trace_ot_otbn_error(s->ot_id, "received unexpected entropy");
        return;
    }

    if (ot_fifo32_is_full(&rnd->packer)) {
        /* too many entropy bits, internal error */
        trace_ot_otbn_error(s->ot_id, "received too many entropy");
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
    g_assert(num == OT_OTBN_RANDOM_WORD_COUNT);
    num *= sizeof(uint32_t);
    g_assert(s != NULL);
    unsigned rnd_ix = (unsigned)(rnd - &s->rnds[0]);
    bool res;
    switch (rnd_ix) {
    case OT_OTBN_URND:
        trace_ot_otbn_proxy_push_entropy(s->ot_id, "urnd", !rnd->no_fips);
        res = ot_otbn_proxy_push_entropy(s->proxy, rnd_ix, buf8, num,
                                         !rnd->no_fips);
        break;
    case OT_OTBN_RND:
        trace_ot_otbn_proxy_push_entropy(s->ot_id, "rnd", !rnd->no_fips);
        res = ot_otbn_proxy_push_entropy(s->proxy, rnd_ix, buf8, num,
                                         !rnd->no_fips);
        break;
    default:
        g_assert_not_reached();
        break;
    }
    ot_fifo32_reset(&rnd->packer);
    rnd->no_fips = false;
    if (!res) {
        trace_ot_otbn_error(s->ot_id, "cannot push entropy");
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

    OtOTBNState *s = rnd->otbn;

    rnd->entropy_requested = true;
    trace_ot_otbn_request_entropy(s->ot_id, rnd->ep);
    if (ot_edn_request_entropy(rnd->device, rnd->ep)) {
        trace_ot_otbn_error(s->ot_id, "failed to request entropy");
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
                      "%s: %s: cannot execute cmd %02X from a not IDLE state\n",
                      __func__, s->ot_id, command);
        return;
    }

    if (s->last_cmd != OT_OTBN_CMD_NONE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: previous command %02X did not complete\n",
                      __func__, s->ot_id, s->last_cmd);
        return;
    }

    ibex_irq_set(&s->clock_active, true);

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
        ibex_irq_set(&s->clock_active, false);
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Invalid command %02X\n",
                      __func__, s->ot_id, s->last_cmd);
        break;
    }
}

static void ot_otbn_clock_input(void *opaque, int irq, int level)
{
    OtOTBNState *s = opaque;

    g_assert(irq == 0);

    s->pclk = (unsigned)level;

    /* TODO: disable OTBN execution when PCLK is 0 */
}

static void ot_otbn_push_key(OtKeySinkIf *ifd, const uint8_t *share0,
                             const uint8_t *share1, size_t key_len, bool valid)
{
    g_assert(!key_len || key_len == OT_OTBN_KEY_SIZE);

    OtOTBNState *s = OT_OTBN(ifd);
    static const uint8_t empty_share[OT_OTBN_KEY_SIZE] = { 0 };

    if (!key_len || !share0) {
        key_len = sizeof(empty_share);
        share0 = empty_share;
    }
    if (!key_len || !share1) {
        key_len = sizeof(empty_share);
        share1 = empty_share;
    }

    TRACE_OTBN("%s: share0 %s, valid: %u", s->ot_id,
               ot_otbn_hexdump(s, share0, OT_OTBN_KEY_SIZE), valid);

    bool res = ot_otbn_proxy_push_key(s->proxy, share0, share1, key_len, valid);
    if (!res) {
        trace_ot_otbn_error(s->ot_id, "Cannot push sideload key");
    }
}

static uint64_t ot_otbn_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtOTBNState *s = opaque;
    (void)size;
    uint32_t val32;

    uint32_t pc = ibex_get_current_pc();

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
        val32 = s->errbits >> 16u;
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
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: %s is write only\n", __func__,
                      s->ot_id, REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: bad offset 0x%x\n", __func__,
                      s->ot_id, (uint32_t)addr);
        val32 = 0;
        break;
    }

    trace_ot_otbn_io_read_out(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32,
                              pc);

    return (uint64_t)val32;
}

static void ot_otbn_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                               unsigned size)
{
    OtOTBNState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_otbn_io_write(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32, pc);

    if (ot_otbn_is_locked(s)) {
        trace_ot_otbn_deny(s->ot_id, pc, "write denied: locked");
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
        s->load_checksum = val32;
        break;
    case R_STATUS:
    case R_FATAL_ALERT_CAUSE:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: %s is read only\n", __func__,
                      s->ot_id, REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: bad offset 0x%x\n", __func__,
                      s->ot_id, (uint32_t)addr);
        break;
    }
}

static void ot_otbn_update_checksum(OtOTBNState *s, bool doi, uint32_t addr,
                                    uint32_t value)
{
    uint8_t buf[6];

    stw_le_p(&buf[4], addr >> 2U);
    buf[5] |= doi ? 0x80u : 0x00u;
    stl_le_p(&buf[0], value);

    s->load_checksum = crc32(s->load_checksum, buf, sizeof(buf));
}

static uint32_t ot_otbn_mem_read(OtOTBNState *s, bool doi, hwaddr addr)
{
    uint32_t value = ot_otbn_proxy_read_memory(s->proxy, doi, addr);
    trace_ot_otbn_mem_read(s->ot_id, doi ? 'I' : 'D', (uint32_t)addr, value);
    return value;
}

static void ot_otbn_mem_write(OtOTBNState *s, bool doi, hwaddr addr,
                              uint32_t value)
{
    bool written = ot_otbn_proxy_write_memory(s->proxy, doi, addr, value);
    trace_ot_otbn_mem_write(s->ot_id, doi ? 'I' : 'D', (uint32_t)addr, value,
                            written ? "" : " FAILED");
    if (written) {
        ot_otbn_update_checksum(s, doi, addr, value);
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

static const Property ot_otbn_properties[] = {
    DEFINE_PROP_STRING(OT_COMMON_DEV_ID, OtOTBNState, ot_id),
    DEFINE_PROP_STRING("clock-name", OtOTBNState, clock_name),
    DEFINE_PROP_LINK("clock-src", OtOTBNState, clock_src, TYPE_DEVICE,
                     DeviceState *),
    DEFINE_PROP_LINK("edn-u", OtOTBNState, rnds[OT_OTBN_URND].device,
                     TYPE_OT_EDN, OtEDNState *),
    DEFINE_PROP_LINK("edn-r", OtOTBNState, rnds[OT_OTBN_RND].device,
                     TYPE_OT_EDN, OtEDNState *),
    DEFINE_PROP_UINT8("edn-u-ep", OtOTBNState, rnds[OT_OTBN_URND].ep,
                      UINT8_MAX),
    DEFINE_PROP_UINT8("edn-r-ep", OtOTBNState, rnds[OT_OTBN_RND].ep, UINT8_MAX),
    DEFINE_PROP_STRING("logfile", OtOTBNState, log_file),
    DEFINE_PROP_BOOL("logasm", OtOTBNState, log_asm, false),
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

static void ot_otbn_reset_enter(Object *obj, ResetType type)
{
    OtOTBNClass *c = OT_OTBN_GET_CLASS(obj);
    OtOTBNState *s = OT_OTBN(obj);

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    timer_del(s->proxy_defer);

    s->intr_state = 0;
    s->intr_enable = 0;
    s->intr_test = 0;
    s->alert_test = 0;
    s->errbits = 0;
    s->load_checksum = 0;

    s->last_cmd = OT_OTBN_CMD_NONE;
    ibex_irq_set(&s->irq_done, 0);
    for (unsigned ix = 0; ix < ALERT_COUNT; ix++) {
        ibex_irq_set(&s->alerts[ix], 0);
    }

    for (unsigned rix = 0; rix < (unsigned)OT_OTBN_RND_COUNT; rix++) {
        OtOTBNRandom *rnd = &s->rnds[rix];
        rnd->otbn = s;
        rnd->no_fips = false;
        rnd->entropy_requested = false;
        ot_fifo32_reset(&rnd->packer);
    }

    if (!s->clock_src_name) {
        IbexClockSrcIfClass *ic = IBEX_CLOCK_SRC_IF_GET_CLASS(s->clock_src);
        IbexClockSrcIf *ii = IBEX_CLOCK_SRC_IF(s->clock_src);

        s->clock_src_name =
            ic->get_clock_source(ii, s->clock_name, DEVICE(s), &error_fatal);
        qemu_irq in_irq =
            qdev_get_gpio_in_named(DEVICE(s), OT_OTBN_CLOCK_INPUT, 0);
        qdev_connect_gpio_out_named(s->clock_src, s->clock_src_name, 0, in_irq);

        if (object_dynamic_cast(OBJECT(s->clock_src), TYPE_OT_CLKMGR)) {
            char *hint_name =
                g_strdup_printf(OT_CLOCK_HINT_PREFIX "%s", s->clock_name);
            qemu_irq hint_irq =
                qdev_get_gpio_in_named(s->clock_src, hint_name, 0);
            g_assert(hint_irq);
            qdev_connect_gpio_out_named(DEVICE(s), OT_OTBN_CLOCK_ACTIVE, 0,
                                        hint_irq);
            g_free(hint_name);
        }
    }
}

static void ot_otbn_reset_exit(Object *obj, ResetType type)
{
    OtOTBNClass *c = OT_OTBN_GET_CLASS(obj);
    OtOTBNState *s = OT_OTBN(obj);

    if (c->parent_phases.exit) {
        c->parent_phases.exit(obj, type);
    }

    if (!s->log_file) {
        s->log_asm = false;
    }

    ot_otbn_proxy_start(s->proxy, false, s->log_file, s->log_asm);
}

static void ot_otbn_realize(DeviceState *dev, Error **errp)
{
    (void)errp;

    OtOTBNState *s = OT_OTBN(dev);
    g_assert(s->ot_id);
    g_assert(s->clock_name);
    g_assert(s->clock_src);

    g_assert(s->rnds[OT_OTBN_URND].device);
    g_assert(s->rnds[OT_OTBN_RND].device);
    g_assert(s->rnds[OT_OTBN_URND].ep != UINT8_MAX);
    g_assert(s->rnds[OT_OTBN_RND].ep != UINT8_MAX);

    qdev_init_gpio_in_named(DEVICE(s), &ot_otbn_clock_input,
                            OT_OTBN_CLOCK_INPUT, 1);
}

static void ot_otbn_init(Object *obj)
{
    OtOTBNState *s = OT_OTBN(obj);

    memory_region_init(&s->mmio, obj, TYPE_OT_OTBN, 0x10000u);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    memory_region_init_io(&s->regs, obj, &ot_otbn_regs_ops, s,
                          TYPE_OT_OTBN ".regs", REGS_SIZE);
    memory_region_add_subregion(&s->mmio, OT_OTBN_REGS_BASE, &s->regs);

    /*
     * IMEM cannot be defined as a RAM region since accesses need to be
     * controlled and checksum to be computed in-order
     */
    memory_region_init_io(&s->imem, obj, &ot_otbn_imem_ops, s,
                          TYPE_OT_OTBN ".imem", OT_OTBN_IMEM_SIZE);
    memory_region_add_subregion(&s->mmio, OT_OTBN_IMEM_BASE, &s->imem);

    /*
     * DMEM cannot be defined as a RAM region since accesses need to be
     * controlled and checksum to be computed in-order
     */
    memory_region_init_io(&s->dmem, obj, &ot_otbn_dmem_ops, s,
                          TYPE_OT_OTBN ".dmem", OT_OTBN_DMEM_SIZE);
    memory_region_add_subregion(&s->mmio, OT_OTBN_DMEM_BASE, &s->dmem);

    ibex_sysbus_init_irq(obj, &s->irq_done);
    ibex_qdev_init_irqs(obj, s->alerts, OT_DEVICE_ALERT, ALERT_COUNT);
    ibex_qdev_init_irq(obj, &s->clock_active, OT_OTBN_CLOCK_ACTIVE);

    for (unsigned rix = 0; rix < (unsigned)OT_OTBN_RND_COUNT; rix++) {
        OtOTBNRandom *r = &s->rnds[rix];
        r->proxy_entropy_req_bh = qemu_bh_new(&ot_otbn_proxy_entropy_req_bh, r);
        ot_fifo32_create(&r->packer, OT_OTBN_RANDOM_WORD_COUNT);
    }

    s->proxy_completion_bh = qemu_bh_new(&ot_otbn_proxy_completion_bh, s);
    s->proxy_defer = timer_new_us(OT_VIRTUAL_CLOCK, &ot_otbn_post_execute, s);
    s->proxy =
        ot_otbn_proxy_new(&ot_otbn_trigger_entropy_req, &s->rnds[OT_OTBN_URND],
                          &ot_otbn_trigger_entropy_req, &s->rnds[OT_OTBN_RND],
                          &ot_otbn_signal_on_completion, s);

#ifdef OT_OTBN_DEBUG
    s->hexstr = g_new0(char, OT_OTBN_HEXSTR_SIZE);
#endif
}

static void ot_otbn_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->realize = &ot_otbn_realize;
    device_class_set_props(dc, ot_otbn_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    ResettableClass *rc = RESETTABLE_CLASS(klass);
    OtOTBNClass *oc = OT_OTBN_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_otbn_reset_enter, NULL,
                                       &ot_otbn_reset_exit, &oc->parent_phases);

    OtKeySinkIfClass *kc = OT_KEY_SINK_IF_CLASS(klass);
    kc->push_key = &ot_otbn_push_key;
}

static const TypeInfo ot_otbn_info = {
    .name = TYPE_OT_OTBN,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtOTBNState),
    .instance_init = &ot_otbn_init,
    .class_size = sizeof(OtOTBNClass),
    .class_init = &ot_otbn_class_init,
    .interfaces =
        (InterfaceInfo[]){
            { TYPE_OT_KEY_SINK_IF },
            {},
        },
};

static void ot_otbn_register_types(void)
{
    type_register_static(&ot_otbn_info);
}

type_init(ot_otbn_register_types);
