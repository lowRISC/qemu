/*
 * QEMU OpenTitan Debug Module device
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

#ifndef HW_PULP_RV_DM_H
#define HW_PULP_RV_DM_H

#include "qemu/osdep.h"
#include "qom/object.h"
#include "hw/irq.h"
#include "hw/sysbus.h"

#define TYPE_PULP_RV_DM "pulp-rv-dm"
OBJECT_DECLARE_SIMPLE_TYPE(PulpRVDMState, PULP_RV_DM)

#define PULP_RV_DM_ACK_OUT_LINES TYPE_PULP_RV_DM ".ack-out"

/* Configuration */
#define PULP_RV_DM_NSCRATCH_COUNT        2u
#define PULP_RV_DM_DATA_COUNT            2u
#define PULP_RV_DM_PROGRAM_BUFFER_COUNT  8u
#define PULP_RV_DM_ABSTRACTCMD_COUNT     10u
#define PULP_RV_DM_FLAGS_COUNT           1u
#define PULP_RV_DM_WHERETO_OFFSET        0x300u
#define PULP_RV_DM_PROGRAM_BUFFER_OFFSET 0x360u
#define PULP_RV_DM_DATAADDR_OFFSET       0x380u

/* ROM entry points */
#define PULP_RV_DM_HALT_OFFSET      0x0000
#define PULP_RV_DM_RESUME_OFFSET    0x0004
#define PULP_RV_DM_EXCEPTION_OFFSET 0x0008

/* Memory regions */
#define PULP_RV_DM_REGS_BASE 0x0u
#define PULP_RV_DM_REGS_SIZE sizeof(uint32_t)
#define PULP_RV_DM_MEM_BASE  0x100u
#define PULP_RV_DM_MEM_SIZE  0x700u
#define PULP_RV_DM_ROM_BASE  0x800u
#define PULP_RV_DM_ROM_SIZE  0x800u

#endif /* HW_PULP_RV_DM_H */
