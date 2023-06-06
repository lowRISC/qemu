/*
 * QEMU OpenTitan Reset Manager device
 *
 * Copyright (c) 2023 Rivos, Inc.
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

#ifndef HW_OPENTITAN_OT_RSTMGR_H
#define HW_OPENTITAN_OT_RSTMGR_H

#include "qom/object.h"

#define TYPE_OT_RSTMGR "ot-rstmgr"
OBJECT_DECLARE_SIMPLE_TYPE(OtRstMgrState, OT_RSTMGR)

#define OPENTITAN_RSTMGR_SW_RST TYPE_OT_RSTMGR "-sw-rst"

typedef enum {
    OT_RSTMGR_RESET_POR,
    OT_RSTMGR_RESET_LOW_POWER,
    OT_RSTMGR_RESET_SW,
    OT_RSTMGR_RESET_SYSCTRL,
    OT_RSTMGR_RESET_AON_TIMER,
    OT_RSTMGR_RESET_PWRMGR,
    OT_RSTMGR_RESET_ALERT_HANDLER,
    OT_RSTMGR_RESET_RV_DM,
    OT_RSTMGR_RESET_COUNT,
} OtRstMgrResetReq;

/*
 * Request a system reset
 *
 * @fastclk true for fast clock domain, @c false for aon/slow clock
 * @req type of reset request
 */
void ot_rstmgr_reset_req(OtRstMgrState *s, bool fastclk, OtRstMgrResetReq req);

#endif /* HW_OPENTITAN_OT_RSTMGR_H */
