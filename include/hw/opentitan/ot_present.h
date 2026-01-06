/*
 * QEMU OpenTitan Present 128-bit key cipher implementation
 *
 * Copyright lowRISC contributors.
 *
 * Licensed under the Apache License, Version 2.0, see LICENSE for details.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HW_OPENTITAN_OT_PRESENT_H
#define HW_OPENTITAN_OT_PRESENT_H

typedef struct OtPresentState OtPresentState;

OtPresentState *ot_present_new(void);
void ot_present_init(OtPresentState *ps, const uint8_t *key);
void ot_present_free(OtPresentState *ps);
void ot_present_encrypt(const OtPresentState *ps, uint64_t src, uint64_t *dst);
void ot_present_decrypt(const OtPresentState *ps, uint64_t src, uint64_t *dst);

#endif /* HW_OPENTITAN_OT_PRESENT_H */
