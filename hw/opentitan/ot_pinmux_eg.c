/*
 * QEMU OpenTitan EarlGrey PinMux device
 *
 * Copyright (c) 2024-2025 Rivos, Inc.
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
#include "qemu/main-loop.h"
#include "qemu/typedefs.h"
#include "qom/object.h"
#include "hw/core/cpu.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_gpio_eg.h"
#include "hw/opentitan/ot_pinmux_eg.h"
#include "hw/opentitan/ot_rstmgr.h"
#include "hw/opentitan/ot_sysrst_ctrl.h"
#include "hw/opentitan/ot_uart.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "system/replay.h"
#include "trace.h"

#define PARAM_N_MIO_PERIPH_IN  57u
#define PARAM_N_MIO_PERIPH_OUT 75u
#define PARAM_N_MIO_PADS       47u
#define PARAM_N_DIO_PADS       16u
#define PARAM_N_WKUP_DETECT    8u
#define PARAM_NUM_ALERTS       1u
#define BIT_PACKED_STATUS_REG  1

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
    FIELD(WKUP_DETECTOR_PADSEL, SEL, 0u, 6u)
REG32(WKUP_CAUSE,
      A_WKUP_DETECTOR_PADSEL + REG_SIZE(PARAM_N_WKUP_DETECT))
/* clang-format on */

#define MIO_PAD_ATTR_MASK               0x0010008fu
#define DIO_PAD_ATTR_MASK               0x0010008fu
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
#define REGS_SIZE                0x1000u
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
} OtPinmuxEgStateRegs;

struct OtPinmuxEgState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    IbexIRQ alert;
    IbexIRQ wkup;
    IbexIRQ *dios;
    IbexIRQ *mios;
    IbexIRQ *gpio_pads;
    IbexIRQ *sysrst_inputs;
    IbexIRQ *gpio_inputs;
    uint64_t pad_in_level;
    uint32_t dio_in_level;
    uint8_t mio_latched_sleep_mode[PARAM_N_MIO_PADS];
    uint8_t dio_latched_sleep_mode[PARAM_N_DIO_PADS];
    uint8_t wkup_prev_level[PARAM_N_WKUP_DETECT];

    OtPinmuxEgStateRegs *regs;
};

struct OtPinmuxEgClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

static OtPinmuxEgState *ot_pinmux_eg_instance;

int ot_pinmux_eg_mio_to_host_pin(unsigned pad)
{
    switch (pad) {
    case 0u ... 8u: /* IOA0..IOA8 */
        return (int)pad;
    case 9u ... 12u: /* IOB0..IOB3 */
        return (int)(pad - 9u);
    case 13u ... 31u: /* IOB4..IOB12, IOC0..IOC9 */
        return (int)pad;
    case 40u: /* IOR5 */
        return 5;
    case 41u: /* IOR6 */
        return 6;
    case 42u: /* IOR7 */
        return 7;
    case 43u: /* IOR10 */
        return 10;
    case 44u: /* IOR11 */
        return 8;
    case 45u: /* IOR12 */
        return 12;
    case 46u: /* IOR13 */
        return 13;
    default:
        return -1;
    }
}

static int ot_pinmux_eg_pad_priority(const OtPinmuxEgState *s, unsigned pad)
{
    const OtPinmuxEgStateRegs *regs = s->regs;
    int prio = 0;
    bool sleep_active =
        (regs->mio_pad_sleep_status[pad / 32u] >> (pad % 32u)) & 1u;
    if (sleep_active) {
        prio += 100;
    }
    if (regs->mio_pad_sleep[pad] & R_MIO_PAD_SLEEP_EN_MASK) {
        prio += 50;
    }
    if (regs->mio_outsel[pad] != 2u) {
        prio += 40;
    }
    for (unsigned i = 0; i < PARAM_N_WKUP_DETECT; i++) {
        if ((regs->wkup_detector[i] & R_WKUP_DETECTOR_EN_MASK) &&
            !(regs->wkup_detector_cfg[i] & R_WKUP_DETECTOR_CFG_MIODIO_MASK) &&
            regs->wkup_detector_padsel[i] == pad + 2u) {
            prio += 30;
            break;
        }
    }
    for (unsigned i = 0; i < PARAM_N_MIO_PERIPH_IN; i++) {
        if (regs->mio_periph_insel[i] == pad + 2u) {
            prio += 20;
            break;
        }
    }
    if (regs->mio_pad_attr[pad] != 0) {
        prio += 5;
    }
    return prio;
}

int ot_pinmux_eg_host_pin_to_gpio(OtPinmuxEgState *s, unsigned host_pin)
{
    const OtPinmuxEgStateRegs *regs = s->regs;
    int best_pad = -1;
    int best_prio = -1;
    for (unsigned pad = 0; pad < PARAM_N_MIO_PADS; pad++) {
        if (ot_pinmux_eg_mio_to_host_pin(pad) == (int)host_pin) {
            int prio = ot_pinmux_eg_pad_priority(s, pad);
            if (prio >= best_prio) {
                best_prio = prio;
                best_pad = (int)pad;
            }
        }
    }
    if (best_pad >= 0) {
        uint32_t outsel = regs->mio_outsel[best_pad];
        if (outsel >= 3u && outsel < 35u) {
            return (int)(outsel - 3u);
        }
        for (unsigned g = 0; g < 32u; g++) {
            if (regs->mio_periph_insel[g] == (unsigned)best_pad + 2u) {
                return (int)g;
            }
        }
    }
    return (int)host_pin;
}

static void ot_pinmux_eg_update_pads(OtPinmuxEgState *s)
{
    OtPinmuxEgStateRegs *regs = s->regs;
    uint32_t gpio_cfg[32] = { 0 };
    bool gpio_cfg_set[32] = { false };
    int gpio_cfg_prio[32] = { 0 };

    for (unsigned pad = 0; pad < PARAM_N_MIO_PADS; pad++) {
        uint32_t cfg = PAD_ATTR_TO_IRQ(regs->mio_pad_attr[pad]);
        bool sleep_active =
            (regs->mio_pad_sleep_status[pad / 32u] >> (pad % 32u)) & 1u;
        if (sleep_active) {
            uint32_t mode = s->mio_latched_sleep_mode[pad];
            uint32_t force_mode;
            switch (mode) {
            case 0u: /* Drive Low */
                force_mode = OT_PINMUX_PAD_FORCE_LOW;
                break;
            case 1u: /* Drive High */
                force_mode = OT_PINMUX_PAD_FORCE_HIGH;
                break;
            case 2u: /* High-Z */
                force_mode = OT_PINMUX_PAD_FORCE_HIZ;
                break;
            case 3u: /* Keep */
            default:
                force_mode = OT_PINMUX_PAD_FORCE_IGNORE;
                break;
            }
            cfg |=
                SHARED_FIELD_DP32(0, OT_PINMUX_PAD_ATTR_FORCE_MODE, force_mode);
        }
        ibex_irq_set(&s->mios[pad], (int)cfg);
        uint32_t outsel = regs->mio_outsel[pad];
        bool mapped = false;
        if (outsel >= 3u && outsel < 35u) {
            unsigned g = outsel - 3u;
            gpio_cfg[g] = cfg;
            gpio_cfg_set[g] = true;
            gpio_cfg_prio[g] = 1000;
            mapped = true;
        }
        for (unsigned g = 0; g < 32u; g++) {
            if (regs->mio_periph_insel[g] == pad + 2u) {
                gpio_cfg[g] = cfg;
                gpio_cfg_set[g] = true;
                gpio_cfg_prio[g] = 1000;
                mapped = true;
            }
        }
        if (!mapped) {
            int host_pin = ot_pinmux_eg_mio_to_host_pin(pad);
            if (host_pin >= 0 && host_pin < 32) {
                int prio = ot_pinmux_eg_pad_priority(s, pad);
                if (prio >= gpio_cfg_prio[host_pin] ||
                    !gpio_cfg_set[host_pin]) {
                    gpio_cfg[host_pin] = cfg;
                    gpio_cfg_prio[host_pin] = prio;
                    gpio_cfg_set[host_pin] = true;
                }
            }
        }
    }
    for (unsigned g = 0; g < 32u; g++) {
        ibex_irq_set(&s->gpio_pads[g], (int)gpio_cfg[g]);
    }
}

static void ot_pinmux_eg_sleep_en(void *opaque, int irq, int level)
{
    OtPinmuxEgState *s = opaque;
    OtPinmuxEgStateRegs *regs = s->regs;
    (void)irq;

    if (level) {
        for (unsigned k = 0; k < PARAM_N_MIO_PADS; k++) {
            if (regs->mio_pad_sleep[k] & R_MIO_PAD_SLEEP_EN_MASK) {
                regs->mio_pad_sleep_status[k / 32u] |= (1u << (k % 32u));
                s->mio_latched_sleep_mode[k] =
                    regs->mio_pad_sleep_mode[k] & R_MIO_PAD_SLEEP_MODE_OUT_MASK;
            }
        }
        for (unsigned k = 0; k < PARAM_N_DIO_PADS; k++) {
            if (regs->dio_pad_sleep[k] & R_DIO_PAD_SLEEP_EN_MASK) {
                regs->dio_pad_sleep_status[k / 32u] |= (1u << (k % 32u));
                s->dio_latched_sleep_mode[k] =
                    regs->dio_pad_sleep_mode[k] & R_DIO_PAD_SLEEP_MODE_OUT_MASK;
            }
        }
        ot_pinmux_eg_update_pads(s);
    }
}

uint32_t ot_pinmux_eg_get_mio_outsel(OtPinmuxEgState *s, unsigned mio_pad)
{
    if (!s || !s->regs || mio_pad >= PARAM_N_MIO_PADS) {
        return 0;
    }
    return s->regs->mio_outsel[mio_pad];
}

uint32_t ot_pinmux_eg_get_mio_pad_attr(OtPinmuxEgState *s, unsigned mio_pad)
{
    if (!s || !s->regs || mio_pad >= PARAM_N_MIO_PADS) {
        return 0;
    }
    return s->regs->mio_pad_attr[mio_pad];
}

uint32_t ot_pinmux_eg_get_dio_pad_attr(unsigned dio_pad)
{
    OtPinmuxEgState *s = ot_pinmux_eg_instance;
    if (!s || !s->regs || dio_pad >= PARAM_N_DIO_PADS) {
        return 0;
    }
    return s->regs->dio_pad_attr[dio_pad];
}

uint32_t ot_pinmux_eg_get_gpio_outsel(OtPinmuxEgState *s, unsigned gpio_pin)
{
    if (!s || !s->regs) {
        return 0;
    }
    uint32_t outsel =
        (gpio_pin < PARAM_N_MIO_PADS) ? s->regs->mio_outsel[gpio_pin] : 0;
    int best_prio = -1;
    for (unsigned pad = 0; pad < PARAM_N_MIO_PADS; pad++) {
        if (ot_pinmux_eg_mio_to_host_pin(pad) == (int)gpio_pin) {
            int prio = ot_pinmux_eg_pad_priority(s, pad);
            if (prio >= best_prio) {
                best_prio = prio;
                outsel = s->regs->mio_outsel[pad];
            }
        }
    }
    return outsel;
}

uint32_t ot_pinmux_eg_get_periph_insel(OtPinmuxEgState *s, unsigned periph_in)
{
    if (!s || !s->regs || periph_in >= PARAM_N_MIO_PERIPH_IN) {
        return 0;
    }
    return s->regs->mio_periph_insel[periph_in];
}

bool ot_pinmux_eg_is_periph_in_zero(unsigned periph_in)
{
    OtPinmuxEgState *s = ot_pinmux_eg_instance;
    if (!s || !s->regs || periph_in >= PARAM_N_MIO_PERIPH_IN) {
        return false;
    }
    return s->regs->mio_periph_insel[periph_in] == 0u;
}

static int ot_pinmux_eg_eval_mio_sel(const OtPinmuxEgState *s, uint32_t sel)
{
    if (sel == 1u) {
        return 1;
    }
    if (sel >= 2u && (sel - 2u) < PARAM_N_MIO_PADS) {
        unsigned mio_pad = sel - 2u;
        int mio_in = ot_gpio_eg_get_mio_pad_in(mio_pad);
        if (mio_in >= 0) {
            return mio_in;
        }
        bool pad_inv = (bool)(s->regs->mio_pad_attr[mio_pad] &
                              OT_PINMUX_PAD_ATTR_INVERT_MASK);
        return (int)((s->pad_in_level >> mio_pad) & 1u) ^ (int)pad_inv;
    }
    return 0;
}

static void ot_pinmux_eg_eval_wkup_detectors(OtPinmuxEgState *s)
{
    OtPinmuxEgStateRegs *regs = s->regs;
    for (unsigned i = 0; i < PARAM_N_WKUP_DETECT; i++) {
        bool det_dio = (bool)(regs->wkup_detector_cfg[i] &
                              R_WKUP_DETECTOR_CFG_MIODIO_MASK);
        int level = 0;
        if (det_dio) {
            uint32_t dio_pad = regs->wkup_detector_padsel[i];
            if (dio_pad < PARAM_N_DIO_PADS) {
                level = (int)((s->dio_in_level >> dio_pad) & 1u);
            }
        } else {
            level = ot_pinmux_eg_eval_mio_sel(s, regs->wkup_detector_padsel[i]);
        }
        int prev = (int)s->wkup_prev_level[i];
        s->wkup_prev_level[i] = (uint8_t)level;
        if (!(regs->wkup_detector[i] & R_WKUP_DETECTOR_EN_MASK)) {
            continue;
        }
        uint32_t mode = regs->wkup_detector_cfg[i] & 0x7u;
        bool match = false;
        switch (mode) {
        case 0: /* Posedge */
        default:
            match = (prev == 0 && level == 1);
            break;
        case 1: /* Negedge */
            match = (prev == 1 && level == 0);
            break;
        case 2: /* Edge */
            match = (prev != level);
            break;
        case 3: /* TimedHigh (pinmux_wkup.sv:52, 65-68: cnt_eq_th ungated by
                   cnt_en) */
            match = (level == 1) || (regs->wkup_detector_cnt_th[i] == 0u);
            break;
        case 4: /* TimedLow (pinmux_wkup.sv:52, 69-72: cnt_eq_th ungated by
                   cnt_en) */
            match = (level == 0) || (regs->wkup_detector_cnt_th[i] == 0u);
            break;
        }
        if (match) {
            regs->wkup_cause |= (1u << i);
            ibex_irq_set(&s->wkup, 1);
        }
    }
}

void ot_pinmux_eg_update_sysrst_inputs(OtPinmuxEgState *s)
{
    for (unsigned i = 0; i < 6u; i++) {
        ibex_irq_set(&s->sysrst_inputs[i],
                     ot_pinmux_eg_eval_mio_sel(s,
                                               s->regs->mio_periph_insel[50u +
                                                                         i]));
    }
    ibex_irq_set(&s->sysrst_inputs[OT_SYSRST_CTRL_IN_EC_RST_L],
                 (int)((s->pad_in_level >> 14u) & 1u));
    ibex_irq_set(&s->sysrst_inputs[OT_SYSRST_CTRL_IN_FLASH_WP_L],
                 (int)((s->pad_in_level >> 15u) & 1u));
    uint32_t usb_sel = s->regs->mio_periph_insel[56u];
    int usb_sense = -1;
    if (usb_sel == 0u) {
        usb_sense = 0;
    } else if (usb_sel == 1u) {
        usb_sense = 1;
    }
    (void)usb_sense;
    ot_pinmux_eg_eval_wkup_detectors(s);
}

static void ot_pinmux_eg_update_gpio_inputs(OtPinmuxEgState *s)
{
    for (unsigned i = 0; i < 32u; i++) {
        ibex_irq_set(&s->gpio_inputs[i], -1);
    }
    ot_gpio_eg_notify_sysrst_change();
}

bool ot_pinmux_eg_trigger_mio_wkup(unsigned mio_pad)
{
    OtPinmuxEgState *s = ot_pinmux_eg_instance;
    if (!s || !s->regs || mio_pad >= PARAM_N_MIO_PADS) {
        return false;
    }
    CPUState *cpu = first_cpu;
    if (!cpu || !cpu->halted) {
        return false;
    }
    OtPinmuxEgStateRegs *regs = s->regs;
    bool triggered = false;
    for (unsigned i = 0; i < PARAM_N_WKUP_DETECT; i++) {
        if ((regs->wkup_detector[i] & R_WKUP_DETECTOR_EN_MASK) &&
            !(regs->wkup_detector_cfg[i] & R_WKUP_DETECTOR_CFG_MIODIO_MASK) &&
            regs->wkup_detector_padsel[i] == mio_pad + 2u) {
            regs->wkup_cause |= (1u << i);
            ibex_irq_set(&s->wkup, 0);
            ibex_irq_set(&s->wkup, 1);
            triggered = true;
        }
    }
    return triggered;
}

int ot_pinmux_eg_get_dio_sleep_val(unsigned dio_pad)
{
    OtPinmuxEgState *s = ot_pinmux_eg_instance;
    if (!s || dio_pad >= PARAM_N_DIO_PADS) {
        return -1;
    }
    OtPinmuxEgStateRegs *regs = s->regs;
    bool sleep_active =
        (bool)((regs->dio_pad_sleep_status[dio_pad / 32u] >> (dio_pad % 32u)) &
               1u);
    if (!sleep_active) {
        return -1;
    }
    switch (s->dio_latched_sleep_mode[dio_pad]) {
    case 0u: /* Drive Low */
        return 0;
    case 1u: /* Drive High */
    default:
        return 1;
    }
}

void ot_pinmux_eg_dio_pad_in(unsigned dio_pad, int level)
{
    OtPinmuxEgState *s = ot_pinmux_eg_instance;
    if (!s || dio_pad >= PARAM_N_DIO_PADS) {
        return;
    }

    uint32_t bit = 1u << dio_pad;
    int prev = (int)((s->dio_in_level >> dio_pad) & 1u);
    if (level) {
        s->dio_in_level |= bit;
    } else {
        s->dio_in_level &= ~bit;
    }

    if (prev == level) {
        return;
    }

    ot_pinmux_eg_eval_wkup_detectors(s);
}

static void
ot_pinmux_eg_set_mio_pad_in(OtPinmuxEgState *s, unsigned pad, int level)
{
    uint64_t bit = 1ULL << pad;
    if (level) {
        s->pad_in_level |= bit;
    } else {
        s->pad_in_level &= ~bit;
    }
}

static void ot_pinmux_eg_pad_in(void *opaque, int irq, int level)
{
    OtPinmuxEgState *s = opaque;

    g_assert((unsigned)irq < PARAM_N_MIO_PADS);

    ot_pinmux_eg_set_mio_pad_in(s, (unsigned)irq, level);
    for (unsigned pad = 0; pad < PARAM_N_MIO_PADS; pad++) {
        if (ot_pinmux_eg_mio_to_host_pin(pad) == irq) {
            ot_pinmux_eg_set_mio_pad_in(s, pad, level);
        }
    }

    ot_pinmux_eg_update_sysrst_inputs(s);
    ot_pinmux_eg_update_gpio_inputs(s);
}

static uint32_t ot_pinmux_eg_sel_mask(unsigned val)
{
    return (1u << (32 - clz32(val))) - 1u;
}

static uint64_t ot_pinmux_eg_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtPinmuxEgState *s = opaque;
    (void)size;
    uint32_t val32;
    hwaddr reg = R32_OFF(addr);
    OtPinmuxEgStateRegs *regs = s->regs;

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
        qemu_log_mask(LOG_GUEST_ERROR, "%s: W/O register 0x%03x\n", __func__,
                      (uint32_t)addr);
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%03x\n", __func__,
                      (uint32_t)addr);
        val32 = 0;
        break;
    }

    uint32_t pc = ibex_get_current_pc();
    trace_ot_pinmux_io_read_out((uint32_t)addr, val32, pc);

    return (uint64_t)val32;
};

#define OT_PINMUX_EG_IS_REGWEN(_off_, _ren_, _rw_) \
    ((regs->_ren_##_regwen[(_off_) - (R_##_rw_)]) & 0x1u)

static void ot_pinmux_eg_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                                    unsigned size)
{
    OtPinmuxEgState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;
    hwaddr reg = R32_OFF(addr);
    OtPinmuxEgStateRegs *regs = s->regs;

    uint32_t pc = ibex_get_current_pc();
    trace_ot_pinmux_io_write((uint32_t)addr, val32, pc);

    switch (reg) {
    case R_ALERT_TEST:
        val32 &= R_ALERT_TEST_FATAL_FAULT_MASK;
        if (val32) {
            ibex_irq_set(&s->alert, 1);
            ibex_irq_set(&s->alert, 0);
        }
        break;
    case CASE_RANGE(MIO_PERIPH_INSEL_REGWEN, PARAM_N_MIO_PERIPH_IN):
        val32 &= R_MIO_PERIPH_INSEL_REGWEN_EN_MASK;
        regs->mio_periph_insel_regwen[reg - R_MIO_PERIPH_INSEL_REGWEN] &= val32;
        break;
    case CASE_RANGE(MIO_PERIPH_INSEL, PARAM_N_MIO_PERIPH_IN):
        if (OT_PINMUX_EG_IS_REGWEN(reg, mio_periph_insel, MIO_PERIPH_INSEL)) {
            if (val32 >= PARAM_N_MIO_PERIPH_IN + 2u) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: 0x%03x too large: %u\n",
                              __func__, (unsigned)reg, val32);
                uint32_t mask =
                    ot_pinmux_eg_sel_mask(PARAM_N_MIO_PERIPH_IN + 2u);
                val32 &= mask;
            }
            regs->mio_periph_insel[reg - R_MIO_PERIPH_INSEL] = val32;
            ot_uart_update_pinmux(regs->mio_outsel, regs->mio_periph_insel);
            ot_pinmux_eg_update_sysrst_inputs(s);
            ot_pinmux_eg_update_gpio_inputs(s);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: 0x%03x access is disabled\n",
                          __func__, (uint32_t)addr);
        }
        break;
    case CASE_RANGE(MIO_OUTSEL_REGWEN, PARAM_N_MIO_PADS):
        val32 &= R_MIO_OUTSEL_REGWEN_EN_MASK;
        regs->mio_outsel_regwen[reg - R_MIO_OUTSEL_REGWEN] &= val32;
        break;
    case CASE_RANGE(MIO_OUTSEL, PARAM_N_MIO_PADS):
        if (OT_PINMUX_EG_IS_REGWEN(reg, mio_outsel, MIO_OUTSEL)) {
            if (val32 >= PARAM_N_MIO_PERIPH_OUT + 3u) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: 0x%03x too large: %u\n",
                              __func__, (unsigned)reg, val32);
                uint32_t mask =
                    ot_pinmux_eg_sel_mask(PARAM_N_MIO_PERIPH_OUT + 3u);
                val32 &= mask;
            }
            regs->mio_outsel[reg - R_MIO_OUTSEL] = val32;
            ot_uart_update_pinmux(regs->mio_outsel, regs->mio_periph_insel);
            ot_pinmux_eg_update_pads(s);
            ot_pinmux_eg_update_gpio_inputs(s);
            ot_gpio_eg_notify_sysrst_change();
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: 0x%03x access is disabled\n",
                          __func__, (uint32_t)addr);
        }
        break;
    case CASE_RANGE(MIO_PAD_ATTR_REGWEN, PARAM_N_MIO_PADS):
        val32 &= R_MIO_PAD_ATTR_REGWEN_EN_MASK;
        regs->mio_pad_attr_regwen[reg - R_MIO_PAD_ATTR_REGWEN] &= val32;
        break;
    case CASE_RANGE(MIO_PAD_ATTR, PARAM_N_MIO_PADS):
        if (OT_PINMUX_EG_IS_REGWEN(reg, mio_pad_attr, MIO_PAD_ATTR)) {
            unsigned pad_no = reg - R_MIO_PAD_ATTR;
            g_assert(pad_no < R_MIO_PAD_ATTR);
            val32 &= MIO_PAD_ATTR_MASK;
            regs->mio_pad_attr[pad_no] = val32;
            ot_pinmux_eg_update_pads(s);
            ot_gpio_eg_notify_sysrst_change();
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: 0x%03x access is disabled\n",
                          __func__, (uint32_t)addr);
        }
        break;
    case CASE_RANGE(DIO_PAD_ATTR_REGWEN, PARAM_N_DIO_PADS):
        val32 &= R_DIO_PAD_ATTR_REGWEN_EN_MASK;
        regs->dio_pad_attr_regwen[reg - R_DIO_PAD_ATTR_REGWEN] &= val32;
        break;
    case CASE_RANGE(DIO_PAD_ATTR, PARAM_N_DIO_PADS):
        if (OT_PINMUX_EG_IS_REGWEN(reg, dio_pad_attr, DIO_PAD_ATTR)) {
            unsigned pad_no = reg - R_DIO_PAD_ATTR;
            g_assert(pad_no < PARAM_N_DIO_PADS);
            val32 &= (pad_no == 12u || pad_no == 13u) ? 0x0000008du :
                                                        DIO_PAD_ATTR_MASK;
            regs->dio_pad_attr[pad_no] = val32;
            ibex_irq_set(&s->dios[pad_no], PAD_ATTR_TO_IRQ(val32));
            ot_pinmux_eg_update_sysrst_inputs(s);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: 0x%03x access is disabled\n",
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
        regs->mio_pad_sleep_status[reg - R_MIO_PAD_SLEEP_STATUS] &= val32;
        ot_pinmux_eg_update_pads(s);
        break;
    case CASE_RANGE(MIO_PAD_SLEEP_REGWEN, PARAM_N_MIO_PADS):
        val32 &= R_MIO_PAD_SLEEP_REGWEN_EN_MASK;
        regs->mio_pad_sleep_regwen[reg - R_MIO_PAD_SLEEP_REGWEN] &= val32;
        break;
    case CASE_RANGE(MIO_PAD_SLEEP, PARAM_N_MIO_PADS):
        if (OT_PINMUX_EG_IS_REGWEN(reg, mio_pad_sleep, MIO_PAD_SLEEP)) {
            val32 &= R_MIO_PAD_SLEEP_EN_MASK;
            regs->mio_pad_sleep[reg - R_MIO_PAD_SLEEP] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: 0x%03x access is disabled\n",
                          __func__, (uint32_t)addr);
        }
        break;
    case CASE_RANGE(MIO_PAD_SLEEP_MODE, PARAM_N_MIO_PADS):
        if (OT_PINMUX_EG_IS_REGWEN(reg, mio_pad_sleep, MIO_PAD_SLEEP_MODE)) {
            val32 &= R_MIO_PAD_SLEEP_MODE_OUT_MASK;
            regs->mio_pad_sleep_mode[reg - R_MIO_PAD_SLEEP_MODE] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: 0x%03x access is disabled\n",
                          __func__, (uint32_t)addr);
        }
        break;
    case CASE_RANGE(DIO_PAD_SLEEP_STATUS, DIO_SLEEP_STATUS_COUNT):
        val32 &= DIO_PAD_SLEEP_STATUS_MASK;
#if defined(BIT_PACKED_STATUS_REG) && (DIO_PAD_SLEEP_STATUS_REM != 0)
        if (reg == R_DIO_PAD_SLEEP_STATUS + DIO_SLEEP_STATUS_COUNT - 1u) {
            val32 &= (1u << DIO_PAD_SLEEP_STATUS_REM) - 1u;
        }
#endif
        regs->dio_pad_sleep_status[reg - R_DIO_PAD_SLEEP_STATUS] &= val32;
        break;
    case CASE_RANGE(DIO_PAD_SLEEP_REGWEN, PARAM_N_DIO_PADS):
        val32 &= R_DIO_PAD_SLEEP_REGWEN_EN_MASK;
        regs->dio_pad_sleep_regwen[reg - R_DIO_PAD_SLEEP_REGWEN] &= val32;
        break;
    case CASE_RANGE(DIO_PAD_SLEEP, PARAM_N_DIO_PADS):
        if (OT_PINMUX_EG_IS_REGWEN(reg, dio_pad_sleep, DIO_PAD_SLEEP)) {
            val32 &= R_DIO_PAD_SLEEP_EN_MASK;
            regs->dio_pad_sleep[reg - R_DIO_PAD_SLEEP] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: 0x%03x access is disabled\n",
                          __func__, (uint32_t)addr);
        }
        break;
    case CASE_RANGE(DIO_PAD_SLEEP_MODE, PARAM_N_DIO_PADS):
        if (OT_PINMUX_EG_IS_REGWEN(reg, dio_pad_sleep, DIO_PAD_SLEEP_MODE)) {
            val32 &= R_DIO_PAD_SLEEP_MODE_OUT_MASK;
            regs->dio_pad_sleep_mode[reg - R_DIO_PAD_SLEEP_MODE] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: 0x%03x access is disabled\n",
                          __func__, (uint32_t)addr);
        }
        break;
    case CASE_RANGE(WKUP_DETECTOR_REGWEN, PARAM_N_WKUP_DETECT):
        val32 &= R_WKUP_DETECTOR_REGWEN_EN_MASK;
        regs->wkup_detector_regwen[reg - R_WKUP_DETECTOR_REGWEN] &= val32;
        break;
    case CASE_RANGE(WKUP_DETECTOR, PARAM_N_WKUP_DETECT):
        if (OT_PINMUX_EG_IS_REGWEN(reg, wkup_detector, WKUP_DETECTOR)) {
            val32 &= R_WKUP_DETECTOR_EN_MASK;
            if (val32 && bql_locked()) {
                replay_mutex_unlock();
                bql_unlock();
                g_usleep(20000);
                replay_mutex_lock();
                bql_lock();
            }
            regs->wkup_detector[reg - R_WKUP_DETECTOR] = val32;
            ot_pinmux_eg_eval_wkup_detectors(s);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: 0x%03x access is disabled\n",
                          __func__, (uint32_t)addr);
        }
        break;
    case CASE_RANGE(WKUP_DETECTOR_CFG, PARAM_N_WKUP_DETECT):
        if (OT_PINMUX_EG_IS_REGWEN(reg, wkup_detector, WKUP_DETECTOR_CFG)) {
            val32 &= WKUP_DETECTOR_CFG_MASK;
            regs->wkup_detector_cfg[reg - R_WKUP_DETECTOR_CFG] = val32;
            ot_pinmux_eg_eval_wkup_detectors(s);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: 0x%03x access is disabled\n",
                          __func__, (uint32_t)addr);
        }
        break;
    case CASE_RANGE(WKUP_DETECTOR_CNT_TH, PARAM_N_WKUP_DETECT):
        if (OT_PINMUX_EG_IS_REGWEN(reg, wkup_detector, WKUP_DETECTOR_CNT_TH)) {
            val32 &= R_WKUP_DETECTOR_CNT_TH_TH_MASK;
            regs->wkup_detector_cnt_th[reg - R_WKUP_DETECTOR_CNT_TH] = val32;
            ot_pinmux_eg_eval_wkup_detectors(s);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: 0x%03x access is disabled\n",
                          __func__, (uint32_t)addr);
        }
        break;
    case CASE_RANGE(WKUP_DETECTOR_PADSEL, PARAM_N_WKUP_DETECT):
        if (OT_PINMUX_EG_IS_REGWEN(reg, wkup_detector, WKUP_DETECTOR_PADSEL)) {
            if (val32 >= PARAM_N_MIO_PADS + 2u) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: 0x%03x too large: %u\n",
                              __func__, (unsigned)reg, val32);
                uint32_t mask = ot_pinmux_eg_sel_mask(PARAM_N_MIO_PADS + 2u);
                val32 &= mask;
            }
            regs->wkup_detector_padsel[reg - R_WKUP_DETECTOR_PADSEL] = val32;
            ot_pinmux_eg_eval_wkup_detectors(s);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: 0x%03x access is disabled\n",
                          __func__, (uint32_t)addr);
        }
        break;
    case CASE_SCALAR(WKUP_CAUSE):
        regs->wkup_cause &= (val32 & WKUP_CAUSE_MASK);
        ibex_irq_set(&s->wkup, (int)(bool)regs->wkup_cause);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%x\n", __func__,
                      (uint32_t)addr);
        break;
    }
};

static uint8_t ot_pinmux_eg_permit(hwaddr reg)
{
    if ((reg >= R_MIO_PAD_ATTR && reg < R_MIO_PAD_ATTR + PARAM_N_MIO_PADS) ||
        (reg >= R_DIO_PAD_ATTR && reg < R_DIO_PAD_ATTR + PARAM_N_DIO_PADS)) {
        return 0x7u;
    }
    if (reg == R_MIO_PAD_SLEEP_STATUS) {
        return 0xfu;
    }
    if (reg == R_MIO_PAD_SLEEP_STATUS + 1u || reg == R_DIO_PAD_SLEEP_STATUS) {
        return 0x3u;
    }
    return 0x1u;
}

static bool ot_pinmux_eg_regs_accepts(void *opaque, hwaddr addr, unsigned size,
                                      bool is_write, MemTxAttrs attrs)
{
    (void)opaque;
    (void)attrs;
    hwaddr reg = R32_OFF(addr);
    uint8_t reg_be = ((1u << size) - 1u) << (addr & 0x3u);
    return reg < REGS_COUNT &&
           (!is_write || (ot_pinmux_eg_permit(reg) & ~reg_be) == 0u);
}

static const MemoryRegionOps ot_pinmux_eg_regs_ops = {
    .read = &ot_pinmux_eg_regs_read,
    .write = &ot_pinmux_eg_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
    .valid.accepts = &ot_pinmux_eg_regs_accepts,
};

static void ot_pinmux_eg_reset_enter(Object *obj, ResetType type)
{
    OtPinmuxEgClass *c = OT_PINMUX_EG_GET_CLASS(obj);
    OtPinmuxEgState *s = OT_PINMUX_EG(obj);

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    OtPinmuxEgStateRegs *regs = s->regs;
    bool is_por = (type == RESET_TYPE_COLD) || ot_rstmgr_is_por_reset();
    if (!is_por && ot_rstmgr_is_low_power_exit()) {
        ot_uart_update_pinmux(regs->mio_outsel, regs->mio_periph_insel);
        ot_pinmux_eg_update_sysrst_inputs(s);
        ot_pinmux_eg_update_gpio_inputs(s);
        ot_pinmux_eg_update_pads(s);
        ot_gpio_eg_notify_sysrst_change();
        ibex_irq_set(&s->wkup, (int)(bool)regs->wkup_cause);
        ibex_irq_set(&s->alert, 0);
        return;
    }

    uint32_t wkup_cause = regs->wkup_cause;
    uint32_t mio_sleep_st[MIO_SLEEP_STATUS_COUNT];
    uint32_t dio_sleep_st[DIO_SLEEP_STATUS_COUNT];
    memcpy(mio_sleep_st, regs->mio_pad_sleep_status, sizeof(mio_sleep_st));
    memcpy(dio_sleep_st, regs->dio_pad_sleep_status, sizeof(dio_sleep_st));

    memset(regs, 0, sizeof(*regs));
    if (!is_por) {
        regs->wkup_cause = wkup_cause;
        memcpy(regs->mio_pad_sleep_status, mio_sleep_st, sizeof(mio_sleep_st));
        memcpy(regs->dio_pad_sleep_status, dio_sleep_st, sizeof(dio_sleep_st));
    } else {
        memset(s->mio_latched_sleep_mode, 0, sizeof(s->mio_latched_sleep_mode));
        memset(s->dio_latched_sleep_mode, 0, sizeof(s->dio_latched_sleep_mode));
    }
    s->dio_in_level = (1u << 13u);

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

    ot_uart_update_pinmux(regs->mio_outsel, regs->mio_periph_insel);
    ot_pinmux_eg_update_sysrst_inputs(s);
    ot_pinmux_eg_update_gpio_inputs(s);
    ot_pinmux_eg_update_pads(s);
    ibex_irq_set(&s->wkup, (int)(bool)regs->wkup_cause);
    ibex_irq_set(&s->alert, 0);
}

static void ot_pinmux_eg_reset_exit(Object *obj, ResetType type)
{
    OtPinmuxEgState *s = OT_PINMUX_EG(obj);
    (void)type;

    ot_pinmux_eg_update_pads(s);
}

static void ot_pinmux_eg_init(Object *obj)
{
    OtPinmuxEgState *s = OT_PINMUX_EG(obj);

    memory_region_init_io(&s->mmio, obj, &ot_pinmux_eg_regs_ops, s,
                          TYPE_OT_PINMUX_EG, REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    s->regs = g_new0(OtPinmuxEgStateRegs, 1u);
    ibex_qdev_init_irq(obj, &s->alert, OT_DEVICE_ALERT);
    ibex_qdev_init_irq(obj, &s->wkup, OT_PINMUX_WKUP);

    s->dios = g_new(IbexIRQ, PARAM_N_DIO_PADS);
    s->mios = g_new(IbexIRQ, PARAM_N_MIO_PADS);
    s->gpio_pads = g_new(IbexIRQ, 32u);
    s->sysrst_inputs = g_new(IbexIRQ, OT_SYSRST_CTRL_IN_COUNT);
    s->gpio_inputs = g_new(IbexIRQ, 32u);
    ibex_qdev_init_irqs_default(obj, s->dios, OT_PINMUX_DIO, PARAM_N_DIO_PADS,
                                PAD_ATTR_ENABLE(false));
    ibex_qdev_init_irqs_default(obj, s->mios, OT_PINMUX_MIO, PARAM_N_MIO_PADS,
                                PAD_ATTR_ENABLE(false));
    ibex_qdev_init_irqs_default(obj, s->gpio_pads, OT_PINMUX_GPIO_PAD, 32u, 0);
    ibex_qdev_init_irqs_default(obj, s->sysrst_inputs, OT_PINMUX_SYSRST_IN,
                                OT_SYSRST_CTRL_IN_COUNT, 0);
    ibex_qdev_init_irqs_default(obj, s->gpio_inputs, OT_PINMUX_GPIO_IN, 32u,
                                -1);
    s->pad_in_level = (1ULL << 14) | (1ULL << 15);
    qdev_init_gpio_in_named(DEVICE(obj), &ot_pinmux_eg_pad_in, OT_PINMUX_PAD_IN,
                            PARAM_N_MIO_PADS);
    qdev_init_gpio_in_named(DEVICE(obj), &ot_pinmux_eg_sleep_en,
                            OT_PINMUX_SLEEP_EN, 1u);
    ot_pinmux_eg_instance = s;
}

static void ot_pinmux_eg_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    ResettableClass *rc = RESETTABLE_CLASS(klass);
    OtPinmuxEgClass *pc = OT_PINMUX_EG_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_pinmux_eg_reset_enter, NULL,
                                       &ot_pinmux_eg_reset_exit,
                                       &pc->parent_phases);
}

static const TypeInfo ot_pinmux_eg_info = {
    .name = TYPE_OT_PINMUX_EG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtPinmuxEgState),
    .instance_init = &ot_pinmux_eg_init,
    .class_size = sizeof(OtPinmuxEgClass),
    .class_init = &ot_pinmux_eg_class_init,
};

static void ot_pinmux_eg_register_types(void)
{
    type_register_static(&ot_pinmux_eg_info);
}

type_init(ot_pinmux_eg_register_types);
