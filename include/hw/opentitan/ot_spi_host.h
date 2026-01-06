/*
 * QEMU OpenTitan SPI Host controller
 *
 * Copyright (C) 2022 Western Digital
 * Copyright (c) 2022-2023 Rivos, Inc.
 * Copyright (c) 2025 lowRISC contributors.
 *
 * Author(s):
 *  Wilfred Mallawa <wilfred.mallawa@wdc.com>
 *  Emmanuel Blot <eblot@rivosinc.com>
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

#ifndef HW_OPENTITAN_OT_SPI_HOST_H
#define HW_OPENTITAN_OT_SPI_HOST_H

#include "qom/object.h"
#include "hw/resettable.h"
#include "hw/sysbus.h"

#define TYPE_OT_SPI_HOST "ot-spi_host"
OBJECT_DECLARE_TYPE(OtSPIHostState, OtSPIHostClass, OT_SPI_HOST)

/* this class is only required to manage on-hold reset */
struct OtSPIHostClass {
    SysBusDeviceClass parent_class;

    /*
     * Transfer a byte over this downstream SPI Host's SPI bus.
     * If Passthrough Enable is not asserted, or the SPI Host supports more
     * than one Chip Select, the transfer does not take place on the bus and a
     * default value is returned.
     *
     * @tx byte to be transferred
     * @return received byte from SPI bus, or a default value
     */
    uint8_t (*ssi_downstream_transfer)(OtSPIHostState *, uint8_t tx);

    ResettablePhases parent_phases;
};

/* IRQ lines from upstream OT SPI Device */
#define OT_SPI_HOST_PASSTHROUGH_EN (TYPE_OT_SPI_HOST "-passthrough-en")
#define OT_SPI_HOST_PASSTHROUGH_CS (TYPE_OT_SPI_HOST "-passthrough-cs")

#endif /* HW_OPENTITAN_OT_SPI_HOST_H */
