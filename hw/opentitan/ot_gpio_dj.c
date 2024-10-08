/*
 * QEMU OpenTitan Darjeeling GPIO device
 *
 * Copyright (c) 2023-2024 Rivos, Inc.
 *
 * Author(s):
 *  Samuel Ortiz <sameo@rivosinc.com>
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
#include "chardev/char-fe.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_gpio_dj.h"
#include "hw/opentitan/ot_pinmux.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_gpio.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "sysemu/runstate.h"
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
REG32(HW_STRAPS_DATA_IN_VALID, 0x14u)
    FIELD(HW_STRAPS_DATA_IN_VALID, VALID, 0u, 1u)
REG32(HW_STRAPS_DATA_IN, 0x18u)
REG32(DIRECT_OUT, 0x1cu)
REG32(MASKED_OUT_LOWER, 0x20u)
    SHARED_FIELD(MASKED_VALUE, 0u, 16u)
    SHARED_FIELD(MASKED_MASK, 16u, 16u)
REG32(MASKED_OUT_UPPER, 0x24u)
REG32(DIRECT_OE, 0x28u)
REG32(MASKED_OE_LOWER, 0x2cu)
REG32(MASKED_OE_UPPER, 0x30u)
REG32(INTR_CTRL_EN_RISING, 0x34u)
REG32(INTR_CTRL_EN_FALLING, 0x38u)
REG32(INTR_CTRL_EN_LVLHIGH, 0x3cu)
REG32(INTR_CTRL_EN_LVLLOW, 0x40u)
REG32(CTRL_EN_INPUT_FILTER, 0x44u)
/* clang-format on */

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_CTRL_EN_INPUT_FILTER)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define ALERT_TEST_MASK (R_ALERT_TEST_FATAL_FAULT_ERR_MASK)

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)

static const char *REG_NAMES[REGS_COUNT] = {
    REG_NAME_ENTRY(INTR_STATE),
    REG_NAME_ENTRY(INTR_ENABLE),
    REG_NAME_ENTRY(INTR_TEST),
    REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(DATA_IN),
    REG_NAME_ENTRY(HW_STRAPS_DATA_IN_VALID),
    REG_NAME_ENTRY(HW_STRAPS_DATA_IN),
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
} OtGpioDjBackendState;

typedef enum {
    IO_IDLE,
    IO_RESET,
    IO_READY,
} OtGpioDjIOState;

struct OtGpioDjState {
    SysBusDevice parent_obj;

    IbexIRQ *irqs;
    IbexIRQ *gpos;
    IbexIRQ alert;

    MemoryRegion mmio;

    uint32_t regs[REGS_COUNT];
    uint32_t data_out; /* output data */
    uint32_t data_oe; /* output enable */
    uint32_t data_in; /* input data */
    uint32_t data_bi; /* ignore backend input */
    uint32_t data_gi; /* ignore GPIO input */
    uint32_t invert; /* invert signal */
    uint32_t opendrain; /* open drain (1 -> hi-z) */
    uint32_t pull_en; /* pull up/down enable */
    uint32_t pull_sel; /* pull up or pull down */
    uint32_t connected; /* connected to an external device */

    char ibuf[PARAM_NUM_IO]; /* backed input buffer */
    unsigned ipos;
    OtGpioDjIOState io_state;
    OtGpioDjBackendState backend_state; /* cache */

    char *ot_id;
    uint32_t reset_in; /* initial input levels */
    uint32_t reset_out; /* initial output levels */
    uint32_t reset_oe; /* initial output enable vs. hi-z levels */
    uint32_t ibex_out; /* output w/ ibex_gpio (vs. tri-state) signalization */
    CharBackend chr; /* communication device */
    guint watch_tag; /* tracker for comm device change */
    bool wipe; /* whether to wipe the backend at reset */
};

struct OtGpioDjClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

static void ot_gpio_dj_update_backend(OtGpioDjState *s);

static void ot_gpio_dj_update_irqs(OtGpioDjState *s)
{
    uint32_t level = s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE];
    trace_ot_gpio_irqs(s->ot_id, s->regs[R_INTR_STATE], s->regs[R_INTR_ENABLE],
                       level);
    for (unsigned ix = 0; ix < PARAM_NUM_IO; ix++) {
        ibex_irq_set(&s->irqs[ix], (int)((level >> ix) & 0x1u));
    }
}

static void ot_gpio_dj_update_intr_level(OtGpioDjState *s)
{
    uint32_t intr_state = 0;

    intr_state |= s->regs[R_INTR_CTRL_EN_LVLLOW] & ~s->regs[R_DATA_IN];
    intr_state |= s->regs[R_INTR_CTRL_EN_LVLHIGH] & s->regs[R_DATA_IN];

    s->regs[R_INTR_STATE] |= intr_state;
}

static void ot_gpio_dj_update_intr_edge(OtGpioDjState *s, uint32_t prev)
{
    uint32_t change = prev ^ s->regs[R_DATA_IN];
    uint32_t rising = change & s->regs[R_DATA_IN];
    uint32_t falling = change & ~s->regs[R_DATA_IN];

    uint32_t intr_state = 0;

    intr_state |= s->regs[R_INTR_CTRL_EN_RISING] & rising;
    intr_state |= s->regs[R_INTR_CTRL_EN_FALLING] & falling;

    s->regs[R_INTR_STATE] |= intr_state;
}

static void ot_gpio_dj_update_data_in(OtGpioDjState *s)
{
    uint32_t prev = s->regs[R_DATA_IN];

    uint32_t ign_mask = s->data_gi & (s->data_bi & ~s->connected);

    /* ignore disabled input pins */
    uint32_t data_in = s->data_in & ~ign_mask;

    /* apply pull up (/down) on non- input enabled pins */
    data_in |= s->pull_en & s->pull_sel;

    trace_ot_gpio_in_ign(s->ot_id, s->data_gi, s->data_bi, s->connected,
                         ign_mask);

    /* apply inversion if any */
    data_in ^= s->invert;

    /* inject back output pin values into input */
    uint32_t data_mix = data_in & ~s->data_oe;
    data_mix |= s->data_out & s->data_oe;

    s->regs[R_DATA_IN] = data_mix;

    trace_ot_gpio_update_input(s->ot_id, s->pull_en, s->pull_sel, s->invert,
                               data_in, data_mix);

    ot_gpio_dj_update_intr_level(s);
    ot_gpio_dj_update_intr_edge(s, prev);
    ot_gpio_dj_update_irqs(s);
}

static void ot_gpio_dj_update_data_out(OtGpioDjState *s)
{
    uint32_t outv = s->data_out;
    /* assume invert is performed on device output data, not on pull up/down */
    outv ^= s->invert; /* OV */

    /*
     *   OE  OD  OV  PE  PU  NA  Wk  IbexOut BinOut
     *  |---|---|---|---|---|---|---|-------|------|
     *    0   X   X   0   X   1   0     z    undef
     *    0   X   X   1   0   1   1     l     0
     *    0   X   X   1   1   1   1     h     1
     *    1   0   0   X   X   0   0     L     0
     *    1   0   1   X   X   0   0     H     1
     *    1   1   0   X   X   0   0     L     0
     *    1   1   1   0   X   1   0     z    undef
     *    1   1   1   1   0   1   1     l     0
     *    1   1   1   1   1   1   1     h     1
     */

    uint32_t not_active =
        ~s->data_oe | (s->data_oe & s->opendrain & outv); /* NA */
    uint32_t hi_z = ~s->pull_en & not_active;
    uint32_t weak = s->pull_en & not_active; /* Wk */

    trace_ot_gpio_update_output(s->ot_id, outv, weak, hi_z);
    for (unsigned ix = 0; ix < PARAM_NUM_IO; ix++) {
        unsigned bit = 1u << ix;
        int level;
        if (s->ibex_out & bit) {
            /* Ibex GPIO output */
            level =
                hi_z & bit ?
                    IBEX_GPIO_HIZ :
                    (weak & bit ? IBEX_GPIO_FROM_WEAK_SIG(s->pull_sel & bit) :
                                  IBEX_GPIO_FROM_ACTIVE_SIG(outv & bit));
        } else {
            /* binary output */
            if (hi_z & bit) {
                /* skip */
                continue;
            }
            level = (int)(bool)((weak ? s->pull_sel : outv) & bit);
        }
        if (level != ibex_irq_get_level(&s->gpos[ix])) {
            if (s->ibex_out & bit) {
                trace_ot_gpio_update_out_line_ibex(s->ot_id, ix,
                                                   ibex_gpio_repr(level));
            } else {
                trace_ot_gpio_update_out_line_bool(s->ot_id, ix, level);
            }
        }
        ibex_irq_set(&s->gpos[ix], level);
    }
}

static void ot_gpio_dj_strap_en(void *opaque, int no, int level)
{
    OtGpioDjState *s = opaque;

    g_assert(no == 0);

    if (level) {
        s->regs[R_HW_STRAPS_DATA_IN] = s->data_in;
        s->regs[R_HW_STRAPS_DATA_IN_VALID] =
            R_HW_STRAPS_DATA_IN_VALID_VALID_MASK;
    }

    trace_ot_gpio_strap_en(s->ot_id, no, (bool)level,
                           s->regs[R_HW_STRAPS_DATA_IN]);
}

static void ot_gpio_dj_in_change(void *opaque, int no, int level)
{
    OtGpioDjState *s = opaque;

    g_assert(no < PARAM_NUM_IO);

    bool ibex_in = ibex_gpio_check(level);
    bool hiz;
    bool on;
    bool weak;

    if (ibex_in) {
        hiz = ibex_gpio_is_hiz(level);
        on = ibex_gpio_level(level);
        weak = ibex_gpio_is_weak(level);
    } else {
        hiz = level < 0;
        on = level > 0;
        weak = false;
    }
    trace_ot_gpio_in_change(s->ot_id, no, hiz, on, weak);

    uint32_t bit = 1u << no;

    /*
     * any time a signal is received from a remote device the pin is considered
     * as connected and backend no longer may update its state.
     */
    s->connected |= bit;

    if (!hiz) {
        if (on) {
            s->data_in |= bit;
        } else {
            s->data_in &= ~bit;
        }
        s->data_gi &= ~bit;
    } else {
        s->data_gi |= bit;
    }

    ot_gpio_dj_update_data_in(s);
    ot_gpio_dj_update_backend(s);
}

static void ot_gpio_dj_pad_attr_change(void *opaque, int no, int level)
{
    OtGpioDjState *s = opaque;

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

    trace_ot_gpio_pad_attr_change(s->ot_id, no, cfg, confstr);

    if (s->io_state == IO_READY) {
        ot_gpio_dj_update_data_out(s);
        ot_gpio_dj_update_backend(s);
        ot_gpio_dj_update_data_in(s);
    }
}

static uint64_t ot_gpio_dj_read(void *opaque, hwaddr addr, unsigned size)
{
    OtGpioDjState *s = opaque;
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_INTR_STATE:
    case R_INTR_ENABLE:
    case R_DATA_IN:
    case R_DIRECT_OUT:
    case R_DIRECT_OE:
    case R_INTR_CTRL_EN_RISING:
    case R_INTR_CTRL_EN_FALLING:
    case R_INTR_CTRL_EN_LVLHIGH:
    case R_INTR_CTRL_EN_LVLLOW:
    case R_CTRL_EN_INPUT_FILTER:
    case R_HW_STRAPS_DATA_IN:
    case R_HW_STRAPS_DATA_IN_VALID:
        val32 = s->regs[reg];
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
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: W/O register 0x%02" HWADDR_PRIx " (%s)\n",
                      __func__, s->ot_id, addr, REG_NAME(reg));
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Bad offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, addr);
        val32 = 0u;
        break;
    }

    uint32_t pc = ibex_get_current_pc();
    trace_ot_gpio_io_read_out(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32,
                              pc);

    return (uint64_t)val32;
};

static void ot_gpio_dj_write(void *opaque, hwaddr addr, uint64_t val64,
                             unsigned size)
{
    OtGpioDjState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;
    uint32_t mask;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_gpio_io_write(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32, pc);

    switch (reg) {
    case R_INTR_STATE:
        s->regs[reg] &= ~val32; /* RW1C */
        ot_gpio_dj_update_irqs(s);
        break;
    case R_INTR_ENABLE:
        s->regs[reg] = val32;
        ot_gpio_dj_update_irqs(s);
        break;
    case R_INTR_TEST:
        s->regs[R_INTR_STATE] |= val32;
        ot_gpio_dj_update_irqs(s);
        break;
    case R_ALERT_TEST:
        val32 &= ALERT_TEST_MASK;
        ibex_irq_set(&s->alert, (int)(bool)val32);
        break;
    case R_DIRECT_OUT:
        s->regs[reg] = val32;
        s->data_out = val32;
        ot_gpio_dj_update_data_out(s);
        ot_gpio_dj_update_backend(s);
        ot_gpio_dj_update_data_in(s);
        break;
    case R_DIRECT_OE:
        s->regs[reg] = val32;
        s->data_oe = val32;
        ot_gpio_dj_update_data_out(s);
        ot_gpio_dj_update_backend(s);
        ot_gpio_dj_update_data_in(s);
        break;
    case R_MASKED_OUT_LOWER:
        s->regs[reg] = val32;
        mask = val32 >> MASKED_MASK_SHIFT;
        s->data_out &= ~mask;
        s->data_out |= val32 & mask;
        ot_gpio_dj_update_data_out(s);
        ot_gpio_dj_update_backend(s);
        ot_gpio_dj_update_data_in(s);
        break;
    case R_MASKED_OUT_UPPER:
        s->regs[reg] = val32;
        mask = val32 & MASKED_MASK_MASK;
        s->data_out &= ~mask;
        s->data_out |= (val32 << MASKED_MASK_SHIFT) & mask;
        ot_gpio_dj_update_data_out(s);
        ot_gpio_dj_update_backend(s);
        ot_gpio_dj_update_data_in(s);
        break;
    case R_MASKED_OE_LOWER:
        s->regs[reg] = val32;
        mask = val32 >> MASKED_MASK_SHIFT;
        s->data_oe &= ~mask;
        s->data_oe |= val32 & mask;
        ot_gpio_dj_update_data_out(s);
        ot_gpio_dj_update_backend(s);
        ot_gpio_dj_update_data_in(s);
        break;
    case R_MASKED_OE_UPPER:
        s->regs[reg] = val32;
        mask = val32 & MASKED_MASK_MASK;
        s->data_oe &= ~mask;
        s->data_oe |= (val32 << MASKED_MASK_SHIFT) & mask;
        ot_gpio_dj_update_data_out(s);
        ot_gpio_dj_update_backend(s);
        ot_gpio_dj_update_data_in(s);
        break;
    case R_INTR_CTRL_EN_RISING:
    case R_INTR_CTRL_EN_FALLING:
        s->regs[reg] = val32;
        break;
    case R_INTR_CTRL_EN_LVLHIGH:
    case R_INTR_CTRL_EN_LVLLOW:
        s->regs[reg] = val32;
        ot_gpio_dj_update_data_in(s);
        break;
    case R_CTRL_EN_INPUT_FILTER:
        /* nothing can be done at QEMU level for sampling that fast */
        s->regs[reg] = val32;
        break;
    case R_DATA_IN:
    case R_HW_STRAPS_DATA_IN:
    case R_HW_STRAPS_DATA_IN_VALID:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: R/O register 0x%02" HWADDR_PRIx " (%s)\n",
                      __func__, s->ot_id, addr, REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Bad offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, addr);
        break;
    }
};

static int ot_gpio_dj_chr_can_receive(void *opaque)
{
    OtGpioDjState *s = opaque;

    return (int)sizeof(s->ibuf) - (int)s->ipos;
}

static void ot_gpio_dj_chr_receive(void *opaque, const uint8_t *buf, int size)
{
    OtGpioDjState *s = opaque;

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
            if (s->ipos > 10u) {
                /* discard any garbage */
                memset(s->ibuf, 0, sizeof(s->ibuf));
                s->ipos = 0;
            }
            return;
        }
        unsigned eolpos = eol - s->ibuf;
        if (eolpos < 10u) {
            /* discard incomplete lines */
            memmove(s->ibuf, eol + 1u, eolpos + 1u);
            s->ipos = 0;
            continue;
        }
        uint32_t data_in = 0;
        char cmd = '\0';

        /* NOLINTNEXTLINE */
        int ret = sscanf(s->ibuf, "%c:%08x", &cmd, &data_in);
        /* discard current command, even if invalid, up to first EOL */
        s->ipos -= eolpos;
        memmove(s->ibuf, eol + 1u, s->ipos);

        if (ret == 2) {
            if (cmd == 'M') {
                s->data_bi = data_in;
                ot_gpio_dj_update_data_in(s);
            } else if (cmd == 'I') {
                s->data_in = data_in;
                ot_gpio_dj_update_data_in(s);
            } else if (cmd == 'R') {
                ot_gpio_dj_update_backend(s);
            }
        }
    }
}

static void ot_gpio_dj_init_backend(OtGpioDjState *s)
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

static void ot_gpio_dj_update_backend(OtGpioDjState *s)
{
    if (!qemu_chr_fe_backend_connected(&s->chr)) {
        return;
    }

    /*
     * use the MS DOS CR LF syntax because some people keep using
     * Windows-style terminal.
     */

    uint32_t outv = s->data_out;
    /* assume invert is performed on device output data, not on pull up/down */
    outv ^= s->invert;

    uint32_t out_en = s->data_oe;

    /* if open drain is active and output is high, disable output enable */
    out_en &= ~(s->opendrain & outv);

    uint32_t active = s->pull_en | out_en;
    outv &= out_en;

    OtGpioDjBackendState bstate = {
        .hi_z = ~active,
        .pull_v = s->pull_sel,
        .out_en = out_en,
        .out_v = outv,
    };

    /*
     * use the MS DOS CR LF syntax because some people keep using
     * Windows-style terminal.
     */

    if (!memcmp(&bstate, &s->backend_state, sizeof(OtGpioDjBackendState))) {
        /* do not emit new state if nothing has changed */
        return;
    }

    char buf[64u];
    size_t len = 0;

    len += snprintf(&buf[len], sizeof(buf) - len, "Z:%08x\r\n", bstate.hi_z);
    len += snprintf(&buf[len], sizeof(buf) - len, "P:%08x\r\n", bstate.pull_v);
    len += snprintf(&buf[len], sizeof(buf) - len, "D:%08x\r\n", bstate.out_en);
    len += snprintf(&buf[len], sizeof(buf) - len, "O:%08x\r\n", bstate.out_v);

    s->backend_state = bstate;

    qemu_chr_fe_write(&s->chr, (const uint8_t *)buf, (int)len);
}

static void ot_gpio_dj_chr_event_hander(void *opaque, QEMUChrEvent event)
{
    OtGpioDjState *s = opaque;

    if (event == CHR_EVENT_CLOSED) {
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        return;
    }

    if (event == CHR_EVENT_OPENED) {
        if (object_dynamic_cast(OBJECT(s->chr.chr), TYPE_CHARDEV_SERIAL)) {
            ot_common_ignore_chr_status_lines(&s->chr);
        }

        if (!qemu_chr_fe_backend_connected(&s->chr)) {
            return;
        }

        ot_gpio_dj_update_backend(s);

        /* query backend for current input status */
        char buf[16u];
        int len = snprintf(buf, sizeof(buf), "Q:%08x\r\n", s->data_oe);
        qemu_chr_fe_write(&s->chr, (const uint8_t *)buf, len);
    }
}

static gboolean
ot_gpio_dj_chr_watch_cb(void *do_not_use, GIOCondition cond, void *opaque)
{
    OtGpioDjState *s = opaque;
    (void)do_not_use;
    (void)cond;

    s->watch_tag = 0;

    return FALSE;
}

static int ot_gpio_dj_chr_be_change(void *opaque)
{
    OtGpioDjState *s = opaque;

    qemu_chr_fe_set_handlers(&s->chr, &ot_gpio_dj_chr_can_receive,
                             &ot_gpio_dj_chr_receive,
                             &ot_gpio_dj_chr_event_hander,
                             &ot_gpio_dj_chr_be_change, s, NULL, true);

    memset(s->ibuf, 0, sizeof(s->ibuf));
    s->ipos = 0;

    if (s->watch_tag > 0) {
        g_source_remove(s->watch_tag);
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        s->watch_tag = qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP,
                                             &ot_gpio_dj_chr_watch_cb, s);
    }

    return 0;
}

static const MemoryRegionOps ot_gpio_dj_regs_ops = {
    .read = &ot_gpio_dj_read,
    .write = &ot_gpio_dj_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static Property ot_gpio_dj_properties[] = {
    DEFINE_PROP_STRING("ot_id", OtGpioDjState, ot_id),
    DEFINE_PROP_UINT32("in", OtGpioDjState, reset_in, 0u),
    DEFINE_PROP_UINT32("out", OtGpioDjState, reset_out, 0u),
    DEFINE_PROP_UINT32("oe", OtGpioDjState, reset_oe, 0u),
    DEFINE_PROP_UINT32("ibex_out", OtGpioDjState, ibex_out, 0u),
    DEFINE_PROP_BOOL("wipe", OtGpioDjState, wipe, false),
    DEFINE_PROP_CHR("chardev", OtGpioDjState, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void ot_gpio_dj_reset_enter(Object *obj, ResetType type)
{
    OtGpioDjClass *c = OT_GPIO_DJ_GET_CLASS(obj);
    OtGpioDjState *s = OT_GPIO_DJ(obj);

    if (!s->ot_id) {
        s->ot_id =
            g_strdup(object_get_canonical_path_component(OBJECT(s)->parent));
    }

    trace_ot_gpio_reset(s->ot_id, "> enter");

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    s->io_state = IO_RESET;

    memset(s->regs, 0, sizeof(s->regs));
    memset(&s->backend_state, 0, sizeof(s->backend_state));

    /* reset_* fields are properties, never get reset */
    s->data_in = s->reset_in;
    s->data_out = s->reset_out;
    s->data_oe = s->reset_oe;
    s->data_bi = 0;
    /* all input disable until signal is received, or output is forced */
    s->data_gi = ~s->reset_oe;
    s->pull_en = 0;
    s->pull_sel = 0;
    s->invert = 0;
    s->connected = 0;

    s->regs[R_DATA_IN] = s->reset_in;
    s->regs[R_DIRECT_OUT] = s->reset_out;
    s->regs[R_DIRECT_OE] = s->reset_oe;

    ot_gpio_dj_update_irqs(s);
    ibex_irq_set(&s->alert, 0);

    trace_ot_gpio_reset(s->ot_id, "< enter");
}

static void ot_gpio_dj_reset_exit(Object *obj, ResetType type)
{
    /*
     * use of a Resettable full API enables performing I/O updates only once
     * the pinmux configuration has been received (from its own reset stage)
     */
    OtGpioDjClass *c = OT_GPIO_DJ_GET_CLASS(obj);
    OtGpioDjState *s = OT_GPIO_DJ(obj);

    trace_ot_gpio_reset(s->ot_id, "> exit");

    if (c->parent_phases.exit) {
        c->parent_phases.exit(obj, type);
    }

    ot_gpio_dj_init_backend(s);
    ot_gpio_dj_update_data_out(s);
    ot_gpio_dj_update_backend(s);
    ot_gpio_dj_update_data_in(s);

    s->io_state = IO_READY;
    /*
     * do not reset the input backend buffer as external GPIO changes is fully
     * async with OT reset. However, it should be reset when the backend changes
     */
    trace_ot_gpio_reset(s->ot_id, "< exit");
}

static void ot_gpio_dj_realize(DeviceState *dev, Error **errp)
{
    OtGpioDjState *s = OT_GPIO_DJ(dev);
    (void)errp;

    qemu_chr_fe_set_handlers(&s->chr, &ot_gpio_dj_chr_can_receive,
                             &ot_gpio_dj_chr_receive,
                             &ot_gpio_dj_chr_event_hander,
                             &ot_gpio_dj_chr_be_change, s, NULL, true);
}

static void ot_gpio_dj_init(Object *obj)
{
    OtGpioDjState *s = OT_GPIO_DJ(obj);

    memory_region_init_io(&s->mmio, obj, &ot_gpio_dj_regs_ops, s,
                          TYPE_OT_GPIO_DJ, REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    s->irqs = g_new(IbexIRQ, PARAM_NUM_IO);
    s->gpos = g_new(IbexIRQ, PARAM_NUM_IO);
    for (unsigned ix = 0; ix < PARAM_NUM_IO; ix++) {
        ibex_sysbus_init_irq(obj, &s->irqs[ix]);
    }
    ibex_qdev_init_irqs_default(obj, s->gpos, OT_GPIO_OUT, PARAM_NUM_IO, -1);
    ibex_qdev_init_irq(obj, &s->alert, OT_DEVICE_ALERT);

    qdev_init_gpio_in_named(DEVICE(obj), &ot_gpio_dj_strap_en, OT_GPIO_STRAP_EN,
                            1);
    qdev_init_gpio_in_named(DEVICE(obj), &ot_gpio_dj_in_change, OT_GPIO_IN,
                            PARAM_NUM_IO);
    qdev_init_gpio_in_named(DEVICE(obj), &ot_gpio_dj_pad_attr_change,
                            OT_PINMUX_PAD, PARAM_NUM_IO);

    s->io_state = IO_IDLE;
}

static void ot_gpio_dj_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->realize = &ot_gpio_dj_realize;
    device_class_set_props(dc, ot_gpio_dj_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    ResettableClass *rc = RESETTABLE_CLASS(dc);
    OtGpioDjClass *pc = OT_GPIO_DJ_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_gpio_dj_reset_enter, NULL,
                                       &ot_gpio_dj_reset_exit,
                                       &pc->parent_phases);
}

static const TypeInfo ot_gpio_dj_info = {
    .name = TYPE_OT_GPIO_DJ,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtGpioDjState),
    .instance_init = &ot_gpio_dj_init,
    .class_init = &ot_gpio_dj_class_init,
    .class_size = sizeof(OtGpioDjClass)
};

static void ot_gpio_dj_register_types(void)
{
    type_register_static(&ot_gpio_dj_info);
}

type_init(ot_gpio_dj_register_types);
