/*
 * QEMU JTAG TAP controller
 *
 * Copyright (c) 2022-2024 Rivos, Inc.
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

#ifndef HW_JTAG_TAP_CTRL_H
#define HW_JTAG_TAP_CTRL_H

#include "qom/object.h"

struct _TapDataHandler;
typedef struct _TapDataHandler TapDataHandler;

struct _TapDataHandler {
    const char *name; /**< Name */
    size_t length; /**< Data register length */
    uint64_t value; /**< Capture/Update value */
    void *opaque; /**< Arbitrary data */
    void (*capture)(TapDataHandler *tdh);
    void (*update)(TapDataHandler *tdh);
};

#define JTAG_MEMTX_REQUESTER_ID UINT16_MAX

/*
 * Create TAP IDCODE
 *
 * @_mfid_ Manufacturer ID
 * @_pnum_ Part number
 * @_ver_  Version
 */
#define JTAG_IDCODE(_mfid_, _pnum_, _ver_) \
    ((((_ver_) & 0xfu) << 28u) | (((_pnum_) & 0xffffu) << 12u) | \
     (((_mfid_) & 0x7ffu) << 1u) | 0b1)

/*
 * Create JEDEC Manufacturer ID
 *
 * @_tbl_ JEDEC table
 * @_id_ Entry in JEDEC table
 */
#define JEDEC_MANUFACTURER_ID(_tbl_, _id_) \
    (((((_tbl_) - 1u) & 0xfu) << 7u) | ((_id_) & 0x7fu))

#define TYPE_TAP_CTRL_IF "tap-ctrl-interface"
typedef struct TapCtrlIfClass TapCtrlIfClass;
DECLARE_CLASS_CHECKERS(TapCtrlIfClass, TAP_CTRL_IF, TYPE_TAP_CTRL_IF)
#define TAP_CTRL_IF(_obj_) INTERFACE_CHECK(TapCtrlIf, (_obj_), TYPE_TAP_CTRL_IF)

typedef struct TapCtrlIf TapCtrlIf;

struct TapCtrlIfClass {
    InterfaceClass parent_class;

    /**
     * Report whether TAP controller is enabled.
     *
     * @return @c true if the TAP can be used.
     */
    bool (*is_enabled)(TapCtrlIf *dev);

    /*
     * Register instruction support on the TAP controller
     *
     * @code instruction code for which to register the handler
     * @tdh TAP data handler to register
     * @return non-zero on error
     */
    int (*register_instruction)(TapCtrlIf *dev, unsigned code,
                                const TapDataHandler *tdh);
};

#endif // HW_JTAG_TAP_CTRL_H
