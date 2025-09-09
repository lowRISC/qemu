/*
 * QEMU OpenTitan Flash controller device
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

#ifndef HW_OPENTITAN_OT_FLASH_H
#define HW_OPENTITAN_OT_FLASH_H

#include "qom/object.h"

#define TYPE_OT_FLASH "ot-flash"
OBJECT_DECLARE_TYPE(OtFlashState, OtFlashClass, OT_FLASH)

/* Input signals from the lc_ctrl */
typedef enum {
    /* "Indication ... that software is allowed to read/write CREATOR_SEED" */
    OT_FLASH_LC_CREATOR_SEED_SW_RW_EN,
    /* "Indication ... that software is allowed to read/write OWNER_SEED" */
    OT_FLASH_LC_OWNER_SEED_SW_RW_EN,
    /* "Indication ... that hardware is allowed to read {CREATOR,OWNER}_SEED" */
    OT_FLASH_LC_SEED_HW_RD_EN,
    /* "Indication ... that software is allowed to read the isolated part" */
    OT_FLASH_LC_ISO_PART_SW_RD_EN,
    /* "Indication ... that software is allowed to write the isolated part" */
    OT_FLASH_LC_ISO_PART_SW_WR_EN,
    /* "Escalation indication" - move FSMs into the error state */
    OT_FLASH_LC_ESCALATE_EN,
    /* "Indication ... that non-volatile memory debug is allowed" */
    OT_FLASH_LC_NVM_DEBUG_EN,
    OT_FLASH_LC_BROADCAST_COUNT,
} OtFlashLcBroadcastType;

typedef enum {
    FLASH_KEYMGR_SECRET_CREATOR_SEED,
    FLASH_KEYMGR_SECRET_OWNER_SEED,
    FLASH_KEYMGR_SECRET_COUNT
} OtFlashKeyMgrSecretType;

#define OT_FLASH_KEYMGR_SECRET_BYTES 32u /* 256 bits */

typedef struct {
    uint8_t secret[OT_FLASH_KEYMGR_SECRET_BYTES]; /* seed data */
    bool valid; /* whether the seed data is valid */
} OtFlashKeyMgrSecret;

struct OtFlashClass {
    DeviceClass parent_class;
    ResettablePhases parent_phases;

    /*
     * Retrieve Key Manager secret (seed) from flash.
     *
     * @s the flash device
     * @type the type of secret to retrieve
     * @secret the key manager secret record to update
     */
    void (*get_keymgr_secret)(const OtFlashState *s,
                              OtFlashKeyMgrSecretType type,
                              OtFlashKeyMgrSecret *secret);
};

#endif /* HW_OPENTITAN_OT_FLASH_H */
