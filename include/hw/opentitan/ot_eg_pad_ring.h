/*
 * QEMU OpenTitan Earlgrey Pad Ring Device
 *
 * Copyright (c) 2025 lowRISC contributors.
 *
 * Author(s):
 *  Emmanuel Blot <eblot@rivosinc.com>
 *  Alex Jones <alex.jones@lowrisc.org>
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

#ifndef HW_OPENTITAN_OT_EG_PAD_RING_H
#define HW_OPENTITAN_OT_EG_PAD_RING_H

#include "qom/object.h"

#define TYPE_OT_EG_PAD_RING "ot-eg-pad-ring"
OBJECT_DECLARE_TYPE(OtEgPadRingState, OtEgPadRingClass, OT_EG_PAD_RING)

/* Pad output signals */
#define OT_EG_PAD_RING_PAD_EGRESS TYPE_OT_EG_PAD_RING "-egress"

/* Separate output signal to map the PoR GPIO to the correct reset request */
#define OT_EG_PAD_RING_POR_REQ TYPE_OT_EG_PAD_RING "-por-req"

/* Exposed I/O on external pads */
typedef enum {
    /*
     * USB (N & P), SPI Host & SPI Device (Csb, Sck, Sd0-3) currently go direct
     * through their respective CharDevs. Their DIOs are thus excluded unless
     * it is determined they are needed/useful for emulating their devices.
     *
     * @todo: IOR8/IOR9 are used by the System Reset Controller (not currently
     * emulated) as outputs for the `ec_rst_l` & `flash_wp_l` signals. These
     * should be added here if it is determined they should be emulated.
     *
     * Flash testing and voltage signal pads (flash_test_mode{0,1} and
     * flash_test_volt) are not needed for emulation. The same is true of the
     * OTP external voltage pad.
     *
     * @todo: There are also 47 MIO pads that should be added here:
     *  - IOR0-7 and IOR10-13,
     *  - IOC0-12,
     *  - IOB0-12, and
     *  - IOA0-8.
     * These pads should be connected to Earlgrey's Pinmux, and then the GPIO
     * CharDev logic should be updated/moved so that the host talks to GPIO
     * via the Pad Ring (going through Pinmux) instead of bypassing Pinmux to
     * talk to GPIO directly. When this is done, because the pad states will
     * not be reset, the work-around in Earlgrey's GPIO to retain values
     * across reset (see commit c232d98) should be removed also.
     */
    OT_EG_PAD_RING_PAD_POR_N, /* Power-on-Reset (negated) */
    OT_EG_PAD_RING_PAD_COUNT,
} OtEgPadRingPad;

#endif /* HW_OPENTITAN_OT_EG_PAD_RING_H */
