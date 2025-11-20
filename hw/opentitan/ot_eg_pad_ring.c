/*
 * QEMU OpenTitan Earlgrey Pad Ring device
 *
 * Copyright (c) 2025 lowRISC contributors.
 *
 * Author(s):
 *  Emmanuel Blot <eblot@rivosinc.com>
 *  Alex Jones <alex.jones@lowrisc.org>
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
 *
 * This device currently only implements the Power-on-Reset (POR) pad, and does
 * not support any of the Muxed IO pads.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_eg_pad_ring.h"
#include "hw/opentitan/ot_rstmgr.h"
#include "hw/qdev-properties.h"
#include "hw/riscv/ibex_gpio.h"
#include "hw/riscv/ibex_irq.h"
#include "trace.h"

struct OtEgPadRingState {
    DeviceState parent_obj;

    IbexIRQ outputs[OT_EG_PAD_RING_PAD_COUNT];
    IbexIRQ por; /* Separate PoR IRQ to send the correct rstmgr reset req */

    ibex_gpio default_levels[OT_EG_PAD_RING_PAD_COUNT];

    /* properties */
    char *ot_id;
};

struct OtEgPadRingClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

static const ibex_gpio DEFAULT_LEVELS[OT_EG_PAD_RING_PAD_COUNT] = {
    [OT_EG_PAD_RING_PAD_POR_N] = IBEX_GPIO_PULL_UP,
};

#define PAD_NAME_ENTRY(_pad_) [OT_EG_PAD_RING_PAD_##_pad_] = stringify(_pad_)
static const char *PAD_NAMES[] = {
    /* clang-format off */
    PAD_NAME_ENTRY(POR_N),
    /* clang-format on */
};
#define PAD_NAME(_pad_) \
    ((((_pad_) < ARRAY_SIZE(PAD_NAMES)) && PAD_NAMES[_pad_]) ? \
         PAD_NAMES[_pad_] : \
         "?")

static void ot_eg_pad_ring_por_update(OtEgPadRingState *s)
{
    int level = ibex_irq_get_level(&s->outputs[OT_EG_PAD_RING_PAD_POR_N]);
    bool blevel = ibex_gpio_level(level);

    /**
     * @todo: The current implementation directly invokes a rstmgr reset req
     * on a falling edge on the PoR pad. In reality, this pin can be held high
     * which should hold the device in reset, but this is not currently
     * supported via the rstmgr interface. Hence, if the host wishes to rely on
     * holding the device in reset, it must stop the VM when setting PoR high &
     * later resume when setting it low again.
     */
    if (ibex_gpio_is_hiz(level) || blevel) {
        return;
    }

    ibex_irq_set(&s->por, OT_RSTMGR_RESET_POR);
}

static Property ot_eg_pad_ring_properties[] = {
    DEFINE_PROP_STRING(OT_COMMON_DEV_ID, OtEgPadRingState, ot_id),
    DEFINE_PROP_END_OF_LIST(),
};

static void ot_eg_pad_ring_reset_enter(Object *obj, ResetType type)
{
    OtEgPadRingClass *c = OT_EG_PAD_RING_GET_CLASS(obj);
    OtEgPadRingState *s = OT_EG_PAD_RING(obj);

    trace_ot_eg_pad_ring_reset(s->ot_id, "enter");

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    for (unsigned ix = 0; ix < OT_EG_PAD_RING_PAD_COUNT; ix++) {
        /* POR may cause and thus should be maintained through reset */
        if (ix != OT_EG_PAD_RING_PAD_POR_N) {
            /* current pad outputs should be cleared (Hi-Z) upon reset */
            ibex_irq_set(&s->outputs[ix], IBEX_GPIO_HIZ);
        }
    }

    /* Reset the dedicated PoR signal */
    ibex_irq_set(&s->por, 0);
}

static void ot_eg_pad_ring_reset_exit(Object *obj, ResetType type)
{
    OtEgPadRingClass *c = OT_EG_PAD_RING_GET_CLASS(obj);
    OtEgPadRingState *s = OT_EG_PAD_RING(obj);

    trace_ot_eg_pad_ring_reset(s->ot_id, "exit");

    if (c->parent_phases.exit) {
        c->parent_phases.exit(obj, type);
    }

    for (unsigned ix = 0; ix < OT_EG_PAD_RING_PAD_COUNT; ix++) {
        trace_ot_eg_pad_ring_reset_value(s->ot_id, PAD_NAME(ix),
                                         ibex_gpio_repr(s->default_levels[ix]));
        ibex_irq_set(&s->outputs[ix], s->default_levels[ix]);
    }

    /* Handle special cases where dedicated output signals are used */
    ot_eg_pad_ring_por_update(s);
}

static void ot_eg_pad_ring_realize(DeviceState *dev, Error **errp)
{
    OtEgPadRingState *s = OT_EG_PAD_RING(dev);
    (void)errp;

    ibex_qdev_init_irqs_default(OBJECT(dev), s->outputs,
                                OT_EG_PAD_RING_PAD_EGRESS,
                                OT_EG_PAD_RING_PAD_COUNT, -1);
}

static void ot_eg_pad_ring_init(Object *obj)
{
    OtEgPadRingState *s = OT_EG_PAD_RING(obj);

    ibex_qdev_init_irq(obj, &s->por, OT_EG_PAD_RING_POR_REQ);

    for (unsigned ix = 0; ix < OT_EG_PAD_RING_PAD_COUNT; ix++) {
        gchar *pad_name = g_ascii_strdown(PAD_NAME(ix), -1);
        s->default_levels[ix] = DEFAULT_LEVELS[ix];
        object_property_add_ibex_gpio(obj, pad_name, &s->default_levels[ix]);
        g_free(pad_name);
    }
}

static void ot_eg_pad_ring_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->realize = &ot_eg_pad_ring_realize;
    device_class_set_props(dc, ot_eg_pad_ring_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    /*
     * Implement the resettable interface to ensure the package configuration is
     * forwarded to the PinMux/GPIO on each reset sequence (this is not yet used
     * in Earlgrey, but is added for future-proofing).
     * The enter stage fully reinitializes the package config to HiZ default
     * values, and the exit stage pushes the actual package configuration to the
     * connected pinmux/IOs.
     */
    ResettableClass *rc = RESETTABLE_CLASS(dc);
    OtEgPadRingClass *pc = OT_EG_PAD_RING_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_eg_pad_ring_reset_enter, NULL,
                                       &ot_eg_pad_ring_reset_exit,
                                       &pc->parent_phases);
}

static const TypeInfo ot_eg_pad_ring_info = {
    .name = TYPE_OT_EG_PAD_RING,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(OtEgPadRingState),
    .instance_init = &ot_eg_pad_ring_init,
    .class_init = &ot_eg_pad_ring_class_init,
    .class_size = sizeof(OtEgPadRingClass)
};

static void ot_eg_pad_ring_register_types(void)
{
    type_register_static(&ot_eg_pad_ring_info);
}

type_init(ot_eg_pad_ring_register_types);
