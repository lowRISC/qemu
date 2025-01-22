/*
 * QEMU OpenTitan PLIC extension
 *
 * Copyright (c) 2024 Rivos, Inc.
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
#include "qapi/error.h"
#include "exec/memory.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_plic_ext.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "trace.h"

/* clang-format off */
REG32(MSIP0, 0x0u)
    FIELD(MSIP0, EN, 0u, 1u)
REG32(ALERT_TEST, 0x4u)
    FIELD(ALERT_TEST, FATAL_FAULT, 0u, 1u)
/* clang-format on */

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_ALERT_TEST)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

struct OtPlicExtState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    IbexIRQ irq;
    qemu_irq alert;

    uint32_t regs[REGS_COUNT];

    char *ot_id;
};

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    REG_NAME_ENTRY(MSIP0),
    REG_NAME_ENTRY(ALERT_TEST),
};
#undef REG_NAME_ENTRY

static uint64_t ot_plic_ext_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtPlicExtState *s = opaque;
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_MSIP0:
        val32 = s->regs[reg];
        break;
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
        val32 = 0;
        break;
    }

    uint32_t pc = ibex_get_current_pc();
    trace_ot_plic_ext_io_read_out(s->ot_id, (uint32_t)addr, REG_NAME(reg),
                                  val32, pc);

    return (uint64_t)val32;
}

static void ot_plic_ext_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                                   unsigned size)
{
    OtPlicExtState *s = opaque;
    uint32_t val32 = (uint32_t)val64;
    (void)size;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_plic_ext_io_write(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32,
                               pc);

    switch (reg) {
    case R_MSIP0:
        val32 &= R_MSIP0_EN_MASK;
        s->regs[reg] = val32;
        ibex_irq_set(&s->irq, (int)(bool)val32);
        break;
    case R_ALERT_TEST:
        val32 &= R_ALERT_TEST_FATAL_FAULT_MASK;
        s->regs[reg] = val32;
        qemu_set_irq(s->alert, (int)(bool)val32);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Bad offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, addr);
        break;
    }
}

static Property ot_plic_ext_properties[] = {
    DEFINE_PROP_STRING("ot_id", OtPlicExtState, ot_id),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_plic_ext_regs_ops = {
    .read = &ot_plic_ext_regs_read,
    .write = &ot_plic_ext_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_plic_ext_reset(DeviceState *dev)
{
    OtPlicExtState *s = OT_PLIC_EXT(dev);

    ibex_irq_set(&s->irq, 0);
    qemu_set_irq(s->alert, 0);
}

static void ot_plic_ext_realize(DeviceState *dev, Error **errp)
{
    (void)errp;

    OtPlicExtState *s = OT_PLIC_EXT(dev);

    if (!s->ot_id) {
        s->ot_id =
            g_strdup(object_get_canonical_path_component(OBJECT(s)->parent));
    }
}

static void ot_plic_ext_init(Object *obj)
{
    OtPlicExtState *s = OT_PLIC_EXT(obj);

    memory_region_init_io(&s->mmio, obj, &ot_plic_ext_regs_ops, s,
                          TYPE_OT_PLIC_EXT, REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    ibex_qdev_init_irq(obj, &s->irq, NULL);
    qdev_init_gpio_out_named(DEVICE(obj), &s->alert, OT_DEVICE_ALERT, 1);
}

static void ot_plic_ext_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->reset = &ot_plic_ext_reset;
    dc->realize = &ot_plic_ext_realize;
    device_class_set_props(dc, ot_plic_ext_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_plic_ext_info = {
    .name = TYPE_OT_PLIC_EXT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtPlicExtState),
    .instance_init = &ot_plic_ext_init,
    .class_init = &ot_plic_ext_class_init,
};

static void ot_plic_ext_register_types(void)
{
    type_register_static(&ot_plic_ext_info);
}

type_init(ot_plic_ext_register_types);
