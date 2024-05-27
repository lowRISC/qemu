/*
 * QEMU OpenTitan Data Object Exchange Mailbox
 *
 * Copyright (c) 2023-2024 Rivos, Inc.
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
 * IMPLIED, INCLUDING BUT NOT LIMIT_ADDRED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * Note that system-side interrupts are not managed by the DOE Mailbox.
 * Registers dedicated to system-side interrupt management are only storage
 * space that the guest software (called the host side to get more confusing,
 * not related to the VM host) should read and act accordingly, using other
 * devices to signal the requester that a response is ready to be read.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "exec/memory.h"
#include "hw/opentitan/ot_address_space.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_mbx.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "trace.h"


/* ------------------------------------------------------------------------ */
/* Register definitions */
/* ------------------------------------------------------------------------ */

#define PARAM_NUM_ALERTS 2u
#define PARAM_NUM_IRQS   3u

/* clang-format off */

/* Internal interface, as seen from the OT responder */
REG32(HOST_INTR_STATE, 0x00u)
    SHARED_FIELD(INTR_MBX_READY, 0u, 1u)
    SHARED_FIELD(INTR_MBX_ABORT, 1u, 1u)
    SHARED_FIELD(INTR_MBX_ERROR, 2u, 1u)
REG32(HOST_INTR_ENABLE, 0x04u)
REG32(HOST_INTR_TEST, 0x08u)
REG32(HOST_ALERT_TEST, 0x0cu)
    FIELD(HOST_ALERT_TEST, FATAL_FAULT, 0u, 1u)
    FIELD(HOST_ALERT_TEST, RECOV_FAULT, 1u, 1u)
REG32(HOST_CONTROL, 0x10u)
    FIELD(HOST_CONTROL, ABORT, 0u, 1u)
    FIELD(HOST_CONTROL, ERROR, 1u, 1u)
REG32(HOST_STATUS, 0x14u)
    FIELD(HOST_STATUS, BUSY, 0u, 1u)
    FIELD(HOST_STATUS, SYS_INTR_STATE, 1u, 1u)
    FIELD(HOST_STATUS, SYS_INTR_ENABLE, 2u, 1u)
REG32(HOST_ADDRESS_RANGE_REGWEN, 0x18u)
    FIELD(HOST_ADDRESS_RANGE_REGWEN, EN, 0u, 4u)
REG32(HOST_ADDRESS_RANGE_VALID, 0x1cu)
    FIELD(HOST_ADDRESS_RANGE_VALID, VALID, 0u, 1u)
REG32(HOST_IN_BASE_ADDR, 0x20u)
REG32(HOST_IN_LIMIT_ADDR, 0x24u)
REG32(HOST_IN_WRITE_PTR, 0x28u)
REG32(HOST_OUT_BASE_ADDR, 0x2cu)
REG32(HOST_OUT_LIMIT_ADDR, 0x30u)
REG32(HOST_OUT_READ_PTR, 0x34u)
REG32(HOST_OUT_OBJECT_SIZE, 0x38u)
    FIELD(HOST_OUT_OBJECT_SIZE, SIZE, 0u, 10u)
REG32(HOST_INTR_MSG_ADDR, 0x3cu)
REG32(HOST_INTR_MSG_DATA, 0x40u)

/*
 * External Mailbox interface, as seen from a requester. Note that in case of
 * a PCIe requester, the first two registers (SYS_INTR_*) are not visible as
 * they are overlaid on the sys side by the PCIe wrapper with DOE EXT CAP and
 * CAP registers
 */
REG32(DEPRECATED_MSG_ADDR, 0x00u)
REG32(DEPRECATED_MSG_DATA, 0x04u)
REG32(SYS_CONTROL, 0x08u)
    FIELD(SYS_CONTROL, ABORT, 0u, 1u)
    FIELD(SYS_CONTROL, SYS_INT_EN, 1u, 1u)
    FIELD(SYS_CONTROL, GO, 31u, 1u)
REG32(SYS_STATUS, 0x0cu)
    FIELD(SYS_STATUS, BUSY, 0u, 1u)
    FIELD(SYS_STATUS, INT, 1u, 1u)
    FIELD(SYS_STATUS, ERROR, 2u, 1u)
    FIELD(SYS_STATUS, READY, 31u, 1u)
REG32(SYS_WRITE_DATA, 0x10u)
REG32(SYS_READ_DATA, 0x14u)
REG32(SYS_INTR_MSG_ADDR, 0x18u)
REG32(SYS_INTR_MSG_DATA, 0x1cu)

/* clang-format on */

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_HOST_LAST_REG (R_HOST_INTR_MSG_DATA)
#define REGS_HOST_COUNT (R_HOST_LAST_REG + 1u)
#define REGS_HOST_SIZE  (REGS_HOST_COUNT * sizeof(uint32_t))

#define R_SYS_LAST_REG (R_SYS_INTR_MSG_DATA)
#define REGS_SYS_COUNT (R_SYS_LAST_REG + 1u)
#define REGS_SYS_SIZE  (REGS_SYS_COUNT * sizeof(uint32_t))

#define R_SYSLOCAL_FIRST_REG (R_SYS_INTR_MSG_ADDR)
#define R_SYSLOCAL_LAST_REG  (R_SYS_INTR_MSG_DATA)
#define REGS_SYSLOCAL_COUNT  (R_SYSLOCAL_LAST_REG - R_SYSLOCAL_FIRST_REG + 1u)
#define REGS_SYSLOCAL_SIZE   (REGS_SYSLOCAL_COUNT * sizeof(uint32_t))

#define HOST_INTR_MASK \
    (INTR_MBX_READY_MASK | INTR_MBX_ABORT_MASK | INTR_MBX_ERROR_MASK)
#define HOST_ALERT_TEST_MASK \
    (R_HOST_ALERT_TEST_FATAL_FAULT_MASK | R_HOST_ALERT_TEST_RECOV_FAULT_MASK)
#define HOST_CONTROL_MASK \
    (R_HOST_CONTROL_ABORT_MASK | R_HOST_CONTROL_ABORT_MASK)

static_assert(OT_MBX_HOST_REGS_COUNT == REGS_HOST_COUNT, "Invalid HOST regs");
static_assert(OT_MBX_SYS_REGS_COUNT == REGS_SYS_COUNT, "Invalid SYS regs");

#define REG_NAME(_kind_, _reg_) \
    ((((_reg_) <= REGS_##_kind_##_COUNT) && REG_##_kind_##_NAMES[_reg_]) ? \
         REG_##_kind_##_NAMES[_reg_] : \
         "?")

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)

#define xtrace_ot_mbx_status(_s_) \
    trace_ot_mbx_status((_s_)->ot_id, __LINE__, ot_mbx_is_on_abort(_s_), \
                        ot_mbx_is_on_error(_s_), ot_mbx_is_busy(_s_))

/* clang-format off */
static const char *REG_HOST_NAMES[REGS_HOST_COUNT] = {
    REG_NAME_ENTRY(HOST_INTR_STATE),
    REG_NAME_ENTRY(HOST_INTR_ENABLE),
    REG_NAME_ENTRY(HOST_INTR_TEST),
    REG_NAME_ENTRY(HOST_ALERT_TEST),
    REG_NAME_ENTRY(HOST_CONTROL),
    REG_NAME_ENTRY(HOST_STATUS),
    REG_NAME_ENTRY(HOST_ADDRESS_RANGE_REGWEN),
    REG_NAME_ENTRY(HOST_ADDRESS_RANGE_VALID),
    REG_NAME_ENTRY(HOST_IN_BASE_ADDR),
    REG_NAME_ENTRY(HOST_IN_LIMIT_ADDR),
    REG_NAME_ENTRY(HOST_IN_WRITE_PTR),
    REG_NAME_ENTRY(HOST_OUT_BASE_ADDR),
    REG_NAME_ENTRY(HOST_OUT_LIMIT_ADDR),
    REG_NAME_ENTRY(HOST_OUT_READ_PTR),
    REG_NAME_ENTRY(HOST_OUT_OBJECT_SIZE),
    REG_NAME_ENTRY(HOST_INTR_MSG_ADDR),
    REG_NAME_ENTRY(HOST_INTR_MSG_DATA),
};

static const char *REG_SYS_NAMES[REGS_SYS_COUNT] = {
    REG_NAME_ENTRY(DEPRECATED_MSG_ADDR),
    REG_NAME_ENTRY(DEPRECATED_MSG_DATA),
    REG_NAME_ENTRY(SYS_CONTROL),
    REG_NAME_ENTRY(SYS_STATUS),
    REG_NAME_ENTRY(SYS_WRITE_DATA),
    REG_NAME_ENTRY(SYS_READ_DATA),
    REG_NAME_ENTRY(SYS_INTR_MSG_ADDR),
    REG_NAME_ENTRY(SYS_INTR_MSG_DATA),
};
/* clang-format on */

#undef REG_NAME_ENTRY

enum {
    ALERT_RECOVERABLE,
    ALERT_FATAL,
};

typedef struct {
    MemoryRegion mmio;
    IbexIRQ irqs[PARAM_NUM_IRQS];
    IbexIRQ alerts[PARAM_NUM_ALERTS];
    uint32_t regs[REGS_HOST_COUNT];
} OtMbxHost;

typedef struct {
    MemoryRegion mmio;
    uint32_t regs[REGS_SYSLOCAL_COUNT];
    AddressSpace *ram_as;
} OtMbxSys;

struct OtMbxState {
    SysBusDevice parent_obj;

    OtMbxHost host;
    OtMbxSys sys;

    char *ot_id;
    char *ram_as_name;
};

static void ot_mbx_host_update_irqs(OtMbxState *s)
{
    OtMbxHost *host = &s->host;
    uint32_t *hregs = host->regs;

    uint32_t levels = hregs[R_HOST_INTR_STATE] & hregs[R_HOST_INTR_ENABLE];

    for (unsigned ix = 0; ix < PARAM_NUM_IRQS; ix++) {
        int level = (int)(bool)(levels & (1u << ix));
        if (level != ibex_irq_get_level(&host->irqs[ix])) {
            trace_ot_mbx_host_update_irq(ibex_irq_get_level(&host->irqs[ix]),
                                         level);
        }
        ibex_irq_set(&host->irqs[ix], level);
    }
}

static bool ot_mbx_is_enabled(const OtMbxState *s)
{
    return s->host.regs[R_HOST_ADDRESS_RANGE_VALID];
}

static bool ot_mbx_is_busy(const OtMbxState *s)
{
    return (bool)(s->host.regs[R_HOST_STATUS] & R_HOST_STATUS_BUSY_MASK);
}

static bool ot_mbx_is_on_error(const OtMbxState *s)
{
    return (bool)(s->host.regs[R_HOST_CONTROL] & R_HOST_CONTROL_ERROR_MASK);
}

static bool ot_mbx_is_on_abort(const OtMbxState *s)
{
    return (bool)(s->host.regs[R_HOST_CONTROL] & R_HOST_CONTROL_ABORT_MASK);
}

static bool ot_mbx_is_sys_interrupt(const OtMbxState *s)
{
    return (bool)(s->host.regs[R_HOST_STATUS] &
                  R_HOST_STATUS_SYS_INTR_STATE_MASK);
}

static void ot_mbx_set_error(OtMbxState *s)
{
    uint32_t *hregs = s->host.regs;

    /* should busy be set? */
    hregs[R_HOST_CONTROL] |= R_HOST_CONTROL_ERROR_MASK;

    if (hregs[R_HOST_STATUS] & R_HOST_STATUS_SYS_INTR_ENABLE_MASK) {
        hregs[R_HOST_STATUS] |= R_HOST_STATUS_SYS_INTR_STATE_MASK;
    }

    /*
     * Note: you should not use this interrupt, as it might create
     * hard-to-manage signalling since IRQ might be raise at unexpected time
     * in mailbox management. You've been warned.
     *
     * On error, wait for GO bit to be set, then handle any HW error at this
     * point. If the SYS side detects the error bit before it sets the GO flag
     * it can immediately trigger an abort.
     */
    hregs[R_HOST_INTR_STATE] |= INTR_MBX_ERROR_MASK;
    ot_mbx_host_update_irqs(s);
}

static void ot_mbx_clear_busy(OtMbxState *s)
{
    uint32_t *hregs = s->host.regs;

    hregs[R_HOST_STATUS] &= ~R_HOST_STATUS_BUSY_MASK;
    hregs[R_HOST_IN_WRITE_PTR] = hregs[R_HOST_IN_BASE_ADDR];
    hregs[R_HOST_OUT_READ_PTR] = hregs[R_HOST_OUT_BASE_ADDR];

    trace_ot_mbx_busy(s->ot_id, "clear");
}

static uint64_t ot_mbx_host_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtMbxState *s = opaque;
    (void)size;
    OtMbxHost *host = &s->host;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_HOST_INTR_STATE:
    case R_HOST_INTR_ENABLE:
    case R_HOST_CONTROL:
    case R_HOST_STATUS:
    case R_HOST_ADDRESS_RANGE_REGWEN:
    case R_HOST_ADDRESS_RANGE_VALID:
    case R_HOST_IN_BASE_ADDR:
    case R_HOST_IN_LIMIT_ADDR:
    case R_HOST_IN_WRITE_PTR:
    case R_HOST_OUT_BASE_ADDR:
    case R_HOST_OUT_LIMIT_ADDR:
    case R_HOST_OUT_READ_PTR:
    case R_HOST_OUT_OBJECT_SIZE:
    case R_HOST_INTR_MSG_ADDR:
    case R_HOST_INTR_MSG_DATA:
        val32 = host->regs[reg];
        break;
    case R_HOST_INTR_TEST:
    case R_HOST_ALERT_TEST:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: W/O register 0x%02" HWADDR_PRIx " (%s)\n", __func__,
                      addr, REG_NAME(HOST, reg));
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, s->ot_id, addr);
        val32 = 0;
        break;
    }

    uint32_t pc = ibex_get_current_pc();
    trace_ot_mbx_host_io_read_out(s->ot_id, (uint32_t)addr, REG_NAME(HOST, reg),
                                  val32, pc);

    return (uint64_t)val32;
};

static void ot_mbx_host_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                                   unsigned size)
{
    OtMbxState *s = opaque;
    (void)size;
    OtMbxHost *host = &s->host;
    uint32_t *hregs = host->regs;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_mbx_host_io_write(s->ot_id, (uint32_t)addr, REG_NAME(HOST, reg),
                               val32, pc);
    switch (reg) {
    case R_HOST_INTR_STATE:
        val32 &= HOST_INTR_MASK;
        hregs[reg] &= ~val32; /* RW1C */
        ot_mbx_host_update_irqs(s);
        break;
    case R_HOST_INTR_ENABLE:
        val32 &= HOST_INTR_MASK;
        hregs[reg] = val32;
        ot_mbx_host_update_irqs(s);
        break;
    case R_HOST_INTR_TEST:
        val32 &= HOST_INTR_MASK;
        hregs[R_HOST_INTR_STATE] |= val32;
        ot_mbx_host_update_irqs(s);
        break;
    case R_HOST_ALERT_TEST:
        val32 &= HOST_ALERT_TEST_MASK;
        for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
            ibex_irq_set(&host->alerts[ix], (int)((val32 >> ix) & 0x1u));
        }
        break;
    case R_HOST_CONTROL:
        if (val32 & R_HOST_CONTROL_ABORT_MASK) {
            /* clear busy once abort is cleared */
            trace_ot_mbx_change_state(s->ot_id, "clear busy");
            ot_mbx_clear_busy(s);
            hregs[reg] &= ~R_HOST_CONTROL_ABORT_MASK; /* RW1C */
        }
        if (val32 & R_HOST_CONTROL_ERROR_MASK) { /* RW1S */
            ot_mbx_set_error(s);
        };
        xtrace_ot_mbx_status(s);
        break;
    case R_HOST_STATUS:
    case R_HOST_IN_WRITE_PTR:
    case R_HOST_OUT_READ_PTR:
    case R_HOST_INTR_MSG_ADDR:
    case R_HOST_INTR_MSG_DATA:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s R/O register 0x%02" HWADDR_PRIx " (%s)\n",
                      __func__, s->ot_id, addr, REG_NAME(HOST, reg));
        break;
    case R_HOST_ADDRESS_RANGE_REGWEN:
        val32 &= R_HOST_ADDRESS_RANGE_REGWEN_EN_MASK;
        hregs[reg] = ot_multibitbool_w0c_write(hregs[reg], val32, 4u);
        break;
    case R_HOST_ADDRESS_RANGE_VALID: {
        val32 &= R_HOST_ADDRESS_RANGE_VALID_VALID_MASK;
        bool validate = !hregs[reg] && (bool)val32;
        hregs[reg] = val32;
        if (validate) {
            trace_ot_mbx_change_state(s->ot_id, "validate");
            ot_mbx_clear_busy(s);
            xtrace_ot_mbx_status(s);
        } else {
            trace_ot_mbx_change_state(s->ot_id, "invalidate");
            hregs[R_HOST_STATUS] |= R_HOST_STATUS_BUSY_MASK;
            xtrace_ot_mbx_status(s);
        }
    } break;
    case R_HOST_IN_BASE_ADDR:
    case R_HOST_IN_LIMIT_ADDR:
    case R_HOST_OUT_BASE_ADDR:
    case R_HOST_OUT_LIMIT_ADDR:
        if (hregs[R_HOST_ADDRESS_RANGE_REGWEN] == OT_MULTIBITBOOL4_TRUE) {
            val32 &= ~0b11u; /*b1..b0 always 0 */
            hregs[reg] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s regwen protected 0x%02" HWADDR_PRIx "\n",
                          __func__, s->ot_id, addr);
        }
        break;
    case R_HOST_OUT_OBJECT_SIZE:
        val32 &= R_HOST_OUT_OBJECT_SIZE_SIZE_MASK;
        if (ot_mbx_is_on_error(s)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s cannot update objsize: on error\n", __func__,
                          s->ot_id);
            break;
        }
        if (ot_mbx_is_on_abort(s)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s cannot update objsize: aborted\n", __func__,
                          s->ot_id);
            break;
        }
        hregs[reg] = val32;
        trace_ot_mbx_change_state(s->ot_id, "response available");
        if (val32) {
            hregs[R_HOST_OUT_READ_PTR] = hregs[R_HOST_OUT_BASE_ADDR];

            if (hregs[R_HOST_STATUS] & R_HOST_STATUS_SYS_INTR_ENABLE_MASK) {
                hregs[R_HOST_STATUS] |= R_HOST_STATUS_SYS_INTR_STATE_MASK;
            }
        }
        xtrace_ot_mbx_status(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, s->ot_id, addr);
        break;
    }
}

static void ot_mbx_sys_abort(OtMbxState *s)
{
    uint32_t *hregs = s->host.regs;

    trace_ot_mbx_change_state(s->ot_id, "abort");

    hregs[R_HOST_CONTROL] |= R_HOST_CONTROL_ABORT_MASK;

    /*
     * "DOE instance shall clear this bit [SYS_READY] in response to a DOE Abort
     * handling, if not already clear" -> SYS_READY is OBJECT_SIZE != 0
     */
    hregs[R_HOST_OUT_OBJECT_SIZE] = 0;

    /*
     * "This bit [BUSY] must be set by the DOE instance while processing an
     * abort command. Cleared when abort handling is complete"
     */
    hregs[R_HOST_STATUS] |= R_HOST_STATUS_BUSY_MASK;
    trace_ot_mbx_busy(s->ot_id, "set on abort");

    /*
     * "Bit [ERROR] is cleared by writing a 1’b1 to the DOE abort bit in the DOE
     * Control Register. DOE Abort is the only mechanism to clear this status
     * bit"
     */
    hregs[R_HOST_CONTROL] &= ~R_HOST_CONTROL_ERROR_MASK;

    hregs[R_HOST_INTR_STATE] |= INTR_MBX_ABORT_MASK;
}

static void ot_mbx_sys_go(OtMbxState *s)
{
    uint32_t *hregs = s->host.regs;

    trace_ot_mbx_change_state(s->ot_id, "go");

    if (!ot_mbx_is_on_abort(s)) {
        /*
         * accept GO even if an error has been flagged so the HOST side can
         * handle it and trigger an interrupt from FW
         */
        hregs[R_HOST_STATUS] |= R_HOST_STATUS_BUSY_MASK;
        trace_ot_mbx_busy(s->ot_id, "set on go");
        /* wild guess as doc is not available */
        hregs[R_HOST_INTR_STATE] |= INTR_MBX_READY_MASK;
        xtrace_ot_mbx_status(s);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s cannot GO: abort on going\n",
                      __func__, s->ot_id);
    }
}

static MemTxResult ot_mbx_sys_regs_read_with_attrs(
    void *opaque, hwaddr addr, uint64_t *val64, unsigned size, MemTxAttrs attrs)
{
    OtMbxState *s = opaque;
    (void)size;
    OtMbxHost *host = &s->host;
    OtMbxSys *sys = &s->sys;
    uint32_t *hregs = host->regs;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_DEPRECATED_MSG_ADDR:
    case R_DEPRECATED_MSG_DATA:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s deprecated reg %s 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, REG_NAME(SYS, reg), addr);
        val32 = 0;
        break;
    case R_SYS_CONTROL:
        val32 = hregs[R_HOST_STATUS] & R_HOST_STATUS_SYS_INTR_ENABLE_MASK ?
                    R_SYS_CONTROL_SYS_INT_EN_MASK :
                    0u;
        break;
    case R_SYS_STATUS:
        val32 = FIELD_DP32(0, SYS_STATUS, BUSY, (uint32_t)ot_mbx_is_busy(s));
        val32 = FIELD_DP32(val32, SYS_STATUS, INT,
                           (uint32_t)ot_mbx_is_sys_interrupt(s));
        val32 = FIELD_DP32(val32, SYS_STATUS, ERROR,
                           (uint32_t)ot_mbx_is_on_error(s));
        val32 = FIELD_DP32(val32, SYS_STATUS, READY,
                           (uint32_t)(hregs[R_HOST_OUT_OBJECT_SIZE] != 0));
        break;
    case R_SYS_READ_DATA:
        if (!ot_mbx_is_enabled(s)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s mailbox is not enabled\n",
                          __func__, s->ot_id);
            val32 = 0;
            break;
        }
        if (ot_mbx_is_on_error(s)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s mailbox is on error\n",
                          __func__, s->ot_id);
            val32 = 0;
            break;
        }
        if (hregs[R_HOST_OUT_OBJECT_SIZE] == 0) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s read underflow\n", __func__,
                          s->ot_id);
            val32 = 0u;
        }
        hwaddr raddr = (hwaddr)hregs[R_HOST_OUT_READ_PTR];
        MemTxResult mres;
        mres = address_space_rw(sys->ram_as, raddr, attrs, &val32,
                                sizeof(val32), false);
        if (mres != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s Cannot read @ 0x%" HWADDR_PRIx ": %u\n",
                          __func__, s->ot_id, raddr, mres);
            ibex_irq_set(&s->host.alerts[ALERT_RECOVERABLE], 1);
            val32 = 0u;
        }
        break;
    case R_SYS_WRITE_DATA:
        /* "Reads of this register must return all 0’s." */
        val32 = 0;
        break;
    case R_SYS_INTR_MSG_ADDR:
        val32 = host->regs[R_HOST_INTR_MSG_ADDR];
        break;
    case R_SYS_INTR_MSG_DATA:
        val32 = host->regs[R_HOST_INTR_MSG_DATA];
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, s->ot_id, addr);
        val32 = 0;
        break;
    }

    trace_ot_mbx_sys_io_read_out(s->ot_id, (uint32_t)addr, REG_NAME(SYS, reg),
                                 val32);

    *val64 = (uint64_t)val32;

    /* never returns an error */
    return MEMTX_OK;
};

static MemTxResult ot_mbx_sys_regs_write_with_attrs(
    void *opaque, hwaddr addr, uint64_t val64, unsigned size, MemTxAttrs attrs)
{
    OtMbxState *s = opaque;
    (void)size;
    OtMbxHost *host = &s->host;
    OtMbxSys *sys = &s->sys;
    uint32_t *hregs = host->regs;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    trace_ot_mbx_sys_io_write(s->ot_id, (uint32_t)addr, REG_NAME(SYS, reg),
                              val32);

    switch (reg) {
    case R_DEPRECATED_MSG_ADDR:
    case R_DEPRECATED_MSG_DATA:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s deprecated reg %s 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, REG_NAME(SYS, reg), addr);
        break;
    case R_SYS_CONTROL:
        if (ot_mbx_is_enabled(s)) {
            if (val32 & R_SYS_CONTROL_ABORT_MASK) {
                ot_mbx_sys_abort(s);
            } else if (val32 & R_SYS_CONTROL_GO_MASK) {
                ot_mbx_sys_go(s);
            }
            if (val32 & R_SYS_CONTROL_SYS_INT_EN_MASK) {
                hregs[R_HOST_STATUS] |= R_HOST_STATUS_SYS_INTR_ENABLE_MASK;
            } else {
                hregs[R_HOST_STATUS] &= ~R_HOST_STATUS_SYS_INTR_ENABLE_MASK;
            }
            ot_mbx_host_update_irqs(s);
        } else {
            if (val32) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "%s: %s mailbox is not enabled\n", __func__,
                              s->ot_id);
            }
        }
        xtrace_ot_mbx_status(s);
        break;
    case R_SYS_STATUS:
        if (val32 & R_SYS_STATUS_INT_MASK) { /* RW1C bit */
            hregs[R_HOST_STATUS] &= ~R_HOST_STATUS_SYS_INTR_STATE_MASK;
        }
        break;
    case R_SYS_WRITE_DATA:
        if (!ot_mbx_is_enabled(s)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s mailbox is not enabled\n",
                          __func__, s->ot_id);
            break;
        }
        if (ot_mbx_is_on_error(s)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s mailbox is on error\n",
                          __func__, s->ot_id);
            break;
        }
        if (ot_mbx_is_busy(s)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s mailbox is busy\n", __func__,
                          s->ot_id);
            break;
        }
        if (hregs[R_HOST_IN_WRITE_PTR] >= hregs[R_HOST_IN_LIMIT_ADDR]) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s write overflow\n", __func__,
                          s->ot_id);
            ot_mbx_set_error(s);
            xtrace_ot_mbx_status(s);
        }
        hwaddr waddr = (hwaddr)hregs[R_HOST_IN_WRITE_PTR];
        MemTxResult mres;
        mres = address_space_rw(sys->ram_as, waddr, attrs, &val32,
                                sizeof(val32), true);
        if (mres != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s Cannot write @ 0x%" HWADDR_PRIx ": %u\n",
                          __func__, s->ot_id, waddr, mres);
            ot_mbx_set_error(s);
            xtrace_ot_mbx_status(s);
            ibex_irq_set(&s->host.alerts[ALERT_RECOVERABLE], 1);
            break;
        }
        hregs[R_HOST_IN_WRITE_PTR] += sizeof(uint32_t);
        break;
    case R_SYS_READ_DATA:
        if (!ot_mbx_is_enabled(s)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s mailbox is not enabled\n",
                          __func__, s->ot_id);
            break;
        }
        if (ot_mbx_is_on_error(s)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s mailbox is on error\n",
                          __func__, s->ot_id);
            break;
        }
        if (hregs[R_HOST_OUT_OBJECT_SIZE]) {
            hregs[R_HOST_OUT_READ_PTR] += sizeof(uint32_t);
            hregs[R_HOST_OUT_OBJECT_SIZE] -= 1u;
        }
        if (hregs[R_HOST_OUT_OBJECT_SIZE] == 0) {
            /* reset the read pointer*/
            hregs[R_HOST_OUT_READ_PTR] = hregs[R_HOST_OUT_BASE_ADDR];
            /* clear busy once the full response has been read */
            ot_mbx_clear_busy(s);
            xtrace_ot_mbx_status(s);
        }
        break;
    case R_SYS_INTR_MSG_ADDR:
        host->regs[R_HOST_INTR_MSG_ADDR] = val32;
        break;
    case R_SYS_INTR_MSG_DATA:
        host->regs[R_HOST_INTR_MSG_DATA] = val32;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, s->ot_id, addr);
        break;
    }

    /* never returns an error */
    return MEMTX_OK;
}

static Property ot_mbx_properties[] = {
    DEFINE_PROP_STRING("ot_id", OtMbxState, ot_id),
    DEFINE_PROP_STRING("ram_as_name", OtMbxState, ram_as_name),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_mbx_host_regs_ops = {
    .read = &ot_mbx_host_regs_read,
    .write = &ot_mbx_host_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static const MemoryRegionOps ot_mbx_sys_regs_ops = {
    .read_with_attrs = &ot_mbx_sys_regs_read_with_attrs,
    .write_with_attrs = &ot_mbx_sys_regs_write_with_attrs,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_mbx_reset(DeviceState *dev)
{
    OtMbxState *s = OT_MBX(dev);

    g_assert(s->ot_id);

    OtMbxHost *host = &s->host;
    OtMbxSys *sys = &s->sys;

    memset(host->regs, 0, sizeof(host->regs));
    memset(sys->regs, 0, sizeof(sys->regs));
    host->regs[R_HOST_ADDRESS_RANGE_REGWEN] = OT_MULTIBITBOOL4_TRUE;
    host->regs[R_HOST_STATUS] = R_SYS_STATUS_BUSY_MASK;

    if (!sys->ram_as) {
        Object *soc = OBJECT(dev)->parent;
        Object *obj =
            object_property_get_link(soc, s->ram_as_name, &error_fatal);
        OtAddressSpaceState *oas =
            OBJECT_CHECK(OtAddressSpaceState, obj, TYPE_OT_ADDRESS_SPACE);
        sys->ram_as = ot_address_space_get(oas);
        g_assert(sys->ram_as);
    }

    ot_mbx_host_update_irqs(s);
    for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
        ibex_irq_set(&s->host.alerts[ix], 0);
    }

    xtrace_ot_mbx_status(s);
}

static void ot_mbx_init(Object *obj)
{
    OtMbxState *s = OT_MBX(obj);

    memory_region_init_io(&s->host.mmio, obj, &ot_mbx_host_regs_ops, s,
                          TYPE_OT_MBX "-host", REGS_HOST_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->host.mmio);
    for (unsigned ix = 0; ix < PARAM_NUM_IRQS; ix++) {
        ibex_sysbus_init_irq(obj, &s->host.irqs[ix]);
    }
    for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
        ibex_qdev_init_irq(obj, &s->host.alerts[ix], OT_DEVICE_ALERT);
    }

    memory_region_init_io(&s->sys.mmio, obj, &ot_mbx_sys_regs_ops, s,
                          TYPE_OT_MBX "-sys", REGS_SYS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->sys.mmio);
}

static void ot_mbx_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->reset = &ot_mbx_reset;
    device_class_set_props(dc, ot_mbx_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_mbx_info = {
    .name = TYPE_OT_MBX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtMbxState),
    .instance_init = &ot_mbx_init,
    .class_init = &ot_mbx_class_init,
};

static void ot_mbx_register_types(void)
{
    type_register_static(&ot_mbx_info);
}

type_init(ot_mbx_register_types);
