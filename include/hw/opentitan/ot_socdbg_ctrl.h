/*
 * QEMU OpenTitan SoC Debug Controller
 *
 * Copyright (c) 2024 Rivos, Inc.
 *
 * Author(s):
 *  Loïc Lefort <loic@rivosinc.com>
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

#ifndef HW_OPENTITAN_OT_SOCDBG_CTRL_H
#define HW_OPENTITAN_OT_SOCDBG_CTRL_H

#include "qom/object.h"

#define TYPE_OT_SOCDBG_CTRL "ot-socdbg_ctrl"
OBJECT_DECLARE_SIMPLE_TYPE(OtSoCDbgCtrlState, OT_SOCDBG_CTRL)

/* SocDbg controller states */
typedef enum {
    OT_SOCDBG_ST_RAW,
    OT_SOCDBG_ST_PRE_PROD,
    OT_SOCDBG_ST_PROD,
    OT_SOCDBG_ST_COUNT,
} OtSoCDbgState;

/* input lines */
#define OT_SOCDBG_HALT_CPU_BOOT TYPE_OT_SOCDBG_CTRL "-halt-cpu-boot"
#define OT_SOCDBG_LC_BCAST      TYPE_OT_SOCDBG_CTRL "-lc-broacast"
#define OT_SOCDBG_STATE         TYPE_OT_SOCDBG_CTRL "-socdbg"
#define OT_SOCDBG_BOOT_STATUS   TYPE_OT_SOCDBG_CTRL "-boot-status"
#define OT_SOCDBG_A0_DEBUG_EN   TYPE_OT_SOCDBG_CTRL "-a0-debug-en"
#define OT_SOCDBG_A0_FORCE_RAW  TYPE_OT_SOCDBG_CTRL "-a0-force-raw"

/* output lines */
#define OT_SOCDBG_CPU_BOOT     TYPE_OT_SOCDBG_CTRL "-cpu-boot"
#define OT_SOCDBG_DEBUG_POLICY TYPE_OT_SOCDBG_CTRL "-debug-policy"

#define OT_SOCDBG_DEBUG_POLICY_MASK 0x0fu
#define OT_SOCDBG_DEBUG_VALID_MASK  0x80u

#endif /* HW_OPENTITAN_OT_SOCDBG_CTRL_H */
