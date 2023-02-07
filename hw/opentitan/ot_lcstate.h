/*
 * QEMU OpenTitan LifeCycle states
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

#ifndef HW_OPENTITAN_OT_LCSTATE_H
#define HW_OPENTITAN_OT_LCSTATE_H

/* Share lifecycle state definitions */
enum lc_state {
    LC_STATE_RAW,
    LC_STATE_TESTUNLOCKED0,
    LC_STATE_TESTLOCKED0,
    LC_STATE_TESTUNLOCKED1,
    LC_STATE_TESTLOCKED1,
    LC_STATE_TESTUNLOCKED2,
    LC_STATE_TESTLOCKED2,
    LC_STATE_TESTUNLOCKED3,
    LC_STATE_TESTLOCKED3,
    LC_STATE_TESTUNLOCKED4,
    LC_STATE_TESTLOCKED4,
    LC_STATE_TESTUNLOCKED5,
    LC_STATE_TESTLOCKED5,
    LC_STATE_TESTUNLOCKED6,
    LC_STATE_TESTLOCKED6,
    LC_STATE_TESTUNLOCKED7,
    LC_STATE_DEV,
    LC_STATE_PROD,
    LC_STATE_PRODEND,
    LC_STATE_RMA,
    LC_STATE_SCRAP,
    LC_STATE_POST_TRANSITION,
    LC_STATE_ESCALATE,
    LC_STATE_INVALID,
};

#endif /* HW_OPENTITAN_OT_LCSTATE_H */
