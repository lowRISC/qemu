/*
 * QEMU OpenTitan Key Sink interface
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

#ifndef HW_OPENTITAN_OT_KEY_SINK_H
#define HW_OPENTITAN_OT_KEY_SINK_H

#include "qom/object.h"

#define TYPE_OT_KEY_SINK_IF "ot-key_sink_if"
typedef struct OtKeySinkIfClass OtKeySinkIfClass;
DECLARE_CLASS_CHECKERS(OtKeySinkIfClass, OT_KEY_SINK_IF, TYPE_OT_KEY_SINK_IF)
#define OT_KEY_SINK_IF(_obj_) \
    INTERFACE_CHECK(OtKeySinkIf, (_obj_), TYPE_OT_KEY_SINK_IF)

typedef struct OtKeySinkIf OtKeySinkIf;

struct OtKeySinkIfClass {
    InterfaceClass parent_class;

    /*
     * Push key material to a key sink.
     *
     * Key is split in two shares, such as: key = share0 ^ share1
     *
     * @share0 first key share, may be NULL
     * @share1 second key share, may be NULL
     * @key_len the length of the key shares in bytes, may be 0
     * @valid whether the key is valid or not
     */
    void (*push_key)(OtKeySinkIf *ifd, const uint8_t *share0,
                     const uint8_t *share1, size_t key_len, bool valid);
};

#endif /* HW_OPENTITAN_OT_KEY_SINK_H */
