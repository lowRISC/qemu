/*
 * QEMU OpenTitan I2C device
 *
 * Copyright (c) 2024-2025 Rivos, Inc.
 * Copyright (c) 2025 lowRISC contributors.
 *
 * Author(s):
 *  Duncan Laurie <duncan@rivosinc.com>
 *  Alice Ziuziakowska <a.ziuziakowska@lowrisc.org>
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

#ifndef HW_OPENTITAN_OT_I2C_H
#define HW_OPENTITAN_OT_I2C_H

#include "qom/object.h"
#include "hw/sysbus.h"

#define TYPE_OT_I2C "ot-i2c"
OBJECT_DECLARE_TYPE(OtI2CState, OtI2CClass, OT_I2C)

#define TYPE_OT_I2C_TARGET TYPE_OT_I2C "-target"
OBJECT_DECLARE_SIMPLE_TYPE(OtI2CTarget, OT_I2C_TARGET)

#endif /* HW_OPENTITAN_OT_I2C_H */
