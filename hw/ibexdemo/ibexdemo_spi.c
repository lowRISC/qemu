/*
 * QEMU lowRISC Ibex Demo SPI host device
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
#include "hw/ibexdemo/ibexdemo_spi.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/ssi/ssi.h"
#include "hw/sysbus.h"
#include "trace.h"

/* clang-format off */
REG32(TX, 0x00u)
    FIELD(TX, DATA, 0u, 8u)
REG32(STATUS, 0x04u)
    FIELD(STATUS, TX_FULL, 0u, 1u)
    FIELD(STATUS, TX_EMPTY, 1u, 1u)
/* clang-format on */

struct IbexDemoSPIState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    SSIBus *ssi;
};

static void ibexdemo_spi_reset(DeviceState *dev)
{
    /* empty */
}

static uint64_t ibexdemo_spi_read(void *opaque, hwaddr addr, unsigned int size)
{
    uint32_t val32;

    switch (addr >> 2u) {
    case R_TX:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: wdata is write only\n", __func__);
        val32 = 0;
        break;
    case R_STATUS:
        val32 = FIELD_DP32(0, STATUS, TX_EMPTY, 1u);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        val32 = 0;
        break;
    }

    return (uint64_t)val32;
}

static void ibexdemo_spi_write(void *opaque, hwaddr addr, uint64_t val64,
                               unsigned int size)
{
    IbexDemoSPIState *s = opaque;

    switch (addr >> 2u) {
    case R_STATUS:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: reg is read only\n", __func__);
        break;
    case R_TX:
        trace_ibexdemo_spi_output((uint8_t)val64);
        (void)ssi_transfer(s->ssi, (uint8_t)val64);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static const MemoryRegionOps ibexdemo_spi_ops = {
    .read = &ibexdemo_spi_read,
    .write = &ibexdemo_spi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static Property ibexdemo_spi_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void ibexdemo_spi_init(Object *obj)
{
    IbexDemoSPIState *s = IBEXDEMO_SPI(obj);

    memory_region_init_io(&s->mmio, obj, &ibexdemo_spi_ops, s,
                          TYPE_IBEXDEMO_SPI, 0x400u);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);

    s->ssi = ssi_create_bus(DEVICE(obj), "spi0");
}

static void ibexdemo_spi_realize(DeviceState *dev, Error **errp)
{
    /* empty */
}

static void ibexdemo_spi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = &ibexdemo_spi_reset;
    dc->realize = &ibexdemo_spi_realize;
    device_class_set_props(dc, ibexdemo_spi_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ibexdemo_spi_info = {
    .name = TYPE_IBEXDEMO_SPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IbexDemoSPIState),
    .instance_init = &ibexdemo_spi_init,
    .class_init = &ibexdemo_spi_class_init,
};

static void ibexdemo_spi_register_types(void)
{
    type_register_static(&ibexdemo_spi_info);
}

type_init(ibexdemo_spi_register_types);
