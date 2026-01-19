/*
 * QEMU Ibex Clock Source interface
 *
 * Copyright (c) 2025 Rivos, Inc.
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

#ifndef HW_RISCV_IBEX_CLOCK_SRC_H
#define HW_RISCV_IBEX_CLOCK_SRC_H

#include "qom/object.h"

#define TYPE_IBEX_CLOCK_SRC_IF "ibex-clock_src_if"
typedef struct IbexClockSrcIfClass IbexClockSrcIfClass;
DECLARE_CLASS_CHECKERS(IbexClockSrcIfClass, IBEX_CLOCK_SRC_IF,
                       TYPE_IBEX_CLOCK_SRC_IF)
#define IBEX_CLOCK_SRC_IF(_obj_) \
    INTERFACE_CHECK(IbexClockSrcIf, (_obj_), TYPE_IBEX_CLOCK_SRC_IF)

typedef struct IbexClockSrcIf IbexClockSrcIf;

struct IbexClockSrcIfClass {
    InterfaceClass parent_class;

    /*
     * Get a clock source by its type/name, to connect to the specified sink
     * device. The clock line may already be connected or not.
     *
     * @name clock name
     * @sink the sink device for the clock line
     * @errp the error to use for reporting an invalid request
     * @return the name of IRQ line to carry the clock information
     */
    const char *(*get_clock_source)(IbexClockSrcIf *ifd, const char *name,
                                    const DeviceState *sink, Error **errp);
};

#endif /* HW_RISCV_IBEX_CLOCK_SRC_H */
