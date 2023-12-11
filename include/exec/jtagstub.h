/*
 * QEMU OpenTitan JTAG TAP controller
 *
 * Copyright (c) 2022-2023 Rivos, Inc.
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

#ifndef JTAGSTUB_H
#define JTAGSTUB_H

#include "qemu/osdep.h"

struct _TAPDataHandler;
typedef struct _TAPDataHandler TAPDataHandler;

struct _TAPDataHandler {
    const char *name; /**< Name */
    size_t length; /**< Data register length */
    uint64_t value; /**< Capture/Update value */
    void *opaque; /**< Arbitrary data */
    void (*capture)(TAPDataHandler *tdh);
    void (*update)(TAPDataHandler *tdh);
};

#define JTAG_MEMTX_REQUESTER_ID UINT16_MAX

/*
 * JTAGserver_start: start the JTAG server
 * @port_or_device: connection spec for JTAG
 */
int jtagserver_start(const char *port_or_device);

/*
 * JTAG_exit: exit JTAG session, reporting inferior status
 * @code: exit code reported
 */
void jtagserver_exit(int code);

/*
 * Configure the JTAG TAP controller
 *
 * @irlength the length in bits of the instruction register
 * @idcode the unique identifier code of the device
 * @return non-zero on error
 */
int jtag_configure_tap(size_t irlength, uint32_t idcode);

/*
 * Register TAP data handler
 *
 * @inst instruction for which to register the handler
 * @tdh TAP data handler to register
 * @return non-zero on error
 */
int jtag_register_handler(unsigned inst, const TAPDataHandler *tdh);

#endif // JTAGSTUB_H
