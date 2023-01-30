/*
 * QEMU lowRISC Ibex Demo Sim Control device
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
#include "hw/ibexdemo/ibexdemo_simctrl.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/sysbus.h"
#include "trace.h"

/* clang-format off */
REG32(OUT, 0x00u)
REG32(CTRL, 0x08u)
/* clang-format on */

struct IbexDemoSimCtrlState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
};

static uint64_t ibexdemo_simctrl_read(void *opaque, hwaddr addr,
                                      unsigned int size)
{
    uint32_t val32;

    switch (addr >> 2u) {
    case R_OUT:
    case R_CTRL:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: wdata is write only\n", __func__);
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

static void ibexdemo_simctrl_write(void *opaque, hwaddr addr, uint64_t val64,
                                   unsigned int size)
{
    switch (addr >> 2u) {
    case R_OUT:
        putc((int)(uint8_t)val64, stderr);
        break;
    case R_CTRL:
        /* would be nicer to receive a value with the code for exiting... */
        exit(100);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static const MemoryRegionOps ibexdemo_simctrl_ops = {
    .read = &ibexdemo_simctrl_read,
    .write = &ibexdemo_simctrl_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static Property ibexdemo_simctrl_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void ibexdemo_simctrl_init(Object *obj)
{
    IbexDemoSimCtrlState *s = IBEXDEMO_SIMCTRL(obj);

    memory_region_init_io(&s->mmio, obj, &ibexdemo_simctrl_ops, s,
                          TYPE_IBEXDEMO_SIMCTRL, 0x400u);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void ibexdemo_simctrl_realize(DeviceState *dev, Error **errp)
{
    /* empty */
}

static void ibexdemo_simctrl_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = &ibexdemo_simctrl_realize;
    device_class_set_props(dc, ibexdemo_simctrl_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ibexdemo_simctrl_info = {
    .name = TYPE_IBEXDEMO_SIMCTRL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IbexDemoSimCtrlState),
    .instance_init = &ibexdemo_simctrl_init,
    .class_init = &ibexdemo_simctrl_class_init,
};

static void ibexdemo_simctrl_register_types(void)
{
    type_register_static(&ibexdemo_simctrl_info);
}

type_init(ibexdemo_simctrl_register_types);
