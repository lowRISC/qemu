/*
 * QEMU lowRISC Ibex Demo Timer device
 *
 * Copyright (c) 2022-2023 Rivos, Inc.
 * based on ibex_timer.c from Western Digital Copyright (c) 2021
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
#include "hw/ibexdemo/ibexdemo_timer.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"


REG32(MTIME, 0x00u)
REG32(MTIMEH, 0x04u)
REG32(MTIMECMP, 0x08u)
REG32(MTIMECMPH, 0x0cu)

struct IbexDemoTimerState {
    SysBusDevice parent_obj;
    QEMUTimer *timer;

    MemoryRegion mmio;

    uint64_t mtimecmp;
    uint64_t timer_compare;
    uint32_t timebase_freq;

    IbexIRQ irq;
};

static uint64_t ibexdemo_timer_read_rtc(IbexDemoTimerState *s)
{
    return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), s->timebase_freq,
                    NANOSECONDS_PER_SECOND);
}

static void ibexdemo_timer_update(IbexDemoTimerState *s)
{
    uint64_t now = ibexdemo_timer_read_rtc(s);

    if (now >= s->mtimecmp) {
        qemu_log_mask(CPU_LOG_EXEC,
                      "[Timer IRQ] %08" PRIx64 " >= %08" PRIx64 "\n", now,
                      s->mtimecmp);
        ibex_irq_set(&s->irq, true);
        return;
    }

    ibex_irq_set(&s->irq, false);

    uint64_t next, diff;

    diff = s->mtimecmp - now;
    next = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
           muldiv64(diff, NANOSECONDS_PER_SECOND, s->timebase_freq);

    if (next < qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) {
        timer_mod(s->timer, INT64_MAX);
    } else {
        timer_mod(s->timer, next);
    }
}

static void ibexdemo_timer_cb(void *opaque)
{
    IbexDemoTimerState *s = opaque;

    ibex_irq_set(&s->irq, true);
}

static void ibexdemo_timer_reset(DeviceState *dev)
{
    IbexDemoTimerState *s = IBEXDEMO_TIMER(dev);

    timer_del(s->timer);
    s->mtimecmp = 0u;
    ibex_irq_set(&s->irq, false);

    ibexdemo_timer_update(s);
}

static uint64_t ibexdemo_timer_read(void *opaque, hwaddr addr, unsigned size)
{
    IbexDemoTimerState *s = opaque;
    uint32_t val32;

    switch (addr >> 2u) {
    case R_MTIME:
        val32 = (uint32_t)ibexdemo_timer_read_rtc(s);
        break;
    case R_MTIMEH:
        val32 = (uint32_t)(ibexdemo_timer_read_rtc(s) >> 32u);
        break;
    case R_MTIMECMP:
        val32 = (uint32_t)s->mtimecmp;
        break;
    case R_MTIMECMPH:
        val32 = (uint32_t)(s->mtimecmp >> 32u);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        val32 = 0;
        break;
    }

    return (uint64_t)val32;
}

static void ibexdemo_timer_write(void *opaque, hwaddr addr, uint64_t val64,
                                 unsigned size)
{
    IbexDemoTimerState *s = opaque;
    uint64_t tmp;

    switch (addr >> 2) {
    case R_MTIME:
    case R_MTIMEH:
        /* todo: how to support write to mtime? */
        qemu_log_mask(LOG_UNIMP, "Changing timer value is not supported\n");
        break;
    case R_MTIMECMP:
        tmp = s->mtimecmp >> 32u;
        tmp <<= 32u;
        s->mtimecmp = tmp | (uint32_t)val64;
        ibexdemo_timer_update(s);
        break;
    case R_MTIMECMPH:
        tmp = s->mtimecmp << 32u;
        tmp >>= 32u;
        val64 <<= 32u;
        s->mtimecmp = val64 | tmp;
        ibexdemo_timer_update(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static const MemoryRegionOps ibexdemo_timer_ops = {
    .read = ibexdemo_timer_read,
    .write = ibexdemo_timer_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static Property ibexdemo_timer_properties[] = {
    DEFINE_PROP_UINT32("timebase-freq", IbexDemoTimerState, timebase_freq,
                       50000000),
    DEFINE_PROP_END_OF_LIST(),
};

static void ibexdemo_timer_init(Object *obj)
{
    IbexDemoTimerState *s = IBEXDEMO_TIMER(obj);

    memory_region_init_io(&s->mmio, obj, &ibexdemo_timer_ops, s,
                          TYPE_IBEXDEMO_TIMER, 0x1000u);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);

    ibex_sysbus_init_irq(obj, &s->irq);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, &ibexdemo_timer_cb, s);

    /*
     * todo: need to check whether mtime CSR is supported. If so, see
     * void riscv_cpu_set_rdtime_fn();
     */
}

static void ibexdemo_timer_realize(DeviceState *dev, Error **errp)
{
    IbexDemoTimerState *s = IBEXDEMO_TIMER(dev);

    qdev_init_gpio_out(dev, &s->irq.irq, 1);
}

static void ibexdemo_timer_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = ibexdemo_timer_reset;
    dc->realize = ibexdemo_timer_realize;
    device_class_set_props(dc, ibexdemo_timer_properties);
}

static const TypeInfo ibexdemo_timer_info = {
    .name = TYPE_IBEXDEMO_TIMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IbexDemoTimerState),
    .instance_init = ibexdemo_timer_init,
    .class_init = ibexdemo_timer_class_init,
};

static void ibexdemo_timer_register_types(void)
{
    type_register_static(&ibexdemo_timer_info);
}

type_init(ibexdemo_timer_register_types);
