/*
 * QEMU OpenTitan PLIC extension
 *
 * Copyright (c) 2024-2025 Rivos, Inc.
 * Copyright (c) 2025 lowRISC contributors.
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

REG32(ALERT_TEST, 0x0u)
    FIELD(ALERT_TEST, FATAL_FAULT, 0u, 1u)
/* clang-format on */

#define OT_PLIC_EXT_MSIP_BASE  0x0u
#define OT_PLIC_EXT_ALERT_BASE 0x4000u
#define OT_PLIC_EXT_APERTURE   0x8000u

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_MSIP_REG (R_MSIP0)
#define MSIP_REGS_COUNT (R_LAST_MSIP_REG + 1u)
#define MSIP_REGS_SIZE  (MSIP_REGS_COUNT * sizeof(uint32_t))
#define MSIP_REG_NAME(_reg_) \
    ((((_reg_) < MSIP_REGS_COUNT) && MSIP_REG_NAMES[_reg_]) ? \
         MSIP_REG_NAMES[_reg_] : \
         "?")

#define R_LAST_ALERT_REG (R_ALERT_TEST)
#define ALERT_REGS_COUNT (R_LAST_ALERT_REG + 1u)
#define ALERT_REGS_SIZE  (ALERT_REGS_COUNT * sizeof(uint32_t))
#define ALERT_REG_NAME(_reg_) \
    ((((_reg_) < ALERT_REGS_COUNT) && ALERT_REG_NAMES[_reg_]) ? \
         ALERT_REG_NAMES[_reg_] : \
         "?")

struct OtPlicExtState {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    MemoryRegion mmio_msip;
    MemoryRegion mmio_alert;

    IbexIRQ irq;
    IbexIRQ alert;

    uint32_t msip_regs[MSIP_REGS_COUNT];

    char *ot_id;
};

struct OtPlicExtClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *MSIP_REG_NAMES[MSIP_REGS_COUNT] = {
    REG_NAME_ENTRY(MSIP0),
};

static const char *ALERT_REG_NAMES[ALERT_REGS_COUNT] = {
    REG_NAME_ENTRY(ALERT_TEST),
};
#undef REG_NAME_ENTRY

static uint64_t ot_plic_ext_msip_read(void *opaque, hwaddr addr, unsigned size)
{
    OtPlicExtState *s = opaque;
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    /* NOLINTNEXTLINE(hicpp-multiway-paths-covered) */
    switch (reg) {
    case R_MSIP0:
        val32 = s->msip_regs[reg];
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Bad offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, addr);
        val32 = 0;
        break;
    }

    uint32_t pc = ibex_get_current_pc();
    trace_ot_plic_ext_io_msip_read_out(s->ot_id, (uint32_t)addr,
                                       MSIP_REG_NAME(reg), val32, pc);

    return (uint64_t)val32;
}

static void ot_plic_ext_msip_write(void *opaque, hwaddr addr, uint64_t val64,
                                   unsigned size)
{
    OtPlicExtState *s = opaque;
    uint32_t val32 = (uint32_t)val64;
    (void)size;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_plic_ext_io_msip_write(s->ot_id, (uint32_t)addr,
                                    MSIP_REG_NAME(reg), val32, pc);

    /* NOLINTNEXTLINE(hicpp-multiway-paths-covered) */
    switch (reg) {
    case R_MSIP0:
        val32 &= R_MSIP0_EN_MASK;
        s->msip_regs[reg] = val32;
        ibex_irq_set(&s->irq, (int)(bool)val32);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Bad offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, addr);
        break;
    }
}

static uint64_t ot_plic_ext_alert_read(void *opaque, hwaddr addr, unsigned size)
{
    OtPlicExtState *s = opaque;
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    /* NOLINTNEXTLINE(hicpp-multiway-paths-covered) */
    switch (reg) {
    case R_ALERT_TEST:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: W/O register 0x%02" HWADDR_PRIx " (%s)\n",
                      __func__, s->ot_id, addr, ALERT_REG_NAME(reg));
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
    trace_ot_plic_ext_io_alert_read_out(s->ot_id, (uint32_t)addr,
                                        ALERT_REG_NAME(reg), val32, pc);

    return (uint64_t)val32;
}

static void ot_plic_ext_alert_write(void *opaque, hwaddr addr, uint64_t val64,
                                    unsigned size)
{
    OtPlicExtState *s = opaque;
    uint32_t val32 = (uint32_t)val64;
    (void)size;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_plic_ext_io_alert_write(s->ot_id, (uint32_t)addr,
                                     ALERT_REG_NAME(reg), val32, pc);

    /* NOLINTNEXTLINE(hicpp-multiway-paths-covered) */
    switch (reg) {
    case R_ALERT_TEST:
        val32 &= R_ALERT_TEST_FATAL_FAULT_MASK;
        ibex_irq_set(&s->alert, (int)(bool)val32);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Bad offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, addr);
        break;
    }
}

static Property ot_plic_ext_properties[] = {
    DEFINE_PROP_STRING(OT_COMMON_DEV_ID, OtPlicExtState, ot_id),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_plic_ext_msip_ops = {
    .read = &ot_plic_ext_msip_read,
    .write = &ot_plic_ext_msip_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static const MemoryRegionOps ot_plic_ext_alert_ops = {
    .read = &ot_plic_ext_alert_read,
    .write = &ot_plic_ext_alert_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_plic_ext_reset_enter(Object *obj, ResetType type)
{
    OtPlicExtClass *c = OT_PLIC_EXT_GET_CLASS(obj);
    OtPlicExtState *s = OT_PLIC_EXT(obj);

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    ibex_irq_set(&s->irq, 0);
    ibex_irq_set(&s->alert, 0);
}

static void ot_plic_ext_realize(DeviceState *dev, Error **errp)
{
    OtPlicExtState *s = OT_PLIC_EXT(dev);
    (void)errp;

    g_assert(s->ot_id);
}

static void ot_plic_ext_init(Object *obj)
{
    OtPlicExtState *s = OT_PLIC_EXT(obj);

    /* Top-level container */
    memory_region_init(&s->mem, obj, TYPE_OT_PLIC_EXT, OT_PLIC_EXT_APERTURE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mem);

    memory_region_init_io(&s->mmio_msip, obj, &ot_plic_ext_msip_ops, s,
                          TYPE_OT_PLIC_EXT ".msip", MSIP_REGS_SIZE);
    memory_region_add_subregion(&s->mem, OT_PLIC_EXT_MSIP_BASE, &s->mmio_msip);

    memory_region_init_io(&s->mmio_alert, obj, &ot_plic_ext_alert_ops, s,
                          TYPE_OT_PLIC_EXT ".alert", ALERT_REGS_SIZE);
    memory_region_add_subregion(&s->mem, OT_PLIC_EXT_ALERT_BASE,
                                &s->mmio_alert);

    ibex_qdev_init_irq(obj, &s->irq, NULL);
    ibex_qdev_init_irq(obj, &s->alert, OT_DEVICE_ALERT);
}

static void ot_plic_ext_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->realize = &ot_plic_ext_realize;
    device_class_set_props(dc, ot_plic_ext_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    ResettableClass *rc = RESETTABLE_CLASS(klass);
    OtPlicExtClass *pc = OT_PLIC_EXT_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_plic_ext_reset_enter, NULL, NULL,
                                       &pc->parent_phases);
}

static const TypeInfo ot_plic_ext_info = {
    .name = TYPE_OT_PLIC_EXT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtPlicExtState),
    .instance_init = &ot_plic_ext_init,
    .class_size = sizeof(OtPlicExtClass),
    .class_init = &ot_plic_ext_class_init,
};

static void ot_plic_ext_register_types(void)
{
    type_register_static(&ot_plic_ext_info);
}

type_init(ot_plic_ext_register_types);
