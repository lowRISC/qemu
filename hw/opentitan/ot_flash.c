/*
 * QEMU OpenTitan Flash controller device
 *
 * Copyright (c) 2023-2025 Rivos, Inc.
 * Copyright (c) 2025 lowRISC contributors.
 *
 * Author(s):
 *  Emmanuel Blot <eblot@rivosinc.com>
 *  Alex Jones <alex.jones@lowrisc.org>
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
 *
 * Known limitations:
 *  - ECC/ICV/Scrambling functionality is not yet implemented in QEMU,
 *    including ECC single error support.
 *  - Alert functionality is only partially modelled.
 *  - Program Repair / High Endurance enables are meaningless in the OpenTitan
 *    Generic Flash Bank and so are not emulated.
 *  - Erase Suspend is not emulated in QEMU (erases are done synchronously, so
 *    you can suspend, but the bit will immediately be cleared).
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/memalign.h"
#include "qemu/timer.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "elf.h"
#include "hw/loader.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_fifo32.h"
#include "hw/opentitan/ot_flash.h"
#include "hw/opentitan/ot_vmapper.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "sysemu/block-backend.h"
#include "trace.h"

/* set to use I/O to access the flash partition */
#define DATA_PART_USE_IO_OPS 0

/* set to log hart GPR on flash data access */
#define LOG_GPR_ON_FLASH_DATA_ACCESS 0

/* temp. */
#pragma GCC diagnostic ignored "-Wunused-variable"

#define PARAM_NUM_IRQS   6u
#define PARAM_NUM_ALERTS 5u

#define FLASH_SEED_BANK           0u
#define FLASH_SEED_INFO_PARTITION 0u

/* clang-format off */
REG32(INTR_STATE, 0x0u)
    SHARED_FIELD(INTR_PROG_EMPTY, 0u, 1u)
    SHARED_FIELD(INTR_PROG_LVL, 1u, 1u)
    SHARED_FIELD(INTR_RD_FULL, 2u, 1u)
    SHARED_FIELD(INTR_RD_LVL, 3u, 1u)
    SHARED_FIELD(INTR_OP_DONE, 4u, 1u)
    SHARED_FIELD(INTR_CORR_ERR, 5u, 1u)
REG32(INTR_ENABLE, 0x4u)
REG32(INTR_TEST, 0x8u)
REG32(ALERT_TEST, 0xcu)
    SHARED_FIELD(ALERT_RECOV_ERR, 0u, 1u)
    SHARED_FIELD(ALERT_FATAL_STD_ERR, 1u, 1u)
    SHARED_FIELD(ALERT_FATAL_ERR, 2u, 1u)
    SHARED_FIELD(ALERT_FATAL_PRIM, 3u, 1u)
    SHARED_FIELD(ALERT_RECOV_PRIM, 4u, 1u)
REG32(DIS, 0x10u)
    FIELD(DIS, VAL, 0u, 4u)
REG32(EXEC, 0x14u)
REG32(INIT, 0x18u)
    FIELD(INIT, VAL, 0u, 1u)
REG32(CTRL_REGWEN, 0x1cu)
    FIELD(CTRL_REGWEN, EN, 0u, 1u)
REG32(CONTROL, 0x20u)
    FIELD(CONTROL, START, 0u, 1u)
    FIELD(CONTROL, OP, 4u, 2u)
    FIELD(CONTROL, PROG_SEL, 6u, 1u)
    FIELD(CONTROL, ERASE_SEL, 7u, 1u)
    FIELD(CONTROL, PARTITION_SEL, 8u, 1u)
    FIELD(CONTROL, INFO_SEL, 9u, 2u)
    FIELD(CONTROL, NUM, 16u, 12u)
REG32(ADDR, 0x24u)
    FIELD(ADDR, START, 0u, 20u)
REG32(PROG_TYPE_EN, 0x28u)
    FIELD(PROG_TYPE_EN, NORMAL, 0u, 1u)
    FIELD(PROG_TYPE_EN, REPAIR, 1u, 1u)
REG32(ERASE_SUSPEND, 0x2cu)
    FIELD(ERASE_SUSPEND, REQ, 0u, 1u)
REG32(REGION_CFG_REGWEN_0, 0x30u)
    SHARED_FIELD(REGWEN_EN, 0u, 1u)
REG32(REGION_CFG_REGWEN_1, 0x34u)
REG32(REGION_CFG_REGWEN_2, 0x38u)
REG32(REGION_CFG_REGWEN_3, 0x3cu)
REG32(REGION_CFG_REGWEN_4, 0x40u)
REG32(REGION_CFG_REGWEN_5, 0x44u)
REG32(REGION_CFG_REGWEN_6, 0x48u)
REG32(REGION_CFG_REGWEN_7, 0x4cu)
REG32(MP_REGION_CFG_0, 0x50u)
    SHARED_FIELD(MP_REGION_CFG_EN, 0u, 4u)
    SHARED_FIELD(MP_REGION_CFG_RD_EN, 4u, 4u)
    SHARED_FIELD(MP_REGION_CFG_PROG_EN, 8u, 4u)
    SHARED_FIELD(MP_REGION_CFG_ERASE_EN, 12u, 4u)
    SHARED_FIELD(MP_REGION_CFG_SCRAMBLE_EN, 16u, 4u)
    SHARED_FIELD(MP_REGION_CFG_ECC_EN, 20u, 4u)
    SHARED_FIELD(MP_REGION_CFG_HE_EN, 24u, 4u)
REG32(MP_REGION_CFG_1, 0x54u)
REG32(MP_REGION_CFG_2, 0x58u)
REG32(MP_REGION_CFG_3, 0x5cu)
REG32(MP_REGION_CFG_4, 0x60u)
REG32(MP_REGION_CFG_5, 0x64u)
REG32(MP_REGION_CFG_6, 0x68u)
REG32(MP_REGION_CFG_7, 0x6cu)
REG32(MP_REGION_0, 0x70u)
    SHARED_FIELD(MP_REGION_BASE, 0u, 9u)
    SHARED_FIELD(MP_REGION_SIZE, 9u, 10u)
REG32(MP_REGION_1, 0x74u)
REG32(MP_REGION_2, 0x78u)
REG32(MP_REGION_3, 0x7cu)
REG32(MP_REGION_4, 0x80u)
REG32(MP_REGION_5, 0x84u)
REG32(MP_REGION_6, 0x88u)
REG32(MP_REGION_7, 0x8cu)
REG32(DEFAULT_REGION, 0x90u)
    FIELD(DEFAULT_REGION, RD_EN, 0u, 4u)
    FIELD(DEFAULT_REGION, PROG_EN, 4u, 4u)
    FIELD(DEFAULT_REGION, ERASE_EN, 8u, 4u)
    FIELD(DEFAULT_REGION, SCRAMBLE_EN, 12u, 4u)
    FIELD(DEFAULT_REGION, ECC_EN, 16u, 4u)
    FIELD(DEFAULT_REGION, HE_EN, 20u, 4u)
REG32(BANK0_INFO0_REGWEN_0, 0x94u)
    SHARED_FIELD(BANK_REGWEN, 0u, 1u)
REG32(BANK0_INFO0_REGWEN_1, 0x98u)
REG32(BANK0_INFO0_REGWEN_2, 0x9cu)
REG32(BANK0_INFO0_REGWEN_3, 0xa0u)
REG32(BANK0_INFO0_REGWEN_4, 0xa4u)
REG32(BANK0_INFO0_REGWEN_5, 0xa8u)
REG32(BANK0_INFO0_REGWEN_6, 0xacu)
REG32(BANK0_INFO0_REGWEN_7, 0xb0u)
REG32(BANK0_INFO0_REGWEN_8, 0xb4u)
REG32(BANK0_INFO0_REGWEN_9, 0xb8u)
REG32(BANK0_INFO0_PAGE_CFG_0, 0xbcu)
    SHARED_FIELD(BANK_INFO_PAGE_CFG_EN, 0u, 4u)
    SHARED_FIELD(BANK_INFO_PAGE_CFG_RD_EN, 4u, 4u)
    SHARED_FIELD(BANK_INFO_PAGE_CFG_PROG_EN, 8u, 4u)
    SHARED_FIELD(BANK_INFO_PAGE_CFG_ERASE_EN, 12u, 4u)
    SHARED_FIELD(BANK_INFO_PAGE_CFG_SCRAMBLE_EN, 16u, 4u)
    SHARED_FIELD(BANK_INFO_PAGE_CFG_ECC_EN, 20u, 4u)
    SHARED_FIELD(BANK_INFO_PAGE_CFG_HE_EN, 24u, 4u)
REG32(BANK0_INFO0_PAGE_CFG_1, 0xc0u)
REG32(BANK0_INFO0_PAGE_CFG_2, 0xc4u)
REG32(BANK0_INFO0_PAGE_CFG_3, 0xc8u)
REG32(BANK0_INFO0_PAGE_CFG_4, 0xccu)
REG32(BANK0_INFO0_PAGE_CFG_5, 0xd0u)
REG32(BANK0_INFO0_PAGE_CFG_6, 0xd4u)
REG32(BANK0_INFO0_PAGE_CFG_7, 0xd8u)
REG32(BANK0_INFO0_PAGE_CFG_8, 0xdcu)
REG32(BANK0_INFO0_PAGE_CFG_9, 0xe0u)
REG32(BANK0_INFO1_REGWEN, 0xe4u)
REG32(BANK0_INFO1_PAGE_CFG, 0xe8u)
REG32(BANK0_INFO2_REGWEN_0, 0xecu)
REG32(BANK0_INFO2_REGWEN_1, 0xf0u)
REG32(BANK0_INFO2_PAGE_CFG_0, 0xf4u)
REG32(BANK0_INFO2_PAGE_CFG_1, 0xf8u)
REG32(BANK1_INFO0_REGWEN_0, 0xfcu)
REG32(BANK1_INFO0_REGWEN_1, 0x100u)
REG32(BANK1_INFO0_REGWEN_2, 0x104u)
REG32(BANK1_INFO0_REGWEN_3, 0x108u)
REG32(BANK1_INFO0_REGWEN_4, 0x10cu)
REG32(BANK1_INFO0_REGWEN_5, 0x110u)
REG32(BANK1_INFO0_REGWEN_6, 0x114u)
REG32(BANK1_INFO0_REGWEN_7, 0x118u)
REG32(BANK1_INFO0_REGWEN_8, 0x11cu)
REG32(BANK1_INFO0_REGWEN_9, 0x120u)
REG32(BANK1_INFO0_PAGE_CFG_0, 0x124u)
REG32(BANK1_INFO0_PAGE_CFG_1, 0x128u)
REG32(BANK1_INFO0_PAGE_CFG_2, 0x12cu)
REG32(BANK1_INFO0_PAGE_CFG_3, 0x130u)
REG32(BANK1_INFO0_PAGE_CFG_4, 0x134u)
REG32(BANK1_INFO0_PAGE_CFG_5, 0x138u)
REG32(BANK1_INFO0_PAGE_CFG_6, 0x13cu)
REG32(BANK1_INFO0_PAGE_CFG_7, 0x140u)
REG32(BANK1_INFO0_PAGE_CFG_8, 0x144u)
REG32(BANK1_INFO0_PAGE_CFG_9, 0x148u)
REG32(BANK1_INFO1_REGWEN, 0x14cu)
REG32(BANK1_INFO1_PAGE_CFG, 0x150u)
REG32(BANK1_INFO2_REGWEN_0, 0x154u)
REG32(BANK1_INFO2_REGWEN_1, 0x158u)
REG32(BANK1_INFO2_PAGE_CFG_0, 0x15cu)
REG32(BANK1_INFO2_PAGE_CFG_1, 0x160u)
REG32(HW_INFO_CFG_OVERRIDE, 0x164u)
    FIELD(HW_INFO_CFG_OVERRIDE, SCRAMBLE_DIS, 0u, 4u)
    FIELD(HW_INFO_CFG_OVERRIDE, ECC_DIS, 4u, 4u)
REG32(BANK_CFG_REGWEN, 0x168u)
REG32(MP_BANK_CFG_SHADOWED, 0x16cu)
    FIELD(MP_BANK_CFG_SHADOWED, ERASE_EN_0, 0u, 1u)
    FIELD(MP_BANK_CFG_SHADOWED, ERASE_EN_1, 1u, 1u)
REG32(OP_STATUS, 0x170u)
    FIELD(OP_STATUS, DONE, 0u, 1u)
    FIELD(OP_STATUS, ERR, 1u, 1u)
REG32(STATUS, 0x174u)
    FIELD(STATUS, RD_FULL, 0u, 1u)
    FIELD(STATUS, RD_EMPTY, 1u, 1u)
    FIELD(STATUS, PROG_FULL, 2u, 1u)
    FIELD(STATUS, PROG_EMPTY, 3u, 1u)
    FIELD(STATUS, INIT_WIP, 4u, 1u)
    FIELD(STATUS, INITIALIZED, 5u, 1u)
REG32(DEBUG_STATE, 0x178u)
    FIELD(DEBUG_STATE, LCMGR_STATE_MASK, 0u, 11u)
REG32(ERR_CODE, 0x17cu)
    FIELD(ERR_CODE, OP_ERR, 0u, 1u)
    FIELD(ERR_CODE, MP_ERR, 1u, 1u)
    FIELD(ERR_CODE, RD_ERR, 2u, 1u)
    FIELD(ERR_CODE, PROG_ERR, 3u, 1u)
    FIELD(ERR_CODE, PROG_WIN_ERR, 4u, 1u)
    FIELD(ERR_CODE, PROG_TYPE_ERR, 5u, 1u)
    FIELD(ERR_CODE, UPDATE_ERR, 6u, 1u)
    FIELD(ERR_CODE, MACRO_ERR, 7u, 1u)
REG32(STD_FAULT_STATUS, 0x180u)
    FIELD(STD_FAULT_STATUS, REG_INTG_ERR, 0u, 1u)
    FIELD(STD_FAULT_STATUS, PROG_INTG_ERR, 1u, 1u)
    FIELD(STD_FAULT_STATUS, LCMGR_ERR, 2u, 1u)
    FIELD(STD_FAULT_STATUS, LCMGR_INTG_ERR, 3u, 1u)
    FIELD(STD_FAULT_STATUS, ARB_FSM_ERR, 4u, 1u)
    FIELD(STD_FAULT_STATUS, STORAGE_ERR, 5u, 1u)
    FIELD(STD_FAULT_STATUS, PHY_FSM_ERR, 6u, 1u)
    FIELD(STD_FAULT_STATUS, CTRL_CNT_ERR, 7u, 1u)
    FIELD(STD_FAULT_STATUS, FIFO_ERR, 8u, 1u)
REG32(FAULT_STATUS, 0x184u)
    FIELD(FAULT_STATUS, OP_ERR, 0u, 1u)
    FIELD(FAULT_STATUS, MP_ERR, 1u, 1u)
    FIELD(FAULT_STATUS, RD_ERR, 2u, 1u)
    FIELD(FAULT_STATUS, PROG_ERR, 3u, 1u)
    FIELD(FAULT_STATUS, PROG_WIN_ERR, 4u, 1u)
    FIELD(FAULT_STATUS, PROG_TYPE_ERR, 5u, 1u)
    FIELD(FAULT_STATUS, SEED_ERR, 6u, 1u)
    FIELD(FAULT_STATUS, PHY_RELBL_ERR, 7u, 1u)
    FIELD(FAULT_STATUS, PHY_STORAGE_ERR, 8u, 1u)
    FIELD(FAULT_STATUS, SPURIOUS_ACK, 9u, 1u)
    FIELD(FAULT_STATUS, ARB_ERR, 10u, 1u)
    FIELD(FAULT_STATUS, HOST_GNT_ERR, 11u, 1u)
REG32(ERR_ADDR, 0x188u)
    FIELD(ERR_ADDR, ERR_ADDR, 0u, 20u)
REG32(ECC_SINGLE_ERR_CNT, 0x18cu)
    FIELD(ECC_SINGLE_ERR_CNT, CNT_0, 0u, 8u)
    FIELD(ECC_SINGLE_ERR_CNT, CNT_1, 8u, 8u)
REG32(ECC_SINGLE_ERR_ADDR_0, 0x190u)
    SHARED_FIELD(ECC_SINGLE_ERR_ADDR, 0u, 20u)
REG32(ECC_SINGLE_ERR_ADDR_1, 0x194u)
REG32(PHY_ALERT_CFG, 0x198u)
    FIELD(PHY_ALERT_CFG, ALERT_ACK, 0u, 1u)
    FIELD(PHY_ALERT_CFG, ALERT_TRIG, 1u, 1u)
REG32(PHY_STATUS, 0x19cu)
    FIELD(PHY_STATUS, INIT_WIP, 0u, 1u)
    FIELD(PHY_STATUS, PROG_NORMAL_AVAIL, 1u, 1u)
    FIELD(PHY_STATUS, PROG_REPAIR_AVAIL, 2u, 1u)
REG32(SCRATCH, 0x1a0u)
REG32(FIFO_LVL, 0x1a4u)
    SHARED_FIELD(FIFO_LVL_PROG, 0u, 5u)
    SHARED_FIELD(FIFO_LVL_RD, 8u, 5u)
REG32(FIFO_RST, 0x1a8u)
    FIELD(FIFO_RST, EN, 0u, 1u)
REG32(CURR_FIFO_LVL, 0x1acu)
REG32(PROG_FIFO, 0x1b0u)
REG32(RD_FIFO, 0x1b4u)

#define INTR_MASK \
    (INTR_PROG_EMPTY_MASK | \
     INTR_PROG_LVL_MASK | \
     INTR_RD_FULL_MASK | \
     INTR_RD_LVL_MASK | \
     INTR_OP_DONE_MASK | \
     INTR_CORR_ERR_MASK)
#define ALERT_MASK \
    (ALERT_RECOV_ERR_MASK | \
     ALERT_FATAL_STD_ERR_MASK | \
     ALERT_FATAL_ERR_MASK | \
     ALERT_FATAL_PRIM_MASK | \
     ALERT_RECOV_PRIM_MASK)
#define BANK_INFO_PAGE_CFG_MASK \
    (BANK_INFO_PAGE_CFG_EN_MASK | \
     BANK_INFO_PAGE_CFG_RD_EN_MASK | \
     BANK_INFO_PAGE_CFG_PROG_EN_MASK | \
     BANK_INFO_PAGE_CFG_ERASE_EN_MASK | \
     BANK_INFO_PAGE_CFG_SCRAMBLE_EN_MASK | \
     BANK_INFO_PAGE_CFG_ECC_EN_MASK | \
     BANK_INFO_PAGE_CFG_HE_EN_MASK)
#define MP_REGION_CFG_MASK \
    (MP_REGION_CFG_EN_MASK | \
     MP_REGION_CFG_RD_EN_MASK | \
     MP_REGION_CFG_PROG_EN_MASK | \
     MP_REGION_CFG_ERASE_EN_MASK | \
     MP_REGION_CFG_SCRAMBLE_EN_MASK | \
     MP_REGION_CFG_ECC_EN_MASK | \
     MP_REGION_CFG_HE_EN_MASK)
#define MP_REGION_MASK \
     (MP_REGION_BASE_MASK | \
      MP_REGION_SIZE_MASK)
#define DEFAULT_REGION_MASK \
    (R_DEFAULT_REGION_RD_EN_MASK | \
     R_DEFAULT_REGION_PROG_EN_MASK | \
     R_DEFAULT_REGION_ERASE_EN_MASK | \
     R_DEFAULT_REGION_SCRAMBLE_EN_MASK | \
     R_DEFAULT_REGION_ECC_EN_MASK | \
     R_DEFAULT_REGION_HE_EN_MASK)
#define CONTROL_MASK \
    (R_CONTROL_START_MASK | \
     R_CONTROL_OP_MASK | \
     R_CONTROL_PROG_SEL_MASK | \
     R_CONTROL_ERASE_SEL_MASK | \
     R_CONTROL_PARTITION_SEL_MASK | \
     R_CONTROL_INFO_SEL_MASK | \
     R_CONTROL_NUM_MASK);
#define ERR_CODE_MASK \
    (R_ERR_CODE_OP_ERR_MASK | \
     R_ERR_CODE_MP_ERR_MASK | \
     R_ERR_CODE_RD_ERR_MASK | \
     R_ERR_CODE_PROG_ERR_MASK | \
     R_ERR_CODE_PROG_WIN_ERR_MASK | \
     R_ERR_CODE_PROG_TYPE_ERR_MASK | \
     R_ERR_CODE_UPDATE_ERR_MASK | \
     R_ERR_CODE_MACRO_ERR_MASK)

REG32(CSR0_REGWEN, 0x0u)
    FIELD(CSR0_REGWEN, FIELD0, 0u, 1u)
REG32(CSR1, 0x4u)
    FIELD(CSR1, FIELD0, 0u, 8u)
    FIELD(CSR1, FIELD1, 8u, 5u)
REG32(CSR2, 0x8u)
    FIELD(CSR2, FIELD0, 0u, 1u)
    FIELD(CSR2, FIELD1, 1u, 1u)
    FIELD(CSR2, FIELD2, 2u, 1u)
    FIELD(CSR2, FIELD3, 3u, 1u)
    FIELD(CSR2, FIELD4, 4u, 1u)
    FIELD(CSR2, FIELD5, 5u, 1u)
    FIELD(CSR2, FIELD6, 6u, 1u)
    FIELD(CSR2, FIELD7, 7u, 1u)
REG32(CSR3, 0xcu)
    FIELD(CSR3, FIELD0, 0u, 4u)
    FIELD(CSR3, FIELD1, 4u, 4u)
    FIELD(CSR3, FIELD2, 8u, 3u)
    FIELD(CSR3, FIELD3, 11u, 3u)
    FIELD(CSR3, FIELD4, 14u, 3u)
    FIELD(CSR3, FIELD5, 17u, 3u)
    FIELD(CSR3, FIELD6, 20u, 1u)
    FIELD(CSR3, FIELD7, 21u, 3u)
    FIELD(CSR3, FIELD8, 24u, 2u)
    FIELD(CSR3, FIELD9, 26u, 2u)
REG32(CSR4, 0x10u)
    FIELD(CSR4, FIELD0, 0u, 3u)
    FIELD(CSR4, FIELD1, 3u, 3u)
    FIELD(CSR4, FIELD2, 6u, 3u)
    FIELD(CSR4, FIELD3, 9u, 3u)
REG32(CSR5, 0x14u)
    FIELD(CSR5, FIELD0, 0u, 3u)
    FIELD(CSR5, FIELD1, 3u, 2u)
    FIELD(CSR5, FIELD2, 5u, 9u)
    FIELD(CSR5, FIELD3, 14u, 5u)
    FIELD(CSR5, FIELD4, 19u, 4u)
REG32(CSR6, 0x18u)
    FIELD(CSR6, FIELD0, 0u, 3u)
    FIELD(CSR6, FIELD1, 3u, 3u)
    FIELD(CSR6, FIELD2, 6u, 8u)
    FIELD(CSR6, FIELD3, 14u, 3u)
    FIELD(CSR6, FIELD4, 17u, 2u)
    FIELD(CSR6, FIELD5, 19u, 2u)
    FIELD(CSR6, FIELD6, 21u, 2u)
    FIELD(CSR6, FIELD7, 23u, 1u)
    FIELD(CSR6, FIELD8, 24u, 1u)
REG32(CSR7, 0x1cu)
    FIELD(CSR7, FIELD0, 0u, 8u)
    FIELD(CSR7, FIELD1, 8u, 9u)
REG32(CSR8, 0x20u)
REG32(CSR9, 0x24u)
REG32(CSR10, 0x28u)
REG32(CSR11, 0x2cu)
REG32(CSR12, 0x30u)
    FIELD(CSR12, FIELD0, 0u, 10u)
REG32(CSR13, 0x34u)
    FIELD(CSR13, FIELD0, 0u, 20u)
    FIELD(CSR13, FIELD1, 20u, 1u)
REG32(CSR14, 0x38u)
    FIELD(CSR14, FIELD0, 0u, 8u)
    FIELD(CSR14, FIELD1, 8u, 1u)
REG32(CSR15, 0x3cu)
    FIELD(CSR15, FIELD0, 0u, 8u)
    FIELD(CSR15, FIELD1, 8u, 1u)
REG32(CSR16, 0x40u)
    FIELD(CSR16, FIELD0, 0u, 8u)
    FIELD(CSR16, FIELD1, 8u, 1u)
REG32(CSR17, 0x44u)
    FIELD(CSR17, FIELD0, 0u, 8u)
    FIELD(CSR17, FIELD1, 8u, 1u)
REG32(CSR18, 0x48u)
    FIELD(CSR18, FIELD0, 0u, 1u)
REG32(CSR19, 0x4cu)
    FIELD(CSR19, FIELD0, 0u, 1u)
REG32(CSR20, 0x50u)
    FIELD(CSR20, FIELD0, 0u, 1u)
    FIELD(CSR20, FIELD1, 1u, 1u)
    FIELD(CSR20, FIELD2, 2u, 1u)

#define REG_NUM_BANKS 2u
#define REG_PAGES_PER_BANK 256u
#define REG_BUS_PGM_RES_BYTES 64u
#define REG_PAGE_WIDTH 8u
#define REG_BANK_WIDTH 1u
#define NUM_REGIONS 8u
#define NUM_INFO_TYPES 3u
#define NUM_INFOS0 10u
#define NUM_INFOS1 1u
#define NUM_INFOS2 2u
#define WORDS_PER_PAGE 256u
#define BYTES_PER_WORD 8u
#define BYTES_PER_PAGE 0x800u /* 2048u */
#define BYTES_PER_BANK 0x80000u /* 524288u */
#define EXEC_EN 0xa26a38f7u /* 2724870391u */
#define MAX_FIFO_DEPTH 16u
#define MAX_FIFO_WIDTH 5u
#define MAX_INFO_PART_COUNT 12u
#define NUM_ALERTS 5u

/* clang-format on */

typedef enum {
    LC_PHASE_SEED,
    LC_PHASE_RMA,
    LC_PHASE_NONE,
    LC_PHASE_INVALID
} OtFlashLifeCyclePhase;

/*
 * todo: init is a command handled by the lcmgr and should not be treated the
 * same as other ops (each SW-commanded `INIT` operation can lead to multiple
 * HW flash program operations for example).
 *
 * this enum should be removed and everything using it should be refactored
 * to directly use s->op.kind (OtFlashControlOperation) instead.
 */
typedef enum {
    OP_NONE,
    OP_INIT,
    OP_READ,
    OP_PROG,
    OP_ERASE,
} OtFlashOperation;

typedef enum {
    CONTROL_OP_READ = 0x0,
    CONTROL_OP_PROG = 0x1,
    CONTROL_OP_ERASE = 0x2,
} OtFlashControlOperation;

typedef enum {
    ERASE_SEL_PAGE = 0x0,
    ERASE_SEL_BANK = 0x1,
} OtFlashControlEraseSelection;

typedef enum {
    PROG_SEL_NORMAL = 0x0,
    PROG_SEL_REPAIR = 0x1,
} OtFlashControlProgramSelection;

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_RD_FIFO)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    REG_NAME_ENTRY(INTR_STATE),
    REG_NAME_ENTRY(INTR_ENABLE),
    REG_NAME_ENTRY(INTR_TEST),
    REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(DIS),
    REG_NAME_ENTRY(EXEC),
    REG_NAME_ENTRY(INIT),
    REG_NAME_ENTRY(CTRL_REGWEN),
    REG_NAME_ENTRY(CONTROL),
    REG_NAME_ENTRY(ADDR),
    REG_NAME_ENTRY(PROG_TYPE_EN),
    REG_NAME_ENTRY(ERASE_SUSPEND),
    REG_NAME_ENTRY(REGION_CFG_REGWEN_0),
    REG_NAME_ENTRY(REGION_CFG_REGWEN_1),
    REG_NAME_ENTRY(REGION_CFG_REGWEN_2),
    REG_NAME_ENTRY(REGION_CFG_REGWEN_3),
    REG_NAME_ENTRY(REGION_CFG_REGWEN_4),
    REG_NAME_ENTRY(REGION_CFG_REGWEN_5),
    REG_NAME_ENTRY(REGION_CFG_REGWEN_6),
    REG_NAME_ENTRY(REGION_CFG_REGWEN_7),
    REG_NAME_ENTRY(MP_REGION_CFG_0),
    REG_NAME_ENTRY(MP_REGION_CFG_1),
    REG_NAME_ENTRY(MP_REGION_CFG_2),
    REG_NAME_ENTRY(MP_REGION_CFG_3),
    REG_NAME_ENTRY(MP_REGION_CFG_4),
    REG_NAME_ENTRY(MP_REGION_CFG_5),
    REG_NAME_ENTRY(MP_REGION_CFG_6),
    REG_NAME_ENTRY(MP_REGION_CFG_7),
    REG_NAME_ENTRY(MP_REGION_0),
    REG_NAME_ENTRY(MP_REGION_1),
    REG_NAME_ENTRY(MP_REGION_2),
    REG_NAME_ENTRY(MP_REGION_3),
    REG_NAME_ENTRY(MP_REGION_4),
    REG_NAME_ENTRY(MP_REGION_5),
    REG_NAME_ENTRY(MP_REGION_6),
    REG_NAME_ENTRY(MP_REGION_7),
    REG_NAME_ENTRY(DEFAULT_REGION),
    REG_NAME_ENTRY(BANK0_INFO0_REGWEN_0),
    REG_NAME_ENTRY(BANK0_INFO0_REGWEN_1),
    REG_NAME_ENTRY(BANK0_INFO0_REGWEN_2),
    REG_NAME_ENTRY(BANK0_INFO0_REGWEN_3),
    REG_NAME_ENTRY(BANK0_INFO0_REGWEN_4),
    REG_NAME_ENTRY(BANK0_INFO0_REGWEN_5),
    REG_NAME_ENTRY(BANK0_INFO0_REGWEN_6),
    REG_NAME_ENTRY(BANK0_INFO0_REGWEN_7),
    REG_NAME_ENTRY(BANK0_INFO0_REGWEN_8),
    REG_NAME_ENTRY(BANK0_INFO0_REGWEN_9),
    REG_NAME_ENTRY(BANK0_INFO0_PAGE_CFG_0),
    REG_NAME_ENTRY(BANK0_INFO0_PAGE_CFG_1),
    REG_NAME_ENTRY(BANK0_INFO0_PAGE_CFG_2),
    REG_NAME_ENTRY(BANK0_INFO0_PAGE_CFG_3),
    REG_NAME_ENTRY(BANK0_INFO0_PAGE_CFG_4),
    REG_NAME_ENTRY(BANK0_INFO0_PAGE_CFG_5),
    REG_NAME_ENTRY(BANK0_INFO0_PAGE_CFG_6),
    REG_NAME_ENTRY(BANK0_INFO0_PAGE_CFG_7),
    REG_NAME_ENTRY(BANK0_INFO0_PAGE_CFG_8),
    REG_NAME_ENTRY(BANK0_INFO0_PAGE_CFG_9),
    REG_NAME_ENTRY(BANK0_INFO1_REGWEN),
    REG_NAME_ENTRY(BANK0_INFO1_PAGE_CFG),
    REG_NAME_ENTRY(BANK0_INFO2_REGWEN_0),
    REG_NAME_ENTRY(BANK0_INFO2_REGWEN_1),
    REG_NAME_ENTRY(BANK0_INFO2_PAGE_CFG_0),
    REG_NAME_ENTRY(BANK0_INFO2_PAGE_CFG_1),
    REG_NAME_ENTRY(BANK1_INFO0_REGWEN_0),
    REG_NAME_ENTRY(BANK1_INFO0_REGWEN_1),
    REG_NAME_ENTRY(BANK1_INFO0_REGWEN_2),
    REG_NAME_ENTRY(BANK1_INFO0_REGWEN_3),
    REG_NAME_ENTRY(BANK1_INFO0_REGWEN_4),
    REG_NAME_ENTRY(BANK1_INFO0_REGWEN_5),
    REG_NAME_ENTRY(BANK1_INFO0_REGWEN_6),
    REG_NAME_ENTRY(BANK1_INFO0_REGWEN_7),
    REG_NAME_ENTRY(BANK1_INFO0_REGWEN_8),
    REG_NAME_ENTRY(BANK1_INFO0_REGWEN_9),
    REG_NAME_ENTRY(BANK1_INFO0_PAGE_CFG_0),
    REG_NAME_ENTRY(BANK1_INFO0_PAGE_CFG_1),
    REG_NAME_ENTRY(BANK1_INFO0_PAGE_CFG_2),
    REG_NAME_ENTRY(BANK1_INFO0_PAGE_CFG_3),
    REG_NAME_ENTRY(BANK1_INFO0_PAGE_CFG_4),
    REG_NAME_ENTRY(BANK1_INFO0_PAGE_CFG_5),
    REG_NAME_ENTRY(BANK1_INFO0_PAGE_CFG_6),
    REG_NAME_ENTRY(BANK1_INFO0_PAGE_CFG_7),
    REG_NAME_ENTRY(BANK1_INFO0_PAGE_CFG_8),
    REG_NAME_ENTRY(BANK1_INFO0_PAGE_CFG_9),
    REG_NAME_ENTRY(BANK1_INFO1_REGWEN),
    REG_NAME_ENTRY(BANK1_INFO1_PAGE_CFG),
    REG_NAME_ENTRY(BANK1_INFO2_REGWEN_0),
    REG_NAME_ENTRY(BANK1_INFO2_REGWEN_1),
    REG_NAME_ENTRY(BANK1_INFO2_PAGE_CFG_0),
    REG_NAME_ENTRY(BANK1_INFO2_PAGE_CFG_1),
    REG_NAME_ENTRY(HW_INFO_CFG_OVERRIDE),
    REG_NAME_ENTRY(BANK_CFG_REGWEN),
    REG_NAME_ENTRY(MP_BANK_CFG_SHADOWED),
    REG_NAME_ENTRY(OP_STATUS),
    REG_NAME_ENTRY(STATUS),
    REG_NAME_ENTRY(DEBUG_STATE),
    REG_NAME_ENTRY(ERR_CODE),
    REG_NAME_ENTRY(STD_FAULT_STATUS),
    REG_NAME_ENTRY(FAULT_STATUS),
    REG_NAME_ENTRY(ERR_ADDR),
    REG_NAME_ENTRY(ECC_SINGLE_ERR_CNT),
    REG_NAME_ENTRY(ECC_SINGLE_ERR_ADDR_0),
    REG_NAME_ENTRY(ECC_SINGLE_ERR_ADDR_1),
    REG_NAME_ENTRY(PHY_ALERT_CFG),
    REG_NAME_ENTRY(PHY_STATUS),
    REG_NAME_ENTRY(SCRATCH),
    REG_NAME_ENTRY(FIFO_LVL),
    REG_NAME_ENTRY(FIFO_RST),
    REG_NAME_ENTRY(CURR_FIFO_LVL),
    REG_NAME_ENTRY(PROG_FIFO),
    REG_NAME_ENTRY(RD_FIFO),
};
#undef REG_NAME_ENTRY

#define R_LAST_CSR (R_CSR20)
#define CSRS_COUNT (R_LAST_CSR + 1u)
#define CSRS_SIZE  (CSRS_COUNT * sizeof(uint32_t))
#define CSR_NAME(_reg_) \
    ((((_reg_) <= CSRS_COUNT) && CSR_NAMES[_reg_]) ? CSR_NAMES[_reg_] : "?")

#define CSR_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *CSR_NAMES[CSRS_COUNT] = {
    CSR_NAME_ENTRY(CSR0_REGWEN), CSR_NAME_ENTRY(CSR1),  CSR_NAME_ENTRY(CSR2),
    CSR_NAME_ENTRY(CSR3),        CSR_NAME_ENTRY(CSR4),  CSR_NAME_ENTRY(CSR5),
    CSR_NAME_ENTRY(CSR6),        CSR_NAME_ENTRY(CSR7),  CSR_NAME_ENTRY(CSR8),
    CSR_NAME_ENTRY(CSR9),        CSR_NAME_ENTRY(CSR10), CSR_NAME_ENTRY(CSR11),
    CSR_NAME_ENTRY(CSR12),       CSR_NAME_ENTRY(CSR13), CSR_NAME_ENTRY(CSR14),
    CSR_NAME_ENTRY(CSR15),       CSR_NAME_ENTRY(CSR16), CSR_NAME_ENTRY(CSR17),
    CSR_NAME_ENTRY(CSR18),       CSR_NAME_ENTRY(CSR19), CSR_NAME_ENTRY(CSR20),
};
#undef CSR_NAME_ENTRY

#define FLASH_NAME_ENTRY(_st_) [_st_] = stringify(_st_)

static const char *LC_PHASE_NAMES[] = {
    FLASH_NAME_ENTRY(LC_PHASE_SEED),
    FLASH_NAME_ENTRY(LC_PHASE_RMA),
    FLASH_NAME_ENTRY(LC_PHASE_NONE),
    FLASH_NAME_ENTRY(LC_PHASE_INVALID),
};

static const char *OP_NAMES[] = {
    FLASH_NAME_ENTRY(OP_NONE),  FLASH_NAME_ENTRY(OP_INIT),
    FLASH_NAME_ENTRY(OP_READ),  FLASH_NAME_ENTRY(OP_PROG),
    FLASH_NAME_ENTRY(OP_ERASE),
};

static const char *CONTROL_OP_NAMES[] = {
    FLASH_NAME_ENTRY(CONTROL_OP_READ),
    FLASH_NAME_ENTRY(CONTROL_OP_PROG),
    FLASH_NAME_ENTRY(CONTROL_OP_ERASE),
};

static const char *ERASE_SELECTION_NAMES[] = {
    FLASH_NAME_ENTRY(ERASE_SEL_PAGE),
    FLASH_NAME_ENTRY(ERASE_SEL_BANK),
};

static const char *PROGRAM_SELECTION_NAMES[] = {
    FLASH_NAME_ENTRY(PROG_SEL_NORMAL),
    FLASH_NAME_ENTRY(PROG_SEL_REPAIR),
};

#undef FLASH_NAME_ENTRY

#define LC_PHASE_NAME(_st_) \
    (((unsigned)(_st_)) < ARRAY_SIZE(LC_PHASE_NAMES) ? \
         LC_PHASE_NAMES[(_st_)] : \
         "?")

#define OP_NAME(_st_) \
    (((unsigned)(_st_)) < ARRAY_SIZE(OP_NAMES) ? OP_NAMES[(_st_)] : "?")

#define CONTROL_OP_NAME(_st_) \
    ((unsigned)(_st_) < ARRAY_SIZE(CONTROL_OP_NAMES) ? \
         CONTROL_OP_NAMES[(_st_)] : \
         "?")

#define ERASE_NAME(_st_) \
    ((unsigned)(_st_) < ARRAY_SIZE(ERASE_SELECTION_NAMES) ? \
         ERASE_SELECTION_NAMES[(_st_)] : \
         "?")

#define PROGRAM_NAME(_st_) \
    ((unsigned)(_st_) < ARRAY_SIZE(PROGRAM_SELECTION_NAMES) ? \
         PROGRAM_SELECTION_NAMES[(_st_)] : \
         "?")

/**
 * Bank 0 information partition type 0 pages.
 *
 * (InfoPageFactoryId,            0x9dc41c33, 0, 0)
 * (InfoPageCreatorSecret,        0xf56af4bb, 0, 1)
 * (InfoPageOwnerSecret,          0x10adc6aa, 0, 2)
 * (InfoPageWaferAuthSecret,      0x118b5dbb, 0, 3)
 * (InfoPageBank0Type0Page4,      0xad3b5bee, 0, 4)
 * (InfoPageBank0Type0Page5,      0xa4f6f6c3, 0, 5)
 * (InfoPageOwnerReserved0,       0xf646f11b, 0, 6)
 * (InfoPageOwnerReserved1,       0x6c86d980, 0, 7)
 * (InfoPageOwnerReserved2,       0xdd7f34dc, 0, 8)
 * (InfoPageOwnerReserved3,       0x5f07277e, 0, 9)
 *
 * Bank 1 information partition type 0 pages.
 *
 * (InfoPageBootData0,            0xfa38c9f6, 1, 0)
 * (InfoPageBootData1,            0x389c449e, 1, 1)
 * (InfoPageOwnerSlot0,           0x238cf15c, 1, 2)
 * (InfoPageOwnerSlot1,           0xad886d3b, 1, 3)
 * (InfoPageBank1Type0Page4,      0x7dfbdf9b, 1, 4)
 * (InfoPageBank1Type0Page5,      0xad5dd31d, 1, 5)
 * (InfoPageCreatorCertificate,   0xe3ffac86, 1, 6)
 * (InfoPageBootServices,         0xf4f48c3d, 1, 7)
 * (InfoPageOwnerCerificate0,     0x9fbb840e, 1, 8)
 * (InfoPageOwnerCerificate1,     0xec309461, 1, 9)
 */

#define xtrace_ot_flash_error(_msg_) \
    trace_ot_flash_error(__func__, __LINE__, _msg_)
#define xtrace_ot_flash_info(_msg_, _val_) \
    trace_ot_flash_info(__func__, __LINE__, _msg_, _val_)

enum {
    BIN_APP_OTRE,
    BIN_APP_OTB0,
    BIN_APP_COUNT,
};

#define OP_INIT_DURATION_NS     1000000u /* 1 ms */
#define ELFNAME_SIZE            256u
#define OT_FLASH_READ_FIFO_SIZE 16u
#define OT_FLASH_PROG_FIFO_SIZE 16u
#define BUS_PGM_RES             ((REG_BUS_PGM_RES_BYTES) / (OT_TL_UL_D_WIDTH_BYTES))

typedef struct {
    unsigned offset; /* storage offset in bank, relative to first info page */
    unsigned size; /* size in bytes of the partition */
} OtFlashInfoPart;

/**
 * See `ot_flash_update_info_page_qualification`. These pages have additional
 * restricted access based upon the current incoming lc_ctrl lifecycle signals.
 */
typedef enum {
    FLASH_QUAL_INFO_PAGE_CREATOR = 1,
    FLASH_QUAL_INFO_PAGE_OWNER = 2,
    FLASH_QUAL_INFO_PAGE_ISOLATED = 3,
} OtFlashQualifiedInfoPage;

typedef union {
    unsigned bitmap;
    struct {
        unsigned en:1;
        unsigned rd_en:1;
        unsigned prog_en:1;
        unsigned erase_en:1;
        unsigned scramble_en:1;
        unsigned ecc_en:1;
        unsigned he_en:1;
    };
} OtFlashPropertyCfg;

static_assert(sizeof(OtFlashPropertyCfg) == sizeof(unsigned int),
              "The flash property config should fit in an unsigned int.");

typedef struct {
    uint32_t *storage; /* overall buffer for the storage backend */
    uint32_t *data; /* data buffer (all partitions/banks) */
    uint32_t *info; /* info buffer (all partitions/banks) */
    unsigned bank_count; /* count of banks */
    unsigned size; /* overall storage size in bytes (excl. header) */
    unsigned data_size; /* data buffer size of a bank in bytes */
    unsigned info_size; /* info buffer size of a bank in bytes */
    unsigned info_part_count; /* count of info partition (per bank) */
    OtFlashInfoPart info_parts[MAX_INFO_PART_COUNT];
} OtFlashStorage;

typedef struct {
    char magic[4u]; /* vFSH */
    uint32_t hlength; /* count of header bytes after this point */
    uint32_t version; /* version of the header */
    uint8_t bank; /* count of bank */
    uint8_t info; /* count of info partitions per bank */
    uint16_t page; /* count of pages per bank */
    uint32_t psize; /* page size in bytes */
    uint8_t ipp[MAX_INFO_PART_COUNT]; /* count of pages for each info part */
} OtFlashBackendHeader;

typedef struct {
    uint32_t *data;
    uint32_t capacity;
    uint32_t head;
    uint32_t num;
} OtFlashFifo;

struct OtFlashState {
    SysBusDevice parent_obj;

    struct {
        MemoryRegion regs;
        MemoryRegion csrs;
        MemoryRegion mem;
    } mmio;
    QEMUTimer *op_delay; /* simulated long lasting operation */
    IbexIRQ irqs[PARAM_NUM_IRQS];
    IbexIRQ alerts[PARAM_NUM_ALERTS];

    uint32_t *regs;
    uint32_t *csrs;

    /* "sticky" alerts that should stay signaled after firing */
    uint32_t latched_alerts;

    struct {
        OtFlashOperation kind;
        unsigned count;
        unsigned remaining;
        unsigned address;
        unsigned info_sel;
        bool info_part;
        bool prog_sel;
        bool erase_sel;
        bool failed;
        bool hw; /* hw- or sw-requested operation? */
    } op;
    OtFlashLifeCyclePhase phase; /* HW LC phase for memory protection / RMA */
    OtFifo32 rd_fifo;
    OtFifo32 prog_fifo;
    OtFlashStorage flash;

    /*
     * HW and SW operations share a program FIFO, but read to separate
     * locations (e.g. keymgr keys, RMA requests).
     */
    OtFifo32 hw_rd_fifo;

    BlockBackend *blk; /* Flash backend */
    OtVMapperState *vmapper; /* to disable execution from flash */
    bool no_mem_prot; /* Flag to disable mem protection features */
};

/* Flash memory protection rules */

static const OtFlashPropertyCfg OT_FLASH_CFG_ALLOW_READ = {
    .en = true,
    .rd_en = true,
    .prog_en = false,
    .erase_en = false,
    .scramble_en = true,
    .ecc_en = true,
    .he_en = true,
};

static const OtFlashPropertyCfg OT_FLASH_CFG_ALLOW_READ_PROG_ERASE = {
    .en = true,
    .rd_en = true,
    .prog_en = true,
    .erase_en = true,
    .scramble_en = true,
    .ecc_en = true,
    .he_en = true,
};

static const OtFlashPropertyCfg OT_FLASH_CFG_INFO_DISABLE = {
    .en = false,
    .rd_en = false,
    .prog_en = false,
    .erase_en = false,
    .scramble_en = false,
    .ecc_en = false,
    .he_en = false,
};

typedef struct {
    /* the page to give HW access to */
    unsigned bank;
    unsigned info_partition;
    unsigned page;
    OtFlashLifeCyclePhase phase; /* the phase this rule applies in */
    OtFlashPropertyCfg cfg; /* cfg to apply to the page */
} OtFlashHwInfoPageRule;

static const OtFlashHwInfoPageRule OT_FLASH_HW_INFO_PAGE_RULES[] = {
    {
        .bank = FLASH_SEED_BANK,
        .info_partition = FLASH_SEED_INFO_PARTITION,
        .page = FLASH_QUAL_INFO_PAGE_CREATOR,
        .phase = LC_PHASE_SEED,
        .cfg = OT_FLASH_CFG_ALLOW_READ,
    },
    {
        .bank = FLASH_SEED_BANK,
        .info_partition = FLASH_SEED_INFO_PARTITION,
        .page = FLASH_QUAL_INFO_PAGE_OWNER,
        .phase = LC_PHASE_SEED,
        .cfg = OT_FLASH_CFG_ALLOW_READ,
    },
    {
        .bank = FLASH_SEED_BANK,
        .info_partition = FLASH_SEED_INFO_PARTITION,
        .page = FLASH_QUAL_INFO_PAGE_CREATOR,
        .phase = LC_PHASE_RMA,
        .cfg = OT_FLASH_CFG_ALLOW_READ_PROG_ERASE,
    },
    {
        .bank = FLASH_SEED_BANK,
        .info_partition = FLASH_SEED_INFO_PARTITION,
        .page = FLASH_QUAL_INFO_PAGE_OWNER,
        .phase = LC_PHASE_RMA,
        .cfg = OT_FLASH_CFG_ALLOW_READ_PROG_ERASE,
    },
    {
        .bank = FLASH_SEED_BANK,
        .info_partition = FLASH_SEED_INFO_PARTITION,
        .page = FLASH_QUAL_INFO_PAGE_ISOLATED,
        .phase = LC_PHASE_RMA,
        .cfg = OT_FLASH_CFG_ALLOW_READ_PROG_ERASE,
    },
};

struct OtFlashClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

static void ot_flash_update_irqs(OtFlashState *s)
{
    uint32_t level = s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE];
    trace_ot_flash_irqs(s->regs[R_INTR_STATE], s->regs[R_INTR_ENABLE], level);
    for (unsigned ix = 0u; ix < PARAM_NUM_IRQS; ix++) {
        ibex_irq_set(&s->irqs[ix], (int)((level >> ix) & 0x1u));
    }
}

static void ot_flash_update_alerts(OtFlashState *s)
{
    uint32_t levels = s->regs[R_ALERT_TEST];

    levels |= s->latched_alerts;

    for (unsigned ix = 0; ix < ARRAY_SIZE(s->alerts); ix++) {
        int level = (int)((levels >> ix) & 0x1u);
        if (level != ibex_irq_get_level(&s->alerts[ix])) {
            trace_ot_flash_update_alert(ibex_irq_get_level(&s->alerts[ix]),
                                        level);
        }
        ibex_irq_set(&s->alerts[ix], level);
    }

    /* alert test is transient */
    if (s->regs[R_ALERT_TEST]) {
        s->regs[R_ALERT_TEST] = 0;

        levels = s->latched_alerts;
        for (unsigned ix = 0; ix < ARRAY_SIZE(s->alerts); ix++) {
            int level = (int)((levels >> ix) & 0x1u);
            if (level != ibex_irq_get_level(&s->alerts[ix])) {
                trace_ot_flash_update_alert(ibex_irq_get_level(&s->alerts[ix]),
                                            level);
            }
            ibex_irq_set(&s->alerts[ix], level);
        }
    }
}

static bool ot_flash_is_backend_writable(const OtFlashState *s)
{
    return (s->blk != NULL) && blk_is_writable(s->blk);
}

static bool ot_flash_write_backend(OtFlashState *s, const void *buffer,
                                   unsigned offset, size_t size)
{
    /* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) */
    return blk_pwrite(s->blk, (int64_t)(intptr_t)offset, (int64_t)size, buffer,
                      (BdrvRequestFlags)0);
    /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */
}

static bool ot_flash_is_disabled(const OtFlashState *s)
{
    return s->regs[R_DIS] != OT_MULTIBITBOOL4_FALSE;
}

static bool ot_flash_regs_is_wr_enabled(const OtFlashState *s, unsigned regwen)
{
    return (bool)(s->regs[regwen] & REGWEN_EN_MASK);
}

static void ot_flash_update_rd_watermark(OtFlashState *s)
{
    unsigned rd_watermark_level =
        SHARED_FIELD_EX32(s->regs[R_FIFO_LVL], FIFO_LVL_RD);
    unsigned lvl = ot_fifo32_num_used(&s->rd_fifo);

    /* Read FIFO watermark generates an interrupt when the Read FIFO fills to
    (equal to or greater than) the watermark level. */
    if (lvl >= rd_watermark_level) {
        s->regs[R_INTR_STATE] |= INTR_RD_LVL_MASK;
    } else {
        s->regs[R_INTR_STATE] &= ~INTR_RD_LVL_MASK;
    }
    trace_ot_flash_update_rd_watermark(lvl, rd_watermark_level);
    ot_flash_update_irqs(s);
}

static void ot_flash_update_prog_watermark(OtFlashState *s)
{
    unsigned prog_watermark_level =
        SHARED_FIELD_EX32(s->regs[R_FIFO_LVL], FIFO_LVL_PROG);
    unsigned lvl = ot_fifo32_num_used(&s->prog_fifo);

    /* Prog FIFO watermark generates an interrupt when the Prog FIFO drains to
    (equal to or less than) the watermark level. */
    if (lvl <= prog_watermark_level) {
        s->regs[R_INTR_STATE] |= INTR_PROG_LVL_MASK;
    } else {
        s->regs[R_INTR_STATE] &= ~INTR_PROG_LVL_MASK;
    }
    trace_ot_flash_update_prog_watermark(lvl, prog_watermark_level);
    ot_flash_update_irqs(s);
}

static void ot_flash_init_complete(void *opaque)
{
    OtFlashState *s = opaque;

    s->regs[R_STATUS] = FIELD_DP32(s->regs[R_STATUS], STATUS, INIT_WIP, 0u);
    s->regs[R_STATUS] = FIELD_DP32(s->regs[R_STATUS], STATUS, INITIALIZED, 1u);
    s->regs[R_PHY_STATUS] =
        FIELD_DP32(s->regs[R_PHY_STATUS], PHY_STATUS, INIT_WIP, 0u);

    trace_ot_flash_op_complete(OP_NAME(s->op.kind), s->op.hw, true);

    s->op.kind = OP_NONE;
}

static bool ot_flash_fifo_in_reset(const OtFlashState *s)
{
    return (bool)s->regs[R_FIFO_RST];
}

static bool ot_flash_in_operation(const OtFlashState *s)
{
    return s->op.kind != OP_NONE;
}

static bool ot_flash_in_hw_operation(const OtFlashState *s)
{
    return s->op.kind != OP_NONE && s->op.hw;
}

static bool ot_flash_operation_ongoing(const OtFlashState *s)
{
    return s->op.kind != OP_NONE && s->op.count;
}

static void ot_flash_initialize(OtFlashState *s)
{
    if (ot_flash_in_operation(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: cannot initialize while in op",
                      __func__);
        return;
    }

    s->phase = LC_PHASE_SEED;
    trace_ot_flash_change_lc_phase(LC_PHASE_NAME(s->phase), s->phase);

    s->op.kind = OP_INIT;
    s->op.hw = false;
    trace_ot_flash_op_start(OP_NAME(s->op.kind), s->op.hw);
    s->regs[R_STATUS] = FIELD_DP32(s->regs[R_STATUS], STATUS, INIT_WIP, 1u);
    s->regs[R_PHY_STATUS] =
        FIELD_DP32(s->regs[R_PHY_STATUS], PHY_STATUS, INIT_WIP, 1u);
    timer_mod(s->op_delay,
              qemu_clock_get_ns(OT_VIRTUAL_CLOCK) + OP_INIT_DURATION_NS);
}

static void ot_flash_reset_rd_fifo(OtFlashState *s)
{
    ot_fifo32_reset(&s->rd_fifo);
    s->regs[R_STATUS] |= R_STATUS_RD_EMPTY_MASK;
    s->regs[R_STATUS] &= ~R_STATUS_RD_FULL_MASK;
    s->regs[R_INTR_STATE] &= ~INTR_RD_FULL_MASK;
    trace_ot_flash_reset_fifo("rd");
    ot_flash_update_rd_watermark(s);
}

static void ot_flash_reset_prog_fifo(OtFlashState *s)
{
    ot_fifo32_reset(&s->prog_fifo);
    s->regs[R_STATUS] |= R_STATUS_PROG_EMPTY_MASK;
    s->regs[R_STATUS] &= ~R_STATUS_PROG_FULL_MASK;
    s->regs[R_INTR_STATE] |= INTR_PROG_EMPTY_MASK;
    trace_ot_flash_reset_fifo("prog");
    ot_flash_update_prog_watermark(s);
}

static void ot_flash_set_error(OtFlashState *s, uint32_t ebit, uint32_t eaddr)
{
    if (ebit) {
        if (s->op.hw) {
            s->regs[R_FAULT_STATUS] |= ebit;
            s->latched_alerts |= ALERT_FATAL_ERR_MASK;
        } else {
            s->regs[R_OP_STATUS] |= R_OP_STATUS_ERR_MASK;
            s->regs[R_INTR_STATE] |= INTR_CORR_ERR_MASK;
            s->regs[R_ERR_ADDR] = FIELD_DP32(0, ERR_ADDR, ERR_ADDR, eaddr);
            s->regs[R_ERR_CODE] = ebit;
            s->regs[R_ALERT_TEST] |= ALERT_RECOV_ERR_MASK;
            ot_flash_update_irqs(s);
        }
        ot_flash_update_alerts(s);
    }
    trace_ot_flash_set_error(OP_NAME(s->op.kind), s->op.hw, ebit, eaddr);
}

static void ot_flash_op_complete(OtFlashState *s)
{
    /*
     * done is always signalled when the full operation is completed, even
     * if there was an error at some point in the operation.
     */
    if (!s->op.hw) {
        s->regs[R_OP_STATUS] |= R_OP_STATUS_DONE_MASK;
        s->regs[R_INTR_STATE] |= INTR_OP_DONE_MASK;
        ot_flash_update_irqs(s);
    }
    s->regs[R_CTRL_REGWEN] |= R_CTRL_REGWEN_EN_MASK;
    trace_ot_flash_op_complete(OP_NAME(s->op.kind), s->op.hw,
                               !s->regs[R_ERR_CODE]);
    s->op.kind = OP_NONE;
}

static uint32_t ot_flash_get_info_page_cfg_reg(
    unsigned bank, unsigned info_partition, unsigned page)
{
    switch (bank) {
    case 0u:
        switch (info_partition) {
        case 0u:
            return R_BANK0_INFO0_PAGE_CFG_0 + page;
        case 1u:
            return R_BANK0_INFO1_PAGE_CFG;
        case 2u:
            return R_BANK0_INFO2_PAGE_CFG_0 + page;
        default:
            qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid info partition: %u\n",
                          __func__, info_partition);
            return 0u;
        }
    case 1u:
        switch (info_partition) {
        case 0u:
            return R_BANK1_INFO0_PAGE_CFG_0 + page;
        case 1u:
            return R_BANK1_INFO1_PAGE_CFG;
        case 2u:
            return R_BANK1_INFO2_PAGE_CFG_0 + page;
        default:
            qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid info partition: %u\n",
                          __func__, info_partition);
            return 0u;
        }
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid bank: %d\n", __func__,
                      bank);
        return 0u;
    }
}

static OtFlashPropertyCfg ot_flash_get_hw_info_page_cfg(
    OtFlashState *s, unsigned bank, unsigned info_partition, unsigned page)
{
    for (unsigned ix = 0; ix < ARRAY_SIZE(OT_FLASH_HW_INFO_PAGE_RULES); ++ix) {
        const OtFlashHwInfoPageRule *rule = &OT_FLASH_HW_INFO_PAGE_RULES[ix];
        if (bank == rule->bank && info_partition == rule->info_partition &&
            page == rule->page && s->phase == rule->phase) {
            return rule->cfg;
        }
    }
    return OT_FLASH_CFG_INFO_DISABLE;
}

static OtFlashPropertyCfg ot_flash_get_info_page_reg_cfg(
    const OtFlashState *s, uint32_t info_page_cfg_reg)
{
    unsigned en_mubi4 =
        SHARED_FIELD_EX32(s->regs[info_page_cfg_reg], BANK_INFO_PAGE_CFG_EN);
    unsigned rd_en_mubi4 =
        SHARED_FIELD_EX32(s->regs[info_page_cfg_reg], BANK_INFO_PAGE_CFG_RD_EN);
    unsigned prog_en_mubi4 = SHARED_FIELD_EX32(s->regs[info_page_cfg_reg],
                                               BANK_INFO_PAGE_CFG_PROG_EN);
    unsigned erase_en_mubi4 = SHARED_FIELD_EX32(s->regs[info_page_cfg_reg],
                                                BANK_INFO_PAGE_CFG_ERASE_EN);
    unsigned scramble_en_mubi4 =
        SHARED_FIELD_EX32(s->regs[info_page_cfg_reg],
                          BANK_INFO_PAGE_CFG_SCRAMBLE_EN);
    unsigned ecc_en_mubi4 = SHARED_FIELD_EX32(s->regs[info_page_cfg_reg],
                                              BANK_INFO_PAGE_CFG_ECC_EN);
    unsigned he_en_mubi4 =
        SHARED_FIELD_EX32(s->regs[info_page_cfg_reg], BANK_INFO_PAGE_CFG_HE_EN);

    return (OtFlashPropertyCfg){
        .en = (uint8_t)(en_mubi4 == OT_MULTIBITBOOL4_TRUE),
        .rd_en = (uint8_t)(rd_en_mubi4 == OT_MULTIBITBOOL4_TRUE),
        .prog_en = (uint8_t)(prog_en_mubi4 == OT_MULTIBITBOOL4_TRUE),
        .erase_en = (uint8_t)(erase_en_mubi4 == OT_MULTIBITBOOL4_TRUE),
        .scramble_en = (uint8_t)(scramble_en_mubi4 == OT_MULTIBITBOOL4_TRUE),
        .ecc_en = (uint8_t)(ecc_en_mubi4 == OT_MULTIBITBOOL4_TRUE),
        .he_en = (uint8_t)(he_en_mubi4 == OT_MULTIBITBOOL4_TRUE),
    };
}

/**
 * Update the current SW register configuration for an info page based on
 * additional "qualification" protections, which limit access to certain flash
 * info pages holding secrets in certain lc_ctrl lifecycle states.
 *
 * This will determine any additional protections for info pages and mask the
 * given config, only enabling an operation if both the SW configuration and
 * the qualifications allow it.
 *
 * See also:
 * https://opentitan.org/book/hw/top_earlgrey/ip_autogen/flash_ctrl/
 * index.html#secret-information-partitions
 *
 * @bank The bank being accessed
 * @info_partition The info partition being accessed
 * @page The page being accessed
 * @cfg The configuration to update/mask (outparam).
 */
static void
ot_flash_update_info_page_qualification(unsigned bank, unsigned info_partition,
                                        unsigned page, OtFlashPropertyCfg *cfg)
{
    OtFlashPropertyCfg qual;
    qual.scramble_en = true;
    qual.ecc_en = true;
    qual.he_en = true;

    /*
     * TODO: these signals are stubbed out to always give permissions to any
     * qualified info pages for now, but in reality they should be connected
     * to the lc_ctrl broadcast signals.
     */
    bool creator_en = true;
    bool owner_en = true;
    bool isolated_rd_en = true;
    bool isolated_wr_en = true;

    /* retrieve additional qualifications for pages containing secrets */
    if (bank != FLASH_SEED_BANK ||
        info_partition != FLASH_SEED_INFO_PARTITION) {
        return;
    }
    switch (page) {
    case FLASH_QUAL_INFO_PAGE_CREATOR:
        qual.en = creator_en;
        qual.rd_en = creator_en;
        qual.prog_en = creator_en;
        qual.erase_en = creator_en;
        break;
    case FLASH_QUAL_INFO_PAGE_OWNER:
        qual.en = owner_en;
        qual.rd_en = owner_en;
        qual.prog_en = owner_en;
        qual.erase_en = owner_en;
        break;
    case FLASH_QUAL_INFO_PAGE_ISOLATED:
        qual.en = true;
        qual.rd_en = isolated_rd_en;
        qual.prog_en = isolated_wr_en;
        qual.erase_en = isolated_wr_en;
        break;
    default:
        return;
    }

    /* merge the reg cfg and the page qualifications */
    uint8_t prev_cfg = cfg->bitmap;
    cfg->bitmap &= qual.bitmap;
    trace_ot_flash_merge_info_qual(bank, info_partition, page, prev_cfg,
                                   qual.bitmap, cfg->bitmap);
}

static bool ot_flash_info_page_cfg_op_enabled(const OtFlashState *s,
                                              const OtFlashPropertyCfg *cfg)
{
    switch (s->op.kind) {
    case OP_READ:
        return cfg->rd_en;
    case OP_PROG:
        return cfg->prog_en;
    case OP_ERASE:
        return cfg->erase_en;
    case OP_NONE:
        xtrace_ot_flash_error("cannot check mp without operation");
        return false;
    default:
        xtrace_ot_flash_error("unsupported operation?");
        return false;
    }
}

static bool
ot_flash_mp_region_cfg_op_enabled(const OtFlashState *s, uint32_t mp_cfg_reg)
{
    unsigned en_field;
    switch (s->op.kind) {
    case OP_READ:
        en_field = SHARED_FIELD_EX32(s->regs[mp_cfg_reg], MP_REGION_CFG_RD_EN);
        break;
    case OP_PROG:
        en_field =
            SHARED_FIELD_EX32(s->regs[mp_cfg_reg], MP_REGION_CFG_PROG_EN);
        break;
    case OP_ERASE:
        en_field =
            SHARED_FIELD_EX32(s->regs[mp_cfg_reg], MP_REGION_CFG_ERASE_EN);
        break;
    case OP_NONE:
        xtrace_ot_flash_error("cannot check mp without operation");
        return false;
    default:
        xtrace_ot_flash_error("unsupported operation?");
        return false;
    }
    return en_field == OT_MULTIBITBOOL4_TRUE;
}

static bool ot_flash_default_region_cfg_op_enabled(const OtFlashState *s)
{
    unsigned en_field;
    switch (s->op.kind) {
    case OP_READ:
        en_field = FIELD_EX32(s->regs[R_DEFAULT_REGION], DEFAULT_REGION, RD_EN);
        break;
    case OP_PROG:
        en_field =
            FIELD_EX32(s->regs[R_DEFAULT_REGION], DEFAULT_REGION, PROG_EN);
        break;
    case OP_ERASE:
        en_field =
            FIELD_EX32(s->regs[R_DEFAULT_REGION], DEFAULT_REGION, ERASE_EN);
        break;
    case OP_NONE:
        xtrace_ot_flash_error("cannot check mp without operation");
        return false;
    default:
        xtrace_ot_flash_error("unsupported operation?");
        return false;
    }
    return en_field == OT_MULTIBITBOOL4_TRUE;
}

static bool ot_flash_can_erase_bank(const OtFlashState *s, unsigned bank)
{
    switch (bank) {
    case 0u:
        return (bool)FIELD_EX32(s->regs[R_MP_BANK_CFG_SHADOWED],
                                MP_BANK_CFG_SHADOWED, ERASE_EN_0);
    case 1u:
        return (bool)FIELD_EX32(s->regs[R_MP_BANK_CFG_SHADOWED],
                                MP_BANK_CFG_SHADOWED, ERASE_EN_1);
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: unknown bank %u for bank erase operation", __func__,
                      bank);
        return false;
    }
}

static unsigned ot_flash_next_info_address(OtFlashState *s)
{
    OtFlashStorage *storage = &s->flash;
    unsigned bank_size = storage->data_size;
    unsigned info_partition = s->op.info_sel;
    uint32_t mp_err_ebit =
        s->op.hw ? R_FAULT_STATUS_MP_ERR_MASK : R_ERR_CODE_MP_ERR_MASK;

    /* offset the address by the number of processed ops to get next addr */
    unsigned op_offset = (s->op.count - s->op.remaining) * sizeof(uint32_t);
    unsigned op_address = s->op.address + op_offset;

    if (info_partition >= storage->info_part_count) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid info partition: %u\n",
                      __func__, s->op.info_sel);
        ot_flash_set_error(s, mp_err_ebit, op_address);
        s->op.failed = true;
        return op_address;
    }

    /* extract the bank & bank-relative address from the address */
    unsigned bank = op_address / bank_size;
    if (bank >= storage->bank_count) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid bank: %d\n", __func__,
                      bank);
        ot_flash_set_error(s, mp_err_ebit, op_address);
        s->op.failed = true;
        return op_address;
    }
    unsigned address_in_bank = op_address % bank_size;
    if (address_in_bank >= storage->info_parts[info_partition].size) {
        /*
         * Purposefuly do not check the whole transaction's address width here:
         * the RTL only errors when it attempts to read an invalid address.
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid address in partition: %u %u\n", __func__,
                      op_address, info_partition);
        ot_flash_set_error(s, mp_err_ebit, op_address);
        s->op.failed = true;
        return op_address;
    }

    /* Retrieve the raw backend byte address in the info partitions. */
    unsigned bank_offset = bank * storage->info_size;
    unsigned info_part_offset = storage->info_parts[info_partition].offset;
    unsigned address = address_in_bank + bank_offset + info_part_offset;
    trace_ot_flash_info_part(s->op.address, s->op.count, s->op.remaining, bank,
                             info_partition, address);
    if (s->no_mem_prot ||
        (s->op.kind == OP_ERASE && s->op.erase_sel == ERASE_SEL_BANK)) {
        return address;
    }

    unsigned page = address_in_bank / BYTES_PER_PAGE;

    /*
     * Check the matching info partition page configuration.
     * For software reads, this depends on the config registers and any
     * additional secret qualifiers. For hardware reads, this depends
     * on the defined hardware rules & any HW overrides.
     */
    OtFlashPropertyCfg cfg;
    if (s->op.hw) {
        cfg = ot_flash_get_hw_info_page_cfg(s, bank, info_partition, page);
        uint32_t override = s->regs[R_HW_INFO_CFG_OVERRIDE];
        bool scramble_disable =
            FIELD_EX32(override, HW_INFO_CFG_OVERRIDE, SCRAMBLE_DIS) ==
            OT_MULTIBITBOOL4_TRUE;
        bool ecc_disable = FIELD_EX32(override, HW_INFO_CFG_OVERRIDE,
                                      ECC_DIS) == OT_MULTIBITBOOL4_TRUE;
        cfg.scramble_en &= !scramble_disable;
        cfg.ecc_en &= !ecc_disable;
    } else {
        uint32_t info_page_cfg_reg =
            ot_flash_get_info_page_cfg_reg(bank, info_partition, page);
        if (!info_page_cfg_reg) {
            ot_flash_set_error(s, mp_err_ebit, op_address);
            s->op.failed = true;
            return address;
        }
        cfg = ot_flash_get_info_page_reg_cfg(s, info_page_cfg_reg);
        ot_flash_update_info_page_qualification(bank, info_partition, page,
                                                &cfg);
    }

    if (!cfg.en || !ot_flash_info_page_cfg_op_enabled(s, &cfg)) {
        const char *op_type = (s->op.hw) ? "hardware" : "software";
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s operation %s on info page %u in partition %u of "
                      "bank %u is disabled by page config\n",
                      __func__, op_type, OP_NAME(s->op.kind), page, bank,
                      info_partition);
        ot_flash_set_error(s, mp_err_ebit, op_address);
        s->op.failed = true;
        return address;
    }

    return address;
}

static unsigned ot_flash_next_data_address(OtFlashState *s)
{
    OtFlashStorage *storage = &s->flash;
    unsigned bank_size = storage->data_size;
    uint32_t mp_err_ebit =
        s->op.hw ? R_FAULT_STATUS_MP_ERR_MASK : R_ERR_CODE_MP_ERR_MASK;

    /* offset the address by the number of proessed ops to get next addr */
    unsigned op_offset = (s->op.count - s->op.remaining) * sizeof(uint32_t);
    unsigned address = s->op.address + op_offset;
    unsigned bank = address / bank_size;
    if (bank >= storage->bank_count) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid bank: %d\n", __func__,
                      bank);
        ot_flash_set_error(s, mp_err_ebit, address);
        s->op.failed = true;
        return address;
    }
    trace_ot_flash_data_part(s->op.address, s->op.count, s->op.remaining, bank,
                             address);

    if (s->no_mem_prot ||
        (s->op.kind == OP_ERASE && s->op.erase_sel == ERASE_SEL_BANK)) {
        return address;
    }

    /*
     * Hardware can only access the data pages in the RMA LC phase, at which
     * point it has full access.
     */
    if (s->op.hw) {
        if (s->phase == LC_PHASE_RMA) {
            return address;
        }
        qemu_log_mask(
            LOG_GUEST_ERROR,
            "%s: hardware operation %s on data pages is not permitted "
            "in the %s flash_ctrl lc phase\n",
            __func__, OP_NAME(s->op.kind), LC_PHASE_NAME(s->phase));
        ot_flash_set_error(s, mp_err_ebit, address);
        s->op.failed = true;
        return address;
    }

    /*
     * Check through the memory protection regions 0->9, and see if the current
     * page falls within a region. If so, and it is enabled, apply its perms.
     * Otherwise apply the default region's permissions. Note that MP region
     * page indexes are cumulative across banks.
     *
     * If any two MP regions overlap, the lower index region has priority.
     */
    unsigned page = address / BYTES_PER_PAGE;
    bool matching_region_found = false;
    for (unsigned region = 0u; region < NUM_REGIONS; region++) {
        /* Ignore disabled regions */
        unsigned r_region_cfg = R_MP_REGION_CFG_0 + region;
        if (SHARED_FIELD_EX32(s->regs[r_region_cfg], MP_REGION_CFG_EN) !=
            OT_MULTIBITBOOL4_TRUE) {
            continue;
        }

        /*
         * Check if the current flash word falls in this region.
         * Size is inclusive at the base, but exclusive at (base+size).
         */
        unsigned r_region = R_MP_REGION_0 + region;
        unsigned region_base_page =
            SHARED_FIELD_EX32(s->regs[r_region], MP_REGION_BASE);
        unsigned region_size =
            SHARED_FIELD_EX32(s->regs[r_region], MP_REGION_SIZE);
        if (page < region_base_page ||
            page >= (region_base_page + region_size)) {
            continue;
        }
        matching_region_found = true;

        /* Page does fall in this region, so check if enabled for operation. */
        if (!ot_flash_mp_region_cfg_op_enabled(s, r_region_cfg)) {
            qemu_log_mask(
                LOG_GUEST_ERROR,
                "%s: software operation %s on page %u of data partition "
                "in bank %u is disabled by MP region %u\n",
                __func__, OP_NAME(s->op.kind), page, bank, region);
            ot_flash_set_error(s, mp_err_ebit, address);
            s->op.failed = true;
            return address;
        }
        break;
    }

    /* If page not in any region, apply the default region's permissions. */
    if (!matching_region_found && !ot_flash_default_region_cfg_op_enabled(s)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: software operation %s on page %u of data partition "
                      "in bank %u is disabled by default region\n",
                      __func__, OP_NAME(s->op.kind), page, bank);
        ot_flash_set_error(s, mp_err_ebit, address);
        s->op.failed = true;
        return address;
    }
    return address;
}

static void ot_flash_op_read(OtFlashState *s)
{
    OtFifo32 *rd_fifo = (s->op.hw) ? &s->hw_rd_fifo : &s->rd_fifo;
    g_assert(rd_fifo);

    if (ot_fifo32_is_full(rd_fifo)) {
        xtrace_ot_flash_error("read while RD FIFO full");
        return;
    }

    OtFlashStorage *storage = &s->flash;
    uint32_t *src = s->op.info_part ? storage->info : storage->data;

    while (s->op.remaining) {
        uint32_t word = 0xFFFFFFFFu;
        if (!s->op.failed) {
            unsigned address = s->op.info_part ? ot_flash_next_info_address(s) :
                                                 ot_flash_next_data_address(s);
            address /= sizeof(uint32_t); /* convert to word address */

            /* For multi-word read access permission errors, return all 1s. */
            if (!s->op.failed) {
                word = src[address];
            }
        }

        s->op.remaining--;
        if (!s->op.hw && ot_flash_fifo_in_reset(s)) {
            /* If fifo in reset, still read but don't push rdata */
            continue;
        }
        ot_fifo32_push(rd_fifo, word);

        if (ot_fifo32_is_full(rd_fifo)) {
            break;
        }
    }

    /*
     * If we finished the entire read operation (i.e. no early exit as FIFO
     * is full), mark the operation as completed.
     */
    if (!s->op.remaining) {
        ot_flash_op_complete(s);
    }
}

static void ot_flash_op_prog(OtFlashState *s)
{
    if (ot_fifo32_is_empty(&s->prog_fifo)) {
        xtrace_ot_flash_error("prog while prog FIFO empty");
        return;
    }

    OtFlashStorage *storage = &s->flash;
    uint32_t *dest = s->op.info_part ? storage->info : storage->data;

    while (s->op.remaining) {
        uint32_t word = ot_fifo32_pop(&s->prog_fifo);
        bool fifo_empty = ot_fifo32_is_empty(&s->prog_fifo);

        /* Must calculate next addr before decrementing the remaining count. */
        unsigned address = 0u;
        if (!s->op.failed) {
            address = s->op.info_part ? ot_flash_next_info_address(s) :
                                        ot_flash_next_data_address(s);
            address /= sizeof(uint32_t); /* convert to word address */
        }
        s->op.remaining--;

        /*
         * On encountering a multi-word write error, we must continue to empty
         * the prog FIFO regardless, hence we retrieve the word to program
         * before checking for errors.
         */
        trace_ot_flash_op_prog(s->op.address, s->op.count, s->op.remaining,
                               s->op.failed, fifo_empty);
        if (s->op.failed) {
            if (!fifo_empty) {
                continue;
            }
            break;
        }

        /*
         * Bits cannot be programmed back to 1 once programmed to 0; they must
         * be erased instead.
         */
        g_assert(address <
                 ((s->op.info_part ? storage->info_size : storage->data_size) *
                  storage->bank_count));
        dest[address] &= word;
        trace_ot_flash_prog_word(s->op.info_part, address, word);
        if (ot_flash_is_backend_writable(s)) {
            uintptr_t dest_offset = (uintptr_t)dest - (uintptr_t)storage->data;
            if (ot_flash_write_backend(s, &dest[address],
                                       (unsigned)(dest_offset + address),
                                       sizeof(uint32_t))) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "%s: cannot update flash backend\n", __func__);
                uint32_t ebit = s->op.hw ? R_FAULT_STATUS_PROG_ERR_MASK :
                                           R_ERR_CODE_PROG_ERR_MASK;
                ot_flash_set_error(s, ebit, address * sizeof(uint32_t));
            }
        }

        if (fifo_empty) {
            break;
        }
    }

    /*
     * If we finished the entire program operation (i.e. no early exit as FIFO
     * is empty), mark the operation as completed.
     */
    if (!s->op.remaining) {
        ot_flash_op_complete(s);
    }
}

static void ot_flash_op_erase_page(OtFlashState *s, unsigned address)
{
    OtFlashStorage *storage = &s->flash;
    uint32_t *dest = s->op.info_part ? storage->info : storage->data;
    unsigned page_size = BYTES_PER_PAGE;
    unsigned page_address = address - (address % page_size);
    page_address /= sizeof(uint32_t); /* convert to word address */

    g_assert((page_address + page_size) <
             ((s->op.info_part ? storage->info_size : storage->data_size) *
              storage->bank_count));
    memset(&dest[page_address], 0xFFu, page_size);
    trace_ot_flash_erase(s->op.info_part, page_address, page_size);
    if (ot_flash_is_backend_writable(s)) {
        uintptr_t dest_offset = (uintptr_t)dest - (uintptr_t)storage->data;
        if (ot_flash_write_backend(s, &dest[page_address],
                                   (unsigned)(dest_offset + page_address),
                                   page_size)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: cannot update flash backend\n",
                          __func__);
            uint32_t ebit = s->op.hw ? R_FAULT_STATUS_PROG_ERR_MASK :
                                       R_ERR_CODE_PROG_ERR_MASK;
            ot_flash_set_error(s, ebit, address * sizeof(uint32_t));
            ot_flash_op_complete(s);
            return;
        }
    }

    ot_flash_op_complete(s);
}

static void ot_flash_op_erase_bank(OtFlashState *s, unsigned address)
{
    OtFlashStorage *storage = &s->flash;
    unsigned bank_size =
        s->op.info_part ? storage->info_size : storage->data_size;
    unsigned bank = address / bank_size;

    if (!ot_flash_can_erase_bank(s, bank)) {
        qemu_log_mask(
            LOG_GUEST_ERROR,
            "%s: cannot erase bank %u when bank-wide erase not enabled\n",
            __func__, bank);
        uint32_t ebit =
            s->op.hw ? R_FAULT_STATUS_MP_ERR_MASK : R_ERR_CODE_MP_ERR_MASK;
        ot_flash_set_error(s, ebit, address);
        ot_flash_op_complete(s);
        return;
    }

    /*
     * For bank erase only, if the data partition is selected, just the
     * data partition is erased. If the info partition is selected, BOTH
     * the data and info partitions are erased.
     */
    unsigned bank_address = address - (address % bank_size);
    bank_address /= sizeof(uint32_t); /* convert to word address */
    unsigned data_address, info_address = 0u;
    if (!s->op.info_part) {
        data_address = bank_address;
        g_assert((data_address + bank_size) <=
                 (storage->data_size * storage->bank_count));
        memset(&storage->data[data_address], 0xFFu, bank_size);
        trace_ot_flash_erase(s->op.info_part, data_address, bank_size);
    } else {
        info_address = bank_address;
        g_assert((info_address + bank_size) <=
                 (storage->info_size * storage->bank_count));
        memset(&storage->info[info_address], 0xFFu, bank_size);
        trace_ot_flash_erase(true, info_address, bank_size);

        bank_size = storage->data_size;
        data_address = bank_size * bank / sizeof(uint32_t);
        g_assert((data_address + bank_size) <=
                 (storage->data_size * storage->bank_count));
        memset(&storage->data[data_address], 0xFFu, bank_size);
        trace_ot_flash_erase(false, data_address, bank_size);
    }

    if (ot_flash_is_backend_writable(s)) {
        int data_write_err =
            ot_flash_write_backend(s, &storage->data[data_address],
                                   (unsigned)(data_address),
                                   storage->data_size);
        int info_write_err = 0u;
        if (s->op.info_part) {
            uintptr_t offset =
                (uintptr_t)storage->info - (uintptr_t)storage->data;
            info_write_err =
                ot_flash_write_backend(s, &storage->info[info_address],
                                       (unsigned)(offset + info_address),
                                       storage->info_size);
        }

        if (data_write_err || info_write_err) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: cannot update flash backend\n",
                          __func__);
            uint32_t ebit = s->op.hw ? R_FAULT_STATUS_PROG_ERR_MASK :
                                       R_ERR_CODE_PROG_ERR_MASK;
            ot_flash_set_error(s, ebit, address * sizeof(uint32_t));
            ot_flash_op_complete(s);
            return;
        }
    }

    ot_flash_op_complete(s);
}

static void ot_flash_op_erase(OtFlashState *s)
{
    unsigned address = s->op.info_part ? ot_flash_next_info_address(s) :
                                         ot_flash_next_data_address(s);
    if (s->op.failed) {
        ot_flash_op_complete(s);
        return;
    }

    /* Try to erase all bits in the page/bank back to 1. */
    if (s->op.erase_sel == ERASE_SEL_PAGE) {
        ot_flash_op_erase_page(s, address);
    } else { /* ERASE_SEL_BANK */
        ot_flash_op_erase_bank(s, address);
    }
}

static void ot_flash_update_fifos_status(OtFlashState *s)
{
    uint32_t rd_full = (uint32_t)ot_fifo32_is_full(&s->rd_fifo);
    uint32_t rd_empty = (uint32_t)ot_fifo32_is_empty(&s->rd_fifo);
    uint32_t prog_full = (uint32_t)ot_fifo32_is_full(&s->prog_fifo);
    uint32_t prog_empty = (uint32_t)ot_fifo32_is_empty(&s->prog_fifo);

    s->regs[R_STATUS] = FIELD_DP32(s->regs[R_STATUS], STATUS, RD_FULL, rd_full);
    s->regs[R_STATUS] =
        FIELD_DP32(s->regs[R_STATUS], STATUS, RD_EMPTY, rd_empty);
    s->regs[R_STATUS] =
        FIELD_DP32(s->regs[R_STATUS], STATUS, PROG_FULL, prog_full);
    s->regs[R_STATUS] =
        FIELD_DP32(s->regs[R_STATUS], STATUS, PROG_EMPTY, prog_empty);

    if (rd_full) {
        s->regs[R_INTR_STATE] |= INTR_RD_FULL_MASK;
    }
    if (prog_empty) {
        s->regs[R_INTR_STATE] |= INTR_PROG_EMPTY_MASK;
    }
    ot_flash_update_rd_watermark(s);
    ot_flash_update_prog_watermark(s);
    ot_flash_update_irqs(s);
}

static void ot_flash_op_execute(OtFlashState *s)
{
    s->regs[R_CTRL_REGWEN] &= ~R_CTRL_REGWEN_EN_MASK;

    switch (s->op.kind) {
    case OP_READ:
        trace_ot_flash_op_execute(OP_NAME(s->op.kind), s->op.hw);
        ot_flash_op_read(s);
        break;
    case OP_PROG:
        trace_ot_flash_op_execute(OP_NAME(s->op.kind), s->op.hw);
        ot_flash_op_prog(s);
        break;
    case OP_ERASE:
        trace_ot_flash_op_execute(OP_NAME(s->op.kind), s->op.hw);
        ot_flash_op_erase(s);
        break;
    default:
        s->regs[R_CTRL_REGWEN] |= R_CTRL_REGWEN_EN_MASK;
        xtrace_ot_flash_error("unsupported");
        break;
    }

    /* Update fifo status reg & intrs if not in a HW operation */
    if (!ot_flash_in_hw_operation(s)) {
        ot_flash_update_fifos_status(s);
    }
}

static void ot_flash_update_exec(OtFlashState *s)
{
    OtVMapperClass *vm = OT_VMAPPER_GET_CLASS(s->vmapper);
    bool ifetch = s->regs[R_EXEC] == EXEC_EN;

    vm->disable_exec(s->vmapper, &s->mmio.mem, !ifetch);
}

static bool ot_flash_check_program_resolution(OtFlashState *s)
{
    unsigned start_address = s->op.address / sizeof(uint32_t);
    unsigned end_address = start_address - 1u + s->op.count;
    unsigned start_window = start_address / BUS_PGM_RES;
    unsigned end_window = end_address / BUS_PGM_RES;
    if (start_window != end_window) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: program resolution error for addr=%u, count=%u "
                      "(start_window=%u, end_window=%u)\n",
                      __func__, s->op.address, s->op.count, start_window,
                      end_window);
        ot_flash_set_error(s, R_ERR_CODE_PROG_WIN_ERR_MASK, s->op.address);
        ot_flash_op_complete(s);
        return false;
    }
    return true;
}

static bool ot_flash_check_program_type(OtFlashState *s)
{
    uint32_t en_mask;
    if (s->op.prog_sel == PROG_SEL_NORMAL) {
        en_mask = R_PROG_TYPE_EN_NORMAL_MASK;
    } else { /* PROG_SEL_REPAIR */
        en_mask = R_PROG_TYPE_EN_REPAIR_MASK;
    }
    if (!(s->regs[R_PROG_TYPE_EN] & en_mask)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: program type not enabled: %s\n",
                      __func__, PROGRAM_NAME(s->op.prog_sel));
        ot_flash_set_error(s, R_ERR_CODE_PROG_TYPE_ERR_MASK, s->op.address);
        ot_flash_op_complete(s);
        return false;
    }
    return true;
}

static uint64_t ot_flash_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtFlashState *s = opaque;
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_INTR_STATE:
    case R_INTR_ENABLE:
    case R_DIS:
    case R_EXEC:
    case R_INIT:
    case R_CTRL_REGWEN:
    case R_CONTROL:
    case R_ADDR:
    case R_PROG_TYPE_EN:
    case R_ERASE_SUSPEND:
    case R_REGION_CFG_REGWEN_0:
    case R_REGION_CFG_REGWEN_1:
    case R_REGION_CFG_REGWEN_2:
    case R_REGION_CFG_REGWEN_3:
    case R_REGION_CFG_REGWEN_4:
    case R_REGION_CFG_REGWEN_5:
    case R_REGION_CFG_REGWEN_6:
    case R_REGION_CFG_REGWEN_7:
    case R_MP_REGION_CFG_0:
    case R_MP_REGION_CFG_1:
    case R_MP_REGION_CFG_2:
    case R_MP_REGION_CFG_3:
    case R_MP_REGION_CFG_4:
    case R_MP_REGION_CFG_5:
    case R_MP_REGION_CFG_6:
    case R_MP_REGION_CFG_7:
    case R_MP_REGION_0:
    case R_MP_REGION_1:
    case R_MP_REGION_2:
    case R_MP_REGION_3:
    case R_MP_REGION_4:
    case R_MP_REGION_5:
    case R_MP_REGION_6:
    case R_MP_REGION_7:
    case R_DEFAULT_REGION:
    case R_BANK0_INFO0_REGWEN_0:
    case R_BANK0_INFO0_REGWEN_1:
    case R_BANK0_INFO0_REGWEN_2:
    case R_BANK0_INFO0_REGWEN_3:
    case R_BANK0_INFO0_REGWEN_4:
    case R_BANK0_INFO0_REGWEN_5:
    case R_BANK0_INFO0_REGWEN_6:
    case R_BANK0_INFO0_REGWEN_7:
    case R_BANK0_INFO0_REGWEN_8:
    case R_BANK0_INFO0_REGWEN_9:
    case R_BANK0_INFO0_PAGE_CFG_0:
    case R_BANK0_INFO0_PAGE_CFG_1:
    case R_BANK0_INFO0_PAGE_CFG_2:
    case R_BANK0_INFO0_PAGE_CFG_3:
    case R_BANK0_INFO0_PAGE_CFG_4:
    case R_BANK0_INFO0_PAGE_CFG_5:
    case R_BANK0_INFO0_PAGE_CFG_6:
    case R_BANK0_INFO0_PAGE_CFG_7:
    case R_BANK0_INFO0_PAGE_CFG_8:
    case R_BANK0_INFO0_PAGE_CFG_9:
    case R_BANK0_INFO1_REGWEN:
    case R_BANK0_INFO1_PAGE_CFG:
    case R_BANK0_INFO2_REGWEN_0:
    case R_BANK0_INFO2_REGWEN_1:
    case R_BANK0_INFO2_PAGE_CFG_0:
    case R_BANK0_INFO2_PAGE_CFG_1:
    case R_BANK1_INFO0_REGWEN_0:
    case R_BANK1_INFO0_REGWEN_1:
    case R_BANK1_INFO0_REGWEN_2:
    case R_BANK1_INFO0_REGWEN_3:
    case R_BANK1_INFO0_REGWEN_4:
    case R_BANK1_INFO0_REGWEN_5:
    case R_BANK1_INFO0_REGWEN_6:
    case R_BANK1_INFO0_REGWEN_7:
    case R_BANK1_INFO0_REGWEN_8:
    case R_BANK1_INFO0_REGWEN_9:
    case R_BANK1_INFO0_PAGE_CFG_0:
    case R_BANK1_INFO0_PAGE_CFG_1:
    case R_BANK1_INFO0_PAGE_CFG_2:
    case R_BANK1_INFO0_PAGE_CFG_3:
    case R_BANK1_INFO0_PAGE_CFG_4:
    case R_BANK1_INFO0_PAGE_CFG_5:
    case R_BANK1_INFO0_PAGE_CFG_6:
    case R_BANK1_INFO0_PAGE_CFG_7:
    case R_BANK1_INFO0_PAGE_CFG_8:
    case R_BANK1_INFO0_PAGE_CFG_9:
    case R_BANK1_INFO1_REGWEN:
    case R_BANK1_INFO1_PAGE_CFG:
    case R_BANK1_INFO2_REGWEN_0:
    case R_BANK1_INFO2_REGWEN_1:
    case R_BANK1_INFO2_PAGE_CFG_0:
    case R_BANK1_INFO2_PAGE_CFG_1:
    case R_HW_INFO_CFG_OVERRIDE:
    case R_BANK_CFG_REGWEN:
    case R_MP_BANK_CFG_SHADOWED:
    case R_OP_STATUS:
    case R_DEBUG_STATE:
    case R_ERR_CODE:
    case R_STD_FAULT_STATUS:
    case R_FAULT_STATUS:
    case R_ERR_ADDR:
    case R_ECC_SINGLE_ERR_CNT:
    case R_ECC_SINGLE_ERR_ADDR_0:
    case R_ECC_SINGLE_ERR_ADDR_1:
    case R_PHY_ALERT_CFG:
    case R_PHY_STATUS:
    case R_SCRATCH:
    case R_FIFO_LVL:
    case R_FIFO_RST:
    case R_STATUS:
        val32 = s->regs[reg];
        break;
    case R_RD_FIFO:
        if (!ot_fifo32_is_empty(&s->rd_fifo)) {
            val32 = ot_fifo32_pop(&s->rd_fifo);
            s->regs[R_STATUS] &= ~R_STATUS_RD_FULL_MASK;
            s->regs[R_INTR_STATE] &= ~INTR_RD_FULL_MASK;
            if (ot_fifo32_is_empty(&s->rd_fifo)) {
                s->regs[R_STATUS] |= R_STATUS_RD_EMPTY_MASK;
            }
            ot_flash_update_rd_watermark(s);
            ot_flash_update_irqs(s);
            if (ot_flash_operation_ongoing(s)) {
                ot_flash_op_execute(s);
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: Read empty FIFO\n", __func__);
            val32 = 0u;
        }
        break;
    case R_CURR_FIFO_LVL:
        val32 =
            SHARED_FIELD_DP32(0u, FIFO_LVL_RD, ot_fifo32_num_used(&s->rd_fifo));
        val32 = SHARED_FIELD_DP32(val32, FIFO_LVL_PROG,
                                  ot_fifo32_num_used(&s->prog_fifo));
        break;
    case R_ALERT_TEST:
    case R_PROG_FIFO:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: W/O register 0x%03" HWADDR_PRIx " (%s)\n", __func__,
                      addr, REG_NAME(reg));
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        val32 = 0;
        break;
    }

    uint32_t pc = ibex_get_current_pc();
    trace_ot_flash_io_read_out((uint32_t)addr, REG_NAME(reg), val32, pc);

    return (uint64_t)val32;
};

static void ot_flash_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                                unsigned size)
{
    OtFlashState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_flash_io_write((uint32_t)addr, REG_NAME(reg), val32, pc);

    switch (reg) {
    case R_INTR_STATE: {
        uint32_t rw1c_mask = INTR_CORR_ERR_MASK | INTR_OP_DONE_MASK;
        if (val32 & ~rw1c_mask) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: Write to R/O field in register 0x%03" HWADDR_PRIx
                          " (%s)\n",
                          __func__, addr, REG_NAME(reg));
        }
        val32 &= rw1c_mask;
        s->regs[reg] &= ~val32;
        ot_flash_update_irqs(s);
        break;
    }
    case R_INTR_ENABLE:
        val32 &= INTR_MASK;
        s->regs[R_INTR_ENABLE] = val32;
        ot_flash_update_irqs(s);
        break;
    case R_INTR_TEST:
        val32 &= INTR_MASK;
        s->regs[R_INTR_STATE] |= val32;
        ot_flash_update_irqs(s);
        break;
    case R_ALERT_TEST:
        val32 &= ALERT_MASK;
        s->regs[reg] = val32;
        ot_flash_update_alerts(s);
        break;
    case R_DIS:
        val32 &= R_DIS_VAL_MASK;
        s->regs[reg] = ot_multibitbool_w1s_write(s->regs[reg], val32, 4u);
        if (ot_flash_is_disabled(s)) {
            xtrace_ot_flash_error("flash controller disabled by SW");
            memory_region_set_enabled(&s->mmio.mem, false);
        }
        break;
    case R_INIT:
        val32 &= R_INIT_VAL_MASK;
        s->regs[reg] = val32;
        if (val32) {
            ot_flash_initialize(s);
        }
        break;
    case R_EXEC:
        s->regs[reg] = val32;
        ot_flash_update_exec(s);
        break;
    case R_CONTROL:
        if (!(s->regs[R_CTRL_REGWEN] & R_CTRL_REGWEN_EN_MASK)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s is not enabled, so %s is protected\n",
                          __func__, REG_NAME(R_CTRL_REGWEN), REG_NAME(reg));
            break;
        }

        val32 &= CONTROL_MASK;
        s->regs[reg] = val32;
        bool start = (bool)FIELD_EX32(val32, CONTROL, START);
        unsigned op = (unsigned)FIELD_EX32(val32, CONTROL, OP);
        bool prog_sel = (bool)FIELD_EX32(val32, CONTROL, PROG_SEL);
        bool erase_sel = (bool)FIELD_EX32(val32, CONTROL, ERASE_SEL);
        bool part_sel = (bool)FIELD_EX32(val32, CONTROL, PARTITION_SEL);
        unsigned info_sel = (unsigned)FIELD_EX32(val32, CONTROL, INFO_SEL);
        unsigned num = (unsigned)FIELD_EX32(val32, CONTROL, NUM);

        if (start && !ot_flash_in_operation(s)) {
            /*
             * If the flash controller is disabled by software, then (a) the
             * flash protocol controller completes existing software commands,
             * (b) the flash physical controller completes existing stateful
             * operations, and (c) the flash protocol controller MP errors
             * back all controller initiated operations.
             */
            if (ot_flash_is_disabled(s)) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: flash has been disabled\n",
                              __func__);
                ot_flash_set_error(s, R_ERR_CODE_MP_ERR_MASK, 0u);
                return;
            }

            s->op.hw = false;

            switch (op) {
            case CONTROL_OP_READ:
                s->op.kind = OP_READ;
                s->op.address = s->regs[R_ADDR] & ~3u;
                s->op.info_part = part_sel;
                s->op.info_sel = info_sel;
                xtrace_ot_flash_info("Read from", s->op.address);
                s->op.count = num + 1u;
                break;
            case CONTROL_OP_PROG:
                s->op.kind = OP_PROG;
                s->op.address = s->regs[R_ADDR] & ~3u;
                s->op.info_part = part_sel;
                s->op.info_sel = info_sel;
                s->op.prog_sel = (bool)prog_sel;
                s->op.count = num + 1u;
                /*
                 * On encountering either a program resolution error or program
                 * type error, do not start the transaction.
                 */
                if (!ot_flash_check_program_resolution(s) ||
                    !ot_flash_check_program_type(s)) {
                    return;
                }
                xtrace_ot_flash_info("Write to", s->op.address);
                break;
            case CONTROL_OP_ERASE:
                s->op.kind = OP_ERASE;
                s->op.address = s->regs[R_ADDR] & ~3u;
                s->op.info_part = part_sel;
                s->op.info_sel = info_sel;
                s->op.erase_sel = (bool)erase_sel;
                /* Erase operations neither go through FIFOs nor use/require a
                 * word count */
                s->op.count = 0u;
                xtrace_ot_flash_info("Erase at", s->op.address);
                break;
            default:
                qemu_log_mask(LOG_GUEST_ERROR,
                              "%s: Operation %u (%s) is invalid\n", __func__,
                              op, CONTROL_OP_NAME(op));
                ot_flash_set_error(s, R_ERR_CODE_OP_ERR_MASK, 0u);
                ot_flash_op_complete(s);
                return;
            }
            s->op.failed = false;
            s->op.remaining = s->op.count;
            trace_ot_flash_op_start(OP_NAME(s->op.kind), s->op.hw);
        }
        ot_flash_op_execute(s);

        break;
    case R_ADDR:
        val32 &= R_ADDR_START_MASK;
        s->regs[reg] = val32;
        break;
    case R_PROG_TYPE_EN:
        val32 &= R_PROG_TYPE_EN_NORMAL_MASK | R_PROG_TYPE_EN_REPAIR_MASK;
        s->regs[reg] = val32;
        break;
    case R_ERASE_SUSPEND:
        /*
         * @todo We do not implement the erase suspend operation in QEMU as we
         * do all erases synchronously, and so just immediately clear the erase
         * suspend request. To implement this feature properly we would have to
         * add delay to bank erases & check for erase suspends at each step.
         */
        s->regs[reg] = 0u;
        break;
    case R_REGION_CFG_REGWEN_0:
    case R_REGION_CFG_REGWEN_1:
    case R_REGION_CFG_REGWEN_2:
    case R_REGION_CFG_REGWEN_3:
    case R_REGION_CFG_REGWEN_4:
    case R_REGION_CFG_REGWEN_5:
    case R_REGION_CFG_REGWEN_6:
    case R_REGION_CFG_REGWEN_7:
    case R_BANK0_INFO0_REGWEN_0:
    case R_BANK0_INFO0_REGWEN_1:
    case R_BANK0_INFO0_REGWEN_2:
    case R_BANK0_INFO0_REGWEN_3:
    case R_BANK0_INFO0_REGWEN_4:
    case R_BANK0_INFO0_REGWEN_5:
    case R_BANK0_INFO0_REGWEN_6:
    case R_BANK0_INFO0_REGWEN_7:
    case R_BANK0_INFO0_REGWEN_8:
    case R_BANK0_INFO0_REGWEN_9:
    case R_BANK0_INFO1_REGWEN:
    case R_BANK0_INFO2_REGWEN_0:
    case R_BANK0_INFO2_REGWEN_1:
    case R_BANK1_INFO0_REGWEN_0:
    case R_BANK1_INFO0_REGWEN_1:
    case R_BANK1_INFO0_REGWEN_2:
    case R_BANK1_INFO0_REGWEN_3:
    case R_BANK1_INFO0_REGWEN_4:
    case R_BANK1_INFO0_REGWEN_5:
    case R_BANK1_INFO0_REGWEN_6:
    case R_BANK1_INFO0_REGWEN_7:
    case R_BANK1_INFO0_REGWEN_8:
    case R_BANK1_INFO0_REGWEN_9:
    case R_BANK1_INFO2_REGWEN_0:
    case R_BANK1_INFO2_REGWEN_1:
    case R_BANK1_INFO1_REGWEN:
    case R_BANK_CFG_REGWEN:
        val32 &= BANK_REGWEN_MASK;
        s->regs[reg] &= val32; /* rw0c */
        break;
    case R_HW_INFO_CFG_OVERRIDE:
        val32 &= R_HW_INFO_CFG_OVERRIDE_SCRAMBLE_DIS_MASK |
                 R_HW_INFO_CFG_OVERRIDE_ECC_DIS_MASK;
        s->regs[reg] = val32;
        break;
    case R_MP_REGION_CFG_0:
    case R_MP_REGION_CFG_1:
    case R_MP_REGION_CFG_2:
    case R_MP_REGION_CFG_3:
    case R_MP_REGION_CFG_4:
    case R_MP_REGION_CFG_5:
    case R_MP_REGION_CFG_6:
    case R_MP_REGION_CFG_7:
        if (ot_flash_regs_is_wr_enabled(s, reg - R_MP_REGION_CFG_0 +
                                               R_REGION_CFG_REGWEN_0)) {
            val32 &= MP_REGION_CFG_MASK;
            s->regs[reg] = val32;
        }
        break;
    case R_MP_REGION_0:
    case R_MP_REGION_1:
    case R_MP_REGION_2:
    case R_MP_REGION_3:
    case R_MP_REGION_4:
    case R_MP_REGION_5:
    case R_MP_REGION_6:
    case R_MP_REGION_7:
        if (ot_flash_regs_is_wr_enabled(s, reg - R_MP_REGION_0 +
                                               R_REGION_CFG_REGWEN_0)) {
            val32 &= MP_REGION_MASK;
            s->regs[reg] = val32;
        }
        break;
    case R_DEFAULT_REGION:
        val32 &= DEFAULT_REGION_MASK;
        s->regs[reg] = val32;
        break;
    case R_BANK0_INFO0_PAGE_CFG_0:
    case R_BANK0_INFO0_PAGE_CFG_1:
    case R_BANK0_INFO0_PAGE_CFG_2:
    case R_BANK0_INFO0_PAGE_CFG_3:
    case R_BANK0_INFO0_PAGE_CFG_4:
    case R_BANK0_INFO0_PAGE_CFG_5:
    case R_BANK0_INFO0_PAGE_CFG_6:
    case R_BANK0_INFO0_PAGE_CFG_7:
    case R_BANK0_INFO0_PAGE_CFG_8:
    case R_BANK0_INFO0_PAGE_CFG_9:
        if (ot_flash_regs_is_wr_enabled(s, reg - R_BANK0_INFO0_PAGE_CFG_0 +
                                               R_BANK0_INFO0_REGWEN_0)) {
            val32 &= BANK_INFO_PAGE_CFG_MASK;
            s->regs[reg] = val32;
        }
        break;
    case R_BANK0_INFO1_PAGE_CFG:
        if (ot_flash_regs_is_wr_enabled(s, R_BANK0_INFO1_REGWEN)) {
            val32 &= BANK_INFO_PAGE_CFG_MASK;
            s->regs[reg] = val32;
        }
        break;
    case R_BANK0_INFO2_PAGE_CFG_0:
    case R_BANK0_INFO2_PAGE_CFG_1:
        if (ot_flash_regs_is_wr_enabled(s, reg - R_BANK0_INFO2_PAGE_CFG_0 +
                                               R_BANK0_INFO2_REGWEN_0)) {
            val32 &= BANK_INFO_PAGE_CFG_MASK;
            s->regs[reg] = val32;
        }
        break;
    case R_BANK1_INFO0_PAGE_CFG_0:
    case R_BANK1_INFO0_PAGE_CFG_1:
    case R_BANK1_INFO0_PAGE_CFG_2:
    case R_BANK1_INFO0_PAGE_CFG_3:
    case R_BANK1_INFO0_PAGE_CFG_4:
    case R_BANK1_INFO0_PAGE_CFG_5:
    case R_BANK1_INFO0_PAGE_CFG_6:
    case R_BANK1_INFO0_PAGE_CFG_7:
    case R_BANK1_INFO0_PAGE_CFG_8:
    case R_BANK1_INFO0_PAGE_CFG_9:
        if (ot_flash_regs_is_wr_enabled(s, reg - R_BANK1_INFO0_PAGE_CFG_0 +
                                               R_BANK1_INFO0_REGWEN_0)) {
            val32 &= BANK_INFO_PAGE_CFG_MASK;
            s->regs[reg] = val32;
        }
        break;
    case R_BANK1_INFO1_PAGE_CFG:
        if (ot_flash_regs_is_wr_enabled(s, R_BANK1_INFO1_REGWEN)) {
            val32 &= BANK_INFO_PAGE_CFG_MASK;
            s->regs[reg] = val32;
        }
        break;
    case R_BANK1_INFO2_PAGE_CFG_0:
    case R_BANK1_INFO2_PAGE_CFG_1:
        if (ot_flash_regs_is_wr_enabled(s, reg - R_BANK1_INFO2_PAGE_CFG_0 +
                                               R_BANK1_INFO2_REGWEN_0)) {
            val32 &= BANK_INFO_PAGE_CFG_MASK;
            s->regs[reg] = val32;
        }
        break;
    case R_MP_BANK_CFG_SHADOWED:
        val32 &= (R_MP_BANK_CFG_SHADOWED_ERASE_EN_0_MASK |
                  R_MP_BANK_CFG_SHADOWED_ERASE_EN_1_MASK);
        s->regs[reg] = val32;
        break;
    case R_OP_STATUS:
        val32 &= R_OP_STATUS_DONE_MASK | R_OP_STATUS_ERR_MASK;
        s->regs[reg] = val32;
        break;
    case R_ERR_CODE:
        val32 &= ERR_CODE_MASK;
        s->regs[reg] &= ~val32; /* RW1C */
        break;
    case R_ECC_SINGLE_ERR_CNT:
        val32 &=
            (R_ECC_SINGLE_ERR_CNT_CNT_0_MASK | R_ECC_SINGLE_ERR_CNT_CNT_1_MASK);
        s->regs[reg] = val32;
        break;
    case R_PHY_ALERT_CFG:
        val32 &=
            (R_PHY_ALERT_CFG_ALERT_ACK_MASK | R_PHY_ALERT_CFG_ALERT_TRIG_MASK);
        s->regs[reg] = val32;
        break;
    case R_SCRATCH:
        s->regs[reg] = val32;
        break;
    case R_FIFO_LVL:
        val32 &= FIFO_LVL_PROG_MASK | FIFO_LVL_RD_MASK;
        s->regs[reg] = val32;
        ot_flash_update_rd_watermark(s);
        ot_flash_update_prog_watermark(s);
        break;
    case R_FIFO_RST:
        val32 &= R_FIFO_RST_EN_MASK;
        s->regs[reg] = val32;
        if (val32) {
            ot_flash_reset_rd_fifo(s);
            ot_flash_reset_prog_fifo(s);
        }
        break;
    case R_PROG_FIFO:
        if (s->op.kind != OP_PROG || s->op.hw) {
            /*
             * "This FIFO can only be programmed by software after a program
             * operation has been initiated via the `CONTROL` register."
             */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: write prog fifo when not in a sw prog op\n",
                          __func__);
            break;
        }
        if (!ot_fifo32_is_full(&s->prog_fifo)) {
            if (!ot_flash_fifo_in_reset(s)) {
                ot_fifo32_push(&s->prog_fifo, val32);
                s->regs[R_STATUS] &= ~R_STATUS_PROG_EMPTY_MASK;
                s->regs[R_INTR_STATE] &= ~INTR_PROG_EMPTY_MASK;
                ot_flash_update_prog_watermark(s);
                ot_flash_update_irqs(s);
            }
            if (ot_fifo32_is_full(&s->prog_fifo)) {
                s->regs[R_STATUS] |= R_STATUS_PROG_FULL_MASK;
            }
            if (ot_flash_operation_ongoing(s)) {
                ot_flash_op_execute(s);
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: Write full FIFO\n", __func__);
        }
        break;
    case R_FAULT_STATUS: {
        uint32_t rw0c_mask = (R_FAULT_STATUS_PHY_RELBL_ERR_MASK |
                              R_FAULT_STATUS_PHY_STORAGE_ERR_MASK);
        if (val32 & ~rw0c_mask) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: Write to R/O field in register 0x%03" HWADDR_PRIx
                          " (%s)\n",
                          __func__, addr, REG_NAME(reg));
        }
        val32 &= rw0c_mask;
        val32 |= ~rw0c_mask;
        s->regs[reg] &= val32;
        break;
    }
    case R_CTRL_REGWEN:
    case R_STATUS:
    case R_DEBUG_STATE:
    case R_RD_FIFO:
    case R_STD_FAULT_STATUS:
    case R_ERR_ADDR:
    case R_ECC_SINGLE_ERR_ADDR_0:
    case R_ECC_SINGLE_ERR_ADDR_1:
    case R_PHY_STATUS:
    case R_CURR_FIFO_LVL:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: R/O register 0x%03" HWADDR_PRIx " (%s)\n", __func__,
                      addr, REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
};

static uint64_t ot_flash_csrs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtFlashState *s = opaque;
    (void)size;
    uint32_t val32;

    hwaddr csr = R32_OFF(addr);

    switch (csr) {
    case R_CSR0_REGWEN:
    case R_CSR1:
    case R_CSR2:
    case R_CSR3:
    case R_CSR4:
    case R_CSR5:
    case R_CSR6:
    case R_CSR7:
    case R_CSR8:
    case R_CSR9:
    case R_CSR10:
    case R_CSR11:
    case R_CSR12:
    case R_CSR13:
    case R_CSR14:
    case R_CSR15:
    case R_CSR16:
    case R_CSR17:
    case R_CSR18:
    case R_CSR19:
    case R_CSR20:
        val32 = s->csrs[csr];
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        val32 = 0;
        break;
    }

    uint32_t pc = ibex_get_current_pc();
    trace_ot_flash_io_read_out((uint32_t)addr, CSR_NAME(csr), val32, pc);

    return (uint64_t)val32;
};

static void ot_flash_csrs_write(void *opaque, hwaddr addr, uint64_t val64,
                                unsigned size)
{
    OtFlashState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;

    hwaddr csr = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_flash_io_write((uint32_t)addr, CSR_NAME(csr), val32, pc);

    bool enable = s->csrs[R_CSR0_REGWEN] & R_CSR0_REGWEN_FIELD0_MASK;
    switch (csr) {
    case R_CSR0_REGWEN:
        val32 &= R_CSR0_REGWEN_FIELD0_MASK;
        break;
    case R_CSR1:
        val32 &= R_CSR1_FIELD0_MASK | R_CSR1_FIELD1_MASK;
        break;
    case R_CSR2:
        val32 &= R_CSR2_FIELD0_MASK | R_CSR2_FIELD1_MASK | R_CSR2_FIELD2_MASK |
                 R_CSR2_FIELD3_MASK | R_CSR2_FIELD4_MASK | R_CSR2_FIELD5_MASK |
                 R_CSR2_FIELD6_MASK | R_CSR2_FIELD7_MASK;
        break;
    case R_CSR3:
        val32 &= R_CSR3_FIELD0_MASK | R_CSR3_FIELD1_MASK | R_CSR3_FIELD2_MASK |
                 R_CSR3_FIELD3_MASK | R_CSR3_FIELD4_MASK | R_CSR3_FIELD5_MASK |
                 R_CSR3_FIELD6_MASK | R_CSR3_FIELD7_MASK | R_CSR3_FIELD8_MASK |
                 R_CSR3_FIELD9_MASK;
        break;
    case R_CSR4:
        val32 &= R_CSR4_FIELD0_MASK | R_CSR4_FIELD1_MASK | R_CSR4_FIELD2_MASK |
                 R_CSR4_FIELD3_MASK;
        break;
    case R_CSR5:
        val32 &= R_CSR5_FIELD0_MASK | R_CSR5_FIELD1_MASK | R_CSR5_FIELD2_MASK |
                 R_CSR5_FIELD3_MASK | R_CSR5_FIELD4_MASK;
        break;
    case R_CSR6:
        val32 &= R_CSR6_FIELD0_MASK | R_CSR6_FIELD1_MASK | R_CSR6_FIELD2_MASK |
                 R_CSR6_FIELD3_MASK | R_CSR6_FIELD4_MASK | R_CSR6_FIELD5_MASK |
                 R_CSR6_FIELD6_MASK | R_CSR6_FIELD7_MASK | R_CSR6_FIELD8_MASK;
        break;
    case R_CSR7:
        val32 &= R_CSR7_FIELD0_MASK | R_CSR7_FIELD1_MASK;
        break;
    case R_CSR8:
    case R_CSR9:
    case R_CSR10:
    case R_CSR11:
        break;
    case R_CSR12:
        val32 &= R_CSR12_FIELD0_MASK;
        break;
    case R_CSR13:
        val32 &= R_CSR13_FIELD0_MASK | R_CSR13_FIELD1_MASK;
        break;
    case R_CSR14:
        val32 &= R_CSR14_FIELD0_MASK | R_CSR14_FIELD1_MASK;
        break;
    case R_CSR15:
        val32 &= R_CSR15_FIELD0_MASK | R_CSR15_FIELD1_MASK;
        break;
    case R_CSR16:
        val32 &= R_CSR16_FIELD0_MASK | R_CSR16_FIELD1_MASK;
        break;
    case R_CSR17:
        val32 &= R_CSR17_FIELD0_MASK | R_CSR17_FIELD1_MASK;
        break;
    case R_CSR18:
        val32 &= R_CSR18_FIELD0_MASK;
        break;
    case R_CSR19:
        val32 &= R_CSR19_FIELD0_MASK;
        break;
    case R_CSR20:
        val32 &=
            R_CSR20_FIELD0_MASK | R_CSR20_FIELD1_MASK | R_CSR20_FIELD2_MASK;
        break;
    default:
        enable = false;
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }

    if (enable) {
        s->csrs[csr] = val32;
    }
}

static void ot_flash_load(OtFlashState *s, Error **errp)
{
    OtFlashStorage *flash = &s->flash;
    memset(flash, 0, sizeof(OtFlashStorage));

    uintptr_t base;
    unsigned flash_size;
    unsigned data_size;
    unsigned info_size;

    memset(flash->info_parts, 0, sizeof(flash->info_parts));

    if (s->blk) {
        uint64_t perm = BLK_PERM_CONSISTENT_READ |
                        (blk_supports_write_perm(s->blk) ? BLK_PERM_WRITE : 0);
        (void)blk_set_perm(s->blk, perm, perm, errp);

        static_assert(sizeof(OtFlashBackendHeader) == 32u,
                      "Invalid backend header size");

        QEMU_AUTO_VFREE OtFlashBackendHeader *header =
            blk_blockalign(s->blk, sizeof(OtFlashBackendHeader));

        int rc;
        /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) */
        rc = blk_pread(s->blk, 0, sizeof(*header), header, 0);
        if (rc < 0) {
            error_setg(errp, "failed to read the flash header content: %d", rc);
            return;
        }

        if (memcmp(header->magic, "vFSH", sizeof(header->magic)) != 0) {
            error_setg(errp, "Flash file is not a valid flash backend");
            return;
        }
        if (header->version != 1u) {
            error_setg(errp, "Flash file version is not supported");
            return;
        }

        /*
         * for now, only assert the flash file header matches local constants,
         * which should match the default configuration. A real implementation
         * should use these dynamic values, but this is fully out-of-scope for
         * now.
         */
        if (header->bank != REG_NUM_BANKS || header->info != NUM_INFO_TYPES ||
            header->page != REG_PAGES_PER_BANK ||
            header->psize != BYTES_PER_PAGE || header->ipp[0u] != NUM_INFOS0 ||
            header->ipp[1u] != NUM_INFOS1 || header->ipp[2u] != NUM_INFOS2 ||
            header->ipp[3u] != 0u) {
            error_setg(errp, "Flash file characteristics not supported");
            return;
        }

        data_size = header->page * header->psize;
        unsigned info_pages = 0;
        unsigned pg_offset = 0;
        for (unsigned ix = 0; ix < header->info; ix++) {
            unsigned size = header->ipp[ix] * header->psize;
            flash->info_parts[ix].size = size;
            flash->info_parts[ix].offset = pg_offset;
            pg_offset += size;
            info_pages += header->ipp[ix];
        }
        flash->info_part_count = header->info;
        info_size = info_pages * header->psize;
        flash_size = header->bank * (data_size + info_size);

        g_assert(pg_offset == info_size);

        flash->storage = blk_blockalign(s->blk, flash_size);
        base = (uintptr_t)flash->storage;
        g_assert(!(base & (sizeof(uint64_t) - 1u)));

        unsigned offset = offsetof(OtFlashBackendHeader, hlength) +
                          sizeof(header->hlength) + header->hlength;

        /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) */
        rc = blk_pread(s->blk, (int64_t)offset, flash_size, flash->storage, 0);
        if (rc < 0) {
            error_setg(errp, "failed to read the initial flash content: %d",
                       rc);
            return;
        }

        flash->bank_count = header->bank;
        flash->size = flash_size;

        /* two banks, OTRE+OTB0 binaries/bank */
        size_t debug_trailer_size =
            (size_t)(flash->bank_count) * ELFNAME_SIZE * BIN_APP_COUNT;
        uint8_t *elfnames = blk_blockalign(s->blk, debug_trailer_size);
        /* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) */
        rc = blk_pread(s->blk, (int64_t)offset + flash_size,
                       (int64_t)debug_trailer_size, elfnames, 0);
        /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */
        if (!rc) {
            const char *elfname = (const char *)elfnames;
            for (unsigned ix = 0; ix < BIN_APP_COUNT; ix++) {
                size_t elflen = strnlen(elfname, ELFNAME_SIZE);
                if (elflen > 0 && elflen < ELFNAME_SIZE) {
                    if (!access(elfname, F_OK)) {
                        if (load_elf_sym(elfname, 0, EM_RISCV, 1)) {
                            xtrace_ot_flash_error("Cannot load ELF symbols");
                        }
                    }
                }
                elfname += ELFNAME_SIZE;
            }
        }

        qemu_vfree(elfnames);
    } else {
        data_size = BYTES_PER_BANK;
        info_size = BYTES_PER_PAGE * (NUM_INFOS0 + NUM_INFOS1 + NUM_INFOS2);
        flash_size = REG_NUM_BANKS * (data_size + info_size);

        flash->storage =
            g_new0(uint32_t, DIV_ROUND_UP(flash_size, sizeof(uint32_t)));
        base = (uintptr_t)flash->storage;

        memset(flash->storage, 0xff, flash_size);

        flash->info_parts[0u].size = NUM_INFOS0 * BYTES_PER_PAGE;
        flash->info_parts[0u].offset = 0;
        flash->info_parts[1u].size = NUM_INFOS1 * BYTES_PER_PAGE;
        flash->info_parts[1u].offset =
            flash->info_parts[0u].offset + flash->info_parts[0u].size;
        flash->info_parts[2u].size = NUM_INFOS2 * BYTES_PER_PAGE;
        flash->info_parts[2u].offset =
            flash->info_parts[1u].offset + flash->info_parts[1u].size;
        flash->info_part_count = NUM_INFO_TYPES;

        flash->bank_count = REG_NUM_BANKS;
    }

    /*
     * Raw backend structure:
     * - HEADER
     * - DATA_PART bank 0
     * - DATA_PART bank 1
     * - INFO_PARTS bank 0:
     *   - INFO0 bank 0
     *   - INFO1 bank 0
     *   - INFO2 bank 0
     * - INFO_PARTS bank 1:
     *   - INFO0 bank 1
     *   - INFO1 bank 1
     *   - INFO2 bank 1
     * - Debug info (ELF file names)
     *
     * @todo Add ECC section to backend for ECC/ICV support also
     */
    flash->data = (uint32_t *)(base);
    flash->info =
        (uint32_t *)(base + (uintptr_t)(flash->bank_count * data_size));
    flash->data_size = data_size;
    flash->info_size = info_size;
}

#if DATA_PART_USE_IO_OPS
static uint64_t ot_flash_mem_read(void *opaque, hwaddr addr, unsigned size)
{
    OtFlashState *s = opaque;
    uint32_t val32;

    if (ot_flash_is_disabled(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: flash has been disabled\n",
                      __func__);
        return 0u;
    }

    if (addr < s->flash.bank_count * s->flash.data_size) {
        val32 = s->flash.data[addr >> 2u];
        unsigned offset = (unsigned)(addr & 0x3u);
        val32 >>= offset << 3u;
        uint32_t pc = ibex_get_current_pc();
#if LOG_GPR_ON_FLASH_DATA_ACCESS
#if LOG_GPR_ON_FLASH_DATA_ACCESS != UINT32_MAX
        if (pc == (uint32_t)LOG_GPR_ON_FLASH_DATA_ACCESS)
#endif
            ibex_log_vcpu_registers(
                RV_GPR_PC | RV_GPR_T0 | RV_GPR_T1 | RV_GPR_T2 | RV_GPR_A0 |
                RV_GPR_A1 | RV_GPR_A2);
#endif /* LOG_GPR_ON_FLASH_DATA_ACCESS */
        trace_ot_flash_mem_read_out((uint32_t)addr, size, val32, pc);
    } else {
        uint32_t pc = ibex_get_current_pc();
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad offset 0x%" HWADDR_PRIx ", pc=0x%x\n", __func__,
                      addr, pc);
        val32 = 0;
    }

    return (uint64_t)val32;
};
#endif /* #if DATA_PART_USE_IO_OPS */

static Property ot_flash_properties[] = {
    DEFINE_PROP_DRIVE("drive", OtFlashState, blk),
    DEFINE_PROP_LINK("vmapper", OtFlashState, vmapper, TYPE_OT_VMAPPER,
                     OtVMapperState *),
    /* Optionally disable memory protection, as searching for valid memory
    regions and checking their config can slow down regular operation. */
    DEFINE_PROP_BOOL("no-mem-prot", OtFlashState, no_mem_prot, false),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_flash_regs_ops = {
    .read = &ot_flash_regs_read,
    .write = &ot_flash_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static const MemoryRegionOps ot_flash_csrs_ops = {
    .read = &ot_flash_csrs_read,
    .write = &ot_flash_csrs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

#if DATA_PART_USE_IO_OPS
static const MemoryRegionOps ot_flash_mem_ops = {
    .read = &ot_flash_mem_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 1u,
    .impl.max_access_size = 4u,
};
#else
#if LOG_GPR_ON_FLASH_DATA_ACCESS
#warning "Cannot use LOG_GPR_ON_FLASH_DATA_ACCESS w/o DATA_PART_USE_IO_OPS"
#endif
#endif /* DATA_PART_USE_IO_OPS */

static void ot_flash_reset_enter(Object *obj, ResetType type)
{
    OtFlashClass *c = OT_FLASH_GET_CLASS(obj);
    OtFlashState *s = OT_FLASH(obj);

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    timer_del(s->op_delay);
    s->op.kind = OP_NONE;

    memset(s->regs, 0, REGS_SIZE);
    s->regs[R_INTR_STATE] = 0x3u;
    s->regs[R_DIS] = 0x9u;
    s->regs[R_CTRL_REGWEN] = 0x1u;
    s->regs[R_PROG_TYPE_EN] = 0x3u;
    s->regs[R_REGION_CFG_REGWEN_0] = 0x1u;
    s->regs[R_REGION_CFG_REGWEN_1] = 0x1u;
    s->regs[R_REGION_CFG_REGWEN_2] = 0x1u;
    s->regs[R_REGION_CFG_REGWEN_3] = 0x1u;
    s->regs[R_REGION_CFG_REGWEN_4] = 0x1u;
    s->regs[R_REGION_CFG_REGWEN_5] = 0x1u;
    s->regs[R_REGION_CFG_REGWEN_6] = 0x1u;
    s->regs[R_REGION_CFG_REGWEN_7] = 0x1u;
    s->regs[R_MP_REGION_CFG_0] = 0x9999999u;
    s->regs[R_MP_REGION_CFG_1] = 0x9999999u;
    s->regs[R_MP_REGION_CFG_2] = 0x9999999u;
    s->regs[R_MP_REGION_CFG_3] = 0x9999999u;
    s->regs[R_MP_REGION_CFG_4] = 0x9999999u;
    s->regs[R_MP_REGION_CFG_5] = 0x9999999u;
    s->regs[R_MP_REGION_CFG_6] = 0x9999999u;
    s->regs[R_MP_REGION_CFG_7] = 0x9999999u;
    s->regs[R_DEFAULT_REGION] = 0x999999u;
    s->regs[R_BANK0_INFO0_REGWEN_0] = 0x1u;
    s->regs[R_BANK0_INFO0_REGWEN_1] = 0x1u;
    s->regs[R_BANK0_INFO0_REGWEN_2] = 0x1u;
    s->regs[R_BANK0_INFO0_REGWEN_3] = 0x1u;
    s->regs[R_BANK0_INFO0_REGWEN_4] = 0x1u;
    s->regs[R_BANK0_INFO0_REGWEN_5] = 0x1u;
    s->regs[R_BANK0_INFO0_REGWEN_6] = 0x1u;
    s->regs[R_BANK0_INFO0_REGWEN_7] = 0x1u;
    s->regs[R_BANK0_INFO0_REGWEN_8] = 0x1u;
    s->regs[R_BANK0_INFO0_REGWEN_9] = 0x1u;
    s->regs[R_BANK0_INFO0_PAGE_CFG_0] = 0x9999999u;
    s->regs[R_BANK0_INFO0_PAGE_CFG_1] = 0x9999999u;
    s->regs[R_BANK0_INFO0_PAGE_CFG_2] = 0x9999999u;
    s->regs[R_BANK0_INFO0_PAGE_CFG_3] = 0x9999999u;
    s->regs[R_BANK0_INFO0_PAGE_CFG_4] = 0x9999999u;
    s->regs[R_BANK0_INFO0_PAGE_CFG_5] = 0x9999999u;
    s->regs[R_BANK0_INFO0_PAGE_CFG_6] = 0x9999999u;
    s->regs[R_BANK0_INFO0_PAGE_CFG_7] = 0x9999999u;
    s->regs[R_BANK0_INFO0_PAGE_CFG_8] = 0x9999999u;
    s->regs[R_BANK0_INFO0_PAGE_CFG_9] = 0x9999999u;
    s->regs[R_BANK0_INFO1_REGWEN] = 0x1u;
    s->regs[R_BANK0_INFO1_PAGE_CFG] = 0x9999999u;
    s->regs[R_BANK0_INFO2_REGWEN_0] = 0x1u;
    s->regs[R_BANK0_INFO2_REGWEN_1] = 0x1u;
    s->regs[R_BANK0_INFO2_PAGE_CFG_0] = 0x9999999u;
    s->regs[R_BANK0_INFO2_PAGE_CFG_1] = 0x9999999u;
    s->regs[R_BANK1_INFO0_REGWEN_0] = 0x1u;
    s->regs[R_BANK1_INFO0_REGWEN_1] = 0x1u;
    s->regs[R_BANK1_INFO0_REGWEN_2] = 0x1u;
    s->regs[R_BANK1_INFO0_REGWEN_3] = 0x1u;
    s->regs[R_BANK1_INFO0_REGWEN_4] = 0x1u;
    s->regs[R_BANK1_INFO0_REGWEN_5] = 0x1u;
    s->regs[R_BANK1_INFO0_REGWEN_6] = 0x1u;
    s->regs[R_BANK1_INFO0_REGWEN_7] = 0x1u;
    s->regs[R_BANK1_INFO0_REGWEN_8] = 0x1u;
    s->regs[R_BANK1_INFO0_REGWEN_9] = 0x1u;
    s->regs[R_BANK1_INFO0_PAGE_CFG_0] = 0x9999999u;
    s->regs[R_BANK1_INFO0_PAGE_CFG_1] = 0x9999999u;
    s->regs[R_BANK1_INFO0_PAGE_CFG_2] = 0x9999999u;
    s->regs[R_BANK1_INFO0_PAGE_CFG_3] = 0x9999999u;
    s->regs[R_BANK1_INFO0_PAGE_CFG_4] = 0x9999999u;
    s->regs[R_BANK1_INFO0_PAGE_CFG_5] = 0x9999999u;
    s->regs[R_BANK1_INFO0_PAGE_CFG_6] = 0x9999999u;
    s->regs[R_BANK1_INFO0_PAGE_CFG_7] = 0x9999999u;
    s->regs[R_BANK1_INFO0_PAGE_CFG_8] = 0x9999999u;
    s->regs[R_BANK1_INFO0_PAGE_CFG_9] = 0x9999999u;
    s->regs[R_BANK1_INFO1_REGWEN] = 0x1u;
    s->regs[R_BANK1_INFO1_PAGE_CFG] = 0x9999999u;
    s->regs[R_BANK1_INFO2_REGWEN_0] = 0x1u;
    s->regs[R_BANK1_INFO2_REGWEN_1] = 0x1u;
    s->regs[R_BANK1_INFO2_PAGE_CFG_0] = 0x9999999u;
    s->regs[R_BANK1_INFO2_PAGE_CFG_1] = 0x9999999u;
    s->regs[R_HW_INFO_CFG_OVERRIDE] = 0x99u;
    s->regs[R_BANK_CFG_REGWEN] = 0x1u;
    s->regs[R_STATUS] = 0xau;
    s->regs[R_PHY_STATUS] = 0x6u;
    s->regs[R_FIFO_LVL] = 0xf0fu;

    s->csrs[R_CSR0_REGWEN] = 0x1u;

    s->latched_alerts = 0u;

    s->phase = LC_PHASE_NONE;

    ot_flash_update_irqs(s);
    ot_flash_update_alerts(s);

    ot_flash_reset_rd_fifo(s);
    ot_flash_reset_prog_fifo(s);
}

static void ot_flash_reset_exit(Object *obj, ResetType type)
{
    OtFlashClass *c = OT_FLASH_GET_CLASS(obj);
    OtFlashState *s = OT_FLASH(obj);

    if (c->parent_phases.exit) {
        c->parent_phases.exit(obj, type);
    }

    ot_flash_update_exec(s);
}

static void ot_flash_realize(DeviceState *dev, Error **errp)
{
    OtFlashState *s = OT_FLASH(dev);
    (void)errp;

    g_assert(s->vmapper);

    ot_flash_load(s, &error_fatal);

    uint64_t size = (uint64_t)s->flash.data_size * s->flash.bank_count;
    MemoryRegion *mr = &s->mmio.mem;

#if DATA_PART_USE_IO_OPS
    memory_region_init_io(mr, OBJECT(dev), &ot_flash_mem_ops, s,
                          TYPE_OT_FLASH ".mem", size);
#else
    /* there is no "memory_region_init_rom_ptr" - use ram_ptr variant and r/o */
    memory_region_init_ram_ptr(mr, OBJECT(dev), TYPE_OT_FLASH ".mem", size,
                               (void *)s->flash.data);
    mr->readonly = true;
#endif /* DATA_PART_USE_IO_OPS */

    sysbus_init_mmio(SYS_BUS_DEVICE(s), mr);
}

static void ot_flash_init(Object *obj)
{
    OtFlashState *s = OT_FLASH(obj);

    memory_region_init_io(&s->mmio.regs, obj, &ot_flash_regs_ops, s,
                          TYPE_OT_FLASH ".regs", REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio.regs);

    memory_region_init_io(&s->mmio.csrs, obj, &ot_flash_csrs_ops, s,
                          TYPE_OT_FLASH ".csrs", CSRS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio.csrs);

    s->regs = g_new0(uint32_t, REGS_COUNT);
    s->csrs = g_new0(uint32_t, CSRS_COUNT);
    ot_fifo32_create(&s->rd_fifo, OT_FLASH_READ_FIFO_SIZE);
    ot_fifo32_create(&s->prog_fifo, OT_FLASH_PROG_FIFO_SIZE);

    for (unsigned ix = 0; ix < PARAM_NUM_IRQS; ix++) {
        ibex_sysbus_init_irq(obj, &s->irqs[ix]);
    }
    for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
        ibex_qdev_init_irq(obj, &s->alerts[ix], OT_DEVICE_ALERT);
    }
    s->op_delay = timer_new_ns(OT_VIRTUAL_CLOCK, &ot_flash_init_complete, s);
}

static void ot_flash_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->realize = &ot_flash_realize;
    device_class_set_props(dc, ot_flash_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    ResettableClass *rc = RESETTABLE_CLASS(klass);
    OtFlashClass *fc = OT_FLASH_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_flash_reset_enter, NULL,
                                       &ot_flash_reset_exit,
                                       &fc->parent_phases);
}

static const TypeInfo ot_flash_info = {
    .name = TYPE_OT_FLASH,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtFlashState),
    .instance_init = &ot_flash_init,
    .class_size = sizeof(OtFlashClass),
    .class_init = &ot_flash_class_init,
};

static void ot_flash_register_types(void)
{
    type_register_static(&ot_flash_info);
}

type_init(ot_flash_register_types);
