/*
 * QEMU OpenTitan SPI Host controller
 *
 * Copyright (C) 2022 Western Digital
 * Copyright (c) 2022-2023 Rivos, Inc.
 *
 * Author(s):
 *  Wilfred Mallawa <wilfred.mallawa@wdc.com>
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

#ifndef HW_OPENTITAN_OT_SPI_HOST_H
#define HW_OPENTITAN_OT_SPI_HOST_H

#include "qom/object.h"
#include "hw/sysbus.h"

#define TYPE_OT_SPI_HOST "ot-spi_host"
OBJECT_DECLARE_SIMPLE_TYPE(OtSPIHostState, OT_SPI_HOST)

#define OT_SPI_HOST_IRQ_NUM 2

#endif /* HW_OPENTITAN_OT_SPI_HOST_H */
