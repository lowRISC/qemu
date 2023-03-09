/*
 * QEMU OpenTitan Pseudo Random Generator
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

#ifndef HW_OPENTITAN_OT_PRNG_H
#define HW_OPENTITAN_OT_PRNG_H

#include "qemu/osdep.h"

#define TYPE_OT_PRNG "ot_prng"

typedef struct OtPrngState OtPrngState;

OtPrngState *ot_prng_allocate(void);
void ot_prng_release(OtPrngState *prng);
uint32_t ot_prng_random_u32(OtPrngState *prng);
void ot_prng_random_u32_array(OtPrngState *prng, uint32_t *array, size_t count);
void ot_prng_reseed(OtPrngState *prng, uint32_t seed);
void ot_prng_reseed_array(OtPrngState *prng, const uint32_t *seed,
                          size_t length);

#endif /* HW_OPENTITAN_OT_PRNG_H */
