/*
 * QEMU OpenTitan AON Timer device
 *
 * Copyright (c) 2023 Rivos, Inc.
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
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_aon_timer.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "trace.h"

/* clang-format off */
REG32(ALERT_TEST, 0x00u)
    FIELD(ALERT_TEST, FATAL_FAULT, 0u, 1u)
REG32(WKUP_CTRL, 0x04u)
    FIELD(WKUP_CTRL, ENABLE, 0u, 1u)
    FIELD(WKUP_CTRL, PRESCALER, 1u, 12u)
REG32(WKUP_THOLD, 0x08)
REG32(WKUP_COUNT, 0x0cu)
REG32(WDOG_REGWEN, 0x10u)
    FIELD(WDOG_REGWEN, REGWEN, 0u, 1u)
REG32(WDOG_CTRL, 0x14u)
    FIELD(WDOG_CTRL, ENABLE, 0u, 1u)
    FIELD(WDOG_CTRL, PAUSE_IN_SLEEP, 0u, 1u)
REG32(WDOG_BARK_THOLD, 0x18u)
REG32(WDOG_BITE_THOLD, 0x1cu)
REG32(WDOG_COUNT, 0x20u)
REG32(INTR_STATE, 0x24u)
    SHARED_FIELD(INTR_WKUP_TIMER_EXPIRED, 0u, 1u)
    SHARED_FIELD(INTR_WDOG_TIMER_BARK, 1u, 1u)
REG32(INTR_TEST, 0x28u)
REG32(WKUP_CAUSE, 0x2cu)
    FIELD(WKUP_CAUSE, CAUSE, 0u, 1u)
/* clang-format on */

#define INTR_MASK (INTR_WKUP_TIMER_EXPIRED_MASK | INTR_WDOG_TIMER_BARK_MASK)

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_WKUP_CAUSE)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char REG_NAMES[REGS_COUNT][20u] = {
    /* clang-format off */
    REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(WKUP_CTRL),
    REG_NAME_ENTRY(WKUP_THOLD),
    REG_NAME_ENTRY(WKUP_COUNT),
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

struct OtAonTimerState {
    SysBusDevice parent_obj;
    QEMUTimer *wkup_timer;
    QEMUTimer *wdog_timer;

    IbexIRQ irq_wkup;
    IbexIRQ irq_bark;
    IbexIRQ nmi_bark;
    IbexIRQ pwrmgr_wkup;
    IbexIRQ pwrmgr_bite;
    IbexIRQ alert;

    MemoryRegion mmio;

    uint32_t regs[REGS_COUNT];
    uint32_t pclk;

    int64_t wkup_origin_ns;
    int64_t wdog_origin_ns;
    bool wdog_bite;
};

static uint32_t
ot_aon_timer_ns_to_ticks(OtAonTimerState *s, uint32_t prescaler, int64_t ns)
{
    uint64_t ticks = muldiv64((uint64_t)ns, s->pclk, NANOSECONDS_PER_SECOND);
    return (uint32_t)(ticks / (prescaler + 1u));
}

static int64_t
ot_aon_timer_ticks_to_ns(OtAonTimerState *s, uint32_t prescaler, uint32_t ticks)
{
    uint64_t ns = muldiv64((uint64_t)ticks * (prescaler + 1u),
                           NANOSECONDS_PER_SECOND, s->pclk);
    if (ns > INT64_MAX) {
        return INT64_MAX;
    }
    return ns;
}

static uint32_t ot_aon_timer_get_wkup_count(OtAonTimerState *s, uint64_t now)
{
    uint32_t prescaler = FIELD_EX32(s->regs[R_WKUP_CTRL], WKUP_CTRL, PRESCALER);
    return s->regs[R_WKUP_COUNT] +
           ot_aon_timer_ns_to_ticks(s, prescaler, now - s->wkup_origin_ns);
}

static uint32_t ot_aon_timer_get_wdog_count(OtAonTimerState *s, uint64_t now)
{
    return s->regs[R_WDOG_COUNT] +
           ot_aon_timer_ns_to_ticks(s, 0u, now - s->wdog_origin_ns);
}

static int64_t ot_aon_timer_compute_next_timeout(OtAonTimerState *s,
                                                 int64_t now, int64_t delta)
{
    int64_t next;

    /* wait at least 1 peripheral clock tick */
    delta = MAX(delta, (int64_t)(NANOSECONDS_PER_SECOND / s->pclk));

    if (sadd64_overflow(now, delta, &next)) {
        /* we overflowed the timer, just set it as large as we can */
        return INT64_MAX;
    }

    return next;
}

static inline bool ot_aon_timer_is_wkup_enabled(OtAonTimerState *s)
{
    return (s->regs[R_WKUP_CTRL] & R_WKUP_CTRL_ENABLE_MASK) != 0;
}

static inline bool ot_aon_timer_is_wdog_enabled(OtAonTimerState *s)
{
    return (s->regs[R_WDOG_CTRL] & R_WDOG_CTRL_ENABLE_MASK) != 0;
}

static inline bool ot_aon_timer_wdog_register_write_enabled(OtAonTimerState *s)
{
    return (s->regs[R_WDOG_REGWEN] & R_WDOG_REGWEN_REGWEN_MASK) != 0;
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

    trace_ot_aon_timer_irqs(wkup, bark, s->wdog_bite);

    ibex_irq_set(&s->irq_wkup, wkup);
    ibex_irq_set(&s->irq_bark, bark);
    ibex_irq_set(&s->nmi_bark, bark);
    ibex_irq_set(&s->pwrmgr_wkup, wkup);
    ibex_irq_set(&s->pwrmgr_bite, s->wdog_bite);
}

static void ot_aon_timer_rearm_wkup(OtAonTimerState *s, bool reset_origin)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT);

    if (reset_origin) {
        s->wkup_origin_ns = now;
    }

    /* if not enabled, ignore threshold */
    if (!ot_aon_timer_is_wkup_enabled(s)) {
        timer_del(s->wkup_timer);
        ot_aon_timer_update_irqs(s);
        return;
    }

    uint32_t count = ot_aon_timer_get_wkup_count(s, now);
    uint32_t threshold = s->regs[R_WKUP_THOLD];

    if (count >= threshold) {
        s->regs[R_INTR_STATE] |= INTR_WKUP_TIMER_EXPIRED_MASK;
        timer_del(s->wkup_timer);
    } else {
        uint32_t prescaler =
            FIELD_EX32(s->regs[R_WKUP_CTRL], WKUP_CTRL, PRESCALER);
        int64_t delta =
            ot_aon_timer_ticks_to_ns(s, prescaler, threshold - count);
        int64_t next = ot_aon_timer_compute_next_timeout(s, now, delta);
        timer_mod(s->wkup_timer, next);
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
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT);

    if (reset_origin) {
        s->wdog_origin_ns = now;
    }

    /* if not enabled, ignore threshold */
    if (!ot_aon_timer_is_wdog_enabled(s)) {
        timer_del(s->wdog_timer);
        ot_aon_timer_update_irqs(s);
        return;
    }

    uint32_t count = ot_aon_timer_get_wdog_count(s, now);
    uint32_t bark_threshold = s->regs[R_WDOG_BARK_THOLD];
    uint32_t bite_threshold = s->regs[R_WDOG_BITE_THOLD];
    uint32_t threshold = 0;

    if (count >= bark_threshold) {
        s->regs[R_INTR_STATE] |= INTR_WDOG_TIMER_BARK_MASK;
    } else {
        threshold = bark_threshold;
    }

    if (count >= bite_threshold) {
        s->wdog_bite = true;
    } else if (bite_threshold < threshold) {
        threshold = bite_threshold;
    }

    if (count >= threshold) {
        timer_del(s->wdog_timer);
    } else {
        int64_t delta = ot_aon_timer_ticks_to_ns(s, 0u, threshold - count);
        int64_t next = ot_aon_timer_compute_next_timeout(s, now, delta);
        timer_mod(s->wdog_timer, next);
    }

    ot_aon_timer_update_irqs(s);
}

static void ot_aon_timer_wdog_cb(void *opaque)
{
    OtAonTimerState *s = opaque;
    ot_aon_timer_rearm_wdog(s, false);
}

static uint64_t ot_aon_timer_read(void *opaque, hwaddr addr, unsigned size)
{
    OtAonTimerState *s = opaque;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);
    switch (reg) {
    case R_WKUP_CTRL:
    case R_WKUP_THOLD:
    case R_WDOG_REGWEN:
    case R_WDOG_CTRL:
    case R_WDOG_BARK_THOLD:
    case R_WDOG_BITE_THOLD:
    case R_INTR_STATE:
    case R_WKUP_CAUSE:
        val32 = s->regs[reg];
        break;
    case R_WKUP_COUNT: {
        uint64_t now = ot_aon_timer_is_wkup_enabled(s) ?
                           qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT) :
                           s->wkup_origin_ns;
        val32 = ot_aon_timer_get_wkup_count(s, now);
        break;
    }
    case R_WDOG_COUNT: {
        uint64_t now = ot_aon_timer_is_wdog_enabled(s) ?
                           qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT) :
                           s->wdog_origin_ns;
        val32 = ot_aon_timer_get_wdog_count(s, now);
        break;
    }
    case R_ALERT_TEST:
    case R_INTR_TEST:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "W/O register 0x02%" HWADDR_PRIx " (%s)\n", addr,
                      REG_NAME(reg));
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        val32 = 0;
        break;
    }

    uint64_t pc = ibex_get_current_pc();
    trace_ot_aon_timer_read_out((unsigned)addr, REG_NAME(reg), val32, pc);

    return (uint64_t)val32;
}

static void ot_aon_timer_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    OtAonTimerState *s = opaque;
    uint32_t val32 = (uint32_t)value;

    hwaddr reg = R32_OFF(addr);

    uint64_t pc = ibex_get_current_pc();
    trace_ot_aon_timer_write((unsigned)addr, REG_NAME(reg), val32, pc);

    switch (reg) {
    case R_ALERT_TEST:
        s->regs[R_ALERT_TEST] |= val32 & R_ALERT_TEST_FATAL_FAULT_MASK;
        ot_aon_timer_update_alert(s);
        break;
    case R_WKUP_CTRL: {
        uint32_t prev = s->regs[R_WKUP_CTRL];
        s->regs[R_WKUP_CTRL] =
            val32 & (R_WKUP_CTRL_ENABLE_MASK | R_WKUP_CTRL_PRESCALER_MASK);
        uint32_t change = prev ^ s->regs[R_WKUP_CTRL];
        if (change & R_WKUP_CTRL_ENABLE_MASK) {
            if (ot_aon_timer_is_wkup_enabled(s)) {
                /* start timer */
                ot_aon_timer_rearm_wkup(s, true);
            } else {
                /* stop timer */
                timer_del(s->wkup_timer);
                /* save current count */
                uint32_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT);
                s->regs[R_WKUP_COUNT] = ot_aon_timer_get_wkup_count(s, now);
                s->wkup_origin_ns = now;
            }
        }
        break;
    }
    case R_WKUP_THOLD:
        s->regs[R_WKUP_THOLD] = val32;
        ot_aon_timer_rearm_wkup(s, false);
        break;
    case R_WKUP_COUNT:
        s->regs[R_WKUP_COUNT] = val32;
        ot_aon_timer_rearm_wkup(s, true);
        break;
    case R_WDOG_REGWEN:
        s->regs[R_WDOG_REGWEN] &= val32 & R_WDOG_REGWEN_REGWEN_MASK; /* rw0c */
        break;
    case R_WDOG_CTRL:
        if (ot_aon_timer_wdog_register_write_enabled(s)) {
            uint32_t prev = s->regs[R_WDOG_CTRL];
            s->regs[R_WDOG_CTRL] =
                val32 &
                (R_WDOG_CTRL_ENABLE_MASK | R_WDOG_CTRL_PAUSE_IN_SLEEP_MASK);
            uint32_t change = prev ^ s->regs[R_WDOG_CTRL];
            if (change & R_WDOG_CTRL_ENABLE_MASK) {
                if (ot_aon_timer_is_wdog_enabled(s)) {
                    /* start timer */
                    ot_aon_timer_rearm_wdog(s, true);
                } else {
                    /* stop timer */
                    timer_del(s->wdog_timer);
                    /* save current count */
                    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT);
                    s->regs[R_WDOG_COUNT] = ot_aon_timer_get_wdog_count(s, now);
                    s->wdog_origin_ns = now;
                }
            }
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
    case R_INTR_STATE: {
        uint32_t prev = s->regs[R_INTR_STATE];
        s->regs[R_INTR_STATE] &= ~(val32 & INTR_MASK); /* rw1c */
        uint32_t change = prev ^ s->regs[R_INTR_STATE];
        ot_aon_timer_update_irqs(s);
        /*
         * schedule the timer for the next peripheral clock tick to check again
         * for interrupt condition
         */
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT);
        int64_t next = ot_aon_timer_compute_next_timeout(s, now, 0);
        if (change & INTR_WKUP_TIMER_EXPIRED_MASK) {
            timer_mod_anticipate(s->wkup_timer, next);
        }
        if (change & INTR_WDOG_TIMER_BARK_MASK) {
            timer_mod_anticipate(s->wdog_timer, next);
        }
        break;
    }
    case R_INTR_TEST:
        s->regs[R_INTR_STATE] |= val32 & INTR_MASK;
        ot_aon_timer_update_irqs(s);
        break;
    case R_WKUP_CAUSE:
        /* ignore write, in QEMU wkup_cause is always 0 */
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
    }
}

static const MemoryRegionOps ot_aon_timer_ops = {
    .read = &ot_aon_timer_read,
    .write = &ot_aon_timer_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static Property ot_aon_timer_properties[] = {
    DEFINE_PROP_UINT32("pclk", OtAonTimerState, pclk, 0u),
    DEFINE_PROP_END_OF_LIST(),
};

static void ot_aon_timer_reset(DeviceState *dev)
{
    OtAonTimerState *s = OT_AON_TIMER(dev);

    g_assert(s->pclk > 0);

    timer_del(s->wkup_timer);
    timer_del(s->wdog_timer);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[R_WDOG_REGWEN] = 1u;
    s->wdog_bite = false;

    ot_aon_timer_update_irqs(s);
    ot_aon_timer_update_alert(s);
}

static void ot_aon_timer_init(Object *obj)
{
    OtAonTimerState *s = OT_AON_TIMER(obj);

    ibex_sysbus_init_irq(obj, &s->irq_wkup);
    ibex_sysbus_init_irq(obj, &s->irq_bark);
    ibex_qdev_init_irq(obj, &s->nmi_bark, OPENTITAN_AON_TIMER_BARK);
    ibex_qdev_init_irq(obj, &s->pwrmgr_wkup, OPENTITAN_AON_TIMER_WKUP);
    ibex_qdev_init_irq(obj, &s->pwrmgr_bite, OPENTITAN_AON_TIMER_BITE);
    ibex_qdev_init_irq(obj, &s->alert, OPENTITAN_DEVICE_ALERT);

    memory_region_init_io(&s->mmio, obj, &ot_aon_timer_ops, s,
                          TYPE_OT_AON_TIMER, REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);

    s->wkup_timer =
        timer_new_ns(QEMU_CLOCK_VIRTUAL_RT, &ot_aon_timer_wkup_cb, s);
    s->wdog_timer =
        timer_new_ns(QEMU_CLOCK_VIRTUAL_RT, &ot_aon_timer_wdog_cb, s);
}

static void ot_aon_timer_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = ot_aon_timer_reset;
    device_class_set_props(dc, ot_aon_timer_properties);
}

static const TypeInfo ot_aon_timer_info = {
    .name = TYPE_OT_AON_TIMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtAonTimerState),
    .instance_init = ot_aon_timer_init,
    .class_init = ot_aon_timer_class_init,
};

static void ot_aon_timer_register_types(void)
{
    type_register_static(&ot_aon_timer_info);
}

type_init(ot_aon_timer_register_types)
