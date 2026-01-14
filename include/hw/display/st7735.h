/*
 * QEMU Sitronix ST7735 controller device
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

#ifndef HW_DISPLAY_ST7735_H
#define HW_DISPLAY_ST7735_H

#include "qemu/osdep.h"
#include "qom/object.h"
#include "exec/hwaddr.h"

#define TYPE_ST7735 "st7735"
OBJECT_DECLARE_SIMPLE_TYPE(St7735State, ST7735)

#define ST7735_IO_LINES TYPE_ST7735 ".io"

enum {
    ST7735_IO_RESET,
    ST7735_IO_D_C,
    ST7735_IO_COUNT,
};

void st7735_configure(DeviceState *dev, hwaddr addr);

#endif /* HW_DISPLAY_ST7735_H */
