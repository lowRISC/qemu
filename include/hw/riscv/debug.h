/*
 * QEMU RISC-V Debug
 *
 * Copyright (c) 2023-2024 Rivos, Inc.
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

#ifndef HW_RISCV_DEBUG_H
#define HW_RISCV_DEBUG_H

#include "qom/object.h"
#include "hw/qdev-core.h"
#include "hw/sysbus.h"

#define RISCV_DEBUG_PREFIX "riscv-debug"

#define TYPE_RISCV_DEBUG_DEVICE RISCV_DEBUG_PREFIX "_device"
OBJECT_DECLARE_TYPE(RISCVDebugDeviceState, RISCVDebugDeviceClass,
                    RISCV_DEBUG_DEVICE)

typedef enum RISCVDebugResult {
    RISCV_DEBUG_NOERR, /* Previous operation completed successfully */
    RISCV_DEBUG_RSV, /* Reserved value, eq. to FAILED */
    RISCV_DEBUG_FAILED, /* Previous operation failed */
    RISCV_DEBUG_BUSY, /* New op. while a DMI request is still in progress */
} RISCVDebugResult;

/* Debug Module Interface access */
struct RISCVDebugDeviceClass {
    DeviceClass parent_class;

    /*
     * Debugger request to write to address.
     */
    RISCVDebugResult (*write_rq)(RISCVDebugDeviceState *dev, uint32_t addr,
                                 uint32_t value);

    /*
     * Debugger request to read from address.
     */
    RISCVDebugResult (*read_rq)(RISCVDebugDeviceState *dev, uint32_t addr);

    /*
     * Read back value.
     */
    uint32_t (*read_value)(RISCVDebugDeviceState *dev);

    /*
     * Set next DM address
     */
    void (*set_next_dm)(RISCVDebugDeviceState *dev, uint32_t addr);
};

struct RISCVDebugDeviceState {
    DeviceState parent_obj;
};

#endif /* HW_RISCV_DEBUG_H */
