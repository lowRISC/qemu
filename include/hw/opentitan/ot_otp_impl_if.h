/*
 * QEMU OpenTitan One Time Programmable (OTP) implementer interface.
 * An OTP device should implement this API used by the OTP engine.
 *
 * Copyright (c) 2025 Rivos, Inc.
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

#ifndef HW_OPENTITAN_OT_OTP_IMPL_IF_H
#define HW_OPENTITAN_OT_OTP_IMPL_IF_H

#include "qom/object.h"

#define TYPE_OT_OTP_IMPL_IF "ot-otp_impl_if"
typedef struct OtOTPImplIfClass OtOTPImplIfClass;
typedef struct OtOTPImplIf OtOTPImplIf;
DECLARE_CLASS_CHECKERS(OtOTPImplIfClass, OT_OTP_IMPL_IF, TYPE_OT_OTP_IMPL_IF)
#define OT_OTP_IMPL_IF(_obj_) \
    INTERFACE_CHECK(OtOTPImplIf, (_obj_), TYPE_OT_OTP_IMPL_IF)

/* @todo WR and RD locks need to be rewritten (not simple boolean) */
typedef struct {
    const char *name;
    uint16_t size;
    uint16_t offset;
    uint16_t digest_offset;
    uint16_t zer_offset;
    uint16_t hw_digest:1;
    uint16_t sw_digest:1;
    uint16_t secret:1;
    uint16_t buffered:1;
    uint16_t write_lock:1;
    uint16_t read_lock:1;
    uint16_t read_lock_csr:1;
    uint16_t integrity:1;
    uint16_t zeroizable:1;
    uint16_t iskeymgr_creator:1;
    uint16_t iskeymgr_owner:1;
} OtOTPPartDesc;

typedef struct {
    unsigned partition;
    unsigned offset; /* byte offset in partition */
    unsigned size; /* size in bytes */
} OtOTPKeySeed;


/* Supported statuses */
typedef enum {
    OT_OTP_STATUS_NONE,
    OT_OTP_STATUS_DAI, /* Direct Access Interface */
    OT_OTP_STATUS_LCI, /* Life Cycle Interface */
    OT_OTP_STATUS_COUNT
} OtOTPStatus;

struct OtOTPImplIfClass {
    InterfaceClass parent_class;

    /* Array of part_count partition descriptors */
    const OtOTPPartDesc *part_descs;

    /* Array of OTP_KEY_COUNT descriptors */
    const OtOTPKeySeed *key_seeds;

    /* Number of partition */
    unsigned part_count;

    /* Index of the life cycle partition in part_descs */
    unsigned part_lc_num;

    /* Number of SRAM KEY requester slots */
    unsigned sram_key_req_slot_count;

    /* Whether embedded flash support is present */
    bool has_flash_support;

    /* Whether zeroification feature is supported */
    bool has_zer_support;

    /*
     * Update the R_STATUS register.
     * The OTP concrete device should provide an implementation
     *
     * @dev the OTP device
     * @error the error type to update
     * @set whether to set or clear the error
     */
    void (*update_status_error)(OtOTPImplIf *dev, OtOTPStatus error, bool set);

    /*
     * Signal that power management has triggered the OTP initialization
     * sequence. May be left unimplemented (NULL).
     *
     * @dev the OTP device
     */
    void (*signal_pwr_sequence)(OtOTPImplIf *dev);
};

#endif /* HW_OPENTITAN_OT_OTP_IMPL_IF_H */
