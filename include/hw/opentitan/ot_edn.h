/*
 * QEMU OpenTitan Entropy Distribution Network device
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

#ifndef HW_OPENTITAN_OT_EDN_H
#define HW_OPENTITAN_OT_EDN_H

#include "qom/object.h"
#include "hw/irq.h"

#define TYPE_OT_EDN "ot-edn"
OBJECT_DECLARE_SIMPLE_TYPE(OtEDNState, OT_EDN)

/*
 * Function called by the EDN instance whenever entropy has been requested
 * and an entropy packet is available for the requester endpoint.
 *
 * @opaque the opaque pointer as registered with the ot_end_connect_endpoint
 *         function. This is usually the requester device instance.
 * @bits the entropy bits
 * @fips   whether the entropy adhere to NIST requirements (simulated only,
 *         current implementation does not support FIPS requirements)
 */
typedef void (*ot_edn_push_entropy_fn)(void *opaque, uint32_t bits, bool fips);

/**
 * Connect a device endpoint to the EDN device.
 *
 * @s the EDN device
 * @ep_id the endpoint unique identifier for the EDN instance.
 * @fn the function to call when an entropy packet is available for the
 *      requester.
 * @opaque a opaque pointer to forward to the entropy function
 */
void ot_edn_connect_endpoint(OtEDNState *s, unsigned ep_id,
                             ot_edn_push_entropy_fn fn, void *opaque);

/**
 * Request a new entropy packet.
 *
 * @s the EDN device
 * @ep_id the endpoint unique identifier for the EDN instance.
 * @return 0 on success, -1 otherwise. If non-zero, the entropy function is not
 *         called for this request.
 */
int ot_edn_request_entropy(OtEDNState *s, unsigned ep_id);


#endif /* HW_OPENTITAN_OT_EDN_H */
