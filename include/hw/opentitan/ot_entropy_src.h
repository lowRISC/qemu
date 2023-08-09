/*
 * QEMU OpenTitan Entropy Source device
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

#ifndef HW_OPENTITAN_OT_ENTROPY_SRC_H
#define HW_OPENTITAN_OT_ENTROPY_SRC_H

#include "qom/object.h"

#define TYPE_OT_ENTROPY_SRC "ot-entropy_src"
OBJECT_DECLARE_SIMPLE_TYPE(OtEntropySrcState, OT_ENTROPY_SRC)

#define OT_ENTROPY_SRC_PACKET_SIZE_BITS 384u
#define OT_ENTROPY_SRC_BYTE_COUNT       (OT_ENTROPY_SRC_PACKET_SIZE_BITS / 8u)
#define OT_ENTROPY_SRC_WORD_COUNT       (OT_ENTROPY_SRC_BYTE_COUNT / sizeof(uint32_t))
#define OT_ENTROPY_SRC_DWORD_COUNT \
    ((OT_ENTROPY_SRC_BYTE_COUNT / sizeof(uint64_t)))

/* see hw/ip/edn/doc/#multiple-edns-in-boot-time-request-mode */
#define OT_ENTROPY_SRC_BOOT_DELAY_NS 2000000u /* 2 ms */

/*
 * Tell whether the entropy source is available, i.e. whether the entropy
 * source module has been enabled.
 *
 * @return 0 is the entropy_src is disabled, or a positive, monotonic increase
 *         generation number which indicates the number of time the entropy_src
 *         has been cycled (enabled from a disable state). This generation
 *         identifier should be passed on any subsequent
 *         #ot_entropy_src_get_random request
 */
int ot_entropy_src_get_generation(OtEntropySrcState *s);

/*
 * Fill up a buffer with random values
 *
 * @s the entropy state instance
 * @genid the generation identifier, from #ot_entropy_src_get_generation
 * @random the buffer to fill in with entropy data
 * @fips on success, updated to @true if entropy data are FIPS-compliant
 * @return 0 on success,
 *         -1 if the entropy source is not available, i.e. if the module is not
 *            enabled or if the selected route is not the HW one,
 *         -2 if the generation ID does not match and execution cannot process
 *            any further,
 *         1 if the entropy source is still initializing or not enough entropy
 *           is available to fill the output buffer.
 */
int ot_entropy_src_get_random(OtEntropySrcState *s, int genid,
                              uint64_t random[OT_ENTROPY_SRC_DWORD_COUNT],
                              bool *fips);

#endif /* HW_OPENTITAN_OT_ENTROPY_SRC_H */
