/*
 * QEMU OpenTitan Darjeeling PinMux device
 *
 * Copyright (c) 2024 Rivos, Inc.
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
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/typedefs.h"
#include "qom/object.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_pinmux_dj.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "trace.h"

#define PARAM_N_MIO_PERIPH_IN  4u
#define PARAM_N_MIO_PERIPH_OUT 5u
#define PARAM_N_MIO_PADS       12u
#define PARAM_N_DIO_PADS       73u
#define PARAM_N_WKUP_DETECT    8u
#define PARAM_NUM_ALERTS       1u
#undef BIT_PACKED_STATUS_REG

#define REG_SIZE(_n_) ((_n_) * sizeof(uint32_t))

#ifdef BIT_PACKED_STATUS_REG
#define MIO_SLEEP_STATUS_COUNT    DIV_ROUND_UP(PARAM_N_MIO_PADS, 32u)
#define DIO_SLEEP_STATUS_COUNT    DIV_ROUND_UP(PARAM_N_DIO_PADS, 32u)
#define MIO_PAD_SLEEP_STATUS_REM  (PARAM_N_MIO_PADS & 31u)
#define DIO_PAD_SLEEP_STATUS_REM  (PARAM_N_DIO_PADS & 31u)
#define DIO_PAD_SLEEP_STATUS_MASK UINT32_MAX
#define MIO_PAD_SLEEP_STATUS_MASK UINT32_MAX
#else
#define MIO_SLEEP_STATUS_COUNT    PARAM_N_MIO_PADS
#define DIO_SLEEP_STATUS_COUNT    PARAM_N_DIO_PADS
#define MIO_PAD_SLEEP_STATUS_REM  0
#define DIO_PAD_SLEEP_STATUS_REM  0
#define DIO_PAD_SLEEP_STATUS_MASK 1u
#define MIO_PAD_SLEEP_STATUS_MASK 1u
#endif
#define N_MAX_PADS MAX(PARAM_N_MIO_PADS, PARAM_N_DIO_PADS)


/* clang-format off */
REG32(ALERT_TEST, 0x0u)
    FIELD(ALERT_TEST, FATAL_FAULT, 0u, 1u)
REG32(MIO_PERIPH_INSEL_REGWEN, A_ALERT_TEST + REG_SIZE(1u))
    FIELD(MIO_PERIPH_INSEL_REGWEN, EN, 0u, 1u)
REG32(MIO_PERIPH_INSEL,
      A_MIO_PERIPH_INSEL_REGWEN + REG_SIZE(PARAM_N_MIO_PERIPH_IN))
REG32(MIO_OUTSEL_REGWEN,
      A_MIO_PERIPH_INSEL + REG_SIZE(PARAM_N_MIO_PERIPH_IN))
    FIELD(MIO_OUTSEL_REGWEN, EN, 0u, 1u)
REG32(MIO_OUTSEL,
      A_MIO_OUTSEL_REGWEN + REG_SIZE(PARAM_N_MIO_PADS))
REG32(MIO_PAD_ATTR_REGWEN,
      A_MIO_OUTSEL + REG_SIZE(PARAM_N_MIO_PADS))
    FIELD(MIO_PAD_ATTR_REGWEN, EN, 0u, 1u)
REG32(MIO_PAD_ATTR,
      A_MIO_PAD_ATTR_REGWEN + REG_SIZE(PARAM_N_MIO_PADS))
REG32(DIO_PAD_ATTR_REGWEN,
      A_MIO_PAD_ATTR + REG_SIZE(PARAM_N_MIO_PADS))
    FIELD(DIO_PAD_ATTR_REGWEN, EN, 0u, 1u)
REG32(DIO_PAD_ATTR,
      A_DIO_PAD_ATTR_REGWEN + REG_SIZE(PARAM_N_DIO_PADS))
REG32(MIO_PAD_SLEEP_STATUS,
      A_DIO_PAD_ATTR + REG_SIZE(PARAM_N_DIO_PADS))
REG32(MIO_PAD_SLEEP_REGWEN,
      A_MIO_PAD_SLEEP_STATUS + REG_SIZE(MIO_SLEEP_STATUS_COUNT))
    FIELD(MIO_PAD_SLEEP_REGWEN, EN, 0u, 1u)
REG32(MIO_PAD_SLEEP,
      A_MIO_PAD_SLEEP_REGWEN + REG_SIZE(PARAM_N_MIO_PADS))
    FIELD(MIO_PAD_SLEEP, EN, 0u, 1u)
REG32(MIO_PAD_SLEEP_MODE,
      A_MIO_PAD_SLEEP + REG_SIZE(PARAM_N_MIO_PADS))
    FIELD(MIO_PAD_SLEEP_MODE, OUT, 0u, 2u)
REG32(DIO_PAD_SLEEP_STATUS,
      A_MIO_PAD_SLEEP_MODE + REG_SIZE(PARAM_N_MIO_PADS))
REG32(DIO_PAD_SLEEP_REGWEN,
      A_DIO_PAD_SLEEP_STATUS + REG_SIZE(DIO_SLEEP_STATUS_COUNT))
    FIELD(DIO_PAD_SLEEP_REGWEN, EN, 0u, 1u)
REG32(DIO_PAD_SLEEP,
      A_DIO_PAD_SLEEP_REGWEN + REG_SIZE(PARAM_N_DIO_PADS))
    FIELD(DIO_PAD_SLEEP, EN, 0u, 1u)
REG32(DIO_PAD_SLEEP_MODE,
      A_DIO_PAD_SLEEP + REG_SIZE(PARAM_N_DIO_PADS))
    FIELD(DIO_PAD_SLEEP_MODE, OUT, 0u, 2u)
REG32(WKUP_DETECTOR_REGWEN,
     A_DIO_PAD_SLEEP_MODE + REG_SIZE(PARAM_N_DIO_PADS))
    FIELD(WKUP_DETECTOR_REGWEN, EN, 0u, 1u)
REG32(WKUP_DETECTOR,
      A_WKUP_DETECTOR_REGWEN + REG_SIZE(PARAM_N_WKUP_DETECT))
    FIELD(WKUP_DETECTOR, EN, 0u, 1u)
REG32(WKUP_DETECTOR_CFG,
      A_WKUP_DETECTOR + REG_SIZE(PARAM_N_WKUP_DETECT))
    FIELD(WKUP_DETECTOR_CFG, MODE, 0u, 3u)
    FIELD(WKUP_DETECTOR_CFG, FILTER, 3u, 1u)
    FIELD(WKUP_DETECTOR_CFG, MIODIO, 4u, 1u)
REG32(WKUP_DETECTOR_CNT_TH,
      A_WKUP_DETECTOR_CFG + REG_SIZE(PARAM_N_WKUP_DETECT))
    FIELD(WKUP_DETECTOR_CNT_TH, TH, 0u, 8u)
REG32(WKUP_DETECTOR_PADSEL,
      A_WKUP_DETECTOR_CNT_TH + REG_SIZE(PARAM_N_WKUP_DETECT))
REG32(WKUP_CAUSE,
      A_WKUP_DETECTOR_PADSEL + REG_SIZE(PARAM_N_WKUP_DETECT))
/* clang-format on */

#define MIO_PAD_ATTR_MASK               OT_PINMUX_PAD_ATTR_MASK
#define DIO_PAD_ATTR_MASK               OT_PINMUX_PAD_ATTR_MASK
#define MIO_PAD_SLEEP_MODE_OUT_TIE_LOW  0x0u
#define MIO_PAD_SLEEP_MODE_OUT_TIE_HIGH 0x1u
#define MIO_PAD_SLEEP_MODE_OUT_HIGH_Z   0x2u
#define MIO_PAD_SLEEP_MODE_OUT_KEEP     0x3u
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
#define PAD_ATTR_TO_IRQ(_pad_)   ((int)((_pad_) & INT32_MAX))
#define PAD_ATTR_ENABLE(_en_)    (((unsigned)!(_en_)) << 31u)

static_assert((OT_PINMUX_PAD_ATTR_MASK | OT_PINMUX_PAD_ATTR_FORCE_MODE_MASK) <
                  (1u << 31u),
              "Cannot encode PAD attr as IRQ");

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
} OtPinmuxDjStateRegs;

struct OtPinmuxDjState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    IbexIRQ alert;
    IbexIRQ *dios;
    IbexIRQ *mios;

    OtPinmuxDjStateRegs *regs;
};

static uint32_t ot_pinmux_dj_sel_mask(unsigned val)
{
    return (1u << (32 - clz32(val))) - 1u;
}

static uint64_t ot_pinmux_dj_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtPinmuxDjState *s = opaque;
    (void)size;
    uint32_t val32;
    hwaddr reg = R32_OFF(addr);
    OtPinmuxDjStateRegs *regs = s->regs;

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
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: W/O register 0x%03" HWADDR_PRIx "\n", __func__,
                      addr);
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        val32 = 0;
        break;
    }

    uint32_t pc = ibex_get_current_pc();
    trace_ot_pinmux_io_read_out((uint32_t)addr, val32, pc);

    return (uint64_t)val32;
};

#define OT_PINMUX_DJ_IS_REGWEN(_off_, _ren_, _rw_) \
    ((regs->_ren_##_regwen[(_off_) - (R_##_rw_)]) & 0x1u)

static void ot_pinmux_dj_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                                    unsigned size)
{
    OtPinmuxDjState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;
    hwaddr reg = R32_OFF(addr);
    OtPinmuxDjStateRegs *regs = s->regs;

    uint32_t pc = ibex_get_current_pc();
    trace_ot_pinmux_io_write((uint32_t)addr, val32, pc);

    switch (reg) {
    case R_ALERT_TEST:
        val32 &= R_ALERT_TEST_FATAL_FAULT_MASK;
        regs->alert_test = val32;
        ibex_irq_set(&s->alert, (int)(bool)val32);
        break;
    case CASE_RANGE(MIO_PERIPH_INSEL_REGWEN, PARAM_N_MIO_PERIPH_IN):
        val32 &= R_MIO_PERIPH_INSEL_REGWEN_EN_MASK;
        regs->mio_periph_insel_regwen[reg - R_MIO_PERIPH_INSEL_REGWEN] = val32;
        break;
    case CASE_RANGE(MIO_PERIPH_INSEL, PARAM_N_MIO_PERIPH_IN):
        if (OT_PINMUX_DJ_IS_REGWEN(reg, mio_periph_insel, MIO_PERIPH_INSEL)) {
            if (val32 >= PARAM_N_MIO_PERIPH_IN + 2u) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: 0x%04x too large: %u\n",
                              __func__, (unsigned)reg, val32);
                uint32_t mask =
                    ot_pinmux_dj_sel_mask(PARAM_N_MIO_PERIPH_IN + 2u);
                val32 &= mask;
            }
            regs->mio_periph_insel[reg - R_MIO_PERIPH_INSEL] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: 0x%04x access is disabled\n",
                          __func__, (uint32_t)addr);
        }
        break;
    case CASE_RANGE(MIO_OUTSEL_REGWEN, PARAM_N_MIO_PADS):
        val32 &= R_MIO_OUTSEL_REGWEN_EN_MASK;
        regs->mio_outsel_regwen[reg - R_MIO_OUTSEL_REGWEN] = val32;
        break;
    case CASE_RANGE(MIO_OUTSEL, PARAM_N_MIO_PADS):
        if (OT_PINMUX_DJ_IS_REGWEN(reg, mio_outsel, MIO_OUTSEL)) {
            if (val32 >= PARAM_N_MIO_PERIPH_OUT + 2u) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: 0x%04x too large: %u\n",
                              __func__, (unsigned)reg, val32);
                uint32_t mask =
                    ot_pinmux_dj_sel_mask(PARAM_N_MIO_PERIPH_OUT + 2u);
                val32 &= mask;
            }
            regs->mio_outsel[reg - R_MIO_OUTSEL] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: 0x%04x access is disabled\n",
                          __func__, (uint32_t)addr);
        }
        break;
    case CASE_RANGE(MIO_PAD_ATTR_REGWEN, PARAM_N_MIO_PADS):
        val32 &= R_MIO_PAD_ATTR_REGWEN_EN_MASK;
        regs->mio_pad_attr_regwen[reg - R_MIO_PAD_ATTR_REGWEN] = val32;
        break;
    case CASE_RANGE(MIO_PAD_ATTR, PARAM_N_MIO_PADS):
        if (OT_PINMUX_DJ_IS_REGWEN(reg, mio_pad_attr, MIO_PAD_ATTR)) {
            val32 &= MIO_PAD_ATTR_MASK;
            unsigned pad_no = reg - R_MIO_PAD_ATTR;
            g_assert(pad_no < R_MIO_PAD_ATTR);
            regs->mio_pad_attr[pad_no] = val32;
            ibex_irq_set(&s->mios[pad_no], PAD_ATTR_TO_IRQ(val32));
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: 0x%04x access is disabled\n",
                          __func__, (uint32_t)addr);
        }
        break;
    case CASE_RANGE(DIO_PAD_ATTR_REGWEN, PARAM_N_DIO_PADS):
        val32 &= R_DIO_PAD_ATTR_REGWEN_EN_MASK;
        regs->dio_pad_attr_regwen[reg - R_DIO_PAD_ATTR_REGWEN] = val32;
        break;
    case CASE_RANGE(DIO_PAD_ATTR, PARAM_N_DIO_PADS):
        if (OT_PINMUX_DJ_IS_REGWEN(reg, dio_pad_attr, DIO_PAD_ATTR)) {
            val32 &= DIO_PAD_ATTR_MASK;
            unsigned pad_no = reg - R_DIO_PAD_ATTR;
            g_assert(pad_no < PARAM_N_DIO_PADS);
            regs->dio_pad_attr[pad_no] = val32;
            ibex_irq_set(&s->dios[pad_no], PAD_ATTR_TO_IRQ(val32));
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: 0x%04x access is disabled\n",
                          __func__, (uint32_t)addr);
        }
        break;
    case CASE_RANGE(MIO_PAD_SLEEP_STATUS, MIO_SLEEP_STATUS_COUNT):
        val32 &= MIO_PAD_SLEEP_STATUS_MASK;
#if defined(BIT_PACKED_STATUS_REG) && (MIO_PAD_SLEEP_STATUS_REM != 0)
        if (reg == R_MIO_PAD_SLEEP_STATUS + MIO_SLEEP_STATUS_COUNT - 1u) {
            val32 &= (1u << MIO_PAD_SLEEP_STATUS_REM) - 1u;
        }
#endif
        regs->mio_pad_sleep_status[reg - R_MIO_PAD_SLEEP_STATUS] = val32;
        break;
    case CASE_RANGE(MIO_PAD_SLEEP_REGWEN, PARAM_N_MIO_PADS):
        val32 &= R_MIO_PAD_SLEEP_REGWEN_EN_MASK;
        regs->mio_pad_sleep_regwen[reg - R_MIO_PAD_SLEEP_REGWEN] = val32;
        break;
    case CASE_RANGE(MIO_PAD_SLEEP, PARAM_N_MIO_PADS):
        if (OT_PINMUX_DJ_IS_REGWEN(reg, mio_pad_sleep, MIO_PAD_SLEEP)) {
            val32 &= R_MIO_PAD_SLEEP_EN_MASK;
            regs->mio_pad_sleep[reg - R_MIO_PAD_SLEEP] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: 0x%04x access is disabled\n",
                          __func__, (uint32_t)addr);
        }
        break;
    case CASE_RANGE(MIO_PAD_SLEEP_MODE, PARAM_N_MIO_PADS):
        val32 &= R_MIO_PAD_SLEEP_MODE_OUT_MASK;
        regs->mio_pad_sleep_mode[reg - R_MIO_PAD_SLEEP_MODE] = val32;
        break;
    case CASE_RANGE(DIO_PAD_SLEEP_STATUS, DIO_SLEEP_STATUS_COUNT):
        val32 &= DIO_PAD_SLEEP_STATUS_MASK;
#if defined(BIT_PACKED_STATUS_REG) && (DIO_PAD_SLEEP_STATUS_REM != 0)
        if (reg == R_DIO_PAD_SLEEP_STATUS + DIO_SLEEP_STATUS_COUNT - 1u) {
            val32 &= (1u << DIO_PAD_SLEEP_STATUS_REM) - 1u;
        }
#endif
        regs->dio_pad_sleep_status[reg - R_DIO_PAD_SLEEP_STATUS] = val32;
        break;
    case CASE_RANGE(DIO_PAD_SLEEP_REGWEN, PARAM_N_DIO_PADS):
        val32 &= R_DIO_PAD_SLEEP_REGWEN_EN_MASK;
        regs->dio_pad_sleep_regwen[reg - R_DIO_PAD_SLEEP_REGWEN] = val32;
        break;
    case CASE_RANGE(DIO_PAD_SLEEP, PARAM_N_DIO_PADS):
        if (OT_PINMUX_DJ_IS_REGWEN(reg, dio_pad_sleep, DIO_PAD_SLEEP)) {
            val32 &= R_DIO_PAD_SLEEP_EN_MASK;
            regs->dio_pad_sleep[reg - R_DIO_PAD_SLEEP] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: 0x%04x access is disabled\n",
                          __func__, (uint32_t)addr);
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
        if (OT_PINMUX_DJ_IS_REGWEN(reg, wkup_detector, WKUP_DETECTOR)) {
            val32 &= R_WKUP_DETECTOR_EN_MASK;
            regs->wkup_detector[reg - R_WKUP_DETECTOR] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: 0x%04x access is disabled\n",
                          __func__, (uint32_t)addr);
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
        if (val32 >= N_MAX_PADS) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: 0x%04x too large: %u\n",
                          __func__, (unsigned)reg, val32);
            uint32_t mask = ot_pinmux_dj_sel_mask(N_MAX_PADS);
            val32 &= mask;
        }
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

static Property ot_pinmux_dj_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_pinmux_dj_regs_ops = {
    .read = &ot_pinmux_dj_regs_read,
    .write = &ot_pinmux_dj_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_pinmux_dj_reset(DeviceState *dev)
{
    OtPinmuxDjState *s = OT_PINMUX_DJ(dev);

    OtPinmuxDjStateRegs *regs = s->regs;
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

static void ot_pinmux_dj_init(Object *obj)
{
    OtPinmuxDjState *s = OT_PINMUX_DJ(obj);

    memory_region_init_io(&s->mmio, obj, &ot_pinmux_dj_regs_ops, s,
                          TYPE_OT_PINMUX_DJ, REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    s->regs = g_new0(OtPinmuxDjStateRegs, 1u);
    ibex_qdev_init_irq(obj, &s->alert, OT_DEVICE_ALERT);

    s->dios = g_new(IbexIRQ, PARAM_N_DIO_PADS);
    s->mios = g_new(IbexIRQ, PARAM_N_MIO_PADS);
    ibex_qdev_init_irqs_default(obj, s->dios, OT_PINMUX_DIO, PARAM_N_DIO_PADS,
                                PAD_ATTR_ENABLE(false));
    ibex_qdev_init_irqs_default(obj, s->mios, OT_PINMUX_MIO, PARAM_N_MIO_PADS,
                                PAD_ATTR_ENABLE(false));
}

static void ot_pinmux_dj_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->reset = &ot_pinmux_dj_reset;
    device_class_set_props(dc, ot_pinmux_dj_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_pinmux_info = {
    .name = TYPE_OT_PINMUX_DJ,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtPinmuxDjState),
    .instance_init = &ot_pinmux_dj_init,
    .class_init = &ot_pinmux_dj_class_init,
};

static void ot_pinmux_dj_register_types(void)
{
    type_register_static(&ot_pinmux_info);
}

type_init(ot_pinmux_dj_register_types);
