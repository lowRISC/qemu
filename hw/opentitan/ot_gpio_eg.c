/*
 * QEMU OpenTitan Earlgrey GPIO device
 *
 * Copyright (c) 2023-2025 Rivos, Inc.
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
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/typedefs.h"
#include "chardev/char-fe.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_gpio_eg.h"
#include "hw/opentitan/ot_i2c.h"
#include "hw/opentitan/ot_pattgen.h"
#include "hw/opentitan/ot_pinmux.h"
#include "hw/opentitan/ot_pinmux_eg.h"
#include "hw/opentitan/ot_pwm.h"
#include "hw/opentitan/ot_spi_host.h"
#include "hw/opentitan/ot_sysrst_ctrl.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "system/replay.h"
#include "system/runstate.h"
#include "trace.h"

#define PARAM_NUM_ALERTS 1u
#define PARAM_NUM_IO     32u

/* clang-format off */
REG32(INTR_STATE, 0x0u)
REG32(INTR_ENABLE, 0x4u)
REG32(INTR_TEST, 0x8u)
REG32(ALERT_TEST, 0xcu)
    FIELD(ALERT_TEST, FATAL_FAULT_ERR, 0u, 1u)
REG32(DATA_IN, 0x10u)
REG32(DIRECT_OUT, 0x14u)
REG32(MASKED_OUT_LOWER, 0x18u)
    SHARED_FIELD(MASKED_VALUE, 0u, 16u)
    SHARED_FIELD(MASKED_MASK, 16u, 16u)
REG32(MASKED_OUT_UPPER, 0x1cu)
REG32(DIRECT_OE, 0x20u)
REG32(MASKED_OE_LOWER, 0x24u)
REG32(MASKED_OE_UPPER, 0x28u)
REG32(INTR_CTRL_EN_RISING, 0x2cu)
REG32(INTR_CTRL_EN_FALLING, 0x30u)
REG32(INTR_CTRL_EN_LVLHIGH, 0x34u)
REG32(INTR_CTRL_EN_LVLLOW, 0x38u)
REG32(CTRL_EN_INPUT_FILTER, 0x3cu)
/* clang-format on */

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_CTRL_EN_INPUT_FILTER)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) < REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define ALERT_TEST_MASK (R_ALERT_TEST_FATAL_FAULT_ERR_MASK)

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)

static const char *REG_NAMES[REGS_COUNT] = {
    REG_NAME_ENTRY(INTR_STATE),
    REG_NAME_ENTRY(INTR_ENABLE),
    REG_NAME_ENTRY(INTR_TEST),
    REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(DATA_IN),
    REG_NAME_ENTRY(DIRECT_OUT),
    REG_NAME_ENTRY(MASKED_OUT_LOWER),
    REG_NAME_ENTRY(MASKED_OUT_UPPER),
    REG_NAME_ENTRY(DIRECT_OE),
    REG_NAME_ENTRY(MASKED_OE_LOWER),
    REG_NAME_ENTRY(MASKED_OE_UPPER),
    REG_NAME_ENTRY(INTR_CTRL_EN_RISING),
    REG_NAME_ENTRY(INTR_CTRL_EN_FALLING),
    REG_NAME_ENTRY(INTR_CTRL_EN_LVLHIGH),
    REG_NAME_ENTRY(INTR_CTRL_EN_LVLLOW),
    REG_NAME_ENTRY(CTRL_EN_INPUT_FILTER),
};
#undef REG_NAME_ENTRY

typedef struct {
    uint32_t hi_z;
    uint32_t pull_v;
    uint32_t out_en;
    uint32_t out_v;
    uint32_t in_m;
} OtGpioEgBackendState;

struct OtGpioEgState {
    SysBusDevice parent_obj;

    IbexIRQ *irqs;
    IbexIRQ *gpos;
    IbexIRQ alert;

    MemoryRegion mmio;

    uint32_t regs[REGS_COUNT];
    uint32_t data_out; /* output data */
    uint32_t data_oe; /* output enable */
    uint32_t data_ii; /* input data from IRQ lines */
    uint32_t data_ib; /* input data from backend */
    uint32_t data_bi; /* ignore backend input */
    uint32_t host_pull; /* host pull up/down from backend */
    uint32_t data_gi; /* ignore GPIO input */
    uint32_t invert; /* invert signal */
    uint32_t opendrain; /* open drain (1 -> hi-z) */
    uint32_t pull_en; /* pull up/down enable */
    uint32_t pull_sel; /* pull up or pull down */
    uint32_t connected; /* connected to an external device */
    uint32_t force_en; /* pinmux sleep force enable mask */
    uint32_t force_oe; /* pinmux sleep force output enable mask */
    uint32_t force_out; /* pinmux sleep force output value mask */

    char ibuf[65536]; /* backed input buffer */
    unsigned ipos;
    OtGpioEgBackendState backend_state; /* cache */

    OtPinmuxEgState *pinmux;
    OtPattgenState *pattgen;
    OtPwmState *pwm;
    OtSPIHostState *spi_host1;
    OtSysrstCtrlState *sysrst_ctrl;
    OtI2CState *i2c[3];
    uint64_t time_offset_us;
    uint64_t last_total_us;
    bool explicit_time;
    bool batch_mode;
    bool in_reset;
    bool pending_input_ack;
    GString *batch_buf;
    QEMUTimer *bb_timer;
    QEMUTimer *sync_timer;
    uint32_t sync_seq;
    uint8_t *bb_wbuf;
    uint8_t *bb_rbuf;
    uint64_t bb_period_ns;
    uint32_t bb_num_samples;
    uint32_t bb_idx;
    uint32_t last_read_data_in;
    uint32_t poll_count;
    int64_t bb_start_ns;
    unsigned bb_pins[32];
    unsigned bb_npins;
    unsigned bb_settle_ticks;
    unsigned bb_wait_pwm_ticks;
    unsigned bb_tail_ticks;

    char *ot_id;
    uint32_t reset_in; /* initial input levels */
    uint32_t reset_out; /* initial output levels */
    uint32_t reset_oe; /* initial output enable vs. hi-z levels */
    CharFrontend chr; /* communication device */
    guint watch_tag; /* tracker for comm device change */
    bool wipe; /* whether to wipe the backend at reset */
};

struct OtGpioEgClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

static OtGpioEgState *ot_gpio_eg_instance;

static void ot_gpio_eg_update_backend(OtGpioEgState *s, bool force);
static void ot_gpio_eg_update_data_in(OtGpioEgState *s);

void ot_gpio_eg_notify_sysrst_change(void)
{
    if (ot_gpio_eg_instance) {
        ot_gpio_eg_update_data_in(ot_gpio_eg_instance);
        ot_gpio_eg_update_backend(ot_gpio_eg_instance, false);
    }
}

static void ot_gpio_eg_update_irqs(OtGpioEgState *s)
{
    uint32_t level = s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE];
    trace_ot_gpio_irqs(s->ot_id, s->regs[R_INTR_STATE], s->regs[R_INTR_ENABLE],
                       level);
    for (unsigned ix = 0; ix < PARAM_NUM_IO; ix++) {
        ibex_irq_set(&s->irqs[ix], (int)((level >> ix) & 0x1u));
    }
}

static void ot_gpio_eg_update_intr_level(OtGpioEgState *s)
{
    uint32_t intr_state = 0;

    intr_state |= s->regs[R_INTR_CTRL_EN_LVLLOW] & ~s->regs[R_DATA_IN];
    intr_state |= s->regs[R_INTR_CTRL_EN_LVLHIGH] & s->regs[R_DATA_IN];

    s->regs[R_INTR_STATE] |= intr_state;
}

static void ot_gpio_eg_update_intr_edge(OtGpioEgState *s, uint32_t prev)
{
    uint32_t change = prev ^ s->regs[R_DATA_IN];
    uint32_t rising = change & s->regs[R_DATA_IN];
    uint32_t falling = change & ~s->regs[R_DATA_IN];

    uint32_t intr_state = 0;

    intr_state |= s->regs[R_INTR_CTRL_EN_RISING] & rising;
    intr_state |= s->regs[R_INTR_CTRL_EN_FALLING] & falling;

    s->regs[R_INTR_STATE] |= intr_state;
}

static void ot_gpio_eg_update_pad_in_lines(OtGpioEgState *s)
{
    uint32_t periph_out_v = s->data_out ^ s->invert;
    uint32_t periph_out_en = s->data_oe & ~(s->opendrain & periph_out_v);
    periph_out_en =
        (periph_out_en & ~s->force_en) | (s->force_oe & s->force_en);
    periph_out_v = (periph_out_v & ~s->force_en) | (s->force_out & s->force_en);
    periph_out_v &= periph_out_en;

    uint32_t periph_pull_en = s->pull_en & ~s->force_en;
    uint32_t periph_pull_v = s->pull_sel;

    for (unsigned p = 0; p < PARAM_NUM_IO; p++) {
        bool driven_by_dev = false;
        int level = 0;

        if (s->pinmux) {
            uint32_t outsel = ot_pinmux_eg_get_gpio_outsel(s->pinmux, p);
            int g_pull = ot_pinmux_eg_host_pin_to_gpio(s->pinmux, p);
            unsigned gp = (g_pull >= 0 && g_pull < (int)PARAM_NUM_IO) ?
                              (unsigned)g_pull :
                              p;
            if ((s->force_en >> gp) & 1u) {
                if ((s->force_oe >> gp) & 1u) {
                    driven_by_dev = true;
                    level = (int)((s->force_out >> gp) & 1u);
                }
            } else if (outsel == 0u) {
                driven_by_dev = true;
                level = 0;
            } else if (outsel == 1u) {
                driven_by_dev = true;
                level = 1;
            } else if (outsel >= 3u && outsel < 35u) {
                unsigned g = outsel - 3u;
                if ((periph_out_en >> g) & 1u) {
                    driven_by_dev = true;
                    level = (int)((periph_out_v >> g) & 1u);
                }
            } else if (outsel >= 35u && outsel <= 42u && s->sysrst_ctrl) {
                driven_by_dev = true;
                level = ot_sysrst_ctrl_get_outsel_level(s->sysrst_ctrl, outsel);
            } else if (outsel >= 49u && outsel <= 52u && s->pattgen) {
                driven_by_dev = true;
                level = (int)ot_pattgen_get_pin_level(s->pattgen, outsel - 49u);
            } else if (outsel >= 65u && outsel < 71u && s->pwm) {
                driven_by_dev = true;
                level = (int)ot_pwm_get_channel_level(s->pwm, outsel - 65u,
                                                      qemu_clock_get_ns(
                                                          QEMU_CLOCK_VIRTUAL));
            }
            if (!driven_by_dev) {
                if (p == 14u || p == 15u) {
                    /* Open-drain DIO pins (IOR8/ec_rst_l, IOR9/flash_wp_l) with
                     * board pull-ups */
                    bool host_low =
                        !((s->data_bi >> p) & 1u) && !((s->data_ib >> p) & 1u);
                    level = host_low ? 0 : 1;
                } else if (!((s->data_bi >> p) & 1u)) {
                    level = (int)((s->data_ib >> p) & 1u);
                } else if ((periph_pull_en >> gp) & 1u) {
                    level = (int)((periph_pull_v >> gp) & 1u);
                } else if ((s->host_pull >> p) & 1u) {
                    level = (int)((s->host_pull >> p) & 1u);
                } else {
                    level = (int)((s->regs[R_DATA_IN] >> p) & 1u);
                }
            }
        }

        if (level != ibex_irq_get_level(&s->gpos[p])) {
            trace_ot_gpio_out_update_line_bool(s->ot_id, p, level);
            ibex_irq_set(&s->gpos[p], level);
        }
    }
}

int ot_gpio_eg_get_mio_pad_in(unsigned mio_pad)
{
    OtGpioEgState *s = ot_gpio_eg_instance;
    if (!s || !s->pinmux || mio_pad >= 47u) {
        return -1;
    }
    uint32_t pad_attr = ot_pinmux_eg_get_mio_pad_attr(s->pinmux, mio_pad);
    bool pad_inv = (bool)(pad_attr & OT_PINMUX_PAD_ATTR_INVERT_MASK);
    bool pad_in_dis = (bool)(pad_attr & OT_PINMUX_PAD_ATTR_INPUT_DISABLE_MASK);
    if (pad_in_dis) {
        return pad_inv ? 1 : 0;
    }
    uint32_t outsel = ot_pinmux_eg_get_mio_outsel(s->pinmux, mio_pad);
    if (outsel == 0u) {
        return 0;
    }
    if (outsel == 1u) {
        return 1;
    }
    if (outsel >= 3u && outsel < 35u) {
        unsigned g_out = outsel - 3u;
        if ((s->data_oe >> g_out) & 1u) {
            bool g_out_val = (bool)((s->data_out >> g_out) & 1u);
            bool od =
                (bool)(pad_attr & (OT_PINMUX_PAD_ATTR_OD_EN_MASK |
                                   OT_PINMUX_PAD_ATTR_VIRTUAL_OD_EN_MASK));
            if (!(od && (g_out_val ^ pad_inv))) {
                return g_out_val ? 1 : 0;
            }
        }
    } else if (outsel >= 72u && outsel <= 77u && s->sysrst_ctrl) {
        return ot_sysrst_ctrl_get_outsel_level(s->sysrst_ctrl, outsel);
    } else if (outsel >= 49u && outsel <= 52u && s->pattgen) {
        return ot_pattgen_get_pin_level(s->pattgen, outsel - 49u) ? 1 : 0;
    } else if (outsel >= 65u && outsel < 71u && s->pwm) {
        return ot_pwm_get_channel_level(s->pwm, outsel - 65u,
                                        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) ?
                   1 :
                   0;
    }
    return -1;
}

static void ot_gpio_eg_update_data_in(OtGpioEgState *s)
{
    uint32_t prev = s->regs[R_DATA_IN];

    uint32_t routed_ib = 0;
    uint32_t routed_hp = 0;
    uint32_t routed_bi = UINT32_MAX;
    uint32_t eff_oe = 0;
    if (s->pinmux) {
        bool all_muxed = true;
        for (unsigned g = 0; g < PARAM_NUM_IO; g++) {
            if (ot_pinmux_eg_get_periph_insel(s->pinmux, g) < 2u) {
                all_muxed = false;
                break;
            }
        }
        uint32_t eff_data_ib =
            (all_muxed && (s->data_ib & ~0x0000c000u) == 0u) ? 0u : s->data_ib;
        for (unsigned g = 0; g < PARAM_NUM_IO; g++) {
            uint32_t insel = ot_pinmux_eg_get_periph_insel(s->pinmux, g);
            if (insel == 0u) {
                routed_bi &= ~(1u << g);
            } else if (insel == 1u) {
                routed_ib |= (1u << g);
                routed_bi &= ~(1u << g);
            } else if (insel >= 2u && (insel - 2u) < 47u) {
                unsigned mio_pad = insel - 2u;
                int hp =
                    all_muxed ? (int)g : ot_pinmux_eg_mio_to_host_pin(mio_pad);
                if (hp >= 0 && hp < (int)PARAM_NUM_IO) {
                    unsigned host_pin = (unsigned)hp;
                    if ((eff_data_ib >> host_pin) & 1u) {
                        routed_ib |= (1u << g);
                    }
                    if (!all_muxed && ((s->host_pull >> host_pin) & 1u)) {
                        routed_hp |= (1u << g);
                    }
                    if (!((s->data_bi >> host_pin) & 1u)) {
                        routed_bi &= ~(1u << g);
                    }
                }
                if (((s->data_oe >> g) & 1u) &&
                    ot_pinmux_eg_get_mio_outsel(s->pinmux, mio_pad) ==
                        (3u + g)) {
                    eff_oe |= (1u << g);
                }
            }
        }
    }

    uint32_t ii_mask = s->connected & ~s->data_gi;
    uint32_t bi_mask = ~s->connected & ~routed_bi & ~eff_oe;
    uint32_t pi_mask = ~s->connected & routed_bi & ~eff_oe;

    uint32_t data_ii = s->data_ii & ii_mask;
    uint32_t data_ib = routed_ib & bi_mask;
    uint32_t pull_in =
        ((s->pull_en & s->pull_sel) | (~s->pull_en & routed_hp)) & pi_mask;
    uint32_t ext_in = data_ii | data_ib | pull_in;

    trace_ot_gpio_in_ignore(s->ot_id, s->connected, s->data_gi, s->data_bi,
                            s->data_oe);
    trace_ot_gpio_in_line(s->ot_id, s->data_ii, ii_mask, data_ii);
    trace_ot_gpio_in_backend(s->ot_id, s->data_ib, bi_mask, data_ib);
    trace_ot_gpio_in_pull(s->ot_id, s->pull_en, s->pull_sel, pi_mask, pull_in);

    uint32_t data_mix = 0;
    if (s->pinmux) {
        for (unsigned g = 0; g < PARAM_NUM_IO; g++) {
            uint32_t insel = ot_pinmux_eg_get_periph_insel(s->pinmux, g);
            bool in_o = false;
            if (insel == 1u) {
                in_o = true;
            } else if (insel >= 2u && (insel - 2u) < 47u) {
                unsigned mio_pad = insel - 2u;
                int mio_in = ot_gpio_eg_get_mio_pad_in(mio_pad);
                if (mio_in >= 0) {
                    in_o = (bool)mio_in;
                } else {
                    bool pad_inv =
                        (bool)(ot_pinmux_eg_get_mio_pad_attr(s->pinmux,
                                                             mio_pad) &
                               OT_PINMUX_PAD_ATTR_INVERT_MASK);
                    bool raw = ((ext_in >> g) & 1u) != 0;
                    in_o = raw ^ pad_inv;
                }
            }
            if (in_o) {
                data_mix |= (1u << g);
            }
        }
    }

    s->regs[R_DATA_IN] = data_mix;

    trace_ot_gpio_in_update(s->ot_id, s->invert, ext_in, s->data_out, data_mix);

    ot_gpio_eg_update_intr_level(s);
    ot_gpio_eg_update_intr_edge(s, prev);
    ot_gpio_eg_update_irqs(s);

    ot_gpio_eg_update_pad_in_lines(s);
}

static void ot_gpio_eg_update_data_out(OtGpioEgState *s)
{
    uint32_t outv = s->data_out;
    /* assume invert is performed on device output data, not on pull up/down */
    outv ^= s->invert;

    uint32_t out_en = s->data_oe;

    /* if open drain is active and output is high, disable output enable */
    out_en &= ~(s->opendrain & outv);

    /* Apply PINMUX sleep retention force overrides */
    out_en = (out_en & ~s->force_en) | (s->force_oe & s->force_en);
    outv = (outv & ~s->force_en) | (s->force_out & s->force_en);

    /* keep non- opendrain high values */
    outv &= out_en;

    trace_ot_gpio_out_update(s->ot_id, outv, 0, 0);
    ot_gpio_eg_update_pad_in_lines(s);
    if (s->pinmux) {
        ot_pinmux_eg_update_sysrst_inputs(s->pinmux);
    }
}

static void ot_gpio_eg_in_change(void *opaque, int no, int level)
{
    OtGpioEgState *s = opaque;

    trace_ot_gpio_in_change(s->ot_id, no, level < 0, level > 0, 0);

    g_assert(no < PARAM_NUM_IO);

    bool ignore = level < 0;
    bool on = (bool)level;
    uint32_t bit = 1u << no;

    /*
     * any time a signal is received from a remote device the pin is considered
     * as connected and backend no longer may update its state.
     */
    s->connected |= bit;

    if (!ignore) {
        if (on) {
            s->data_ii |= bit;
        } else {
            s->data_ii &= ~bit;
        }
        s->data_gi &= ~bit;
    } else {
        s->data_gi |= bit;
    }

    ot_gpio_eg_update_data_in(s);
    ot_gpio_eg_update_backend(s, false);
}

static void ot_gpio_eg_pad_attr_change(void *opaque, int no, int level)
{
    OtGpioEgState *s = opaque;

    g_assert(no < PARAM_NUM_IO);

    uint32_t cfg = (uint32_t)level;
    uint32_t bit = 1u << no;
    char confstr[4u];

    if (cfg & OT_PINMUX_PAD_ATTR_INVERT_MASK) {
        s->invert |= bit;
        confstr[0u] = '!';
    } else {
        s->invert &= ~bit;
        confstr[0u] = '.';
    }

    if (cfg & (OT_PINMUX_PAD_ATTR_OD_EN_MASK |
               OT_PINMUX_PAD_ATTR_VIRTUAL_OD_EN_MASK)) {
        s->opendrain |= bit;
        confstr[1u] = 'o';
    } else {
        s->opendrain &= ~bit;
        confstr[1u] = '.';
    }

    if (cfg & OT_PINMUX_PAD_ATTR_PULL_SELECT_MASK) {
        s->pull_sel |= bit;
        confstr[2u] = 'h';
    } else {
        s->pull_sel &= ~bit;
        confstr[2u] = 'l';
    }

    if (cfg & OT_PINMUX_PAD_ATTR_PULL_EN_MASK) {
        s->pull_en |= bit;
    } else {
        s->pull_en &= ~bit;
        confstr[2u] = '.';
    }
    confstr[3u] = '\0';

    uint32_t force_mode = SHARED_FIELD_EX32(cfg, OT_PINMUX_PAD_ATTR_FORCE_MODE);
    switch (force_mode) {
    case OT_PINMUX_PAD_FORCE_LOW:
        s->force_en |= bit;
        s->force_oe |= bit;
        s->force_out &= ~bit;
        break;
    case OT_PINMUX_PAD_FORCE_HIGH:
        s->force_en |= bit;
        s->force_oe |= bit;
        s->force_out |= bit;
        break;
    case OT_PINMUX_PAD_FORCE_HIZ:
        s->force_en |= bit;
        s->force_oe &= ~bit;
        s->force_out &= ~bit;
        break;
    case OT_PINMUX_PAD_FORCE_IGNORE:
    default:
        s->force_en &= ~bit;
        s->force_oe &= ~bit;
        s->force_out &= ~bit;
        break;
    }
    trace_ot_gpio_pad_attr_change(s->ot_id, no, cfg, confstr);

    ot_gpio_eg_update_data_in(s);
    ot_gpio_eg_update_data_out(s);
    ot_gpio_eg_update_backend(s, false);
}

static bool ot_gpio_eg_accepts(void *opaque, hwaddr addr, unsigned size,
                               bool is_write, MemTxAttrs attrs)
{
    (void)opaque;
    (void)attrs;
    hwaddr reg = R32_OFF(addr);
    uint8_t reg_be = (uint8_t)(((1u << size) - 1u) << (addr & 3u));
    uint8_t permit = (reg == R_ALERT_TEST) ? 0x1u : 0xfu;
    return reg < REGS_COUNT && (addr & 3u) + size <= 4u &&
           (!is_write || (permit & ~reg_be) == 0u);
}

static uint64_t ot_gpio_eg_read(void *opaque, hwaddr addr, unsigned size)
{
    OtGpioEgState *s = opaque;
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_DATA_IN:
        ot_gpio_eg_update_data_in(s);
        val32 = s->regs[reg];
        if (val32 == s->last_read_data_in) {
            s->poll_count++;
            if (s->poll_count > 16u && qemu_chr_fe_backend_connected(&s->chr)) {
                bql_unlock();
                replay_mutex_unlock();
                g_usleep(50);
                replay_mutex_lock();
                bql_lock();
                ot_gpio_eg_update_data_in(s);
                val32 = s->regs[reg];
            }
        } else {
            s->poll_count = 0;
            s->last_read_data_in = val32;
        }
        break;
    case R_INTR_STATE:
    case R_INTR_ENABLE:
    case R_INTR_CTRL_EN_RISING:
    case R_INTR_CTRL_EN_FALLING:
    case R_INTR_CTRL_EN_LVLHIGH:
    case R_INTR_CTRL_EN_LVLLOW:
    case R_CTRL_EN_INPUT_FILTER:
        val32 = s->regs[reg];
        break;
    case R_DIRECT_OUT:
        /*
         * DIRECT_OUT and MASKED_OUT_{LOWER,UPPER} are alternate views of the
         * same output data register: a masked write must be observable through
         * a DIRECT_OUT read. Return the live output value rather than the last
         * value written specifically through DIRECT_OUT.
         */
        val32 = s->data_out;
        break;
    case R_DIRECT_OE:
        /* Same aliasing applies to the output-enable register. */
        val32 = s->data_oe;
        break;
    case R_MASKED_OUT_LOWER:
        val32 = s->data_out & MASKED_VALUE_MASK;
        break;
    case R_MASKED_OUT_UPPER:
        val32 = (s->data_out >> MASKED_MASK_SHIFT) & MASKED_VALUE_MASK;
        break;
    case R_MASKED_OE_LOWER:
        val32 = s->data_oe & MASKED_VALUE_MASK;
        break;
    case R_MASKED_OE_UPPER:
        val32 = (s->data_oe >> MASKED_MASK_SHIFT) & MASKED_VALUE_MASK;
        break;
    case R_INTR_TEST:
    case R_ALERT_TEST:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: W/O register 0x%02x (%s)\n",
                      __func__, s->ot_id, (uint32_t)addr, REG_NAME(reg));
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Bad offset 0x%02x\n", __func__,
                      s->ot_id, (uint32_t)addr);
        val32 = 0u;
        break;
    }

    uint32_t pc = ibex_get_current_pc();
    trace_ot_gpio_io_read_out(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32,
                              pc);

    return (uint64_t)val32;
}

static void ot_gpio_eg_write(void *opaque, hwaddr addr, uint64_t val64,
                             unsigned size)
{
    OtGpioEgState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;
    uint32_t mask;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_gpio_io_write(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32, pc);

    switch (reg) {
    case R_INTR_STATE:
        s->regs[reg] &= ~val32; /* RW1C */
        ot_gpio_eg_update_intr_level(s);
        ot_gpio_eg_update_irqs(s);
        break;
    case R_INTR_ENABLE:
        s->regs[reg] = val32;
        ot_gpio_eg_update_irqs(s);
        break;
    case R_INTR_TEST:
        s->regs[R_INTR_STATE] |= val32;
        ot_gpio_eg_update_irqs(s);
        break;
    case R_ALERT_TEST:
        val32 &= ALERT_TEST_MASK;
        if (val32) {
            ibex_irq_set(&s->alert, 1);
            ibex_irq_set(&s->alert, 0);
        }
        break;
    case R_DIRECT_OUT:
        s->regs[reg] = val32;
        s->data_out = val32;
        ot_gpio_eg_update_data_out(s);
        ot_gpio_eg_update_backend(s, false);
        ot_gpio_eg_update_data_in(s);
        break;
    case R_DIRECT_OE:
        s->regs[reg] = val32;
        s->data_oe = val32;
        ot_gpio_eg_update_data_out(s);
        ot_gpio_eg_update_backend(s, false);
        ot_gpio_eg_update_data_in(s);
        break;
    case R_MASKED_OUT_LOWER:
        s->regs[reg] = val32;
        mask = val32 >> MASKED_MASK_SHIFT;
        s->data_out &= ~mask;
        s->data_out |= val32 & mask;
        ot_gpio_eg_update_data_out(s);
        ot_gpio_eg_update_backend(s, false);
        ot_gpio_eg_update_data_in(s);
        break;
    case R_MASKED_OUT_UPPER:
        s->regs[reg] = val32;
        mask = val32 & MASKED_MASK_MASK;
        s->data_out &= ~mask;
        s->data_out |= (val32 << MASKED_MASK_SHIFT) & mask;
        ot_gpio_eg_update_data_out(s);
        ot_gpio_eg_update_backend(s, false);
        ot_gpio_eg_update_data_in(s);
        break;
    case R_MASKED_OE_LOWER:
        s->regs[reg] = val32;
        mask = val32 >> MASKED_MASK_SHIFT;
        s->data_oe &= ~mask;
        s->data_oe |= val32 & mask;
        ot_gpio_eg_update_data_out(s);
        ot_gpio_eg_update_backend(s, false);
        ot_gpio_eg_update_data_in(s);
        break;
    case R_MASKED_OE_UPPER:
        s->regs[reg] = val32;
        mask = val32 & MASKED_MASK_MASK;
        s->data_oe &= ~mask;
        s->data_oe |= (val32 << MASKED_MASK_SHIFT) & mask;
        ot_gpio_eg_update_data_out(s);
        ot_gpio_eg_update_backend(s, false);
        ot_gpio_eg_update_data_in(s);
        break;
    case R_INTR_CTRL_EN_RISING:
    case R_INTR_CTRL_EN_FALLING:
        s->regs[reg] = val32;
        break;
    case R_INTR_CTRL_EN_LVLHIGH:
    case R_INTR_CTRL_EN_LVLLOW:
        s->regs[reg] = val32;
        ot_gpio_eg_update_data_in(s);
        break;
    case R_CTRL_EN_INPUT_FILTER:
        /* nothing can be done at QEMU level for sampling that fast */
        s->regs[reg] = val32;
        break;
    case R_DATA_IN:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: R/O register 0x%02x (%s)\n",
                      __func__, s->ot_id, (uint32_t)addr, REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Bad offset 0x%02x\n", __func__,
                      s->ot_id, (uint32_t)addr);
        break;
    }
}

static bool ot_gpio_eg_bitbang_uses_spi_host1(OtGpioEgState *s)
{
    if (!s->pinmux || !s->spi_host1) {
        return false;
    }
    for (unsigned bit = 0; bit < s->bb_npins; bit++) {
        uint32_t outsel =
            ot_pinmux_eg_get_gpio_outsel(s->pinmux, s->bb_pins[bit]);
        if (outsel == 41u || outsel == 42u || outsel == 53u || outsel == 54u) {
            return true;
        }
    }
    return false;
}

static OtI2CState *ot_gpio_eg_get_active_override_i2c(OtGpioEgState *s)
{
    bool has_i2c_pad = false;
    for (unsigned i = 0; i < s->bb_npins; i++) {
        unsigned pad = s->bb_pins[i];
        if (pad == 7u || pad == 8u) {
            has_i2c_pad = true;
            break;
        }
        if (s->pinmux) {
            uint32_t outsel = ot_pinmux_eg_get_gpio_outsel(s->pinmux, pad);
            if (outsel >= 35u && outsel <= 40u) {
                has_i2c_pad = true;
                break;
            }
        }
    }
    if (!has_i2c_pad) {
        return NULL;
    }
    for (unsigned i = 0; i < 3u; i++) {
        if (s->i2c[i] && ot_i2c_is_override_waveform_ready(s->i2c[i])) {
            return s->i2c[i];
        }
    }
    for (unsigned i = 0; i < 3u; i++) {
        if (s->i2c[i] && ot_i2c_is_override_enabled(s->i2c[i])) {
            return s->i2c[i];
        }
    }
    return NULL;
}

static OtI2CState *ot_gpio_eg_get_active_target_i2c(OtGpioEgState *s)
{
    bool has_i2c_pad = false;
    for (unsigned i = 0; i < s->bb_npins; i++) {
        unsigned pad = s->bb_pins[i];
        if (pad == 7u || pad == 8u) {
            has_i2c_pad = true;
            break;
        }
    }
    if (!has_i2c_pad) {
        return NULL;
    }
    for (unsigned i = 0; i < 3u; i++) {
        if (s->i2c[i] && ot_i2c_is_target_enabled(s->i2c[i]) &&
            !ot_i2c_is_override_enabled(s->i2c[i])) {
            return s->i2c[i];
        }
    }
    return NULL;
}

static bool ot_gpio_eg_sample_pad(OtGpioEgState *s, unsigned pad,
                                  int64_t sample_ns)
{
    if (pad == 7u || pad == 8u) {
        OtI2CState *active_i2c = ot_gpio_eg_get_active_override_i2c(s);
        if (active_i2c) {
            return ot_i2c_get_override_pin_level(active_i2c, pad == 7u,
                                                 sample_ns);
        }
    }
    if (s->pinmux) {
        uint32_t outsel = ot_pinmux_eg_get_gpio_outsel(s->pinmux, pad);
        if (outsel == 0u) {
            return false;
        }
        if (outsel == 1u) {
            return true;
        }
        if (outsel >= 65u && outsel < 71u && s->pwm) {
            return ot_pwm_get_channel_level(s->pwm, outsel - 65u, sample_ns);
        }
        if ((outsel == 41u || outsel == 53u || outsel == 54u) && s->spi_host1) {
            return ot_spi_host_get_pin_level(s->spi_host1, outsel, sample_ns);
        }
    }
    if (pad < PARAM_NUM_IO && !((s->data_bi >> pad) & 1u)) {
        return (bool)((s->data_ib >> pad) & 1u);
    }
    return false;
}

static void ot_gpio_eg_bitbang_step(void *opaque)
{
    OtGpioEgState *s = opaque;

    if (!s->bb_wbuf || !s->bb_rbuf) {
        return;
    }

    bool uses_spi_host1 = ot_gpio_eg_bitbang_uses_spi_host1(s);
    OtI2CState *active_i2c =
        uses_spi_host1 ? NULL : ot_gpio_eg_get_active_override_i2c(s);
    OtI2CState *target_i2c =
        (uses_spi_host1 || active_i2c) ? NULL :
                                         ot_gpio_eg_get_active_target_i2c(s);
    bool uses_pwm =
        !uses_spi_host1 && !active_i2c && !target_i2c && (s->pwm != NULL);

    if (s->bb_idx < s->bb_num_samples) {
        uint8_t wval = s->bb_wbuf[s->bb_idx];

        if (target_i2c && s->bb_idx == 0u &&
            !ot_i2c_is_target_polling_acq(target_i2c) &&
            s->bb_wait_pwm_ticks > 0) {
            s->bb_wait_pwm_ticks--;
            int64_t next_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 500000LL;
            s->bb_start_ns = next_ns;
            timer_mod_ns(s->bb_timer, next_ns);
            return;
        }

        /* Apply host-driven pins to s->data_ib */
        uint32_t prev_ib = s->data_ib;
        uint32_t prev_bi = s->data_bi;
        for (unsigned bit = 0; bit < s->bb_npins; bit++) {
            unsigned pad = s->bb_pins[bit];
            if (pad < PARAM_NUM_IO) {
                s->data_bi &= ~(1u << pad);
                if ((wval >> bit) & 1u) {
                    s->data_ib |= (1u << pad);
                } else {
                    s->data_ib &= ~(1u << pad);
                }
            }
        }
        if (s->data_ib != prev_ib || s->data_bi != prev_bi || s->bb_idx == 0) {
            ot_gpio_eg_update_data_in(s);
        }

        if (target_i2c) {
            bool sda_in = true;
            bool scl_in = true;
            for (unsigned bit = 0; bit < s->bb_npins; bit++) {
                unsigned pad = s->bb_pins[bit];
                bool level = ((wval >> bit) & 1u) != 0;
                if (pad == 7u) {
                    sda_in = level;
                } else if (pad == 8u) {
                    scl_in = level;
                }
            }
            bool sda_out =
                ot_i2c_bitbang_target_step(target_i2c, scl_in, sda_in);
            uint8_t sample_byte = 0;
            for (unsigned bit = 0; bit < s->bb_npins; bit++) {
                unsigned pad = s->bb_pins[bit];
                bool level = (pad == 7u) ? sda_out : scl_in;
                if (level) {
                    sample_byte |= (uint8_t)(1u << bit);
                }
            }
            s->bb_rbuf[s->bb_idx] = sample_byte;
            s->bb_idx++;
            if (s->bb_idx >= s->bb_num_samples) {
                s->bb_wait_pwm_ticks = 50000u;
                s->bb_tail_ticks = 20u;
            }
            int64_t next_ns =
                s->bb_start_ns + (int64_t)s->bb_idx * (int64_t)s->bb_period_ns;
            timer_mod_ns(s->bb_timer, next_ns);
            return;
        }

        /*
         * At sample 5 (after IOA7 has gone low at sample 2), if PWM is present
         * and not yet active, pause bitbang sample index progression while
         * advancing QEMU virtual time in 100 us steps until firmware finishes
         * its debounce and enables PWM.
         */
        if (s->bb_idx == 5u && s->bb_wait_pwm_ticks > 0 &&
            ((uses_pwm && !ot_pwm_is_active(s->pwm)) ||
             (uses_spi_host1 && !ot_spi_host_is_waveform_ready(s->spi_host1)) ||
             (active_i2c && !ot_i2c_is_override_waveform_ready(active_i2c)))) {
            s->bb_wait_pwm_ticks--;
            int64_t next_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                              (active_i2c ? 1000000LL : 100000LL);
            s->bb_start_ns =
                next_ns - (int64_t)s->bb_idx * (int64_t)s->bb_period_ns;
            timer_mod_ns(s->bb_timer, next_ns);
            return;
        }

        if (s->bb_idx == 5u) {
            s->bb_start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                             (int64_t)s->bb_idx * (int64_t)s->bb_period_ns;
            if (uses_spi_host1) {
                ot_spi_host_set_waveform_start_ns(s->spi_host1,
                                                  s->bb_start_ns +
                                                      10LL * (int64_t)s
                                                                 ->bb_period_ns,
                                                  false);
            }
            if (active_i2c) {
                ot_i2c_set_waveform_start_ns(active_i2c,
                                             s->bb_start_ns +
                                                 100LL *
                                                     (int64_t)s->bb_period_ns,
                                             false);
            }
        }

        uint32_t chunk = 1u;
        if (s->bb_idx >= 5u) {
            while ((s->bb_idx + chunk) < s->bb_num_samples &&
                   s->bb_wbuf[s->bb_idx + chunk] == wval) {
                chunk++;
            }
        }

        for (uint32_t k = 0; k < chunk; k++) {
            int64_t sample_ns = s->bb_start_ns + (int64_t)(s->bb_idx + k) *
                                                     (int64_t)s->bb_period_ns;
            uint8_t sample_byte = 0;
            for (unsigned bit = 0; bit < s->bb_npins; bit++) {
                unsigned pad = s->bb_pins[bit];
                if (ot_gpio_eg_sample_pad(s, pad, sample_ns)) {
                    sample_byte |= (uint8_t)(1u << bit);
                }
            }
            s->bb_rbuf[s->bb_idx + k] = sample_byte;
        }

        if (chunk > 1u) {
            int64_t skipped_ns =
                (int64_t)(chunk - 1u) * (int64_t)s->bb_period_ns;
            s->bb_start_ns -= skipped_ns;
            if (s->pwm) {
                ot_pwm_shift_start_ns(s->pwm, -skipped_ns);
            }
            if (s->spi_host1) {
                ot_spi_host_set_waveform_start_ns(s->spi_host1, -skipped_ns,
                                                  true);
            }
            if (active_i2c) {
                ot_i2c_set_waveform_start_ns(active_i2c, -skipped_ns, true);
            }
        }

        s->bb_idx += chunk;
        if (s->bb_idx >= s->bb_num_samples) {
            s->bb_wait_pwm_ticks = 50000u;
            s->bb_tail_ticks = (uses_spi_host1 || active_i2c) ? 0u : 20u;
        }
        int64_t next_ns =
            s->bb_start_ns + (int64_t)s->bb_idx * (int64_t)s->bb_period_ns;
        timer_mod_ns(s->bb_timer, next_ns);
        return;
    }

    if (s->bb_tail_ticks > 0) {
        s->bb_tail_ticks--;
        int64_t next_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000LL;
        timer_mod_ns(s->bb_timer, next_ns);
        return;
    }

    /* Encode s->bb_rbuf as RLE and send "B:<rle>\r\n" */
    GString *resp = g_string_sized_new(4096);
    g_string_append(resp, "B:");
    if (s->bb_num_samples > 0) {
        uint8_t cur = s->bb_rbuf[0];
        uint32_t count = 1u;
        for (uint32_t i = 1u; i < s->bb_num_samples; i++) {
            if (s->bb_rbuf[i] == cur) {
                count++;
            } else {
                g_string_append_printf(resp, "%u*%02x,", count, cur);
                cur = s->bb_rbuf[i];
                count = 1u;
            }
        }
        g_string_append_printf(resp, "%u*%02x", count, cur);
    }
    g_string_append(resp, "\r\n");

    if (qemu_chr_fe_backend_connected(&s->chr)) {
        qemu_chr_fe_write(&s->chr, (const uint8_t *)resp->str, (int)resp->len);
    }
    g_string_free(resp, TRUE);

    if (uses_spi_host1) {
        ot_spi_host_clear_waveform(s->spi_host1);
    }
    if (active_i2c) {
        ot_i2c_consume_override_waveform(active_i2c);
    }

    g_free(s->bb_wbuf);
    g_free(s->bb_rbuf);
    s->bb_wbuf = NULL;
    s->bb_rbuf = NULL;
}

static void ot_gpio_eg_handle_bitbang_cmd(OtGpioEgState *s, const char *args)
{
    gchar **parts = g_strsplit(args, ":", 4);
    g_assert(parts && parts[0] && parts[1] && parts[2] && parts[3]);

    uint64_t period_ns = g_ascii_strtoull(parts[0], NULL, 10);
    uint32_t num_samples = (uint32_t)g_ascii_strtoull(parts[1], NULL, 10);
    const char *pins_str = parts[2];
    const char *rle_str = parts[3];

    g_assert(period_ns > 0 && num_samples > 0 && num_samples <= 10000000u);

    gchar **pin_tokens = g_strsplit(pins_str, ".", 32);
    s->bb_npins = 0;
    for (unsigned i = 0; pin_tokens && pin_tokens[i] && i < 32u; i++) {
        unsigned pad = (unsigned)g_ascii_strtoull(pin_tokens[i], NULL, 10);
        s->bb_pins[s->bb_npins++] = pad;
        if (pad < PARAM_NUM_IO) {
            s->data_bi &= ~(1u << pad);
        }
    }
    g_strfreev(pin_tokens);

    g_free(s->bb_wbuf);
    g_free(s->bb_rbuf);
    s->bb_wbuf = g_malloc0(num_samples);
    s->bb_rbuf = g_malloc0(num_samples);
    s->bb_period_ns = period_ns;
    s->bb_num_samples = num_samples;
    s->bb_idx = 0;
    s->bb_settle_ticks = 0;
    s->bb_wait_pwm_ticks = 50000u;
    s->bb_tail_ticks = 0;

    gchar **runs = g_strsplit(rle_str, ",", -1);
    uint32_t widx = 0;
    for (unsigned i = 0; runs && runs[i]; i++) {
        char *star = strchr(runs[i], '*');
        g_assert(star != NULL);
        *star = '\0';
        uint32_t count = (uint32_t)g_ascii_strtoull(runs[i], NULL, 10);
        uint8_t val = (uint8_t)g_ascii_strtoull(star + 1, NULL, 16);
        for (uint32_t k = 0; k < count && widx < num_samples; k++) {
            s->bb_wbuf[widx++] = val;
        }
    }
    g_strfreev(runs);
    g_strfreev(parts);

    OtI2CState *target_i2c = ot_gpio_eg_get_active_target_i2c(s);
    if (target_i2c) {
        ot_i2c_reset_bitbang_target(target_i2c);
    }

    s->bb_start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_mod_ns(s->bb_timer, s->bb_start_ns);
}

static int ot_gpio_eg_chr_can_receive(void *opaque)
{
    OtGpioEgState *s = opaque;

    return (int)sizeof(s->ibuf) - (int)s->ipos;
}

uint64_t ot_gpio_eg_get_total_us(OtGpioEgState *s)
{
    if (!s) {
        return 0;
    }
    if (s->explicit_time) {
        return s->last_total_us;
    }
    uint64_t rt_us =
        (uint64_t)(qemu_clock_get_ns(QEMU_CLOCK_REALTIME) / 1000LL);
    s->last_total_us = MAX(s->last_total_us, rt_us + s->time_offset_us);
    return s->last_total_us;
}

void ot_gpio_eg_set_pattgen_batch(OtGpioEgState *s, bool active)
{
    if (!s) {
        return;
    }
    s->batch_mode = active;
    if (!active) {
        if (s->batch_buf && s->batch_buf->len > 0) {
            const uint8_t *ptr = (const uint8_t *)s->batch_buf->str;
            size_t remaining = s->batch_buf->len;
            while (remaining > 0) {
                int chunk = (int)MIN(remaining, 4096u);
                qemu_chr_fe_write(&s->chr, ptr, chunk);
                ptr += chunk;
                remaining -= (size_t)chunk;
            }
            g_string_set_size(s->batch_buf, 0);
        }
        uint64_t rt_us =
            (uint64_t)(qemu_clock_get_ns(QEMU_CLOCK_REALTIME) / 1000LL);
        s->time_offset_us = s->last_total_us - rt_us;
        s->explicit_time = false;
    }
}

static void ot_gpio_eg_send_input_ack(OtGpioEgState *s)
{
    uint32_t now_us = (uint32_t)ot_gpio_eg_get_total_us(s);
    char buf[32u];
    int len = snprintf(buf, sizeof(buf), "T:%08x\r\nY:%08x\r\n", now_us,
                       s->regs[R_DATA_IN]);
    s->backend_state.in_m = s->regs[R_DATA_IN];
    int written = qemu_chr_fe_write(&s->chr, (const uint8_t *)buf, len);
    s->pending_input_ack = (written != len);
}

static void ot_gpio_eg_sync_timer_cb(void *opaque)
{
    OtGpioEgState *s = opaque;
    ot_gpio_eg_update_backend(s, true);
    char ack[16u];
    int len = snprintf(ack, sizeof(ack), "R:%08x\r\n", s->sync_seq);
    qemu_chr_fe_write(&s->chr, (const uint8_t *)ack, len);
}

static void ot_gpio_eg_chr_receive(void *opaque, const uint8_t *buf, int size)
{
    OtGpioEgState *s = opaque;

    if (s->ipos + (unsigned)size > sizeof(s->ibuf)) {
        error_report("%s: %s: Unexpected chardev receive\n", __func__,
                     s->ot_id);
        return;
    }

    memcpy(&s->ibuf[s->ipos], buf, (size_t)size);
    s->ipos += (unsigned)size;

    for (;;) {
        const char *eol = memchr(s->ibuf, (int)'\n', s->ipos);
        if (!eol) {
            if (s->ipos >= sizeof(s->ibuf) - 1u) {
                /* discard any buffer overflow */
                memset(s->ibuf, 0, sizeof(s->ibuf));
                s->ipos = 0;
            }
            return;
        }
        unsigned eolpos = (unsigned)(eol - s->ibuf);
        char *line = g_strchomp(g_strndup(s->ibuf, eolpos));

        const char *next = eol + 1u;
        unsigned rem = (unsigned)(&s->ibuf[s->ipos] - next);
        memmove(s->ibuf, next, rem);
        s->ipos = rem;

        if (line[0] == 'B' && line[1] == ':') {
            ot_gpio_eg_handle_bitbang_cmd(s, line + 2);
            g_free(line);
            continue;
        }

        uint32_t data_in = 0;
        char cmd = '\0';

        /* NOLINTNEXTLINE */
        int ret = sscanf(line, "%c:%08x", &cmd, &data_in);
        g_free(line);

        if (ret == 2) {
            trace_ot_gpio_backend_recv(s->ot_id, cmd, data_in);
            if (cmd == 'M') {
                s->data_bi = data_in;
                ot_gpio_eg_update_data_in(s);
                ot_gpio_eg_send_input_ack(s);
                ot_gpio_eg_update_backend(s, false);
            } else if (cmd == 'I') {
                s->data_ib = data_in;
                ot_gpio_eg_update_data_in(s);
                ot_gpio_eg_send_input_ack(s);
                ot_gpio_eg_update_backend(s, false);
            } else if (cmd == 'P') {
                s->host_pull = data_in;
                ot_gpio_eg_update_data_in(s);
                ot_gpio_eg_send_input_ack(s);
                ot_gpio_eg_update_backend(s, false);
            } else if (cmd == 'R') {
                s->sync_seq = data_in;
                timer_mod(s->sync_timer,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000LL);
            } else {
                qemu_log_mask(LOG_UNIMP, "%s: unsupported command %c\n",
                              __func__, cmd);
            }
        }
    }
}

static void ot_gpio_eg_init_backend(OtGpioEgState *s)
{
    if (!qemu_chr_fe_backend_connected(&s->chr)) {
        return;
    }

    if (s->wipe) {
        /* query backend for current input status */
        char buf[16u];
        int len = snprintf(buf, sizeof(buf), "C:%08x\r\n", 0);
        qemu_chr_fe_write(&s->chr, (const uint8_t *)buf, len);
    }
}

static void ot_gpio_eg_update_backend(OtGpioEgState *s, bool force)
{
    if (s->in_reset || !qemu_chr_fe_backend_connected(&s->chr)) {
        return;
    }

    /*
     * use the MS DOS CR LF syntax because some people keep using
     * Windows-style terminal.
     */

    uint32_t periph_out_v = s->data_out;
    /* assume invert is performed on device output data, not on pull up/down */
    periph_out_v ^= s->invert;

    uint32_t periph_out_en = s->data_oe;

    /* if open drain is active and output is high, disable output enable */
    periph_out_en &= ~(s->opendrain & periph_out_v);

    /* Apply PINMUX sleep retention force overrides */
    periph_out_en =
        (periph_out_en & ~s->force_en) | (s->force_oe & s->force_en);
    periph_out_v = (periph_out_v & ~s->force_en) | (s->force_out & s->force_en);

    periph_out_v &= periph_out_en;

    uint32_t out_en = 0;
    uint32_t out_v = 0;
    uint32_t pull_en = 0;
    uint32_t pull_v = 0;
    uint32_t periph_pull_en = s->pull_en & ~s->force_en;
    uint32_t periph_pull_v = s->pull_sel;
    if (s->pinmux) {
        for (unsigned p = 0; p < PARAM_NUM_IO; p++) {
            if ((p == 14u || p == 15u) && s->sysrst_ctrl) {
                bool host_low =
                    !((s->data_bi >> p) & 1u) && !((s->data_ib >> p) & 1u);
                int drv =
                    ot_sysrst_ctrl_get_outsel_level(s->sysrst_ctrl, 86u + p);
                out_en |= (1u << p);
                if (drv != 0 && !host_low) {
                    out_v |= (1u << p);
                }
                continue;
            }
            uint32_t outsel = ot_pinmux_eg_get_gpio_outsel(s->pinmux, p);
            int g_pull = ot_pinmux_eg_host_pin_to_gpio(s->pinmux, p);
            unsigned gp = (g_pull >= 0 && g_pull < (int)PARAM_NUM_IO) ?
                              (unsigned)g_pull :
                              p;
            if (g_pull >= 0 || (outsel >= 3u && outsel < 35u)) {
                if ((periph_pull_en >> gp) & 1u) {
                    pull_en |= (1u << p);
                }
                if ((periph_pull_v >> gp) & 1u) {
                    pull_v |= (1u << p);
                }
            }
            if ((s->force_en >> gp) & 1u) {
                if ((s->force_oe >> gp) & 1u) {
                    out_en |= (1u << p);
                }
                if ((s->force_out >> gp) & 1u) {
                    out_v |= (1u << p);
                }
            } else if (outsel == 0u) {
                out_en |= (1u << p);
            } else if (outsel == 1u) {
                out_en |= (1u << p);
                out_v |= (1u << p);
            } else if (outsel >= 49u && outsel <= 52u && s->pattgen) {
                out_en |= (1u << p);
                if (ot_pattgen_get_pin_level(s->pattgen, outsel - 49u)) {
                    out_v |= (1u << p);
                }
            } else if (outsel >= 72u && outsel <= 77u && s->sysrst_ctrl) {
                int drv =
                    ot_sysrst_ctrl_get_outsel_level(s->sysrst_ctrl, outsel);
                out_en |= (1u << p);
                if (drv != 0) {
                    out_v |= (1u << p);
                }
            } else if (outsel >= 3u && outsel < 35u) {
                unsigned g = outsel - 3u;
                if ((periph_out_en >> g) & 1u) {
                    out_en |= (1u << p);
                }
                if ((periph_out_v >> g) & 1u) {
                    out_v |= (1u << p);
                }
            }
        }
    } else {
        out_en = periph_out_en;
        out_v = periph_out_v;
        pull_en = periph_pull_en;
        pull_v = periph_pull_v;
    }

    uint32_t active = pull_en | out_en;
    out_v &= out_en;

    OtGpioEgBackendState bstate = { .hi_z = ~active,
                                    .pull_v = pull_v,
                                    .out_en = out_en,
                                    .out_v = out_v,
                                    .in_m = s->regs[R_DATA_IN] };

    /*
     * use the MS DOS CR LF syntax because some people keep using
     * Windows-style terminal.
     */

    if (!memcmp(&bstate, &s->backend_state, sizeof(OtGpioEgBackendState)) &&
        !force && !s->pending_input_ack) {
        /* do not emit new state if nothing has changed */
        return;
    }

    char buf[128u];
    size_t len = 0;

    uint32_t now_us = (uint32_t)ot_gpio_eg_get_total_us(s);
    len += snprintf(&buf[len], sizeof(buf) - len, "T:%08x\r\n", now_us);

    if (force || bstate.hi_z != s->backend_state.hi_z) {
        len +=
            snprintf(&buf[len], sizeof(buf) - len, "Z:%08x\r\n", bstate.hi_z);
    }
    if (force || bstate.pull_v != s->backend_state.pull_v) {
        len +=
            snprintf(&buf[len], sizeof(buf) - len, "P:%08x\r\n", bstate.pull_v);
    }
    if (force || bstate.out_en != s->backend_state.out_en) {
        len +=
            snprintf(&buf[len], sizeof(buf) - len, "D:%08x\r\n", bstate.out_en);
    }
    if (force || bstate.out_v != s->backend_state.out_v) {
        len +=
            snprintf(&buf[len], sizeof(buf) - len, "O:%08x\r\n", bstate.out_v);
    }
    if (force || s->pending_input_ack || bstate.in_m != s->backend_state.in_m) {
        len +=
            snprintf(&buf[len], sizeof(buf) - len, "Y:%08x\r\n", bstate.in_m);
    }

    s->backend_state = bstate;

    if (len > 0) {
        if (s->batch_mode && s->batch_buf) {
            g_string_append_len(s->batch_buf, buf, (gssize)len);
            s->pending_input_ack = false;
        } else {
            int written =
                qemu_chr_fe_write(&s->chr, (const uint8_t *)buf, (int)len);
            if (written == (int)len) {
                s->pending_input_ack = false;
            }
        }
        /* replace CRLF so that trace is kept on a single line */
        for (size_t i = 0; i + 1 < len; i++) {
            if (buf[i] == '\r' && buf[i + 1] == '\n') {
                if (i + 2 < len) {
                    buf[i] = ',';
                    buf[i + 1] = ' ';
                } else {
                    buf[i] = '\0';
                }
            }
        }
        trace_ot_gpio_backend_send(s->ot_id, buf);
    }
}

void ot_gpio_eg_notify_pattgen_change(OtGpioEgState *s, uint64_t step_us)
{
    if (s) {
        if (step_us > 0) {
            s->last_total_us = MAX(s->last_total_us, step_us);
            s->explicit_time = true;
        }
        ot_gpio_eg_update_data_in(s);
        ot_gpio_eg_update_backend(s, false);
    }
}

static void ot_gpio_eg_chr_event_hander(void *opaque, QEMUChrEvent event)
{
    OtGpioEgState *s = opaque;

    if (event == CHR_EVENT_OPENED) {
        if (object_dynamic_cast(OBJECT(s->chr.chr), TYPE_CHARDEV_SERIAL)) {
            ot_common_ignore_chr_status_lines(&s->chr);
        }

        if (!qemu_chr_fe_backend_connected(&s->chr)) {
            return;
        }

        ot_gpio_eg_update_backend(s, true);

        /* query backend for current input status */
        char buf[16u];
        int len = snprintf(buf, sizeof(buf), "Q:%08x\r\n", s->data_oe);
        qemu_chr_fe_write(&s->chr, (const uint8_t *)buf, len);
        buf[MIN((unsigned)len - 1, sizeof(buf) - 1u)] = '\0';
        trace_ot_gpio_backend_send(s->ot_id, buf);
    }
}

static gboolean
ot_gpio_eg_chr_watch_cb(void *do_not_use, GIOCondition cond, void *opaque)
{
    OtGpioEgState *s = opaque;
    (void)do_not_use;
    (void)cond;

    s->watch_tag = 0;

    return FALSE;
}

static int ot_gpio_eg_chr_be_change(void *opaque)
{
    OtGpioEgState *s = opaque;

    qemu_chr_fe_set_handlers(&s->chr, &ot_gpio_eg_chr_can_receive,
                             &ot_gpio_eg_chr_receive,
                             &ot_gpio_eg_chr_event_hander,
                             &ot_gpio_eg_chr_be_change, s, NULL, true);

    memset(s->ibuf, 0, sizeof(s->ibuf));
    s->ipos = 0;

    if (s->watch_tag > 0) {
        g_source_remove(s->watch_tag);
        /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) */
        s->watch_tag = qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP,
                                             &ot_gpio_eg_chr_watch_cb, s);
    }

    return 0;
}

static const MemoryRegionOps ot_gpio_eg_regs_ops = {
    .read = &ot_gpio_eg_read,
    .write = &ot_gpio_eg_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
    .valid.min_access_size = 1u,
    .valid.max_access_size = 4u,
    .valid.accepts = &ot_gpio_eg_accepts,
};

static const Property ot_gpio_eg_properties[] = {
    DEFINE_PROP_STRING(OT_COMMON_DEV_ID, OtGpioEgState, ot_id),
    DEFINE_PROP_UINT32("in", OtGpioEgState, reset_in, 0u),
    DEFINE_PROP_UINT32("out", OtGpioEgState, reset_out, 0u),
    DEFINE_PROP_UINT32("oe", OtGpioEgState, reset_oe, 0u),
    DEFINE_PROP_BOOL("wipe", OtGpioEgState, wipe, false),
    DEFINE_PROP_CHR("chardev", OtGpioEgState, chr),
    DEFINE_PROP_LINK("pinmux", OtGpioEgState, pinmux, TYPE_OT_PINMUX_EG,
                     OtPinmuxEgState *),
    DEFINE_PROP_LINK("pattgen", OtGpioEgState, pattgen, TYPE_OT_PATTGEN,
                     OtPattgenState *),
    DEFINE_PROP_LINK("pwm", OtGpioEgState, pwm, TYPE_OT_PWM, OtPwmState *),
    DEFINE_PROP_LINK("spi-host1", OtGpioEgState, spi_host1, TYPE_OT_SPI_HOST,
                     OtSPIHostState *),
    DEFINE_PROP_LINK("sysrst-ctrl", OtGpioEgState, sysrst_ctrl,
                     TYPE_OT_SYSRST_CTRL, OtSysrstCtrlState *),
    DEFINE_PROP_LINK("i2c0", OtGpioEgState, i2c[0], TYPE_OT_I2C, OtI2CState *),
    DEFINE_PROP_LINK("i2c1", OtGpioEgState, i2c[1], TYPE_OT_I2C, OtI2CState *),
    DEFINE_PROP_LINK("i2c2", OtGpioEgState, i2c[2], TYPE_OT_I2C, OtI2CState *),
};

static void ot_gpio_eg_reset_enter(Object *obj, ResetType type)
{
    OtGpioEgClass *c = OT_GPIO_EG_GET_CLASS(obj);
    OtGpioEgState *s = OT_GPIO_EG(obj);

    trace_ot_gpio_reset(s->ot_id, "> enter");
    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    timer_del(s->bb_timer);
    g_free(s->bb_wbuf);
    g_free(s->bb_rbuf);
    s->bb_wbuf = NULL;
    s->bb_rbuf = NULL;

    s->in_reset = true;

    memset(s->regs, 0, sizeof(s->regs));
    if (s->wipe || type == RESET_TYPE_COLD) {
        memset(&s->backend_state, 0, sizeof(s->backend_state));
    }

    /* reset_* fields are properties, never get reset */
    s->data_ii = s->reset_in;
    s->data_out = s->reset_out;
    s->data_oe = s->reset_oe;
    /* all input disable until signal is received, or output is forced */
    s->data_gi = ~s->reset_oe;
    s->pull_en = 0;
    s->pull_sel = 0;
    s->invert = 0;
    s->connected = 0;
    s->force_en = 0;
    s->force_oe = 0;
    s->force_out = 0;

    s->regs[R_DATA_IN] = s->reset_in;
    s->regs[R_DIRECT_OUT] = s->reset_out;
    s->regs[R_DIRECT_OE] = s->reset_oe;

    /*
     * Sample the input lines (e.g. the chardev) which persist their state
     * across reset. This is important for cases like straps which are held
     * asserted across a reset.
     */
    ot_gpio_eg_update_data_in(s);

    ibex_irq_set(&s->alert, 0);

    trace_ot_gpio_reset(s->ot_id, "< enter");
}

static void ot_gpio_eg_reset_exit(Object *obj, ResetType type)
{
    /*
     * use of a Resettable full API enables performing I/O updates only once
     * the pinmux configuration has been received (from its own reset stage)
     */
    OtGpioEgClass *c = OT_GPIO_EG_GET_CLASS(obj);
    OtGpioEgState *s = OT_GPIO_EG(obj);

    trace_ot_gpio_reset(s->ot_id, "> exit");

    if (c->parent_phases.exit) {
        c->parent_phases.exit(obj, type);
    }

    s->in_reset = false;

    ot_gpio_eg_init_backend(s);
    ot_gpio_eg_update_data_out(s);
    ot_gpio_eg_update_backend(s, s->wipe || type == RESET_TYPE_COLD);

    /*
     * do not reset the input backend buffer as external GPIO changes is fully
     * async with OT reset. However, it should be reset when the backend changes
     */
    trace_ot_gpio_reset(s->ot_id, "< exit");
}

static void ot_gpio_eg_realize(DeviceState *dev, Error **errp)
{
    OtGpioEgState *s = OT_GPIO_EG(dev);
    (void)errp;

    g_assert(s->ot_id);

    qemu_chr_fe_set_handlers(&s->chr, &ot_gpio_eg_chr_can_receive,
                             &ot_gpio_eg_chr_receive,
                             &ot_gpio_eg_chr_event_hander,
                             &ot_gpio_eg_chr_be_change, s, NULL, true);
}

static void ot_gpio_eg_init(Object *obj)
{
    OtGpioEgState *s = OT_GPIO_EG(obj);

    memory_region_init_io(&s->mmio, obj, &ot_gpio_eg_regs_ops, s,
                          TYPE_OT_GPIO_EG, REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    s->irqs = g_new(IbexIRQ, PARAM_NUM_IO);
    s->gpos = g_new(IbexIRQ, PARAM_NUM_IO);
    for (unsigned ix = 0; ix < PARAM_NUM_IO; ix++) {
        ibex_sysbus_init_irq(obj, &s->irqs[ix]);
    }
    ibex_qdev_init_irqs_default(obj, s->gpos, OT_GPIO_OUT, PARAM_NUM_IO, -1);
    ibex_qdev_init_irq(obj, &s->alert, OT_DEVICE_ALERT);

    qdev_init_gpio_in_named(DEVICE(obj), &ot_gpio_eg_in_change, OT_GPIO_IN,
                            PARAM_NUM_IO);
    qdev_init_gpio_in_named(DEVICE(obj), &ot_gpio_eg_pad_attr_change,
                            OT_PINMUX_PAD, PARAM_NUM_IO);

    s->bb_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, &ot_gpio_eg_bitbang_step, s);
    s->sync_timer =
        timer_new_ns(QEMU_CLOCK_VIRTUAL, &ot_gpio_eg_sync_timer_cb, s);
    s->batch_buf = g_string_new(NULL);

    /* Backend state persists across reset so initialise it once now */
    s->data_ib = 0u;
    s->data_bi = UINT32_MAX;
    s->host_pull = (1u << 7);
    ot_gpio_eg_instance = s;
}

static void ot_gpio_eg_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->realize = &ot_gpio_eg_realize;
    device_class_set_props(dc, ot_gpio_eg_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    ResettableClass *rc = RESETTABLE_CLASS(dc);
    OtGpioEgClass *gc = OT_GPIO_EG_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_gpio_eg_reset_enter, NULL,
                                       &ot_gpio_eg_reset_exit,
                                       &gc->parent_phases);
}

static const TypeInfo ot_gpio_eg_info = {
    .name = TYPE_OT_GPIO_EG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtGpioEgState),
    .instance_init = &ot_gpio_eg_init,
    .class_size = sizeof(OtGpioEgClass),
    .class_init = &ot_gpio_eg_class_init,
};

static void ot_gpio_eg_register_types(void)
{
    type_register_static(&ot_gpio_eg_info);
}

type_init(ot_gpio_eg_register_types);
