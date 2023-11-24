/*
 * QEMU RISC-V Debug Module
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

#ifndef HW_RISCV_DM_H
#define HW_RISCV_DM_H

#include "exec/hwaddr.h"
#include "hw/irq.h"

#define TYPE_RISCV_DM "riscv-dm"
OBJECT_DECLARE_SIMPLE_TYPE(RISCVDMState, RISCV_DM)

#define RISCV_DM_ACK_LINES TYPE_RISCV_DM ".ack"

/*
 * Note: these offsets depends on the debug module implementation, so they
 * should be better defined as yet another configurable properties
 */
#define RISCV_DM_HALTED_OFFSET    0x100u
#define RISCV_DM_GOING_OFFSET     0x104u
#define RISCV_DM_RESUMING_OFFSET  0x108u
#define RISCV_DM_EXCEPTION_OFFSET 0x10cu
#define RISCV_DM_FLAGS_OFFSET     0x400u

enum RISCVDMAckInterface {
    ACK_HALTED, /* HartID of the hart that has been halted */
    ACK_GOING, /* A hart has started execution */
    ACK_RESUMING, /* HardID of the hart that has resumed non-debug op. */
    ACK_EXCEPTION, /* An exception has occured in debug mode */
    ACK_COUNT, /* Count of acknownledgement lines */
};

#endif /* HW_RISCV_DM_H */
