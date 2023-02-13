/*
 * QEMU OpenTitan PinMux device
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
 * Note: for now, only a minimalist subset of Power Manager device is
 *       implemented in order to enable OpenTitan's ROM boot to progress
 */

#include "qemu/osdep.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_pinmux.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "trace.h"


#define PARAM_N_MIO_PERIPH_IN  57u
#define PARAM_N_MIO_PERIPH_OUT 75u
#define PARAM_N_MIO_PADS       47u
#define PARAM_N_DIO_PADS       16u
#define PARAM_N_WKUP_DETECT    8u
#define PARAM_NUM_ALERTS       1u

/* clang-format off */
REG32(ALERT_TEST, 0x0u)
    FIELD(ALERT_TEST, FATAL_FAULT, 0u, 1u)
REG32(MIO_PERIPH_INSEL_REGWEN, 0x4u)
    FIELD(MIO_PERIPH_INSEL_REGWEN, EN, 0u, 1u)
REG32(MIO_PERIPH_INSEL, 0xe8u)
    FIELD(MIO_PERIPH_INSEL, IN, 0u, 6u)
REG32(MIO_OUTSEL_REGWEN, 0x1ccu)
    FIELD(MIO_OUTSEL_REGWEN, EN, 0u, 1u)
REG32(MIO_OUTSEL, 0x288u)
    FIELD(MIO_PERIPH_OUTSEL, OUT, 0u, 6u)
REG32(MIO_PAD_ATTR_REGWEN, 0x344u)
    FIELD(MIO_PAD_ATTR_REGWEN, EN, 0u, 1u)
REG32(MIO_PAD_ATTR, 0x400u)
    FIELD(MIO_PAD_ATTR, INVERT, 0u, 1u)
    FIELD(MIO_PAD_ATTR, VIRTUAL_OD_EN, 1u, 1u)
    FIELD(MIO_PAD_ATTR, PULL_EN, 2u, 1u)
    FIELD(MIO_PAD_ATTR, PULL_SELECT, 3u, 1u)
    FIELD(MIO_PAD_ATTR, KEEPER_EN, 4u, 1u)
    FIELD(MIO_PAD_ATTR, SCHMITT_EN, 5u, 1u)
    FIELD(MIO_PAD_ATTR, OD_EN, 6u, 1u)
    FIELD(MIO_PAD_ATTR, SLEW_RATE, 16u, 0x2u)
    FIELD(MIO_PAD_ATTR, DRIVE_STRENGTH, 20u, 4u)
REG32(DIO_PAD_ATTR_REGWEN, 0x4bcu)
    FIELD(DIO_PAD_ATTR_REGWEN, EN, 0u, 1u)
REG32(DIO_PAD_ATTR, 0x4fcu)
    FIELD(DIO_PAD_ATTR, INVERT, 0u, 1u)
    FIELD(DIO_PAD_ATTR, VIRTUAL_OD_EN, 1u, 1u)
    FIELD(DIO_PAD_ATTR, PULL_EN, 2u, 1u)
    FIELD(DIO_PAD_ATTR, PULL_SELECT, 3u, 1u)
    FIELD(DIO_PAD_ATTR, KEEPER_EN, 4u, 1u)
    FIELD(DIO_PAD_ATTR, SCHMITT_EN, 5u, 1u)
    FIELD(DIO_PAD_ATTR, OD_EN, 6u, 1u)
    FIELD(DIO_PAD_ATTR, SLEW_RATE, 16u, 2u)
    FIELD(DIO_PAD_ATTR, DRIVE_STRENGTH, 20u, 4u)
REG32(MIO_PAD_SLEEP_STATUS, 0x53cu)
REG32(MIO_PAD_SLEEP_REGWEN, 0x544u)
    FIELD(MIO_PAD_SLEEP_REGWEN, EN, 0u, 1u)
REG32(MIO_PAD_SLEEP, 0x600u)
    FIELD(MIO_PAD_SLEEP, EN, 0u, 1u)
REG32(MIO_PAD_SLEEP_MODE, 0x6bcu)
    FIELD(MIO_PAD_SLEEP_MODE, OUT, 0u, 2u)
REG32(DIO_PAD_SLEEP_STATUS, 0x778u)
REG32(DIO_PAD_SLEEP_REGWEN, 0x77cu)
    FIELD(DIO_PAD_SLEEP_REGWEN, EN, 0u, 1u)
REG32(DIO_PAD_SLEEP, 0x7bcu)
    FIELD(DIO_PAD_SLEEP, EN, 0u, 1u)
REG32(DIO_PAD_SLEEP_MODE, 0x7fcu)
    FIELD(DIO_PAD_SLEEP_MODE, OUT, 0u, 2u)
REG32(WKUP_DETECTOR_REGWEN, 0x83cu)
    FIELD(WKUP_DETECTOR_REGWEN, EN, 0u, 1u)
REG32(WKUP_DETECTOR, 0x85cu)
    FIELD(WKUP_DETECTOR, EN, 0u, 1u)
REG32(WKUP_DETECTOR_CFG, 0x87cu)
    FIELD(WKUP_DETECTOR_CFG, MODE, 0u, 3u)
    FIELD(WKUP_DETECTOR_CFG, FILTER, 3u, 1u)
    FIELD(WKUP_DETECTOR_CFG, MIODIO, 4u, 1u)
REG32(WKUP_DETECTOR_CNT_TH, 0x89cu)
    FIELD(WKUP_DETECTOR_CNT_TH, TH, 0u, 8u)
REG32(WKUP_DETECTOR_PADSEL, 0x8bcu)
    FIELD(WKUP_DETECTOR_PADSEL, SEL, 0u, 6u)
REG32(WKUP_CAUSE, 0x8dcu)
/* clang-format on */

#define MIO_SLEEP_STATUS_COUNT DIV_ROUND_UP(PARAM_N_MIO_PADS, 32u)
#define DIO_SLEEP_STATUS_COUNT DIV_ROUND_UP(PARAM_N_DIO_PADS, 32u)

#define MIO_PAD_ATTR_MASK \
    (R_MIO_PAD_ATTR_INVERT_MASK | R_MIO_PAD_ATTR_VIRTUAL_OD_EN_MASK | \
     R_MIO_PAD_ATTR_PULL_EN_MASK | R_MIO_PAD_ATTR_PULL_SELECT_MASK | \
     R_MIO_PAD_ATTR_KEEPER_EN_MASK | R_MIO_PAD_ATTR_SCHMITT_EN_MASK | \
     R_MIO_PAD_ATTR_OD_EN_MASK | R_MIO_PAD_ATTR_SLEW_RATE_MASK | \
     R_MIO_PAD_ATTR_DRIVE_STRENGTH_MASK)
#define DIO_PAD_ATTR_MASK \
    (R_DIO_PAD_ATTR_INVERT_MASK | R_DIO_PAD_ATTR_VIRTUAL_OD_EN_MASK | \
     R_DIO_PAD_ATTR_PULL_EN_MASK | R_DIO_PAD_ATTR_PULL_SELECT_MASK | \
     R_DIO_PAD_ATTR_KEEPER_EN_MASK | R_DIO_PAD_ATTR_SCHMITT_EN_MASK | \
     R_DIO_PAD_ATTR_OD_EN_MASK | R_DIO_PAD_ATTR_SLEW_RATE_MASK | \
     R_DIO_PAD_ATTR_DRIVE_STRENGTH_MASK)
#define MIO_PAD_SLEEP_STATUS_MASK       UINT32_MAX
#define MIO_PAD_SLEEP_MODE_OUT_TIE_LOW  0x0u
#define MIO_PAD_SLEEP_MODE_OUT_TIE_HIGH 0x1u
#define MIO_PAD_SLEEP_MODE_OUT_HIGH_Z   0x2u
#define MIO_PAD_SLEEP_MODE_OUT_KEEP     0x3u
#define DIO_PAD_SLEEP_STATUS_MASK       ((1u << PARAM_N_DIO_PADS) - 1u)
#define DIO_PAD_SLEEP_MODE_OUT_TIE_LOW  0x0u
#define DIO_PAD_SLEEP_MODE_OUT_TIE_HIGH 0x1u
#define DIO_PAD_SLEEP_MODE_OUT_HIGH_Z   0x2u
#define DIO_PAD_SLEEP_MODE_OUT_KEEP     0x3u
#define WKUP_DETECTOR_MODE_POSEDGE      0x0u
#define WKUP_DETECTOR_MODE_NEGEDGE      0x1u
#define WKUP_DETECTOR_MODE_EDGE         0x2u
#define WKUP_DETECTOR_MODE_TIMEDHIGH    0x3u
#define WKUP_DETECTOR_MODE_TIMEDLOW     0x4u
#define WKUP_CAUSE_MASK                 ((1u << PARAM_N_WKUP_DETECT) - 1u)
#define WKUP_DETECTOR_CFG_MASK \
    (R_WKUP_DETECTOR_CFG_MODE_MASK | R_WKUP_DETECTOR_CFG_FILTER_MASK | \
     R_WKUP_DETECTOR_CFG_MIODIO_MASK)

#define R32_OFF(_r_)             ((_r_) / sizeof(uint32_t))
#define R_LAST_REG               (R_WKUP_CAUSE)
#define REGS_COUNT               (R_LAST_REG + 1u)
#define REGS_SIZE                (REGS_COUNT * sizeof(uint32_t))
#define CASE_SCALAR(_reg_)       R_##_reg_
#define CASE_RANGE(_reg_, _rpt_) R_##_reg_...(R_##_reg_ + (_rpt_) - (1u))

typedef struct {
    uint32_t alert_test;
    uint32_t mio_periph_insel_regwen[PARAM_N_MIO_PERIPH_IN];
    uint32_t mio_periph_insel[PARAM_N_MIO_PERIPH_IN];
    uint32_t mio_outsel_regwen[PARAM_N_MIO_PADS];
    uint32_t mio_outsel[PARAM_N_MIO_PADS];
    uint32_t mio_pad_attr_regwen[PARAM_N_MIO_PADS];
    uint32_t mio_pad_attr[PARAM_N_MIO_PADS];
    uint32_t dio_pad_attr_regwen[PARAM_N_DIO_PADS];
    uint32_t dio_pad_attr[PARAM_N_DIO_PADS];
    uint32_t mio_pad_sleep_status[MIO_SLEEP_STATUS_COUNT];
    uint32_t mio_pad_sleep_regwen[PARAM_N_MIO_PADS];
    uint32_t mio_pad_sleep[PARAM_N_MIO_PADS];
    uint32_t mio_pad_sleep_mode[PARAM_N_MIO_PADS];
    uint32_t dio_pad_sleep_status[DIO_SLEEP_STATUS_COUNT];
    uint32_t dio_pad_sleep_regwen[PARAM_N_DIO_PADS];
    uint32_t dio_pad_sleep[PARAM_N_DIO_PADS];
    uint32_t dio_pad_sleep_mode[PARAM_N_DIO_PADS];
    uint32_t wkup_detector_regwen[PARAM_N_WKUP_DETECT];
    uint32_t wkup_detector[PARAM_N_WKUP_DETECT];
    uint32_t wkup_detector_cfg[PARAM_N_WKUP_DETECT];
    uint32_t wkup_detector_cnt_th[PARAM_N_WKUP_DETECT];
    uint32_t wkup_detector_padsel[PARAM_N_WKUP_DETECT];
    uint32_t wkup_cause;
} OtPinmuxStateRegs;

struct OtPinmuxState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    IbexIRQ alert;

    OtPinmuxStateRegs *regs;
};

static uint64_t ot_pinmux_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtPinmuxState *s = opaque;
    uint32_t val32;
    hwaddr reg = R32_OFF(addr);
    OtPinmuxStateRegs *regs = s->regs;

    switch (reg) {
    case CASE_RANGE(MIO_PERIPH_INSEL_REGWEN, PARAM_N_MIO_PERIPH_IN):
        val32 = regs->mio_periph_insel_regwen[reg - R_MIO_PERIPH_INSEL_REGWEN];
        break;
    case CASE_RANGE(MIO_PERIPH_INSEL, PARAM_N_MIO_PERIPH_IN):
        val32 = regs->mio_periph_insel[reg - R_MIO_PERIPH_INSEL];
        break;
    case CASE_RANGE(MIO_OUTSEL_REGWEN, PARAM_N_MIO_PADS):
        val32 = regs->mio_outsel_regwen[reg - R_MIO_OUTSEL_REGWEN];
        break;
    case CASE_RANGE(MIO_OUTSEL, PARAM_N_MIO_PADS):
        val32 = regs->mio_outsel[reg - R_MIO_OUTSEL];
        break;
    case CASE_RANGE(MIO_PAD_ATTR_REGWEN, PARAM_N_MIO_PADS):
        val32 = regs->mio_pad_attr_regwen[reg - R_MIO_PAD_ATTR_REGWEN];
        break;
    case CASE_RANGE(MIO_PAD_ATTR, PARAM_N_MIO_PADS):
        val32 = regs->mio_pad_attr[reg - R_MIO_PAD_ATTR];
        break;
    case CASE_RANGE(DIO_PAD_ATTR_REGWEN, PARAM_N_DIO_PADS):
        val32 = regs->dio_pad_attr_regwen[reg - R_DIO_PAD_ATTR_REGWEN];
        break;
    case CASE_RANGE(DIO_PAD_ATTR, PARAM_N_DIO_PADS):
        val32 = regs->dio_pad_attr[reg - R_DIO_PAD_ATTR];
        break;
    case CASE_RANGE(MIO_PAD_SLEEP_STATUS, MIO_SLEEP_STATUS_COUNT):
        val32 = regs->mio_pad_sleep_status[reg - R_MIO_PAD_SLEEP_STATUS];
        break;
    case CASE_RANGE(MIO_PAD_SLEEP_REGWEN, PARAM_N_MIO_PADS):
        val32 = regs->mio_pad_sleep_regwen[reg - R_MIO_PAD_SLEEP_REGWEN];
        break;
    case CASE_RANGE(MIO_PAD_SLEEP, PARAM_N_MIO_PADS):
        val32 = regs->mio_pad_sleep[reg - R_MIO_PAD_SLEEP];
        break;
    case CASE_RANGE(MIO_PAD_SLEEP_MODE, PARAM_N_MIO_PADS):
        val32 = regs->mio_pad_sleep_mode[reg - R_MIO_PAD_SLEEP_MODE];
        break;
    case CASE_RANGE(DIO_PAD_SLEEP_STATUS, DIO_SLEEP_STATUS_COUNT):
        val32 = regs->dio_pad_sleep_status[reg - R_DIO_PAD_SLEEP_STATUS];
        break;
    case CASE_RANGE(DIO_PAD_SLEEP_REGWEN, PARAM_N_DIO_PADS):
        val32 = regs->dio_pad_sleep_regwen[reg - R_DIO_PAD_SLEEP_REGWEN];
        break;
    case CASE_RANGE(DIO_PAD_SLEEP, PARAM_N_DIO_PADS):
        val32 = regs->dio_pad_sleep[reg - R_DIO_PAD_SLEEP];
        break;
    case CASE_RANGE(DIO_PAD_SLEEP_MODE, PARAM_N_DIO_PADS):
        val32 = regs->dio_pad_sleep_mode[reg - R_DIO_PAD_SLEEP_MODE];
        break;
    case CASE_RANGE(WKUP_DETECTOR_REGWEN, PARAM_N_WKUP_DETECT):
        val32 = regs->wkup_detector_regwen[reg - R_WKUP_DETECTOR_REGWEN];
        break;
    case CASE_RANGE(WKUP_DETECTOR, PARAM_N_WKUP_DETECT):
        val32 = regs->wkup_detector[reg - R_WKUP_DETECTOR];
        break;
    case CASE_RANGE(WKUP_DETECTOR_CFG, PARAM_N_WKUP_DETECT):
        val32 = regs->wkup_detector_cfg[reg - R_WKUP_DETECTOR_CFG];
        break;
    case CASE_RANGE(WKUP_DETECTOR_CNT_TH, PARAM_N_WKUP_DETECT):
        val32 = regs->wkup_detector_cnt_th[reg - R_WKUP_DETECTOR_CNT_TH];
        break;
    case CASE_RANGE(WKUP_DETECTOR_PADSEL, PARAM_N_WKUP_DETECT):
        val32 = regs->wkup_detector_padsel[reg - R_WKUP_DETECTOR_PADSEL];
        break;
    case CASE_SCALAR(WKUP_CAUSE):
        val32 = regs->wkup_cause;
        break;
    case CASE_SCALAR(ALERT_TEST):
        qemu_log_mask(LOG_GUEST_ERROR, "W/O register 0x02%" HWADDR_PRIx "\n",
                      addr);
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        val32 = 0;
        break;
    }

    uint64_t pc = ibex_get_current_pc();
    trace_ot_pinmux_io_read_out((unsigned)addr, (uint64_t)val32, pc);

    return (uint64_t)val32;
};

#define OT_PINMUX_IS_REGWEN(_off_, _ren_, _rw_) \
    ((regs->_ren_##_regwen[(_off_) - (R_##_rw_)]) & 0x1u)

static void ot_pinmux_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                                 unsigned size)
{
    OtPinmuxState *s = opaque;
    uint32_t val32 = (uint32_t)val64;
    hwaddr reg = R32_OFF(addr);
    OtPinmuxStateRegs *regs = s->regs;

    uint64_t pc = ibex_get_current_pc();
    trace_ot_pinmux_io_write((unsigned)addr, val64, pc);

    switch (reg) {
    case R_ALERT_TEST:
        val32 &= R_ALERT_TEST_FATAL_FAULT_MASK;
        if (val32) {
            ibex_irq_set(&s->alert, (int)val32);
        }
        break;
    case CASE_RANGE(MIO_PERIPH_INSEL_REGWEN, PARAM_N_MIO_PERIPH_IN):
        val32 &= R_MIO_PERIPH_INSEL_REGWEN_EN_MASK;
        regs->mio_periph_insel_regwen[reg - R_MIO_PERIPH_INSEL_REGWEN] = val32;
        break;
    case CASE_RANGE(MIO_PERIPH_INSEL, PARAM_N_MIO_PERIPH_IN):
        if (OT_PINMUX_IS_REGWEN(reg, mio_periph_insel, MIO_PERIPH_INSEL)) {
            val32 &= R_MIO_PERIPH_INSEL_IN_MASK;
            regs->mio_periph_insel[reg - R_MIO_PERIPH_INSEL] = val32;
        }
        break;
    case CASE_RANGE(MIO_OUTSEL_REGWEN, PARAM_N_MIO_PADS):
        val32 &= R_MIO_OUTSEL_REGWEN_EN_MASK;
        regs->mio_outsel_regwen[reg - R_MIO_OUTSEL_REGWEN] = val32;
        break;
    case CASE_RANGE(MIO_OUTSEL, PARAM_N_MIO_PADS):
        if (OT_PINMUX_IS_REGWEN(reg, mio_outsel, MIO_OUTSEL)) {
            val32 &= R_MIO_PERIPH_OUTSEL_OUT_MASK;
            regs->mio_outsel[reg - R_MIO_OUTSEL] = val32;
        }
        break;
    case CASE_RANGE(MIO_PAD_ATTR_REGWEN, PARAM_N_MIO_PADS):
        val32 &= R_MIO_PAD_ATTR_REGWEN_EN_MASK;
        regs->mio_pad_attr_regwen[reg - R_MIO_PAD_ATTR_REGWEN] = val32;
        break;
    case CASE_RANGE(MIO_PAD_ATTR, PARAM_N_MIO_PADS):
        if (OT_PINMUX_IS_REGWEN(reg, mio_pad_attr, MIO_PAD_ATTR)) {
            val32 &= MIO_PAD_ATTR_MASK;
            regs->mio_pad_attr[reg - R_MIO_PAD_ATTR] = val32;
        }
        break;
    case CASE_RANGE(DIO_PAD_ATTR_REGWEN, PARAM_N_DIO_PADS):
        val32 &= R_DIO_PAD_ATTR_REGWEN_EN_MASK;
        regs->dio_pad_attr_regwen[reg - R_DIO_PAD_ATTR_REGWEN] = val32;
        break;
    case CASE_RANGE(DIO_PAD_ATTR, PARAM_N_DIO_PADS):
        if (OT_PINMUX_IS_REGWEN(reg, dio_pad_attr, DIO_PAD_ATTR)) {
            val32 &= DIO_PAD_ATTR_MASK;
            regs->dio_pad_attr[reg - R_DIO_PAD_ATTR] = val32;
        }
        break;
    case CASE_RANGE(MIO_PAD_SLEEP_STATUS, MIO_SLEEP_STATUS_COUNT):
        val32 &= MIO_PAD_SLEEP_STATUS_MASK;
        regs->mio_pad_sleep_status[reg - R_MIO_PAD_SLEEP_STATUS] = val32;
        break;
    case CASE_RANGE(MIO_PAD_SLEEP_REGWEN, PARAM_N_MIO_PADS):
        val32 &= R_MIO_PAD_SLEEP_REGWEN_EN_MASK;
        regs->mio_pad_sleep_regwen[reg - R_MIO_PAD_SLEEP_REGWEN] = val32;
        break;
    case CASE_RANGE(MIO_PAD_SLEEP, PARAM_N_MIO_PADS):
        if (OT_PINMUX_IS_REGWEN(reg, mio_pad_sleep, MIO_PAD_SLEEP)) {
            val32 &= R_MIO_PAD_SLEEP_EN_MASK;
            regs->mio_pad_sleep[reg - R_MIO_PAD_SLEEP] = val32;
        }
        break;
    case CASE_RANGE(MIO_PAD_SLEEP_MODE, PARAM_N_MIO_PADS):
        val32 &= R_MIO_PAD_SLEEP_MODE_OUT_MASK;
        regs->mio_pad_sleep_mode[reg - R_MIO_PAD_SLEEP_MODE] = val32;
        break;
    case CASE_RANGE(DIO_PAD_SLEEP_STATUS, DIO_SLEEP_STATUS_COUNT):
        val32 &= DIO_PAD_SLEEP_STATUS_MASK;
        regs->dio_pad_sleep_status[reg - R_DIO_PAD_SLEEP_STATUS] = val32;
        break;
    case CASE_RANGE(DIO_PAD_SLEEP_REGWEN, PARAM_N_DIO_PADS):
        val32 &= R_DIO_PAD_SLEEP_REGWEN_EN_MASK;
        regs->dio_pad_sleep_regwen[reg - R_DIO_PAD_SLEEP_REGWEN] = val32;
        break;
    case CASE_RANGE(DIO_PAD_SLEEP, PARAM_N_DIO_PADS):
        if (OT_PINMUX_IS_REGWEN(reg, dio_pad_sleep, DIO_PAD_SLEEP)) {
            val32 &= R_DIO_PAD_SLEEP_EN_MASK;
            regs->dio_pad_sleep[reg - R_DIO_PAD_SLEEP] = val32;
        }
        break;
    case CASE_RANGE(DIO_PAD_SLEEP_MODE, PARAM_N_DIO_PADS):
        val32 &= R_DIO_PAD_SLEEP_MODE_OUT_MASK;
        regs->dio_pad_sleep_mode[reg - R_DIO_PAD_SLEEP_MODE] = val32;
        break;
    case CASE_RANGE(WKUP_DETECTOR_REGWEN, PARAM_N_WKUP_DETECT):
        val32 &= R_WKUP_DETECTOR_REGWEN_EN_MASK;
        regs->wkup_detector_regwen[reg - R_WKUP_DETECTOR_REGWEN] = val32;
        break;
    case CASE_RANGE(WKUP_DETECTOR, PARAM_N_WKUP_DETECT):
        if (OT_PINMUX_IS_REGWEN(reg, wkup_detector, WKUP_DETECTOR)) {
            val32 &= R_WKUP_DETECTOR_EN_MASK;
            regs->wkup_detector[reg - R_WKUP_DETECTOR] = val32;
        }
        break;
    case CASE_RANGE(WKUP_DETECTOR_CFG, PARAM_N_WKUP_DETECT):
        val32 &= WKUP_DETECTOR_CFG_MASK;
        regs->wkup_detector[reg - R_WKUP_DETECTOR_CFG] = val32;
        break;
    case CASE_RANGE(WKUP_DETECTOR_CNT_TH, PARAM_N_WKUP_DETECT):
        val32 &= R_WKUP_DETECTOR_CNT_TH_TH_MASK;
        regs->wkup_detector_cnt_th[reg - R_WKUP_DETECTOR_CNT_TH] = val32;
        break;
    case CASE_RANGE(WKUP_DETECTOR_PADSEL, PARAM_N_WKUP_DETECT):
        val32 &= R_WKUP_DETECTOR_PADSEL_SEL_MASK;
        regs->wkup_detector_padsel[reg - R_WKUP_DETECTOR_PADSEL] = val32;
        break;
    case CASE_SCALAR(WKUP_CAUSE):
        val32 %= WKUP_CAUSE_MASK;
        regs->wkup_cause = val32;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
};

static Property ot_pinmux_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_pinmux_regs_ops = {
    .read = &ot_pinmux_regs_read,
    .write = &ot_pinmux_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_pinmux_reset(DeviceState *dev)
{
    OtPinmuxState *s = OT_PINMUX(dev);

    OtPinmuxStateRegs *regs = s->regs;
    memset(regs, 0, sizeof(*regs));

    for (unsigned ix = 0; ix < PARAM_N_MIO_PERIPH_IN; ix++) {
        regs->mio_periph_insel_regwen[ix] = 0x1u;
    }
    for (unsigned ix = 0; ix < PARAM_N_MIO_PADS; ix++) {
        regs->mio_outsel_regwen[ix] = 0x1u;
        regs->mio_outsel[ix] = 0x2u;
        regs->mio_pad_attr_regwen[ix] = 0x1u;
        regs->mio_pad_sleep_regwen[ix] = 0x1u;
        regs->mio_pad_sleep_mode[ix] = 0x2u;
    }
    for (unsigned ix = 0; ix < PARAM_N_DIO_PADS; ix++) {
        regs->dio_pad_attr_regwen[ix] = 0x1u;
        regs->dio_pad_sleep_regwen[ix] = 0x1u;
        regs->dio_pad_sleep_mode[ix] = 0x2u;
    }
    for (unsigned ix = 0; ix < PARAM_N_WKUP_DETECT; ix++) {
        regs->wkup_detector_regwen[ix] = 0x1u;
    }

    ibex_irq_set(&s->alert, 0);
}

static void ot_pinmux_init(Object *obj)
{
    OtPinmuxState *s = OT_PINMUX(obj);

    memory_region_init_io(&s->mmio, obj, &ot_pinmux_regs_ops, s, TYPE_OT_PINMUX,
                          REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    s->regs = g_new0(OtPinmuxStateRegs, 1u);
    ibex_qdev_init_irq(obj, &s->alert, OPENTITAN_DEVICE_ALERT);
}

static void ot_pinmux_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = &ot_pinmux_reset;
    device_class_set_props(dc, ot_pinmux_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_pinmux_info = {
    .name = TYPE_OT_PINMUX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtPinmuxState),
    .instance_init = &ot_pinmux_init,
    .class_init = &ot_pinmux_class_init,
};

static void ot_pinmux_register_types(void)
{
    type_register_static(&ot_pinmux_info);
}

type_init(ot_pinmux_register_types)
