/*
 * QEMU OpenTitan KMAC device
 *
 * Copyright (c) 2023 Rivos, Inc.
 *
 * Author(s):
 *  Loïc Lefort <loic@rivosinc.com>
 *
 * For details check the documentation here:
 *    https://opentitan.org/book/hw/ip/kmac
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

#ifndef HW_OPENTITAN_OT_KMAC_H
#define HW_OPENTITAN_OT_KMAC_H

#include "qom/object.h"
#include "hw/sysbus.h"

#define TYPE_OT_KMAC "ot-kmac"
OBJECT_DECLARE_SIMPLE_TYPE(OtKMACState, OT_KMAC)

enum OtKMACMode {
    OT_KMAC_MODE_NONE,
    OT_KMAC_MODE_SHA3,
    OT_KMAC_MODE_SHAKE,
    OT_KMAC_MODE_CSHAKE,
    OT_KMAC_MODE_KMAC,
};

/* Max size of cSHAKE prefix function name */
#define OT_KMAC_PREFIX_FUNCNAME_LEN 32u

/* Max size of cSHAKE prefix custom string */
#define OT_KMAC_PREFIX_CUSTOMSTR_LEN 32u

typedef struct {
    uint8_t funcname[OT_KMAC_PREFIX_FUNCNAME_LEN];
    size_t funcname_len;
    uint8_t customstr[OT_KMAC_PREFIX_CUSTOMSTR_LEN];
    size_t customstr_len;
} OtKMACPrefix;

typedef struct {
    enum OtKMACMode mode;
    size_t strength;
    OtKMACPrefix prefix;
} OtKMACAppCfg;

#define OT_KMAC_APP_MSG_BYTES    (64u / 8u)
#define OT_KMAC_APP_DIGEST_BYTES (384u / 8u)

typedef struct {
    uint8_t msg_data[OT_KMAC_APP_MSG_BYTES];
    size_t msg_len;
    bool last;
} OtKMACAppReq;

typedef struct {
    uint8_t digest_share0[OT_KMAC_APP_DIGEST_BYTES];
    uint8_t digest_share1[OT_KMAC_APP_DIGEST_BYTES];
    bool done;
} OtKMACAppRsp;

/*
 * Function called by the KMAC instance whenever an application request has been
 * processed.
 *
 * @opaque the opaque pointer as registered with the ot_kmac_connect_app
 *         function. This is usually the requester device instance.
 * @rsp the KMAC response.
 */
typedef void (*ot_kmac_response_fn)(void *opaque, const OtKMACAppRsp *rsp);

/**
 * Connect a application to the KMAC device.
 *
 * @s the KMAC device.
 * @app_idx the application index.
 * @cfg pointer to the KMAC configuration for this application.
 * @fn the function to call when an request has been processed.
 * @opaque a opaque pointer to forward to the response function.
 */
void ot_kmac_connect_app(OtKMACState *s, unsigned app_idx,
                         const OtKMACAppCfg *cfg, ot_kmac_response_fn fn,
                         void *opaque);

/**
 * Send a new application request to the KMAC device.
 *
 * @s the KMAC device.
 * @app_idx the application index.
 * @req the KMAC request to process.
 */
void ot_kmac_app_request(OtKMACState *s, unsigned app_idx,
                         const OtKMACAppReq *req);

#endif /* HW_OPENTITAN_OT_KMAC_H */
