/*
 * QEMU OpenTitan unimplemented device
 *
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
#include "qemu/module.h"
#include "qapi/error.h"
#include "exec/hwaddr.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_unimp.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"

/* Comportable special registers */

REG32(INTR_STATE, 0x00u)
REG32(INTR_ENABLE, 0x04u)
REG32(INTR_TEST, 0x08u)
REG32(ALERT_TEST, 0x0cu)

REG32(ALERT_TEST_NO_IRQ, 0x00u)


struct OtUnimpClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

struct OtUnimpState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    IbexIRQ *irqs;
    IbexIRQ *alerts;

    uint32_t *regs;
    bool r_warned;
    bool w_warned;

    char *ot_id;
    uint32_t size; /* mapped size, in bytes */
    uint32_t irq_ro_mask; /* INTR_STATE R/o bits (vs, RW1C) */
    uint8_t irq_count; /* output IRQ lines, maybe 0 */
    uint8_t alert_count; /* output alert lines, maybe 0 */
    bool warn_once; /* whether to emit a warning once or on each access */
    bool no_store; /* whether to store written values */
};

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

static inline hwaddr ot_unimp_alert_test_reg(const OtUnimpState *s)
{
    return s->alert_count ?
               (s->irq_count ? R_ALERT_TEST : R_ALERT_TEST_NO_IRQ) :
               HWADDR_MAX;
}

static void ot_unimp_update_irqs(OtUnimpState *s)
{
    uint32_t level = s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE];

    for (unsigned ix = 0; ix < s->irq_count; ix++) {
        ibex_irq_set(&s->irqs[ix], (int)((level >> ix) & 0x1u));
    }
}

static void ot_unimp_update_alerts(OtUnimpState *s, uint32_t val32)
{
    for (unsigned ix = 0; ix < s->alert_count; ix++) {
        if ((val32 >> ix) & 0x1u) {
            /* alert test signals are transient */
            ibex_irq_raise(&s->alerts[ix]);
            ibex_irq_lower(&s->alerts[ix]);
        }
    }
}

static uint64_t ot_unimp_io_read(void *opaque, hwaddr addr, unsigned size)
{
    OtUnimpState *s = OT_UNIMP(opaque);
    (void)size;

    hwaddr reg = R32_OFF(addr);
    uint32_t val32;

    if (s->irq_count) {
        switch (reg) {
        case R_INTR_STATE:
        case R_INTR_ENABLE:
            return s->regs[reg];
        case R_INTR_TEST:
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: W/O register 0x%08x\n",
                          __func__, s->ot_id, (uint32_t)addr);
            return 0;
        default:
            break;
        }
    }

    if (reg == ot_unimp_alert_test_reg(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: W/O register 0x%08x\n",
                      __func__, s->ot_id, (uint32_t)addr);
        return 0u;
    }

    if (!s->no_store) {
        val32 = s->regs[reg];
    } else {
        val32 = 0u;
    }

    if (s->warn_once && s->r_warned) {
        return val32;
    }

    qemu_log_mask(LOG_UNIMP,
                  "%s: unimplemented device read @ 0x%08x = 0x%08x\n", s->ot_id,
                  (uint32_t)addr, val32);

    s->r_warned = true;

    return val32;
}

static void ot_unimp_io_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    OtUnimpState *s = OT_UNIMP(opaque);
    (void)size;

    hwaddr reg = R32_OFF(addr);
    uint32_t val32 = (uint32_t)value;

    if (s->irq_count) {
        switch (reg) {
        case R_INTR_STATE:
            val32 &= (1u << s->irq_count) - 1u;
            val32 &= ~s->irq_ro_mask;
            s->regs[R_INTR_STATE] &= ~val32;
            ot_unimp_update_irqs(s);
            return;
        case R_INTR_ENABLE:
            val32 &= (1u << s->irq_count) - 1u;
            s->regs[R_INTR_ENABLE] = val32;
            ot_unimp_update_irqs(s);
            return;
        case R_INTR_TEST:
            val32 &= (1u << s->irq_count) - 1u;
            s->regs[R_INTR_STATE] |= val32;
            ot_unimp_update_irqs(s);
            return;
        default:
            break;
        }
    }

    if (reg == ot_unimp_alert_test_reg(s)) {
        val32 &= (1u << s->alert_count) - 1u;
        ot_unimp_update_alerts(s, val32);
        return;
    }

    if (!s->no_store) {
        s->regs[reg] = val32;
    }

    if (s->warn_once && s->w_warned) {
        return;
    }

    qemu_log_mask(LOG_UNIMP,
                  "%s: unimplemented device write @ 0x%08x = 0x%08x\n",
                  s->ot_id, (uint32_t)addr, (uint32_t)value);

    s->w_warned = true;
}

/* clang-format off */
static const MemoryRegionOps ot_unimp_ops = {
    .read = ot_unimp_io_read,
    .write = ot_unimp_io_write,
    /* OpenTitan default LE */
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4u,
        .max_access_size = 4u,
    }
};
/* clang-format on */

static Property ot_unimp_properties[] = {
    DEFINE_PROP_STRING("ot_id", OtUnimpState, ot_id),
    DEFINE_PROP_UINT32("size", OtUnimpState, size, 0),
    DEFINE_PROP_UINT32("irq-ro-mask", OtUnimpState, irq_ro_mask, 0),
    DEFINE_PROP_UINT8("irq-count", OtUnimpState, irq_count, 0),
    DEFINE_PROP_UINT8("alert-count", OtUnimpState, alert_count, 0),
    DEFINE_PROP_BOOL("warn-once", OtUnimpState, warn_once, false),
    DEFINE_PROP_BOOL("no-store", OtUnimpState, no_store, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void ot_unimp_reset_enter(Object *obj, ResetType type)
{
    OtUnimpClass *c = OT_UNIMP_GET_CLASS(obj);
    OtUnimpState *s = OT_UNIMP(obj);

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    memset(s->regs, 0, s->size);

    for (unsigned ix = 0; ix < (unsigned)s->irq_count; ix++) {
        ibex_irq_lower(&s->irqs[ix]);
    }

    for (unsigned ix = 0; ix < (unsigned)s->alert_count; ix++) {
        ibex_irq_lower(&s->alerts[ix]);
    }

    /* note: warn-once booleans are not reset */
}

static void ot_unimp_realize(DeviceState *dev, Error **errp)
{
    OtUnimpState *s = OT_UNIMP(dev);

    if (!s->ot_id) {
        error_setg(errp, "ot_id property required for " TYPE_OT_UNIMP);
        return;
    }

    if (!s->size || (s->size & 3u)) {
        error_setg(errp, "%s: invalid size", s->ot_id);
        return;
    }

    if (s->irq_ro_mask & ~((1u << s->irq_count) - 1u)) {
        error_setg(errp, "%s: invalid irq mask/count", s->ot_id);
        return;
    }

    s->regs = !s->no_store ? g_new(uint32_t, s->size / sizeof(uint32_t)) :
                             (s->irq_count ? g_new(uint32_t, 2u) : NULL);
    s->irqs = s->irq_count ? g_new(IbexIRQ, s->irq_count) : NULL;
    s->alerts = s->alert_count ? g_new(IbexIRQ, s->alert_count) : NULL;

    if (s->irq_count) {
        ibex_qdev_init_irqs(OBJECT(dev), s->irqs, SYSBUS_DEVICE_GPIO_IRQ,
                            s->irq_count);
    }
    if (s->alert_count) {
        ibex_qdev_init_irqs(OBJECT(dev), s->alerts, OT_DEVICE_ALERT,
                            s->alert_count);
    }

    memory_region_init_io(&s->mmio, OBJECT(s), &ot_unimp_ops, s, s->ot_id,
                          s->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
}

static void ot_unimp_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->realize = ot_unimp_realize;
    device_class_set_props(dc, ot_unimp_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    ResettableClass *rc = RESETTABLE_CLASS(klass);
    OtUnimpClass *uc = OT_UNIMP_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_unimp_reset_enter, NULL, NULL,
                                       &uc->parent_phases);
}

static const TypeInfo ot_unimp_info = {
    .name = TYPE_OT_UNIMP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtUnimpState),
    .class_init = ot_unimp_class_init,
    .class_size = sizeof(OtUnimpClass),
};

static void ot_unimp_register_types(void)
{
    type_register_static(&ot_unimp_info);
}

type_init(ot_unimp_register_types)
