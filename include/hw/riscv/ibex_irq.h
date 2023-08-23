/*
 * QEMU lowRISC Ibex IRQ wrapper
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

#ifndef HW_RISCV_IBEX_IRQ_H
#define HW_RISCV_IBEX_IRQ_H

#include "qemu/osdep.h"
#include "qom/object.h"
#include "hw/irq.h"
#include "hw/qdev-core.h"
#include "hw/sysbus.h"


/** Simple IRQ wrapper to limit propagation of no-change calls */
typedef struct {
    qemu_irq irq;
    int level;
} IbexIRQ;

static inline bool ibex_irq_is_connected(const IbexIRQ *ibex_irq)
{
    return qemu_irq_is_connected(ibex_irq->irq);
}

static inline int ibex_irq_get_level(const IbexIRQ *ibex_irq)
{
    return ibex_irq->level;
}

static inline bool ibex_irq_set(IbexIRQ *ibex_irq, int level)
{
    if (level != ibex_irq->level) {
        ibex_irq->level = level;
        qemu_set_irq(ibex_irq->irq, level);
        return true;
    }

    return false;
}

static inline bool ibex_irq_raise(IbexIRQ *irq)
{
    return ibex_irq_set(irq, 1);
}

static inline bool ibex_irq_lower(IbexIRQ *irq)
{
    return ibex_irq_set(irq, 0);
}

static inline void ibex_qdev_init_irq(Object *obj, IbexIRQ *irq,
                                      const char *name)
{
    irq->level = 0;
    qdev_init_gpio_out_named(DEVICE(obj), &irq->irq, name, 1);
}

static inline void ibex_qdev_init_irqs(Object *obj, IbexIRQ *irqs,
                                       const char *name, unsigned count)
{
    for (unsigned ix = 0; ix < count; ix++) {
        irqs[ix].level = 0;
        qdev_init_gpio_out_named(DEVICE(obj), &irqs[ix].irq, name, 1);
    }
}

static inline void ibex_sysbus_init_irq(Object *obj, IbexIRQ *irq)
{
    irq->level = 0;
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &irq->irq);
}

#endif /* HW_RISCV_IBEX_IRQ_H */
