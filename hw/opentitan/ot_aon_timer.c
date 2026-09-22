/*
 * QEMU OpenTitan AON Timer device
 *
 * Copyright (c) 2023-2025 Rivos, Inc.
 * Copyright (c) 2025 lowRISC contributors.
 *
 * Author(s):
 *  Loïc Lefort <loic@rivosinc.com>
 *
 * Currently missing from implementation:
 *   - "pause in sleep" and "pause during escalation" features
 *     (i.e. "counter-run" and "low-power" inputs)
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
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_aon_timer.h"
#include "hw/opentitan/ot_common.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_clock_src.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "trace.h"

/* clang-format off */
REG32(ALERT_TEST, 0x00u)
    FIELD(ALERT_TEST, FATAL_FAULT, 0u, 1u)
REG32(WKUP_CTRL, 0x04u)
    FIELD(WKUP_CTRL, ENABLE, 0u, 1u)
    FIELD(WKUP_CTRL, PRESCALER, 1u, 12u)
REG32(WKUP_THOLD_HI, 0x08u)
REG32(WKUP_THOLD_LO, 0x0cu)
REG32(WKUP_COUNT_HI, 0x10u)
REG32(WKUP_COUNT_LO, 0x14u)
REG32(WDOG_REGWEN, 0x18u)
    FIELD(WDOG_REGWEN, REGWEN, 0u, 1u)
REG32(WDOG_CTRL, 0x1cu)
    FIELD(WDOG_CTRL, ENABLE, 0u, 1u)
    FIELD(WDOG_CTRL, PAUSE_IN_SLEEP, 1u, 1u)
REG32(WDOG_BARK_THOLD, 0x20u)
REG32(WDOG_BITE_THOLD, 0x24u)
REG32(WDOG_COUNT, 0x28u)
REG32(INTR_STATE, 0x2cu)
    SHARED_FIELD(INTR_WKUP_TIMER_EXPIRED, 0u, 1u)
    SHARED_FIELD(INTR_WDOG_TIMER_BARK, 1u, 1u)
REG32(INTR_TEST, 0x30u)
REG32(WKUP_CAUSE, 0x34u)
    FIELD(WKUP_CAUSE, CAUSE, 0u, 1u)
/* clang-format on */

#define INTR_MASK (INTR_WKUP_TIMER_EXPIRED_MASK | INTR_WDOG_TIMER_BARK_MASK)

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_WKUP_CAUSE)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) < REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char REG_NAMES[REGS_COUNT][20u] = {
    /* clang-format off */
    REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(WKUP_CTRL),
    REG_NAME_ENTRY(WKUP_THOLD_HI),
    REG_NAME_ENTRY(WKUP_THOLD_LO),
    REG_NAME_ENTRY(WKUP_COUNT_HI),
    REG_NAME_ENTRY(WKUP_COUNT_LO),
    REG_NAME_ENTRY(WDOG_REGWEN),
    REG_NAME_ENTRY(WDOG_CTRL),
    REG_NAME_ENTRY(WDOG_BARK_THOLD),
    REG_NAME_ENTRY(WDOG_BITE_THOLD),
    REG_NAME_ENTRY(WDOG_COUNT),
    REG_NAME_ENTRY(INTR_STATE),
    REG_NAME_ENTRY(INTR_TEST),
    REG_NAME_ENTRY(WKUP_CAUSE),
    /* clang-format on */
};
#undef REG_NAME_ENTRY

typedef enum {
    OT_AON_TIMER_CLOCK_SRC_IO,
    OT_AON_TIMER_CLOCK_SRC_AON,
    OT_AON_TIMER_CLOCK_SRC_COUNT
} OtAonTimerClockSrc;

struct OtAonTimerState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;

    IbexIRQ irq_wkup;
    IbexIRQ irq_bark;
    IbexIRQ nmi_bark;
    IbexIRQ pwrmgr_wkup;
    IbexIRQ pwrmgr_bite;
    IbexIRQ alert;

    QEMUTimer *wkup_timer;
    QEMUTimer *wdog_timer;

    uint32_t regs[REGS_COUNT];

    int64_t wkup_origin_ns;
    int64_t wdog_origin_ns;
    uint32_t prescale_count;
    bool wkup_intr_level;
    bool wdog_bark_level;
    bool wdog_bite;
    bool lc_escalate_en;
    bool sleep_mode;
    bool low_power_exit;
    uint32_t pclks[OT_AON_TIMER_CLOCK_SRC_COUNT];
    const char *clock_src_names[OT_AON_TIMER_CLOCK_SRC_COUNT];

    char *ot_id;
    char *clock_names[OT_AON_TIMER_CLOCK_SRC_COUNT];
    DeviceState *clock_src;
};

struct OtAonTimerClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

static uint64_t
ot_aon_timer_ns_to_ticks(OtAonTimerState *s, uint32_t prescaler, int64_t ns)
{
    uint64_t ticks =
        muldiv64((uint64_t)ns, s->pclks[OT_AON_TIMER_CLOCK_SRC_AON],
                 NANOSECONDS_PER_SECOND);
    return ticks / (prescaler + 1u);
}

static int64_t
ot_aon_timer_ticks_to_ns(OtAonTimerState *s, uint32_t prescaler, uint64_t ticks)
{
    uint64_t ns = muldiv64(ticks * (prescaler + 1u), NANOSECONDS_PER_SECOND,
                           s->pclks[OT_AON_TIMER_CLOCK_SRC_AON]);
    if (ns > INT64_MAX) {
        return INT64_MAX;
    }
    return (int64_t)ns;
}

static int64_t ot_aon_timer_compute_next_timeout(OtAonTimerState *s,
                                                 int64_t now, int64_t delta)
{
    int64_t next;

    g_assert(s->pclks[OT_AON_TIMER_CLOCK_SRC_AON]);

    /* wait at least 1 peripheral clock tick */
    delta = MAX(delta, (int64_t)(NANOSECONDS_PER_SECOND /
                                 s->pclks[OT_AON_TIMER_CLOCK_SRC_AON]));

    if (sadd64_overflow(now, delta, &next)) {
        /* we overflowed the timer, just set it as large as we can */
        return INT64_MAX;
    }

    return next;
}

static inline bool ot_aon_timer_is_wkup_enabled(OtAonTimerState *s)
{
    return (bool)FIELD_EX32(s->regs[R_WKUP_CTRL], WKUP_CTRL, ENABLE) &&
           !s->lc_escalate_en;
}

static inline bool ot_aon_timer_is_wdog_enabled(OtAonTimerState *s)
{
    bool pause =
        s->sleep_mode &&
        (bool)FIELD_EX32(s->regs[R_WDOG_CTRL], WDOG_CTRL, PAUSE_IN_SLEEP);
    return (bool)FIELD_EX32(s->regs[R_WDOG_CTRL], WDOG_CTRL, ENABLE) &&
           !s->lc_escalate_en && !pause;
}

bool ot_aon_timer_is_active(OtAonTimerState *s)
{
    if (!s) {
        return false;
    }
    return ot_aon_timer_is_wkup_enabled(s) || ot_aon_timer_is_wdog_enabled(s);
}

static inline bool ot_aon_timer_wdog_register_write_enabled(OtAonTimerState *s)
{
    return (s->regs[R_WDOG_REGWEN] & R_WDOG_REGWEN_REGWEN_MASK) != 0;
}

static uint32_t ot_aon_timer_first_wkup_incr_clks(const OtAonTimerState *s)
{
    uint32_t prescaler = FIELD_EX32(s->regs[R_WKUP_CTRL], WKUP_CTRL, PRESCALER);
    if (s->prescale_count <= prescaler) {
        return (prescaler - s->prescale_count) + 1u;
    }
    return (4096u - s->prescale_count) + prescaler + 1u;
}

static void ot_aon_timer_sync_wkup(OtAonTimerState *s)
{
    if (!s->pclks[OT_AON_TIMER_CLOCK_SRC_AON]) {
        return;
    }

    int64_t now = qemu_clock_get_ns(OT_VIRTUAL_CLOCK);
    uint64_t now_clks = ot_aon_timer_ns_to_ticks(s, 0u, now);
    uint64_t origin_clks = ot_aon_timer_ns_to_ticks(s, 0u, s->wkup_origin_ns);

    if (!ot_aon_timer_is_wkup_enabled(s)) {
        s->wkup_intr_level = false;
        s->wkup_origin_ns = ot_aon_timer_ticks_to_ns(s, 0u, now_clks);
        return;
    }

    uint64_t elapsed_clks =
        (now_clks > origin_clks) ? (now_clks - origin_clks) : 0u;
    if (elapsed_clks == 0u) {
        return;
    }

    uint32_t prescaler = FIELD_EX32(s->regs[R_WKUP_CTRL], WKUP_CTRL, PRESCALER);
    uint32_t first_incr_clks = ot_aon_timer_first_wkup_incr_clks(s);
    uint64_t wkup_incrs;

    if (elapsed_clks < first_incr_clks) {
        wkup_incrs = 0u;
        s->prescale_count =
            (s->prescale_count + (uint32_t)elapsed_clks) & 0xfffu;
        s->wkup_intr_level = false;
    } else {
        uint64_t rem = elapsed_clks - first_incr_clks;
        uint32_t period = prescaler + 1u;
        wkup_incrs = 1u + (rem / period);
        s->prescale_count = (uint32_t)(rem % period);
    }

    if (wkup_incrs > 0u) {
        uint64_t old_count = ((uint64_t)s->regs[R_WKUP_COUNT_HI] << 32u) |
                             (uint64_t)s->regs[R_WKUP_COUNT_LO];
        uint64_t new_count = old_count + wkup_incrs;
        uint64_t threshold = ((uint64_t)s->regs[R_WKUP_THOLD_HI] << 32u) |
                             (uint64_t)s->regs[R_WKUP_THOLD_LO];
        bool was_continuous_high =
            s->wkup_intr_level && (first_incr_clks == 1u) &&
            (prescaler == 0u) && (old_count >= threshold);

        if ((new_count - 1u) >= threshold) {
            if (!was_continuous_high) {
                s->regs[R_INTR_STATE] |= INTR_WKUP_TIMER_EXPIRED_MASK;
            }
            s->regs[R_WKUP_CAUSE] |= R_WKUP_CAUSE_CAUSE_MASK;
        }
        s->wkup_intr_level =
            (s->prescale_count == 0u) && ((new_count - 1u) >= threshold);

        s->regs[R_WKUP_COUNT_HI] = (uint32_t)(new_count >> 32u);
        s->regs[R_WKUP_COUNT_LO] = (uint32_t)new_count;
    }

    s->wkup_origin_ns = ot_aon_timer_ticks_to_ns(s, 0u, now_clks);
}

static void ot_aon_timer_sync_wdog(OtAonTimerState *s)
{
    if (!s->pclks[OT_AON_TIMER_CLOCK_SRC_AON]) {
        return;
    }

    int64_t now = qemu_clock_get_ns(OT_VIRTUAL_CLOCK);
    uint64_t now_clks = ot_aon_timer_ns_to_ticks(s, 0u, now);
    uint64_t origin_clks = ot_aon_timer_ns_to_ticks(s, 0u, s->wdog_origin_ns);

    if (!ot_aon_timer_is_wdog_enabled(s)) {
        s->wdog_origin_ns = ot_aon_timer_ticks_to_ns(s, 0u, now_clks);
        return;
    }

    uint64_t elapsed_clks =
        (now_clks > origin_clks) ? (now_clks - origin_clks) : 0u;
    if (elapsed_clks == 0u) {
        return;
    }

    uint64_t old_count = s->regs[R_WDOG_COUNT];
    uint64_t new_count = old_count + elapsed_clks;
    uint32_t bark_threshold = s->regs[R_WDOG_BARK_THOLD];
    uint32_t bite_threshold = s->regs[R_WDOG_BITE_THOLD];
    uint64_t last_pre_incr = new_count - 1u;

    bool bark_level = (last_pre_incr >= bark_threshold);
    if (bark_level && !s->wdog_bark_level) {
        s->regs[R_INTR_STATE] |= INTR_WDOG_TIMER_BARK_MASK;
    }
    s->wdog_bark_level = bark_level;

    if (bark_level) {
        s->regs[R_WKUP_CAUSE] |= R_WKUP_CAUSE_CAUSE_MASK;
    }
    if (last_pre_incr >= bite_threshold) {
        s->wdog_bite = true;
    }

    s->regs[R_WDOG_COUNT] = (uint32_t)new_count;
    s->wdog_origin_ns = ot_aon_timer_ticks_to_ns(s, 0u, now_clks);
}

static void ot_aon_timer_update_alert(OtAonTimerState *s)
{
    bool level = s->regs[R_ALERT_TEST] & R_ALERT_TEST_FATAL_FAULT_MASK;
    ibex_irq_set(&s->alert, level);
}

static void ot_aon_timer_update_irqs(OtAonTimerState *s)
{
    bool wkup = (bool)(s->regs[R_INTR_STATE] & INTR_WKUP_TIMER_EXPIRED_MASK);
    bool bark = (bool)(s->regs[R_INTR_STATE] & INTR_WDOG_TIMER_BARK_MASK);
    bool wkup_req = (bool)(s->regs[R_WKUP_CAUSE] & R_WKUP_CAUSE_CAUSE_MASK);
    trace_ot_aon_timer_irqs(s->ot_id, wkup, bark, s->wdog_bite);

    ibex_irq_set(&s->irq_wkup, wkup);
    ibex_irq_set(&s->irq_bark, bark);
    ibex_irq_set(&s->nmi_bark, bark);
    ibex_irq_set(&s->pwrmgr_wkup, wkup_req);
    ibex_irq_set(&s->pwrmgr_bite, s->wdog_bite);
}

static void ot_aon_timer_rearm_wkup(OtAonTimerState *s, bool reset_origin)
{
    timer_del(s->wkup_timer);

    if (!s->pclks[OT_AON_TIMER_CLOCK_SRC_AON]) {
        return;
    }

    ot_aon_timer_sync_wkup(s);
    if (reset_origin) {
        int64_t now = qemu_clock_get_ns(OT_VIRTUAL_CLOCK);
        s->wkup_origin_ns =
            ot_aon_timer_ticks_to_ns(s, 0u,
                                     ot_aon_timer_ns_to_ticks(s, 0u, now));
    }

    /* if not enabled, ignore threshold */
    if (!ot_aon_timer_is_wkup_enabled(s)) {
        s->wkup_intr_level = false;
        ot_aon_timer_update_irqs(s);
        return;
    }

    uint64_t count = ((uint64_t)s->regs[R_WKUP_COUNT_HI] << 32u) |
                     (uint64_t)s->regs[R_WKUP_COUNT_LO];
    uint64_t threshold = ((uint64_t)s->regs[R_WKUP_THOLD_HI] << 32u) |
                         (uint64_t)s->regs[R_WKUP_THOLD_LO];
    uint32_t prescaler = FIELD_EX32(s->regs[R_WKUP_CTRL], WKUP_CTRL, PRESCALER);
    if (count < threshold) {
        s->wkup_intr_level = false;
    }

    bool intr_done =
        (bool)(s->regs[R_INTR_STATE] & INTR_WKUP_TIMER_EXPIRED_MASK) ||
        (s->wkup_intr_level && prescaler == 0u && count >= threshold);
    bool cause_set = (bool)(s->regs[R_WKUP_CAUSE] & R_WKUP_CAUSE_CAUSE_MASK);

    if ((!intr_done || !cause_set) &&
        ((threshold <= count) ||
         ((threshold - count) <= (INT64_MAX / 5000u) / (prescaler + 1u)))) {
        uint64_t rem_incrs = (count >= threshold) ? 0u : (threshold - count);
        uint64_t needed_clks =
            ot_aon_timer_first_wkup_incr_clks(s) + rem_incrs * (prescaler + 1u);
        int64_t delta = ot_aon_timer_ticks_to_ns(s, 0u, needed_clks);
        int64_t next =
            ot_aon_timer_compute_next_timeout(s, s->wkup_origin_ns, delta);
        if (next < INT64_MAX) {
            timer_mod(s->wkup_timer, next);
        }
    }

    ot_aon_timer_update_irqs(s);
}

static void ot_aon_timer_wkup_cb(void *opaque)
{
    OtAonTimerState *s = opaque;
    ot_aon_timer_rearm_wkup(s, false);
}

static void ot_aon_timer_rearm_wdog(OtAonTimerState *s, bool reset_origin)
{
    if (!s->pclks[OT_AON_TIMER_CLOCK_SRC_AON]) {
        return;
    }

    ot_aon_timer_sync_wdog(s);
    if (reset_origin) {
        int64_t now = qemu_clock_get_ns(OT_VIRTUAL_CLOCK);
        s->wdog_origin_ns =
            ot_aon_timer_ticks_to_ns(s, 0u,
                                     ot_aon_timer_ns_to_ticks(s, 0u, now));
    }

    /* if not enabled, ignore threshold */
    if (!ot_aon_timer_is_wdog_enabled(s)) {
        s->wdog_bark_level = false;
        timer_del(s->wdog_timer);
        ot_aon_timer_update_irqs(s);
        return;
    }

    uint32_t count = s->regs[R_WDOG_COUNT];
    uint32_t bark_threshold = s->regs[R_WDOG_BARK_THOLD];
    uint32_t bite_threshold = s->regs[R_WDOG_BITE_THOLD];
    if (count < bark_threshold) {
        s->wdog_bark_level = false;
    }

    uint64_t needed_clks = UINT64_MAX;
    bool bark_done = (s->wdog_bark_level ||
                      (s->regs[R_INTR_STATE] & INTR_WDOG_TIMER_BARK_MASK)) &&
                     (s->regs[R_WKUP_CAUSE] & R_WKUP_CAUSE_CAUSE_MASK);
    if (!bark_done) {
        uint64_t bark_clks = (count >= bark_threshold) ?
                                 1u :
                                 ((uint64_t)(bark_threshold - count) + 1u);
        needed_clks = MIN(needed_clks, bark_clks);
    }
    if (!s->wdog_bite) {
        uint64_t bite_clks = (count >= bite_threshold) ?
                                 1u :
                                 ((uint64_t)(bite_threshold - count) + 1u);
        needed_clks = MIN(needed_clks, bite_clks);
    }

    timer_del(s->wdog_timer);

    if (needed_clks < UINT64_MAX) {
        int64_t delta = ot_aon_timer_ticks_to_ns(s, 0u, needed_clks);
        int64_t next =
            ot_aon_timer_compute_next_timeout(s, s->wdog_origin_ns, delta);
        if (next < INT64_MAX) {
            trace_ot_aon_timer_set_wdog(s->ot_id, s->wdog_origin_ns, next);
            timer_mod(s->wdog_timer, next);
        }
    }

    ot_aon_timer_update_irqs(s);
}

static void ot_aon_timer_wdog_cb(void *opaque)
{
    OtAonTimerState *s = opaque;
    ot_aon_timer_rearm_wdog(s, false);
}

static void ot_aon_timer_clock_input(void *opaque, int irq, int level)
{
    OtAonTimerState *s = opaque;

    g_assert((unsigned)irq < OT_AON_TIMER_CLOCK_SRC_COUNT);

    s->pclks[irq] = (unsigned)level;

    if (irq == OT_AON_TIMER_CLOCK_SRC_AON) {
        if (!s->pclks[irq]) {
            timer_del(s->wkup_timer);
            timer_del(s->wdog_timer);
        }
    }

    trace_ot_aon_timer_update_clock(s->ot_id, irq, s->pclks[irq]);
    /* TODO: @loic: update on-going timer */
}

static void ot_aon_timer_lc_escalate(void *opaque, int irq, int level)
{
    OtAonTimerState *s = opaque;

    g_assert(irq == 0);

    bool esc = (bool)level;
    if (esc == s->lc_escalate_en) {
        return;
    }

    ot_aon_timer_sync_wkup(s);
    ot_aon_timer_sync_wdog(s);

    s->lc_escalate_en = esc;
    ot_aon_timer_rearm_wkup(s, false);
    ot_aon_timer_rearm_wdog(s, false);
}

/*
 * Per-register byte-enable write permission mask (AON_TIMER_PERMIT in
 * hw/ip/aon_timer/rtl/aon_timer_reg_pkg.sv). A write raises TL-UL d_error=1
 * (wr_err) if any bit in PERMIT is not covered by the active byte enables
 * (i.e. (PERMIT & ~reg_be) != 0).
 */
static const uint8_t AON_TIMER_PERMIT[REGS_COUNT] = {
    [R_ALERT_TEST] = 0x1u,      [R_WKUP_CTRL] = 0x3u,
    [R_WKUP_THOLD_HI] = 0xfu,   [R_WKUP_THOLD_LO] = 0xfu,
    [R_WKUP_COUNT_HI] = 0xfu,   [R_WKUP_COUNT_LO] = 0xfu,
    [R_WDOG_REGWEN] = 0x1u,     [R_WDOG_CTRL] = 0x1u,
    [R_WDOG_BARK_THOLD] = 0xfu, [R_WDOG_BITE_THOLD] = 0xfu,
    [R_WDOG_COUNT] = 0xfu,      [R_INTR_STATE] = 0x1u,
    [R_INTR_TEST] = 0x1u,       [R_WKUP_CAUSE] = 0x1u,
};

static bool ot_aon_timer_accepts(void *opaque, hwaddr addr, unsigned size,
                                 bool is_write, MemTxAttrs attrs)
{
    (void)opaque;
    (void)attrs;
    if (!is_write) {
        return true;
    }
    hwaddr reg = R32_OFF(addr);
    uint8_t reg_be = (uint8_t)(((1u << size) - 1u) << (addr & 3u));
    return reg < REGS_COUNT && (AON_TIMER_PERMIT[reg] & ~reg_be) == 0u;
}

static uint64_t ot_aon_timer_read(void *opaque, hwaddr addr, unsigned size)
{
    OtAonTimerState *s = opaque;
    (void)size;
    uint32_t val32;

    s->low_power_exit = false;

    ot_aon_timer_sync_wkup(s);
    ot_aon_timer_sync_wdog(s);
    ot_aon_timer_update_irqs(s);

    hwaddr reg = R32_OFF(addr);
    switch (reg) {
    case R_WKUP_CTRL:
    case R_WKUP_THOLD_HI:
    case R_WKUP_THOLD_LO:
    case R_WKUP_COUNT_HI:
    case R_WKUP_COUNT_LO:
    case R_WDOG_REGWEN:
    case R_WDOG_CTRL:
    case R_WDOG_BARK_THOLD:
    case R_WDOG_BITE_THOLD:
    case R_WDOG_COUNT:
    case R_INTR_STATE:
    case R_WKUP_CAUSE:
        val32 = s->regs[reg];
        break;
    case R_ALERT_TEST:
    case R_INTR_TEST:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: W/O register 0x%02x (%s)\n",
                      __func__, (uint32_t)addr, REG_NAME(reg));
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%02x\n", __func__,
                      (uint32_t)addr);
        val32 = 0;
        break;
    }

    uint32_t pc = ibex_get_current_pc();
    trace_ot_aon_timer_io_read_out(s->ot_id, (uint32_t)addr, REG_NAME(reg),
                                   val32, pc);

    return (uint64_t)val32;
}

static void ot_aon_timer_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    OtAonTimerState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)value;

    s->low_power_exit = false;

    ot_aon_timer_sync_wkup(s);
    ot_aon_timer_sync_wdog(s);

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_aon_timer_io_write(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32,
                                pc);

    switch (reg) {
    case R_ALERT_TEST:
        if (val32 & R_ALERT_TEST_FATAL_FAULT_MASK) {
            ibex_irq_set(&s->alert, 1);
            ibex_irq_set(&s->alert, 0);
        }
        break;
    case R_WKUP_CTRL:
        s->regs[R_WKUP_CTRL] =
            val32 & (R_WKUP_CTRL_ENABLE_MASK | R_WKUP_CTRL_PRESCALER_MASK);
        ot_aon_timer_rearm_wkup(s, false);
        break;
    case R_WKUP_THOLD_HI:
    case R_WKUP_THOLD_LO:
        s->regs[reg] = val32;
        ot_aon_timer_rearm_wkup(s, false);
        break;
    case R_WKUP_COUNT_HI:
    case R_WKUP_COUNT_LO:
        s->regs[reg] = val32;
        ot_aon_timer_rearm_wkup(s, true);
        break;
    case R_WDOG_REGWEN:
        s->regs[R_WDOG_REGWEN] &= val32 & R_WDOG_REGWEN_REGWEN_MASK; /* rw0c */
        break;
    case R_WDOG_CTRL:
        if (ot_aon_timer_wdog_register_write_enabled(s)) {
            s->regs[R_WDOG_CTRL] =
                val32 &
                (R_WDOG_CTRL_ENABLE_MASK | R_WDOG_CTRL_PAUSE_IN_SLEEP_MASK);
            ot_aon_timer_rearm_wdog(s, false);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Ignoring write to locked WDOG_CTRL register\n");
        }
        break;
    case R_WDOG_BARK_THOLD:
    case R_WDOG_BITE_THOLD:
        if (ot_aon_timer_wdog_register_write_enabled(s)) {
            s->regs[reg] = val32;
            ot_aon_timer_rearm_wdog(s, false);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Ignoring write to locked %s register\n",
                          REG_NAME(reg));
        }
        break;
    case R_WDOG_COUNT:
        s->regs[R_WDOG_COUNT] = val32;
        ot_aon_timer_rearm_wdog(s, true);
        break;
    case R_INTR_STATE:
        s->regs[R_INTR_STATE] &= ~(val32 & INTR_MASK); /* rw1c */
        ot_aon_timer_rearm_wkup(s, false);
        ot_aon_timer_rearm_wdog(s, false);
        break;
    case R_INTR_TEST:
        s->regs[R_INTR_STATE] |= val32 & INTR_MASK;
        ot_aon_timer_update_irqs(s);
        break;
    case R_WKUP_CAUSE:
        s->regs[R_WKUP_CAUSE] &= val32 & R_WKUP_CAUSE_CAUSE_MASK; /* rw0c */
        ot_aon_timer_rearm_wkup(s, false);
        ot_aon_timer_rearm_wdog(s, false);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%02x\n", __func__,
                      (uint32_t)addr);
    }
}

static const MemoryRegionOps ot_aon_timer_ops = {
    .read = &ot_aon_timer_read,
    .write = &ot_aon_timer_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
    .valid.accepts = &ot_aon_timer_accepts,
};

static const Property ot_aon_timer_properties[] = {
    DEFINE_PROP_STRING(OT_COMMON_DEV_ID, OtAonTimerState, ot_id),
    DEFINE_PROP_STRING("clock-name", OtAonTimerState,
                       clock_names[OT_AON_TIMER_CLOCK_SRC_IO]),
    DEFINE_PROP_STRING("clock-name-aon", OtAonTimerState,
                       clock_names[OT_AON_TIMER_CLOCK_SRC_AON]),
    DEFINE_PROP_LINK("clock-src", OtAonTimerState, clock_src, TYPE_DEVICE,
                     DeviceState *),
};

void ot_aon_timer_set_sleep_mode(OtAonTimerState *s, bool sleep_mode)
{
    if (!s || sleep_mode == s->sleep_mode) {
        return;
    }

    ot_aon_timer_sync_wkup(s);
    ot_aon_timer_sync_wdog(s);

    if (s->sleep_mode && !sleep_mode) {
        s->low_power_exit = true;
    }
    s->sleep_mode = sleep_mode;
    ot_aon_timer_rearm_wkup(s, false);
    ot_aon_timer_rearm_wdog(s, false);
}

static void ot_aon_timer_reset_enter(Object *obj, ResetType type)
{
    OtAonTimerClass *c = OT_AON_TIMER_GET_CLASS(obj);
    OtAonTimerState *s = OT_AON_TIMER(obj);

    trace_ot_aon_timer_reset(s->ot_id, "enter");

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    timer_del(s->wkup_timer);
    timer_del(s->wdog_timer);

    if (type != RESET_TYPE_COLD && (s->sleep_mode || s->low_power_exit) &&
        !s->wdog_bite) {
        ot_aon_timer_sync_wkup(s);
        ot_aon_timer_sync_wdog(s);
        s->sleep_mode = false;
        s->low_power_exit = false;
        ot_aon_timer_rearm_wkup(s, false);
        ot_aon_timer_rearm_wdog(s, false);
    } else {
        memset(s->regs, 0, sizeof(s->regs));
        s->regs[R_WDOG_REGWEN] = 1u;
        s->prescale_count = 0u;
        s->wkup_origin_ns = 0;
        s->wdog_origin_ns = 0;
        s->wkup_intr_level = false;
        s->wdog_bark_level = false;
        s->wdog_bite = false;
        s->lc_escalate_en = false;
        s->sleep_mode = false;
        s->low_power_exit = false;
    }

    ot_aon_timer_update_irqs(s);
    ot_aon_timer_update_alert(s);

    for (unsigned ix = 0; ix < OT_AON_TIMER_CLOCK_SRC_COUNT; ix++) {
        if (s->clock_src_names[ix]) {
            continue;
        }
        IbexClockSrcIfClass *ic = IBEX_CLOCK_SRC_IF_GET_CLASS(s->clock_src);
        IbexClockSrcIf *ii = IBEX_CLOCK_SRC_IF(s->clock_src);

        s->clock_src_names[ix] = ic->get_clock_source(ii, s->clock_names[ix],
                                                      DEVICE(s), &error_fatal);
        qemu_irq in_irq =
            qdev_get_gpio_in_named(DEVICE(s), "clock-in", (int)ix);
        qdev_connect_gpio_out_named(s->clock_src, s->clock_src_names[ix], 0,
                                    in_irq);
    }
}

static void ot_aon_timer_realize(DeviceState *dev, Error **errp)
{
    OtAonTimerState *s = OT_AON_TIMER(dev);

    (void)errp;

    g_assert(s->ot_id);
    for (unsigned ix = 0; ix < OT_AON_TIMER_CLOCK_SRC_COUNT; ix++) {
        g_assert(s->clock_names[ix]);
    }
    g_assert(s->clock_src);
    OBJECT_CHECK(IbexClockSrcIf, s->clock_src, TYPE_IBEX_CLOCK_SRC_IF);

    qdev_init_gpio_in_named(DEVICE(s), &ot_aon_timer_clock_input, "clock-in",
                            OT_AON_TIMER_CLOCK_SRC_COUNT);
}

static void ot_aon_timer_init(Object *obj)
{
    OtAonTimerState *s = OT_AON_TIMER(obj);

    ibex_sysbus_init_irq(obj, &s->irq_wkup);
    ibex_sysbus_init_irq(obj, &s->irq_bark);
    ibex_qdev_init_irq(obj, &s->nmi_bark, OT_AON_TIMER_BARK);
    ibex_qdev_init_irq(obj, &s->pwrmgr_wkup, OT_AON_TIMER_WKUP);
    ibex_qdev_init_irq(obj, &s->pwrmgr_bite, OT_AON_TIMER_BITE);
    ibex_qdev_init_irq(obj, &s->alert, OT_DEVICE_ALERT);

    qdev_init_gpio_in_named(DEVICE(obj), &ot_aon_timer_lc_escalate,
                            OT_AON_TIMER_LC_ESCALATE, 1);

    memory_region_init_io(&s->mmio, obj, &ot_aon_timer_ops, s,
                          TYPE_OT_AON_TIMER, REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);

    s->wkup_timer = timer_new_ns(OT_VIRTUAL_CLOCK, &ot_aon_timer_wkup_cb, s);
    s->wdog_timer = timer_new_ns(OT_VIRTUAL_CLOCK, &ot_aon_timer_wdog_cb, s);
}

static void ot_aon_timer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->realize = ot_aon_timer_realize;
    device_class_set_props(dc, ot_aon_timer_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    ResettableClass *rc = RESETTABLE_CLASS(klass);
    OtAonTimerClass *ac = OT_AON_TIMER_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_aon_timer_reset_enter, NULL,
                                       NULL, &ac->parent_phases);
}

static const TypeInfo ot_aon_timer_info = {
    .name = TYPE_OT_AON_TIMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtAonTimerState),
    .instance_init = ot_aon_timer_init,
    .class_size = sizeof(OtAonTimerClass),
    .class_init = ot_aon_timer_class_init,
};

static void ot_aon_timer_register_types(void)
{
    type_register_static(&ot_aon_timer_info);
}

type_init(ot_aon_timer_register_types);
