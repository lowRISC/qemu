/*
 * QEMU OpenTitan virtual mapper device
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

#ifndef HW_OPENTITAN_OT_VMAPPER_H
#define HW_OPENTITAN_OT_VMAPPER_H

#include "qom/object.h"
#include "exec/hwaddr.h"
#include "system/memory.h"

#define TYPE_OT_VMAPPER "ot-vmapper"
OBJECT_DECLARE_TYPE(OtVMapperState, OtVMapperClass, OT_VMAPPER)

/*
 * Configure the address translation for an address range.
 *
 * @insn whether the translation is for an instruction.
 * @slot the execution slot to use for the translation.
 * @src the first address of the range to match.
 * @dst the first physical address of the range to translate to.
 * @size the size (extentof the range to match. If 0, disable the translation
 *       for the selected slot.
 */
typedef void (*OtVMapperTranslate)(OtVMapperState *s, bool insn, unsigned slot,
                                   hwaddr src, hwaddr dst, size_t size);

/*
 * Disable the execution of an address range.
 *
 * @mr the memory region to manage
 * @disable whether to disable execution or (re-)enable it
 */
typedef void (*OtVMapperDisableExec)(OtVMapperState *s, const MemoryRegion *mr,
                                     bool disable);

struct OtVMapperClass {
    DeviceClass parent_class;
    ResettablePhases parent_phases;

    OtVMapperTranslate translate;
    OtVMapperDisableExec disable_exec;

    OtVMapperState **instances;
    unsigned num_instances;
};

#endif /* HW_OPENTITAN_OT_VMAPPER_H */
