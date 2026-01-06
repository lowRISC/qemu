/*
 * QEMU OpenTitan PinMux device
 *
 * Copyright (c) 2023-2024 Rivos, Inc.
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

#ifndef HW_OPENTITAN_OT_PINMUX_H
#define HW_OPENTITAN_OT_PINMUX_H

#include "qom/object.h"
#include "hw/registerfields.h"

#define TYPE_OT_PINMUX "ot-pinmux"

#define OT_PINMUX_DIO TYPE_OT_PINMUX "-dio"
#define OT_PINMUX_MIO TYPE_OT_PINMUX "-mio"
#define OT_PINMUX_PAD TYPE_OT_PINMUX "-pad" /* for devices using pinmux pad */

SHARED_FIELD(OT_PINMUX_PAD_ATTR_INVERT, 0u, 1u)
SHARED_FIELD(OT_PINMUX_PAD_ATTR_VIRTUAL_OD_EN, 1u, 1u)
SHARED_FIELD(OT_PINMUX_PAD_ATTR_PULL_EN, 2u, 1u)
SHARED_FIELD(OT_PINMUX_PAD_ATTR_PULL_SELECT, 3u, 1u)
SHARED_FIELD(OT_PINMUX_PAD_ATTR_KEEPER_EN, 4u, 1u)
SHARED_FIELD(OT_PINMUX_PAD_ATTR_SCHMITT_EN, 5u, 1u)
SHARED_FIELD(OT_PINMUX_PAD_ATTR_OD_EN, 6u, 1u)
SHARED_FIELD(OT_PINMUX_PAD_ATTR_SLEW_RATE, 16u, 0x2u)
SHARED_FIELD(OT_PINMUX_PAD_ATTR_DRIVE_STRENGTH, 20u, 4u)
SHARED_FIELD(OT_PINMUX_PAD_ATTR_FORCE_MODE, 28u, 2u)

typedef enum {
    OT_PINMUX_PAD_FORCE_IGNORE,
    OT_PINMUX_PAD_FORCE_LOW,
    OT_PINMUX_PAD_FORCE_HIGH,
    OT_PINMUX_PAD_FORCE_HIZ,
} OtPinmuxPadForceMode;

#define OT_PINMUX_PAD_ATTR_MASK \
    (OT_PINMUX_PAD_ATTR_INVERT_MASK | OT_PINMUX_PAD_ATTR_VIRTUAL_OD_EN_MASK | \
     OT_PINMUX_PAD_ATTR_PULL_EN_MASK | OT_PINMUX_PAD_ATTR_PULL_SELECT_MASK | \
     OT_PINMUX_PAD_ATTR_KEEPER_EN_MASK | OT_PINMUX_PAD_ATTR_SCHMITT_EN_MASK | \
     OT_PINMUX_PAD_ATTR_OD_EN_MASK | OT_PINMUX_PAD_ATTR_SLEW_RATE_MASK | \
     OT_PINMUX_PAD_ATTR_DRIVE_STRENGTH_MASK)

#endif /* HW_OPENTITAN_OT_PINMUX_H */
