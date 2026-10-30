/*
 * QEMU OpenTitan Pattern Generator (PATTGEN) device
 *
 * Copyright lowRISC contributors.
 *
 * Licensed under the Apache License, Version 2.0, see LICENSE for details.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HW_OPENTITAN_OT_PATTGEN_H
#define HW_OPENTITAN_OT_PATTGEN_H

#include "qom/object.h"
#include "hw/sysbus.h"

#define TYPE_OT_PATTGEN "ot-pattgen"
OBJECT_DECLARE_SIMPLE_TYPE(OtPattgenState, OT_PATTGEN)

bool ot_pattgen_get_pin_level(OtPattgenState *s, unsigned pin_idx);

#endif /* HW_OPENTITAN_OT_PATTGEN_H */
