/*
 * QEMU OpenTitan Noise Source interface
 *
 * Copyright (c) 2025 Rivos, Inc.
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

#ifndef HW_OPENTITAN_OT_NOISE_SRC_H
#define HW_OPENTITAN_OT_NOISE_SRC_H

#include "qom/object.h"

#define TYPE_OT_NOISE_SRC_IF "ot-noise_src_if"
typedef struct OtNoiseSrcIfClass OtNoiseSrcIfClass;
DECLARE_CLASS_CHECKERS(OtNoiseSrcIfClass, OT_NOISE_SRC_IF, TYPE_OT_NOISE_SRC_IF)
#define OT_NOISE_SRC_IF(_obj_) \
    INTERFACE_CHECK(OtNoiseSrcIf, (_obj_), TYPE_OT_NOISE_SRC_IF)

typedef struct OtNoiseSrcIf OtNoiseSrcIf;

struct OtNoiseSrcIfClass {
    InterfaceClass parent_class;

    /*
     * Report the fill rate of this source.
     *
     * @return the fill rate, in bytes per second
     */
    unsigned (*get_fill_rate)(OtNoiseSrcIf *dev);

    /*
     * Fill up a buffer with noise data
     *
     * @buffer destination buffer
     * @length number of bytes to fill in
     */
    void (*get_noise)(OtNoiseSrcIf *dev, uint8_t *buffer, size_t length);
};

#endif /* HW_OPENTITAN_OT_NOISE_SRC_H */
