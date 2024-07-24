/*
 * QEMU OpenTitan OTP backend interface
 *
 * Copyright (c) 2024 Rivos, Inc.
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

#ifndef HW_OPENTITAN_OT_OTP_BE_IF_H
#define HW_OPENTITAN_OT_OTP_BE_IF_H

#include "qom/object.h"
#include "memory.h"

#define TYPE_OT_OTP_BE_IF "ot-otp-be-interface"
typedef struct OtOtpBeIfClass OtOtpBeIfClass;
DECLARE_CLASS_CHECKERS(OtOtpBeIfClass, OT_OTP_BE_IF, TYPE_OT_OTP_BE_IF)
#define OT_OTP_BE_IF(_obj_) \
    INTERFACE_CHECK(OtOtpBeIf, (_obj_), TYPE_OT_OTP_BE_IF)

typedef struct OtOtpBeIf OtOtpBeIf;

struct OtOtpBeIfClass {
    InterfaceClass parent_class;

    /*
     * Report whether ECC feature is enabled
     *
     * @dev the OTP device this backend belongs to
     * @return @c true if ECC mode is active, @c false otherwise
     */
    bool (*is_ecc_enabled)(OtOtpBeIf *beif);
};

#endif /* HW_OPENTITAN_OT_OTP_BE_IF_H */
