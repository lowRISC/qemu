/*
 * QEMU OpenTitan Clock manager device
 *
 * Copyright (c) 2023 Rivos, Inc.
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
 * For now, only clock hinting for transactional blocks is actually implemented.
 */

#include "qemu/osdep.h"
#include "qemu/bitmap.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "hw/irq.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_clkmgr.h"
#include "hw/opentitan/ot_common.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "trace.h"

#define PARAM_NUM_GROUPS             7u
#define PARAM_NUM_SW_GATEABLE_CLOCKS 4u
#define PARAM_NUM_HINTABLE_CLOCKS    4u
#define PARAM_NUM_ALERTS             2u

/* clang-format off */
REG32(ALERT_TEST, 0x0u)
    FIELD(ALERT_TEST, RECOV_FAULT, 0u, 1u)
    FIELD(ALERT_TEST, FATAL_FAULT, 1u, 1u)
REG32(EXTCLK_CTRL_REGWEN, 0x4u)
    FIELD(EXTCLK_CTRL, REGWEN_EN, 0u, 1u)
REG32(EXTCLK_CTRL, 0x8u)
    FIELD(EXTCLK_CTRL, SEL, 0u, 4u)
    FIELD(EXTCLK_CTRL, HI_SPEED_SEL, 4u, 4u)
REG32(EXTCLK_STATUS, 0xcu)
    FIELD(EXTCLK_STATUS, ACK, 0u, 4u)
REG32(JITTER_REGWEN, 0x10u)
    FIELD(JITTER_REGWEN, EN, 0u, 1u)
REG32(JITTER_ENABLE, 0x14u)
    FIELD(JITTER_ENABLE, VAL, 0, 4u)
REG32(CLK_ENABLES, 0x18u)
    FIELD(CLK_ENABLES, CLK_IO_DIV4_PERI_EN, 0u, 1u)
    FIELD(CLK_ENABLES, CLK_IO_DIV2_PERI_EN, 1u, 1u)
    FIELD(CLK_ENABLES, CLK_IO_PERI_EN, 2u, 1u)
    FIELD(CLK_ENABLES, CLK_USB_PERI_EN, 3u, 1u)
REG32(CLK_HINTS, 0x1cu)
    SHARED_FIELD(CLK_HINTS_MAIN_AES, (unsigned)OT_CLKMGR_HINT_AES, 1u)
    SHARED_FIELD(CLK_HINTS_MAIN_HMAC, (unsigned)OT_CLKMGR_HINT_HMAC, 1u)
    SHARED_FIELD(CLK_HINTS_MAIN_KMAC, (unsigned)OT_CLKMGR_HINT_KMAC, 1u)
    SHARED_FIELD(CLK_HINTS_MAIN_OTBN, (unsigned)OT_CLKMGR_HINT_OTBN, 1u)
REG32(CLK_HINTS_STATUS, 0x20u)
REG32(MEASURE_CTRL_REGWEN, 0x24u)
    FIELD(MEASURE_CTRL_REGWEN, EN, 0u, 1u)
REG32(IO_MEAS_CTRL_EN, 0x28u)
    FIELD(IO_MEAS_CTRL_EN, EN, 0u, 4u)
REG32(IO_MEAS_CTRL_SHADOWED, 0x2cu)
    FIELD(IO_MEAS_CTRL_SHADOWED, HI, 0u, 10u)
    FIELD(IO_MEAS_CTRL_SHADOWED, LO, 10u, 1u)
REG32(IO_DIV2_MEAS_CTRL_EN, 0x30u)
    FIELD(IO_DIV2_MEAS_CTRL_EN, EN, 0u, 4u)
REG32(IO_DIV2_MEAS_CTRL_SHADOWED, 0x34u)
    FIELD(IO_DIV2_MEAS_CTRL_SHADOWED, HI, 0u, 9u)
    FIELD(IO_DIV2_MEAS_CTRL_SHADOWED, LO, 9u, 9u)
REG32(IO_DIV4_MEAS_CTRL_EN, 0x38u)
    FIELD(IO_DIV4_MEAS_CTRL_EN, EN, 0u, 4u)
REG32(IO_DIV4_MEAS_CTRL_SHADOWED, 0x3cu)
    FIELD(IO_DIV4_MEAS_CTRL_SHADOWED, HI, 0u, 8u)
    FIELD(IO_DIV4_MEAS_CTRL_SHADOWED, LO, 8u, 8u)
REG32(MAIN_MEAS_CTRL_EN, 0x40u)
    FIELD(MAIN_MEAS_CTRL_EN, EN, 0u, 4u)
REG32(MAIN_MEAS_CTRL_SHADOWED, 0x44u)
    FIELD(MAIN_MEAS_CTRL_SHADOWED, HI, 0u, 10u)
    FIELD(MAIN_MEAS_CTRL_SHADOWED, LO, 10u, 10u)
REG32(USB_MEAS_CTRL_EN, 0x48u)
    FIELD(USB_MEAS_CTRL_EN, EN, 0u, 4u)
REG32(USB_MEAS_CTRL_SHADOWED, 0x4cu)
    FIELD(USB_MEAS_CTRL_SHADOWED, HI, 0u, 9u)
    FIELD(USB_MEAS_CTRL_SHADOWED, LO, 9u, 9u)
REG32(RECOV_ERR_CODE, 0x50u)
    FIELD(RECOV_ERR_CODE, SHADOW_UPDATE_ERR, 0u, 1u)
    FIELD(RECOV_ERR_CODE, IO_MEASURE_ERR, 1u, 1u)
    FIELD(RECOV_ERR_CODE, IO_DIV2_MEASURE_ERR, 2u, 1u)
    FIELD(RECOV_ERR_CODE, IO_DIV4_MEASURE_ERR, 3u, 1u)
    FIELD(RECOV_ERR_CODE, MAIN_MEASURE_ERR, 4u, 1u)
    FIELD(RECOV_ERR_CODE, USB_MEASURE_ERR, 5u, 1u)
    FIELD(RECOV_ERR_CODE, IO_TIMEOUT_ERR, 6u, 1u)
    FIELD(RECOV_ERR_CODE, IO_DIV2_TIMEOUT_ERR, 7u, 1u)
    FIELD(RECOV_ERR_CODE, IO_DIV4_TIMEOUT_ERR, 8u, 1u)
    FIELD(RECOV_ERR_CODE, MAIN_TIMEOUT_ERR, 9u, 1u)
    FIELD(RECOV_ERR_CODE, USB_TIMEOUT_ERR, 10u, 1u)
REG32(FATAL_ERR_CODE, 0x54u)
    FIELD(FATAL_ERR_CODE, REG_INTG, 0u, 1u)
    FIELD(FATAL_ERR_CODE, IDLE_CNT, 1u, 1u)
    FIELD(FATAL_ERR_CODE, SHADOW_STORAGE_ERR, 2u, 1u)
/* clang-format on */

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_FATAL_ERR_CODE)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define ALERT_TEST_MASK \
    (R_ALERT_TEST_RECOV_FAULT_MASK | R_ALERT_TEST_FATAL_FAULT_MASK)
#define CLK_ENABLES_MASK \
    (R_CLK_ENABLES_CLK_IO_DIV4_PERI_EN_MASK | \
     R_CLK_ENABLES_CLK_IO_DIV2_PERI_EN_MASK | \
     R_CLK_ENABLES_CLK_IO_PERI_EN_MASK | R_CLK_ENABLES_CLK_USB_PERI_EN_MASK)
#define CLK_HINTS_MASK \
    (CLK_HINTS_MAIN_AES_MASK | CLK_HINTS_MAIN_HMAC_MASK | \
     CLK_HINTS_MAIN_KMAC_MASK | CLK_HINTS_MAIN_OTBN_MASK)
#define RECOV_ERR_CODE_MASK \
    (R_RECOV_ERR_CODE_SHADOW_UPDATE_ERR_MASK | \
     R_RECOV_ERR_CODE_IO_MEASURE_ERR_MASK | \
     R_RECOV_ERR_CODE_IO_DIV2_MEASURE_ERR_MASK | \
     R_RECOV_ERR_CODE_IO_DIV4_MEASURE_ERR_MASK | \
     R_RECOV_ERR_CODE_MAIN_MEASURE_ERR_MASK | \
     R_RECOV_ERR_CODE_USB_MEASURE_ERR_MASK | \
     R_RECOV_ERR_CODE_IO_TIMEOUT_ERR_MASK | \
     R_RECOV_ERR_CODE_IO_DIV2_TIMEOUT_ERR_MASK | \
     R_RECOV_ERR_CODE_IO_DIV4_TIMEOUT_ERR_MASK | \
     R_RECOV_ERR_CODE_MAIN_TIMEOUT_ERR_MASK | \
     R_RECOV_ERR_CODE_USB_TIMEOUT_ERR_MASK)

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(EXTCLK_CTRL_REGWEN),
    REG_NAME_ENTRY(EXTCLK_CTRL),
    REG_NAME_ENTRY(EXTCLK_STATUS),
    REG_NAME_ENTRY(JITTER_REGWEN),
    REG_NAME_ENTRY(JITTER_ENABLE),
    REG_NAME_ENTRY(CLK_ENABLES),
    REG_NAME_ENTRY(CLK_HINTS),
    REG_NAME_ENTRY(CLK_HINTS_STATUS),
    REG_NAME_ENTRY(MEASURE_CTRL_REGWEN),
    REG_NAME_ENTRY(IO_MEAS_CTRL_EN),
    REG_NAME_ENTRY(IO_MEAS_CTRL_SHADOWED),
    REG_NAME_ENTRY(IO_DIV2_MEAS_CTRL_EN),
    REG_NAME_ENTRY(IO_DIV2_MEAS_CTRL_SHADOWED),
    REG_NAME_ENTRY(IO_DIV4_MEAS_CTRL_EN),
    REG_NAME_ENTRY(IO_DIV4_MEAS_CTRL_SHADOWED),
    REG_NAME_ENTRY(MAIN_MEAS_CTRL_EN),
    REG_NAME_ENTRY(MAIN_MEAS_CTRL_SHADOWED),
    REG_NAME_ENTRY(USB_MEAS_CTRL_EN),
    REG_NAME_ENTRY(USB_MEAS_CTRL_SHADOWED),
    REG_NAME_ENTRY(RECOV_ERR_CODE),
    REG_NAME_ENTRY(FATAL_ERR_CODE),
};
#undef REG_NAME_ENTRY

enum {
    ALERT_RECOVERABLE,
    ALERT_FATAL,
};

typedef struct OtClkMgrShadowRegisters {
    OtShadowReg io_meas_ctrl;
    OtShadowReg io_div2_meas_ctrl;
    OtShadowReg io_div4_meas_ctrl;
    OtShadowReg main_meas_ctrl;
    OtShadowReg usb_meas_ctrl;
} OtClkMgrRegisters;

struct OtClkMgrState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    IbexIRQ hints[OT_CLKMGR_HINT_COUNT];
    IbexIRQ alerts[PARAM_NUM_ALERTS];

    uint32_t clock_states; /* bit set: active, reset: clock is idle */
    uint32_t regs[REGS_COUNT]; /* shadowed slots are not used */
    OtClkMgrRegisters sdw_regs;
};

static const char *CLOCK_NAMES[OT_CLKMGR_HINT_COUNT] = {
    [OT_CLKMGR_HINT_AES] = "AES",
    [OT_CLKMGR_HINT_HMAC] = "HMAC",
    [OT_CLKMGR_HINT_KMAC] = "KMAC",
    [OT_CLKMGR_HINT_OTBN] = "OTBN",
};
#define CLOCK_NAME(_clk_) \
    ((_clk_) < ARRAY_SIZE(CLOCK_NAMES) ? CLOCK_NAMES[(_clk_)] : "?")

static void ot_clkmgr_update_alerts(OtClkMgrState *s)
{
    bool recov = (bool)(s->regs[R_RECOV_ERR_CODE] &
                        R_RECOV_ERR_CODE_SHADOW_UPDATE_ERR_MASK);
    ibex_irq_set(&s->alerts[ALERT_RECOVERABLE], recov);
}

static void ot_clkmgr_clock_hint(void *opaque, int irq, int level)
{
    OtClkMgrState *s = opaque;

    unsigned clock = (unsigned)irq;

    assert(clock < OT_CLKMGR_HINT_COUNT);

    trace_ot_clkmgr_clock_hint(CLOCK_NAME(clock), clock, (bool)level);

    if (level) {
        s->clock_states |= 1u << clock;
    } else {
        s->clock_states &= ~(1u << clock);
    }
}

static uint32_t ot_clkmgr_get_clock_hints(OtClkMgrState *s)
{
    uint32_t hint_status = s->regs[R_CLK_HINTS] | s->clock_states;

    trace_ot_clkmgr_get_clock_hints(s->regs[R_CLK_HINTS], s->clock_states,
                                    hint_status);

    return hint_status;
}

static uint64_t ot_clkmgr_read(void *opaque, hwaddr addr, unsigned size)
{
    OtClkMgrState *s = opaque;

    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_EXTCLK_CTRL_REGWEN:
    case R_EXTCLK_CTRL:
    case R_EXTCLK_STATUS:
    case R_JITTER_REGWEN:
    case R_JITTER_ENABLE:
    case R_CLK_ENABLES:
    case R_CLK_HINTS:
    case R_MEASURE_CTRL_REGWEN:
    case R_IO_MEAS_CTRL_EN:
    case R_IO_DIV2_MEAS_CTRL_EN:
    case R_IO_DIV4_MEAS_CTRL_EN:
    case R_MAIN_MEAS_CTRL_EN:
    case R_USB_MEAS_CTRL_EN:
    case R_RECOV_ERR_CODE:
    case R_FATAL_ERR_CODE:
        val32 = s->regs[reg];
        break;
    case R_IO_MEAS_CTRL_SHADOWED:
        val32 = ot_shadow_reg_read(&s->sdw_regs.io_meas_ctrl);
        break;
    case R_IO_DIV2_MEAS_CTRL_SHADOWED:
        val32 = ot_shadow_reg_read(&s->sdw_regs.io_div2_meas_ctrl);
        break;
    case R_IO_DIV4_MEAS_CTRL_SHADOWED:
        val32 = ot_shadow_reg_read(&s->sdw_regs.io_div4_meas_ctrl);
        break;
    case R_MAIN_MEAS_CTRL_SHADOWED:
        val32 = ot_shadow_reg_read(&s->sdw_regs.main_meas_ctrl);
        break;
    case R_USB_MEAS_CTRL_SHADOWED:
        val32 = ot_shadow_reg_read(&s->sdw_regs.usb_meas_ctrl);
        break;
    case R_CLK_HINTS_STATUS:
        val32 = ot_clkmgr_get_clock_hints(s);
        break;
    case R_ALERT_TEST:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "W/O register 0x02%" HWADDR_PRIx " (%s)\n", addr,
                      REG_NAME(reg));
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        val32 = 0u;
        break;
    }

    uint64_t pc = ibex_get_current_pc();
    trace_ot_clkmgr_io_read_out((unsigned)addr, REG_NAME(reg), (uint64_t)val32,
                                pc);

    return (uint64_t)val32;
};

static void ot_clkmgr_write(void *opaque, hwaddr addr, uint64_t val64,
                            unsigned size)
{
    OtClkMgrState *s = opaque;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint64_t pc = ibex_get_current_pc();
    trace_ot_clkmgr_io_write((unsigned)addr, REG_NAME(reg), val64, pc);

    switch (reg) {
    case R_ALERT_TEST:
        val32 &= ALERT_TEST_MASK;
        if (val32) {
            for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
                ibex_irq_set(&s->alerts[ix], (int)((val32 >> ix) & 0x1u));
            }
        }
        break;
    case R_EXTCLK_CTRL_REGWEN:
        val32 &= R_EXTCLK_CTRL_REGWEN_EN_MASK;
        s->regs[reg] &= val32;
        break;
    case R_EXTCLK_CTRL:
        if (s->regs[R_EXTCLK_CTRL_REGWEN]) {
            val32 &= R_EXTCLK_CTRL_SEL_MASK | R_EXTCLK_CTRL_HI_SPEED_SEL_MASK;
            s->regs[reg] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: EXTCLK_CTRL protected w/ REGWEN\n", __func__);
        }
        break;
    case R_JITTER_REGWEN:
        val32 &= R_JITTER_REGWEN_EN_MASK;
        s->regs[reg] &= val32;
        break;
    case R_JITTER_ENABLE:
        if (s->regs[R_JITTER_REGWEN]) {
            val32 &= R_JITTER_ENABLE_VAL_MASK;
            s->regs[reg] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: JITTER_ENABLE protected w/ REGWEN\n", __func__);
        }
        break;
    case R_CLK_ENABLES:
        val32 &= CLK_ENABLES_MASK;
        s->regs[reg] = val32;
        break;
    case R_CLK_HINTS:
        val32 &= CLK_HINTS_MASK;
        s->regs[reg] = val32;
        break;
    case R_MEASURE_CTRL_REGWEN:
        val32 &= R_MEASURE_CTRL_REGWEN_EN_MASK;
        s->regs[reg] &= val32;
        break;
    case R_IO_MEAS_CTRL_EN:
        if (s->regs[R_MEASURE_CTRL_REGWEN]) {
            val32 &= R_IO_MEAS_CTRL_EN_EN_MASK;
            s->regs[reg] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: IO_MEAS_CTRL_EN protected w/ REGWEN\n",
                          __func__);
        }
        break;
    case R_IO_MEAS_CTRL_SHADOWED:
        if (s->regs[R_MEASURE_CTRL_REGWEN]) {
            val32 &= R_IO_MEAS_CTRL_EN_EN_MASK;
            switch (ot_shadow_reg_write(&s->sdw_regs.io_meas_ctrl, val32)) {
            case OT_SHADOW_REG_STAGED:
            case OT_SHADOW_REG_COMMITTED:
                break;
            case OT_SHADOW_REG_ERROR:
            default:
                s->regs[R_RECOV_ERR_CODE] |=
                    R_RECOV_ERR_CODE_SHADOW_UPDATE_ERR_MASK;
                ot_clkmgr_update_alerts(s);
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: IO_MEAS_CTRL_SHADOWED protected w/ REGWEN\n",
                          __func__);
        }
        break;
    case R_IO_DIV2_MEAS_CTRL_EN:
        if (s->regs[R_MEASURE_CTRL_REGWEN]) {
            val32 &= R_IO_DIV2_MEAS_CTRL_EN_EN_MASK;
            s->regs[reg] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: R_IO_DIV2_MEAS_CTRL_EN protected w/ REGWEN\n",
                          __func__);
        }
        break;
    case R_IO_DIV2_MEAS_CTRL_SHADOWED:
        if (s->regs[R_MEASURE_CTRL_REGWEN]) {
            val32 &= R_IO_MEAS_CTRL_EN_EN_MASK;
            switch (
                ot_shadow_reg_write(&s->sdw_regs.io_div2_meas_ctrl, val32)) {
            case OT_SHADOW_REG_STAGED:
            case OT_SHADOW_REG_COMMITTED:
                break;
            case OT_SHADOW_REG_ERROR:
            default:
                s->regs[R_RECOV_ERR_CODE] |=
                    R_RECOV_ERR_CODE_SHADOW_UPDATE_ERR_MASK;
                ot_clkmgr_update_alerts(s);
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: IO_MEAS_CTRL_SHADOWED protected w/ REGWEN\n",
                          __func__);
        }
        break;
    case R_IO_DIV4_MEAS_CTRL_EN:
        if (s->regs[R_MEASURE_CTRL_REGWEN]) {
            val32 &= R_IO_DIV4_MEAS_CTRL_EN_EN_MASK;
            s->regs[reg] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: R_IO_DIV4_MEAS_CTRL_EN protected w/ REGWEN\n",
                          __func__);
        }
        break;
    case R_IO_DIV4_MEAS_CTRL_SHADOWED:
        if (s->regs[R_MEASURE_CTRL_REGWEN]) {
            val32 &= R_IO_MEAS_CTRL_EN_EN_MASK;
            switch (
                ot_shadow_reg_write(&s->sdw_regs.io_div4_meas_ctrl, val32)) {
            case OT_SHADOW_REG_STAGED:
            case OT_SHADOW_REG_COMMITTED:
                break;
            case OT_SHADOW_REG_ERROR:
            default:
                s->regs[R_RECOV_ERR_CODE] |=
                    R_RECOV_ERR_CODE_SHADOW_UPDATE_ERR_MASK;
                ot_clkmgr_update_alerts(s);
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: IO_MEAS_CTRL_SHADOWED protected w/ REGWEN\n",
                          __func__);
        }
        break;
    case R_MAIN_MEAS_CTRL_EN:
        if (s->regs[R_MEASURE_CTRL_REGWEN]) {
            val32 &= R_MAIN_MEAS_CTRL_EN_EN_MASK;
            s->regs[reg] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: R_MAIN_MEAS_CTRL_EN protected w/ REGWEN\n",
                          __func__);
        }
        break;
    case R_MAIN_MEAS_CTRL_SHADOWED:
        if (s->regs[R_MEASURE_CTRL_REGWEN]) {
            val32 &= R_IO_MEAS_CTRL_EN_EN_MASK;
            switch (ot_shadow_reg_write(&s->sdw_regs.main_meas_ctrl, val32)) {
            case OT_SHADOW_REG_STAGED:
            case OT_SHADOW_REG_COMMITTED:
                break;
            case OT_SHADOW_REG_ERROR:
            default:
                s->regs[R_RECOV_ERR_CODE] |=
                    R_RECOV_ERR_CODE_SHADOW_UPDATE_ERR_MASK;
                ot_clkmgr_update_alerts(s);
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: IO_MEAS_CTRL_SHADOWED protected w/ REGWEN\n",
                          __func__);
        }
        break;
    case R_USB_MEAS_CTRL_EN:
        if (s->regs[R_MEASURE_CTRL_REGWEN]) {
            val32 &= R_USB_MEAS_CTRL_EN_EN_MASK;
            s->regs[reg] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: R_USB_MEAS_CTRL_EN protected w/ REGWEN\n",
                          __func__);
        }
        break;
    case R_USB_MEAS_CTRL_SHADOWED:
        if (s->regs[R_MEASURE_CTRL_REGWEN]) {
            val32 &= R_IO_MEAS_CTRL_EN_EN_MASK;
            switch (ot_shadow_reg_write(&s->sdw_regs.usb_meas_ctrl, val32)) {
            case OT_SHADOW_REG_STAGED:
            case OT_SHADOW_REG_COMMITTED:
                break;
            case OT_SHADOW_REG_ERROR:
            default:
                s->regs[R_RECOV_ERR_CODE] |=
                    R_RECOV_ERR_CODE_SHADOW_UPDATE_ERR_MASK;
                ot_clkmgr_update_alerts(s);
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: IO_MEAS_CTRL_SHADOWED protected w/ REGWEN\n",
                          __func__);
        }
        break;
    case R_RECOV_ERR_CODE:
        val32 &= RECOV_ERR_CODE_MASK;
        s->regs[reg] &= ~val32; /* RW1C */
        break;
    case R_EXTCLK_STATUS:
    case R_CLK_HINTS_STATUS:
    case R_FATAL_ERR_CODE:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: R/O register 0x02%" HWADDR_PRIx " (%s)\n", __func__,
                      addr, REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
};

static Property ot_clkmgr_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_clkmgr_regs_ops = {
    .read = &ot_clkmgr_read,
    .write = &ot_clkmgr_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_clkmgr_reset(DeviceState *dev)
{
    OtClkMgrState *s = OT_CLKMGR(dev);

    memset(s->regs, 0, sizeof(s->regs));

    s->regs[R_EXTCLK_CTRL_REGWEN] = 0x1u;
    s->regs[R_EXTCLK_CTRL] = 0x99u;
    s->regs[R_EXTCLK_STATUS] = 0x9u;
    s->regs[R_JITTER_REGWEN] = 0x1u;
    s->regs[R_JITTER_ENABLE] = 0x9u;
    s->regs[R_CLK_ENABLES] = 0xfu;
    s->regs[R_CLK_HINTS] = 0xfu;
    s->regs[R_CLK_HINTS_STATUS] = 0xfu;
    s->regs[R_MEASURE_CTRL_REGWEN] = 0x1u;
    s->regs[R_IO_MEAS_CTRL_EN] = 0x9u;
    s->regs[R_IO_DIV2_MEAS_CTRL_EN] = 0x9u;
    s->regs[R_IO_DIV4_MEAS_CTRL_EN] = 0x9u;
    s->regs[R_MAIN_MEAS_CTRL_EN] = 0x9u;
    s->regs[R_USB_MEAS_CTRL_EN] = 0x9u;
    ot_shadow_reg_init(&s->sdw_regs.io_meas_ctrl, 0x759eau);
    ot_shadow_reg_init(&s->sdw_regs.io_div2_meas_ctrl, 0x1ccfau);
    ot_shadow_reg_init(&s->sdw_regs.io_div4_meas_ctrl, 0x6e82u);
    ot_shadow_reg_init(&s->sdw_regs.main_meas_ctrl, 0x7a9feu);
    ot_shadow_reg_init(&s->sdw_regs.usb_meas_ctrl, 0x1ccfau);

    for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
        ibex_irq_set(&s->alerts[ix], 0);
    }
}

static void ot_clkmgr_init(Object *obj)
{
    OtClkMgrState *s = OT_CLKMGR(obj);

    memory_region_init_io(&s->mmio, obj, &ot_clkmgr_regs_ops, s, TYPE_OT_CLKMGR,
                          REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
        ibex_qdev_init_irq(obj, &s->alerts[ix], OPENTITAN_DEVICE_ALERT);
    }

    qdev_init_gpio_in_named(DEVICE(obj), &ot_clkmgr_clock_hint,
                            OPENTITAN_CLKMGR_HINT, OT_CLKMGR_HINT_COUNT);
}

static void ot_clkmgr_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = &ot_clkmgr_reset;
    device_class_set_props(dc, ot_clkmgr_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_clkmgr_info = {
    .name = TYPE_OT_CLKMGR,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtClkMgrState),
    .instance_init = &ot_clkmgr_init,
    .class_init = &ot_clkmgr_class_init,
};

static void ot_clkmgr_register_types(void)
{
    type_register_static(&ot_clkmgr_info);
}

type_init(ot_clkmgr_register_types)
