/*
 * QEMU OpenTitan Clock manager device
 *
 * Copyright (c) 2023-2025 Rivos, Inc.
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

#ifndef HW_OPENTITAN_OT_CLKMGR_H
#define HW_OPENTITAN_OT_CLKMGR_H

#include "qom/object.h"

#define TYPE_OT_CLKMGR "ot-clkmgr"
OBJECT_DECLARE_TYPE(OtClkMgrState, OtClkMgrClass, OT_CLKMGR)

/* Supported ClockManager versions */
typedef enum {
    OT_CLKMGR_VERSION_EG_1_0_0,
    OT_CLKMGR_VERSION_DJ,
    OT_CLKMGR_VERSION_COUNT,
} OtClkMgrVersion;

#define OT_CLOCK_HINT_PREFIX "ot-clock-hint-"

#define OT_CLKMGR_CLOCK_INPUT TYPE_OT_CLKMGR "-clock-in"

/* deprecated definitions */
typedef enum {
    OT_CLKMGR_HINT_AES,
    OT_CLKMGR_HINT_HMAC,
    OT_CLKMGR_HINT_KMAC,
    OT_CLKMGR_HINT_OTBN,
    OT_CLKMGR_HINT_COUNT
} OtClkMgrHintSource;

#define OT_CLKMGR_HINT  TYPE_OT_CLKMGR "-hint"
#define OT_CLOCK_ACTIVE "ot-clock-active"

#endif /* HW_OPENTITAN_OT_CLKMGR_H */
