/*
 * QEMU OpenTitan ROM controller
 *
 * Copyright (c) 2023-2025 Rivos, Inc.
 *
 * Author(s):
 *  Loïc Lefort <loic@rivosinc.com>
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

#ifndef HW_OPENTITAN_OT_ROM_CTRL
#define HW_OPENTITAN_OT_ROM_CTRL

#include "qom/object.h"

#define TYPE_OT_ROM_CTRL "ot-rom_ctrl"
OBJECT_DECLARE_TYPE(OtRomCtrlState, OtRomCtrlClass, OT_ROM_CTRL)

#define OT_ROM_CTRL_GOOD TYPE_OT_ROM_CTRL "-good"
#define OT_ROM_CTRL_DONE TYPE_OT_ROM_CTRL "-done"

#define OT_ROM_DIGEST_BYTES 32u

struct OtRomCtrlClass {
    DeviceClass parent_class;
    ResettablePhases parent_phases;

    /*
     * Retrieve ROM digest.
     *
     * @digest a pointer to an array that will be filled with the ROM digest
     */
    void (*get_rom_digest)(const OtRomCtrlState *s,
                           uint8_t digest[OT_ROM_DIGEST_BYTES]);
};

#endif /* HW_OPENTITAN_OT_ROM_CTRL */
