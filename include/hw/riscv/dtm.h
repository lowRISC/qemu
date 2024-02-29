/*
 * QEMU RISC-V Debug Tranport Module
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

#ifndef HW_RISCV_DTM_H
#define HW_RISCV_DTM_H

#include "exec/hwaddr.h"
#include "hw/riscv/debug.h"

#define TYPE_RISCV_DTM "riscv.dtm"
OBJECT_DECLARE_SIMPLE_TYPE(RISCVDTMState, RISCV_DTM)

/**
 * Register a debug module on the Debug Transport Module.
 * It is valid to register the same module multiple time, as long as base_addr
 * and size are not modified.
 *
 * @dev the DTM instance
 * @dmif the DM to register
 * @base_addr the address of the first DM register
 * @size the count of DM registers
 * @return @c true if DTM is enabled, @c false otherwise
 */
bool riscv_dtm_register_dm(DeviceState *dev, RISCVDebugDeviceState *dmif,
                           hwaddr base_addr, hwaddr size);

#endif /* HW_RISCV_DTM_H */
