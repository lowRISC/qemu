/*
 * QEMU lowRISC Ibex Demo SPI host device
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

#ifndef HW_GPIO_IBEXDEMO_GPIO_H
#define HW_GPIO_IBEXDEMO_GPIO_H

#include "qemu/osdep.h"
#include "qom/object.h"
#include "exec/hwaddr.h"

#define TYPE_IBEXDEMO_GPIO "ibexdemo-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(IbexDemoGPIOState, IBEXDEMO_GPIO)

#define IBEXDEMO_GPIO_IN_MAX  8u
#define IBEXDEMO_GPIO_OUT_MAX 16u

#define IBEXDEMO_GPIO_IN_LINES  TYPE_IBEXDEMO_GPIO ".in"
#define IBEXDEMO_GPIO_OUT_LINES TYPE_IBEXDEMO_GPIO ".out"

#endif /* HW_GPIO_IBEXDEMO_GPIO_H */
