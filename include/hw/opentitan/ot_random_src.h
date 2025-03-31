/*
 * QEMU OpenTitan Random Source interface
 *
 * Copyright (c) 2023-2024 Rivos, Inc.
 * Copyright (c) 2025 lowRISC contributors.
 *
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

#ifndef HW_OPENTITAN_OT_RANDOM_SRC_H
#define HW_OPENTITAN_OT_RANDOM_SRC_H

#include "qom/object.h"

#define TYPE_OT_RANDOM_SRC_IF "ot-random_src_if"
typedef struct OtRandomSrcIfClass OtRandomSrcIfClass;
DECLARE_CLASS_CHECKERS(OtRandomSrcIfClass, OT_RANDOM_SRC_IF,
                       TYPE_OT_RANDOM_SRC_IF)
#define OT_RANDOM_SRC_IF(_obj_) \
    INTERFACE_CHECK(OtRandomSrcIf, (_obj_), TYPE_OT_RANDOM_SRC_IF)

#define OT_RANDOM_SRC_PACKET_SIZE_BITS 384u

#define OT_RANDOM_SRC_BYTE_COUNT  (OT_RANDOM_SRC_PACKET_SIZE_BITS / 8u)
#define OT_RANDOM_SRC_WORD_COUNT  (OT_RANDOM_SRC_BYTE_COUNT / sizeof(uint32_t))
#define OT_RANDOM_SRC_DWORD_COUNT (OT_RANDOM_SRC_BYTE_COUNT / sizeof(uint64_t))

typedef struct OtRandomSrcIf OtRandomSrcIf;

struct OtRandomSrcIfClass {
    InterfaceClass parent_class;

    /*
     * Fill up a buffer with random values
     *
     * @dev the random source instance
     * @random the buffer to fill in with random data
     * @fips on success, updated to @true if random data are FIPS-compliant
     * @return 0 on success,
     *         >=1 if the random source is still initializing or not enough
     *           entropy is available to fill the output buffer;
     *           if >1, indicates a hint on how many ns to wait before retrying,
     *         -1 if the random source is not available, i.e. if the module is
     *          not enabled or if the selected route is not the HW one,
     */
    int (*get_random_values)(OtRandomSrcIf *dev,
                             uint64_t random[OT_RANDOM_SRC_DWORD_COUNT],
                             bool *fips);
};

#endif /* HW_OPENTITAN_OT_RANDOM_SRC_H */
