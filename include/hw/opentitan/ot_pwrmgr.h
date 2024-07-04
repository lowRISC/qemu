/*
 * QEMU OpenTitan Power Manager device
 *
 * Copyright (c) 2023-2024 Rivos, Inc.
 *
 * Author(s):
 *  Emmanuel Blot <eblot@rivosinc.com>
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

#ifndef HW_OPENTITAN_OT_PWRMGR_H
#define HW_OPENTITAN_OT_PWRMGR_H

#include "qom/object.h"

#define TYPE_OT_PWRMGR "ot-pwrmgr"
OBJECT_DECLARE_TYPE(OtPwrMgrState, OtPwrMgrClass, OT_PWRMGR)

/* Supported PowerManager versions */
typedef enum {
    OT_PWMGR_VERSION_EG,
    OT_PWMGR_VERSION_DJ,
    OT_PWMGR_VERSION_COUNT,
} OtPwrMgrVersion;

/* Match PWRMGR_PARAM_*_WKUP_REQ_IDX definitions */
typedef enum {
    OT_PWRMGR_WAKEUP_SYSRST,
    OT_PWRMGR_WAKEUP_ADC_CTRL,
    OT_PWRMGR_WAKEUP_PINMUX,
    OT_PWRMGR_WAKEUP_USBDEV,
    OT_PWRMGR_WAKEUP_AON_TIMER,
    OT_PWRMGR_WAKEUP_SENSOR,
    OT_PWRMGR_WAKEUP_COUNT,
} OtPwrMgrWakeup;

/* output lines */
#define OT_PWRMGR_LC_REQ  TYPE_OT_PWRMGR "-lc-req"
#define OT_PWRMGR_OTP_REQ TYPE_OT_PWRMGR "-otp-req"
#define OT_PWRMGR_CPU_EN  TYPE_OT_PWRMGR "-cpu-en"
#define OT_PWRMGR_STRAP   TYPE_OT_PWRMGR "-strap"
#define OT_PWRMGR_RST_REQ TYPE_OT_PWRMGR "-reset-req"

/* input lines */
#define OT_PWRMGR_LC_RSP  TYPE_OT_PWRMGR "-lc-rsp"
#define OT_PWRMGR_OTP_RSP TYPE_OT_PWRMGR "-otp-rsp"

#define OT_PWRMGR_WKUP   TYPE_OT_PWRMGR "-wkup"
#define OT_PWRMGR_RST    TYPE_OT_PWRMGR "-rst"
#define OT_PWRMGR_SW_RST TYPE_OT_PWRMGR "-sw-rst"

#define OT_PWRMGR_ROM_GOOD TYPE_OT_PWRMGR "-rom-good"
#define OT_PWRMGR_ROM_DONE TYPE_OT_PWRMGR "-rom-done"

/* custom extension */
#define OT_PWRMGR_HOLDON_FETCH TYPE_OT_PWRMGR "-holdon-fetch"

#endif /* HW_OPENTITAN_OT_PWRMGR_H */
