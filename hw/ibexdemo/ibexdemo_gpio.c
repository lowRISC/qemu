/*
 * QEMU lowRISC Ibex Demo GPIO device
 *
 * Copyright (c) 2022-2023 Rivos, Inc.
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
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/ibexdemo/ibexdemo_gpio.h"
#include "hw/irq.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/sysbus.h"
#include "trace.h"

/* clang-format off */
REG32(OUT, 0x00u)
REG32(IN, 0x04u)
REG32(IN_DBNC, 0x08u)
REG32(OUT_SHIFT, 0x0cu)
/* clang-format on */

struct IbexDemoGPIOState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;

    uint32_t in_count;
    uint32_t out_count;

    uint32_t input;
    uint32_t output;
    uint32_t output_level;

    qemu_irq *gpo;
};

static void ibexdemo_gpio_update_output(IbexDemoGPIOState *s)
{
    trace_ibexdemo_gpio_output(s->output);

    uint32_t output = s->output;
    uint32_t change = output ^ s->output_level;

    for (unsigned ix = 0; ix < s->out_count; ix++) {
        if (change & 0b1) {
            qemu_set_irq(s->gpo[ix], (int)(output & 0b1));
        }
        output >>= 1u;
        change >>= 1u;
    }

    s->output_level = s->output;
}

static void ibexdemo_gpio_reset(DeviceState *dev)
{
    IbexDemoGPIOState *s = IBEXDEMO_GPIO(dev);

    s->input = 0;
    s->output = 0;
    s->output_level = UINT32_MAX; /* be sure to update all output on reset */

    ibexdemo_gpio_update_output(s);
}

static uint64_t ibexdemo_gpio_read(void *opaque, hwaddr addr, unsigned int size)
{
    IbexDemoGPIOState *s = opaque;
    uint32_t val32;

    switch (addr >> 2u) {
    case R_OUT:
        val32 = s->output;
        break;
    case R_IN:
    case R_IN_DBNC:
        val32 = s->input;
        break;
    case R_OUT_SHIFT:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: not supported\n", __func__);
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        val32 = 0;
        break;
    }

    return (uint64_t)val32;
}

static void ibexdemo_gpio_write(void *opaque, hwaddr addr, uint64_t val64,
                                unsigned int size)
{
    IbexDemoGPIOState *s = opaque;

    switch (addr >> 2u) {
    case R_OUT:
        s->output = val64 & ((1u << s->out_count) - 1u);
        ibexdemo_gpio_update_output(s);
        break;
    case R_IN:
    case R_IN_DBNC:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: W/O registers\n", __func__);
        break;
    case R_OUT_SHIFT:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: not supported\n", __func__);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static void ibexdemo_gpio_input_event(void *opaque, int irq, int level)
{
    IbexDemoGPIOState *s = opaque;

    if (irq < s->in_count) {
        if (level) {
            s->input |= 1u << irq;
        } else {
            s->input &= ~(1u << irq);
        }
    }

    trace_ibexdemo_gpio_input(s->input);
}

static const MemoryRegionOps ibexdemo_gpio_ops = {
    .read = &ibexdemo_gpio_read,
    .write = &ibexdemo_gpio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static Property ibexdemo_gpio_properties[] = {
    DEFINE_PROP_UINT32("in_count", IbexDemoGPIOState, in_count,
                       IBEXDEMO_GPIO_IN_MAX),
    DEFINE_PROP_UINT32("out_count", IbexDemoGPIOState, out_count,
                       IBEXDEMO_GPIO_IN_MAX),
    DEFINE_PROP_END_OF_LIST(),
};

static void ibexdemo_gpio_realize(DeviceState *dev, Error **errp)
{
    IbexDemoGPIOState *s = IBEXDEMO_GPIO(dev);

    if (s->in_count > IBEXDEMO_GPIO_IN_MAX) {
        s->in_count = IBEXDEMO_GPIO_IN_MAX;
    }
    if (s->out_count > IBEXDEMO_GPIO_OUT_MAX) {
        s->out_count = IBEXDEMO_GPIO_OUT_MAX;
    }

    qdev_init_gpio_in_named(dev, &ibexdemo_gpio_input_event,
                            IBEXDEMO_GPIO_IN_LINES, s->in_count);
}

static void ibexdemo_gpio_init(Object *obj)
{
    IbexDemoGPIOState *s = IBEXDEMO_GPIO(obj);

    memory_region_init_io(&s->mmio, obj, &ibexdemo_gpio_ops, s,
                          TYPE_IBEXDEMO_GPIO, 0x1000u);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);

    s->gpo = g_new0(qemu_irq, IBEXDEMO_GPIO_OUT_MAX);

    qdev_init_gpio_out_named(DEVICE(obj), s->gpo, IBEXDEMO_GPIO_OUT_LINES,
                             IBEXDEMO_GPIO_OUT_MAX);
}

static void ibexdemo_gpio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = &ibexdemo_gpio_reset;
    dc->realize = &ibexdemo_gpio_realize;
    device_class_set_props(dc, ibexdemo_gpio_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ibexdemo_gpio_info = {
    .name = TYPE_IBEXDEMO_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IbexDemoGPIOState),
    .instance_init = &ibexdemo_gpio_init,
    .class_init = &ibexdemo_gpio_class_init,
};

static void ibexdemo_gpio_register_types(void)
{
    type_register_static(&ibexdemo_gpio_info);
}

type_init(ibexdemo_gpio_register_types);
