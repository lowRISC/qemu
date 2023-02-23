/*
 * QEMU OpenTitan Cryptographically Secure Random Number Generator
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

#ifndef HW_OPENTITAN_OT_CSRNG_H
#define HW_OPENTITAN_OT_CSRNG_H

#include "qom/object.h"
#include "hw/irq.h"
#include "hw/registerfields.h"

#define TYPE_OT_CSRNG "ot-csrng"
OBJECT_DECLARE_SIMPLE_TYPE(OtCSRNGState, OT_CSRNG)

#define OT_CSRNG_HW_APP_MAX 2u

#define OT_CSRNG_PACKET_SIZE_BITS 128u
#define OT_CSRNG_PACKET_WORD_COUNT \
    ((unsigned)(OT_CSRNG_PACKET_SIZE_BITS / (8u * sizeof(uint32_t))))
#define OT_CSRNG_CMD_WORD_COUNT 1u
#define OT_CSRNG_CMD_ARG_MAX    12u
#define OT_CSRNG_CMD_WORD_MAX   (OT_CSRNG_CMD_WORD_COUNT + OT_CSRNG_CMD_ARG_MAX)

typedef enum {
    OT_CSRNG_CMD_NONE = 0,
    OT_CSRNG_CMD_INSTANTIATE = 1,
    OT_CSRNG_CMD_RESEED = 2,
    OT_CSRNG_CMD_GENERATE = 3,
    OT_CSRNG_CMD_UPDATE = 4,
    OT_CSRNG_CMD_UNINSTANTIATE = 5,
} OtCsrngCmd;

/* clang-format off */
REG32(OT_CSNRG_CMD, 0)
    FIELD(OT_CSNRG_CMD, ACMD, 0u, 4u)
    FIELD(OT_CSNRG_CMD, CLEN, 4u, 4u)
    FIELD(OT_CSNRG_CMD, FLAG0, 8u, 4u)
    FIELD(OT_CSNRG_CMD, GLEN, 12u, 12u)
/* clang-format on */

/*
 * Function called by the CSRNS instance once one entropy packet is available.
 * Once all requested entropy packets have been sent, the req_sts IRQ is
 * signalled.
 *
 * @opaque the opaque pointer as registered with the ot_csrng_connect_endpoint
 *         function. This is usually the requester device instance.
 * @app_id the HW application identifier of the requester
 * @bits the buffer that contains entropy bits
 * @fips   whether the entropy adhere to NIST requirements (simulated only,
 *         current implementation does not support FIPS requirements)
 */
typedef void (*ot_csrng_genbit_filler_fn)(void *opaque, const uint32_t *bits,
                                          bool fips);

/**
 * Connect a HW application to the CSRNG device.
 *
 * @s the CSRNG device
 * @app_id the HW application unique identifier
 * @req_sts the IRQ to signal once a command is completed. The IRQ level signal
 *          the completion status of the command: 0 indicates a sucessful
 *          completion, non-zero a failed one.
 * @fn the filler function to call with one or more entropy packet, when a
 *     generate command is called.
 * @opaque a opaque pointer to forward to the filler function
 * @return an IRQ line that signals whether the HW application is ready to
 *         receive entropy, i.e. genbits_ready
 */
qemu_irq ot_csnrg_connect_hw_app(OtCSRNGState *s, unsigned app_id,
                                 qemu_irq req_sts, ot_csrng_genbit_filler_fn fn,
                                 void *opaque);

/**
 * Request a generated entropy block.
 * CSRNG only deliver (a single) entropy packet after this command is received,
 * thought the genbit_filler function. This function is asynchronously called
 * after this function is received.
 *
 * @s the CSRNG device
 * @app_id the HW application unique identifier, as provided with the connect
 *         command
 * @return 0 on success, -1 otherwise.
 */
int ot_csrng_request_entropy(OtCSRNGState *s, unsigned app_id);

/**
 * Push a new command.
 *
 * @s the CSRNG device
 * @app_id the HW application unique identifier, as provided with the connect
 *         command
 * @command the command to execute (with any payload)
 * @return 0 on success, -1 otherwise. If non-zero, the req_sts is not
 *         signalled for this command.
 */
int ot_csrng_push_command(OtCSRNGState *s, unsigned app_id,
                          const uint32_t *command);

#endif /* HW_OPENTITAN_OT_CSRNG_H */
