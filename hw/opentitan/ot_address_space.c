/*
 * QEMU OpenTitan Address Space container
 *
 * Copyright (c) 2023 Rivos, Inc.
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
 *
 * Note: for now, only a minimalist subset of Analog Sensor Top device is
 *       implemented in order to enable OpenTitan's ROM boot to progress
 */

#include "qemu/osdep.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "hw/opentitan/ot_address_space.h"
#include "trace.h"

struct OtAddressSpaceState {
    Object parent_obj;

    AddressSpace *as;
};

/* -------------------------------------------------------------------------- */
/* Public API */
/* -------------------------------------------------------------------------- */

AddressSpace *ot_address_space_get(OtAddressSpaceState *s)
{
    if (!s->as) {
        error_setg(&error_fatal, "Address space for %s not defined\n",
                   object_get_canonical_path_component(OBJECT(s)));
    }

    return s->as;
}

void ot_address_space_set(OtAddressSpaceState *s, AddressSpace *as)
{
    s->as = as;
}

/* -------------------------------------------------------------------------- */
/* Private implementation */
/* -------------------------------------------------------------------------- */

static const TypeInfo ot_address_space_info = {
    .name = TYPE_OT_ADDRESS_SPACE,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(OtAddressSpaceState),
};

static void ot_address_space_register_types(void)
{
    type_register_static(&ot_address_space_info);
}

type_init(ot_address_space_register_types);
