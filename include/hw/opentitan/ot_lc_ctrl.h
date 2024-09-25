/*
 * QEMU OpenTitan Life Cycle controller device
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

#ifndef HW_OPENTITAN_OT_LC_CTRL_H
#define HW_OPENTITAN_OT_LC_CTRL_H

#include "qom/object.h"

#define TYPE_OT_LC_CTRL "ot-lc_ctrl"
OBJECT_DECLARE_SIMPLE_TYPE(OtLcCtrlState, OT_LC_CTRL)

/* Init request from power manager */
#define OT_LC_PWR TYPE_OT_LC_CTRL "-pwr"

#define OT_LC_BROADCAST   TYPE_OT_LC_CTRL "-broadcast"
#define OT_LC_CTRL_SOCDBG TYPE_OT_LC_CTRL "-socdbg"

/* Life cycle broadcast signals (booleans) */
typedef enum {
    OT_LC_RAW_TEST_RMA, /* SoC debug control */
    OT_LC_DFT_EN, /* device for test */
    OT_LC_NVM_DEBUG_EN, /* for embed. flash, not used in DJ */
    OT_LC_HW_DEBUG_EN, /* unfortunately highly pervasive */
    OT_LC_CPU_EN, /* ibex core */
    OT_LC_KEYMGR_EN, /* key manager */
    OT_LC_ESCALATE_EN, /* unfortunately highly pervasive */
    OT_LC_CHECK_BYP_EN, /* OTP: bypass consistency check while updating */
    OT_LC_CREATOR_SEED_SW_RW_EN, /* for OTP and embed. flash */
    OT_LC_OWNER_SEED_SW_RW_EN, /* for embed. flash, not used in DJ, but should*/
    OT_LC_ISO_PART_SW_RD_EN, /* for embed. flash, not used in DJ */
    OT_LC_ISO_PART_SW_WR_EN, /* for embed. flash, not used in DJ */
    OT_LC_SEED_HW_RD_EN, /* for OTP and embed. flash */
    OT_LC_BROADCAST_COUNT,
} OtLcCtrlBroadcast;

#endif /* HW_OPENTITAN_OT_LC_CTRL_H */
