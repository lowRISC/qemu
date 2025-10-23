/*
 * QEMU OpenTitan Entropy Source device
 *
 * Copyright (c) 2023-2025 Rivos, Inc.
 * Copyright (c) 2025 lowRISC contributors.
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

#ifndef HW_OPENTITAN_OT_ENTROPY_SRC_H
#define HW_OPENTITAN_OT_ENTROPY_SRC_H

#include "qom/object.h"

#define TYPE_OT_ENTROPY_SRC "ot-entropy_src"
OBJECT_DECLARE_TYPE(OtEntropySrcState, OtEntropySrcClass, OT_ENTROPY_SRC)

#define OT_ENTROPY_SRC_PACKET_SIZE_BITS 384u

#define OT_ENTROPY_SRC_BYTE_COUNT (OT_ENTROPY_SRC_PACKET_SIZE_BITS / 8u)
#define OT_ENTROPY_SRC_WORD_COUNT (OT_ENTROPY_SRC_BYTE_COUNT / sizeof(uint32_t))
#define OT_ENTROPY_SRC_DWORD_COUNT \
    (OT_ENTROPY_SRC_BYTE_COUNT / sizeof(uint64_t))

struct OtEntropySrcClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;

    /*
     * Fill up a buffer with random values
     *
     * @ess the entropy source instance
     * @random the buffer to fill in with random data
     * @fips on success, updated to @true if random data are FIPS-compliant
     * @return 0 on success,
     *         >=1 if the random source is still initializing or not enough
     *           entropy is available to fill the output buffer;
     *           if >1, indicates a hint on how many ns to wait before retrying,
     *         -1 if the random source is not available, i.e. if the module is
     *          not enabled or if the selected route is not the HW one,
     */
    int (*get_entropy)(OtEntropySrcState *ess,
                       uint64_t random[OT_ENTROPY_SRC_DWORD_COUNT], bool *fips);
};

#endif /* HW_OPENTITAN_OT_ENTROPY_SRC_H */
