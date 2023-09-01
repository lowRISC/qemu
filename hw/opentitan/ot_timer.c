/*
 * QEMU OpenTitan Timer device
 *
 * Copyright (c) 2022-2023 Rivos, Inc.
 *
 * Author(s):
 *  Loïc Lefort <loic@rivosinc.com>
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
#include "hw/opentitan/ot_timer.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "trace.h"

/* clang-format off */
REG32(ALERT_TEST, 0x00u)
    FIELD(ALERT_TEST, FATAL_FAULT, 0u, 1u)
REG32(CTRL, 0x04u)
    FIELD(CTRL, ACTIVE0, 0u, 1u)
REG32(INTR_ENABLE0, 0x100u)
    SHARED_FIELD(INTR_CMP0, 0u, 1u)
REG32(INTR_STATE0, 0x104u)
REG32(INTR_TEST0, 0x108u)
REG32(CFG0, 0x10cu)
    FIELD(CFG0, PRESCALE, 0u, 12u)
    FIELD(CFG0, STEP, 16u, 8u)
REG32(TIMER_V_LOWER0, 0x110u)
REG32(TIMER_V_UPPER0, 0x114u)
REG32(COMPARE_LOWER0_0, 0x118u)
REG32(COMPARE_UPPER0_0, 0x11cu)
/* clang-format on */

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_COMPARE_UPPER0_0)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    /* clang-format off */
    REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(CTRL),
    REG_NAME_ENTRY(INTR_ENABLE0),
    REG_NAME_ENTRY(INTR_STATE0),
    REG_NAME_ENTRY(INTR_TEST0),
    REG_NAME_ENTRY(CFG0),
    REG_NAME_ENTRY(TIMER_V_LOWER0),
    REG_NAME_ENTRY(TIMER_V_UPPER0),
    REG_NAME_ENTRY(COMPARE_LOWER0_0),
    REG_NAME_ENTRY(COMPARE_UPPER0_0),
    /* clang-format on */
};
#undef REG_NAME_ENTRY

struct OtTimerState {
    SysBusDevice parent_obj;
    QEMUTimer *timer;

    MemoryRegion mmio;

    uint32_t regs[REGS_COUNT];
    uint32_t pclk;

    int64_t origin_ns;

    IbexIRQ m_timer_irq;
    IbexIRQ irq;
    IbexIRQ alert;
};

static uint64_t ot_timer_ns_to_ticks(OtTimerState *s, int64_t ns)
{
    uint32_t prescaler = FIELD_EX32(s->regs[R_CFG0], CFG0, PRESCALE);
    uint64_t ticks = muldiv64((uint64_t)ns, s->pclk, NANOSECONDS_PER_SECOND);
    uint64_t step = FIELD_EX32(s->regs[R_CFG0], CFG0, STEP);
    return (ticks / (prescaler + 1u)) * step;
}

static int64_t ot_timer_ticks_to_ns(OtTimerState *s, uint64_t ticks)
{
    uint32_t prescaler = FIELD_EX32(s->regs[R_CFG0], CFG0, PRESCALE);
    uint32_t step = FIELD_EX32(s->regs[R_CFG0], CFG0, STEP);
    uint64_t ns = muldiv64(ticks, (prescaler + 1u), step);
    ns = muldiv64(ns, NANOSECONDS_PER_SECOND, s->pclk);
    if (ns > INT64_MAX) {
        return INT64_MAX;
    }
    return ns;
}

static uint64_t ot_timer_get_mtime(OtTimerState *s, int64_t now)
{
    uint64_t mtime = s->regs[R_TIMER_V_LOWER0] |
                     ((uint64_t)s->regs[R_TIMER_V_UPPER0] << 32u);
    return mtime + ot_timer_ns_to_ticks(s, now - s->origin_ns);
}

static int64_t
ot_timer_compute_next_timeout(OtTimerState *s, int64_t now, int64_t delta)
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

static inline bool ot_timer_is_active(OtTimerState *s)
{
    return s->regs[R_CTRL] & R_CTRL_ACTIVE0_MASK;
}

static void ot_timer_update_alert(OtTimerState *s)
{
    bool level = s->regs[R_ALERT_TEST] & R_ALERT_TEST_FATAL_FAULT_MASK;
    ibex_irq_set(&s->irq, level);
}

static void ot_timer_update_irqs(OtTimerState *s)
{
    bool level =
        s->regs[R_INTR_STATE0] & s->regs[R_INTR_ENABLE0] & INTR_CMP0_MASK;
    ibex_irq_set(&s->m_timer_irq, level);
    ibex_irq_set(&s->irq, level);
}

static void ot_timer_rearm(OtTimerState *s, bool reset_origin)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT);

    if (reset_origin) {
        s->origin_ns = now;
    }

    if (!ot_timer_is_active(s)) {
        timer_del(s->timer);
        return;
    }

    uint64_t mtime = ot_timer_get_mtime(s, now);
    uint64_t mtimecmp = s->regs[R_COMPARE_LOWER0_0] |
                        ((uint64_t)s->regs[R_COMPARE_UPPER0_0] << 32u);

    if (mtime >= mtimecmp) {
        s->regs[R_INTR_STATE0] |= INTR_CMP0_MASK;
        timer_del(s->timer);
    } else {
        int64_t delta = ot_timer_ticks_to_ns(s, mtimecmp - mtime);
        int64_t next = ot_timer_compute_next_timeout(s, now, delta);
        timer_mod(s->timer, next);
    }

    ot_timer_update_irqs(s);
}

static void ot_timer_cb(void *opaque)
{
    OtTimerState *s = opaque;
    ot_timer_rearm(s, false);
}

static uint64_t ot_timer_read(void *opaque, hwaddr addr, unsigned size)
{
    OtTimerState *s = opaque;
    uint32_t val32 = 0;

    hwaddr reg = R32_OFF(addr);
    switch (reg) {
    case R_CTRL:
    case R_INTR_ENABLE0:
    case R_INTR_STATE0:
    case R_CFG0:
    case R_COMPARE_LOWER0_0:
    case R_COMPARE_UPPER0_0:
        val32 = s->regs[reg];
        break;
    case R_TIMER_V_LOWER0: {
        int64_t now = ot_timer_is_active(s) ?
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT) :
                          s->origin_ns;
        val32 = ot_timer_get_mtime(s, now);
        break;
    }
    case R_TIMER_V_UPPER0: {
        int64_t now = ot_timer_is_active(s) ?
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT) :
                          s->origin_ns;
        val32 = ot_timer_get_mtime(s, now) >> 32u;
        break;
    }
    case R_ALERT_TEST:
    case R_INTR_TEST0:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "W/O register 0x%02" HWADDR_PRIx " (%s)\n", addr,
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
    trace_ot_timer_read_out((unsigned)addr, REG_NAME(reg), val32, pc);

    return (uint32_t)val32;
}

static void ot_timer_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned size)
{
    OtTimerState *s = opaque;
    uint32_t val32 = (uint32_t)value;

    hwaddr reg = R32_OFF(addr);

    uint64_t pc = ibex_get_current_pc();
    trace_ot_timer_write((unsigned)addr, REG_NAME(reg), val32, pc);

    switch (reg) {
    case R_ALERT_TEST:
        s->regs[R_ALERT_TEST] |= val32 & R_ALERT_TEST_FATAL_FAULT_MASK;
        ot_timer_update_alert(s);
        break;
    case R_CTRL: {
        uint32_t prev = s->regs[R_CTRL];
        s->regs[R_CTRL] = val32 & R_CTRL_ACTIVE0_MASK;
        uint32_t change = prev ^ s->regs[R_CTRL];
        if (change & R_CTRL_ACTIVE0_MASK) {
            if (ot_timer_is_active(s)) {
                /* start timer */
                ot_timer_rearm(s, true);
            } else {
                /* stop timer */
                timer_del(s->timer);
                /* save current mtime */
                int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT);
                uint64_t mtime = ot_timer_get_mtime(s, now);
                s->regs[R_TIMER_V_LOWER0] = (uint32_t)mtime;
                s->regs[R_TIMER_V_UPPER0] = (uint32_t)(mtime >> 32u);
                s->origin_ns = now;
            }
        }
        break;
    }
    case R_INTR_ENABLE0:
        s->regs[R_INTR_ENABLE0] = val32 & INTR_CMP0_MASK;
        ot_timer_update_irqs(s);
        break;
    case R_INTR_STATE0: {
        s->regs[R_INTR_STATE0] &= ~(val32 & INTR_CMP0_MASK);
        ot_timer_update_irqs(s);
        /*
         * schedule the timer for the next peripheral clock tick to check again
         * for interrupt condition
         */
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT);
        int64_t next = ot_timer_compute_next_timeout(s, now, 0);
        timer_mod_anticipate(s->timer, next);
        break;
    }
    case R_INTR_TEST0:
        s->regs[R_INTR_STATE0] |= val32 & INTR_CMP0_MASK;
        ot_timer_update_irqs(s);
        break;
    case R_CFG0:
        if (!ot_timer_is_active(s)) {
            s->regs[R_CFG0] = val32 & (R_CFG0_PRESCALE_MASK | R_CFG0_STEP_MASK);
        }
        break;
    case R_TIMER_V_LOWER0:
        s->regs[R_TIMER_V_LOWER0] = val32;
        ot_timer_rearm(s, true);
        break;
    case R_TIMER_V_UPPER0:
        s->regs[R_TIMER_V_UPPER0] = val32;
        ot_timer_rearm(s, true);
        break;
    case R_COMPARE_LOWER0_0:
        s->regs[R_COMPARE_LOWER0_0] = val32;
        /* clear IRQ on compare change */
        s->regs[R_INTR_STATE0] &= ~INTR_CMP0_MASK;
        ot_timer_rearm(s, false);
        break;
    case R_COMPARE_UPPER0_0:
        s->regs[R_COMPARE_UPPER0_0] = val32;
        /* clear IRQ on compare change */
        s->regs[R_INTR_STATE0] &= ~INTR_CMP0_MASK;
        ot_timer_rearm(s, false);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
    }
}

static const MemoryRegionOps ot_timer_ops = {
    .read = &ot_timer_read,
    .write = &ot_timer_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static Property ot_timer_properties[] = {
    DEFINE_PROP_UINT32("pclk", OtTimerState, pclk, 0u),
    DEFINE_PROP_END_OF_LIST(),
};

static void ot_timer_reset(DeviceState *dev)
{
    OtTimerState *s = OT_TIMER(dev);

    g_assert(s->pclk > 0);

    timer_del(s->timer);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[R_CFG0] = 1u << R_CFG0_STEP_SHIFT;
    s->regs[R_COMPARE_LOWER0_0] = UINT32_MAX;
    s->regs[R_COMPARE_UPPER0_0] = UINT32_MAX;

    ot_timer_update_irqs(s);
}

static void ot_timer_init(Object *obj)
{
    OtTimerState *s = OT_TIMER(obj);

    ibex_sysbus_init_irq(obj, &s->irq);
    ibex_qdev_init_irq(obj, &s->m_timer_irq, NULL);
    ibex_qdev_init_irq(obj, &s->alert, OPENTITAN_DEVICE_ALERT);

    memory_region_init_io(&s->mmio, obj, &ot_timer_ops, s, TYPE_OT_TIMER,
                          REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL_RT, &ot_timer_cb, s);
}

static void ot_timer_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = &ot_timer_reset;
    device_class_set_props(dc, ot_timer_properties);
}

static const TypeInfo ot_timer_info = {
    .name = TYPE_OT_TIMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtTimerState),
    .instance_init = &ot_timer_init,
    .class_init = &ot_timer_class_init,
};

static void ot_timer_register_types(void)
{
    type_register_static(&ot_timer_info);
}

type_init(ot_timer_register_types)
