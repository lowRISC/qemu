/*
 * QEMU OpenTitan SoC Debug Controller
 *
 * Copyright (c) 2024-2025 Rivos, Inc.
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

#ifndef HW_OPENTITAN_OT_SOC_DBG_CTRL_H
#define HW_OPENTITAN_OT_SOC_DBG_CTRL_H

#include "qom/object.h"

#define TYPE_OT_SOC_DBG_CTRL "ot-soc_dbg_ctrl"
OBJECT_DECLARE_TYPE(OtSoCDbgCtrlState, OtSoCDbgCtrlClass, OT_SOC_DBG_CTRL)

/* SocDbg controller states */
typedef enum {
    OT_SOC_DBG_ST_BLANK,
    OT_SOC_DBG_ST_PRE_PROD,
    OT_SOC_DBG_ST_PROD,
    OT_SOC_DBG_ST_COUNT,
} OtSoCDbgState;

/* Signal carried over OT_SOC_DBG_DEBUG_POLICY */
typedef union {
    struct {
        /*
         * Active categories:
         * b0..b1: unused
         * b2: CAT2
         * b3: CAT3
         * b4: CAT4
         * b5..b7: unused
         */
        uint8_t cat_bm;
        bool relocked;
    };
    int i32;
} OtSocDbgDebugPolicy;

/* input lines */
#define OT_SOC_DBG_HALT_CPU_BOOT TYPE_OT_SOC_DBG_CTRL "-halt-cpu-boot"
#define OT_SOC_DBG_LC_BCAST      TYPE_OT_SOC_DBG_CTRL "-lc-broadcast"
#define OT_SOC_DBG_STATE         TYPE_OT_SOC_DBG_CTRL "-soc-dbg"
#define OT_SOC_DBG_BOOT_STATUS   TYPE_OT_SOC_DBG_CTRL "-boot-status"

/* output lines */
#define OT_SOC_DBG_CONTINUE_CPU_BOOT TYPE_OT_SOC_DBG_CTRL "-continue-cpu-boot"
#define OT_SOC_DBG_DEBUG_POLICY      TYPE_OT_SOC_DBG_CTRL "-debug-policy"

#define OT_SOC_DBG_DEBUG_POLICY_MASK 0x0fu
#define OT_SOC_DBG_DEBUG_VALID_MASK  0x80u

#endif /* HW_OPENTITAN_OT_SOC_DBG_CTRL_H */
