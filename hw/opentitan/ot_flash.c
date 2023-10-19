/*
 * QEMU OpenTitan Flash controller device
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
 *
 * Note: for now, only a minimalist subset of Power Manager device is
 *       implemented in order to enable OpenTitan's ROM boot to progress
 */

#include "qemu/osdep.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
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
#include "hw/opentitan/ot_otp.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "sysemu/block-backend.h"
#include "trace.h"

/* set to abort QEMU whenever guest code disable flash access */
#define ABORT_ON_DISABLEMENT 1

/* set to use I/O to access the flash partition */
#define DATA_PART_USE_IO_OPS 0

/* set to log hart GPR on flash data access */
#define LOG_GPR_ON_FLASH_DATA_ACCESS 0

/* temp. */
#pragma GCC diagnostic ignored "-Wunused-variable"

#define PARAM_NUM_IRQS   6u
#define PARAM_NUM_ALERTS 5u

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
    FIELD(ALERT_TEST, RECOV_ERR, 0u, 1u)
    FIELD(ALERT_TEST, FATAL_STD_ERR, 1u, 1u)
    FIELD(ALERT_TEST, FATAL_ERR, 2u, 1u)
    FIELD(ALERT_TEST, FATAL_PRIM, 3u, 1u)
    FIELD(ALERT_TEST, RECOV_PRIM, 4u, 1u)
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
    FIELD(CONTROL, NUM, 16, 12u)
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
    SHARED_FIELD(MP_REGION_CFG_RD_EN, 0u, 4u)
    SHARED_FIELD(MP_REGION_CFG_PROG_EN, 4u, 4u)
    SHARED_FIELD(MP_REGION_CFG_ERASE_EN, 8u, 4u)
    SHARED_FIELD(MP_REGION_CFG_SCRAMBLE_EN, 12u, 4u)
    SHARED_FIELD(MP_REGION_CFG_ECC_EN, 16u, 4u)
    SHARED_FIELD(MP_REGION_CFG_HE_EN, 20u, 4u)
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
    (R_ALERT_TEST_RECOV_ERR_MASK | \
     R_ALERT_TEST_FATAL_STD_ERR_MASK | \
     R_ALERT_TEST_FATAL_ERR_MASK | \
     R_ALERT_TEST_FATAL_PRIM_MASK | \
     R_ALERT_TEST_RECOV_PRIM_MASK)
#define BANK_INFO_PAGE_CFG_MASK \
    (BANK_INFO_PAGE_CFG_EN_MASK | \
     BANK_INFO_PAGE_CFG_RD_EN_MASK | \
     BANK_INFO_PAGE_CFG_PROG_EN_MASK | \
     BANK_INFO_PAGE_CFG_ERASE_EN_MASK | \
     BANK_INFO_PAGE_CFG_SCRAMBLE_EN_MASK | \
     BANK_INFO_PAGE_CFG_ECC_EN_MASK | \
     BANK_INFO_PAGE_CFG_HE_EN_MASK)
#define MP_REGION_CFG_MASK \
    (MP_REGION_CFG_RD_EN_MASK | \
     MP_REGION_CFG_PROG_EN_MASK | \
     MP_REGION_CFG_ERASE_EN_MASK | \
     MP_REGION_CFG_SCRAMBLE_EN_MASK | \
     MP_REGION_CFG_ECC_EN_MASK | \
     MP_REGION_CFG_HE_EN_MASK)
#define MP_REGION_MASK \
     (MP_REGION_BASE_MASK | \
      MP_REGION_SIZE_MASK)
#define CONTROL_MASK \
    (R_CONTROL_START_MASK | \
     R_CONTROL_OP_MASK | \
     R_CONTROL_PROG_SEL_MASK | \
     R_CONTROL_ERASE_SEL_MASK | \
     R_CONTROL_PARTITION_SEL_MASK | \
     R_CONTROL_INFO_SEL_MASK | \
     R_CONTROL_NUM_MASK);

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

typedef enum {
    OP_NONE,
    OP_INIT,
    OP_READ,
} OtFlashOperation;

enum {
    BIN_APP_OTRE,
    BIN_APP_OTB0,
    BIN_APP_COUNT,
};

#define OP_INIT_DURATION_NS     1000000u /* 1 ms */
#define ELFNAME_SIZE            256u
#define OT_FLASH_READ_FIFO_SIZE 16u

typedef struct {
    unsigned offset; /* storage offset in bank, relative to first info page */
    unsigned size; /* size in bytes of the partition */
} OtFlashInfoPart;

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

    struct {
        OtFlashOperation kind;
        unsigned count;
        unsigned address;
        unsigned info_sel;
        bool info_part;
    } op;
    OtFifo32 rd_fifo;
    OtFlashStorage flash;

    BlockBackend *blk; /* Flash backend */
};

static void ot_flash_update_irqs(OtFlashState *s)
{
    uint32_t level = s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE];
    trace_ot_csrng_irqs(s->regs[R_INTR_STATE], s->regs[R_INTR_ENABLE], level);
    for (unsigned ix = 0; ix < PARAM_NUM_IRQS; ix++) {
        ibex_irq_set(&s->irqs[ix], (int)((level >> ix) & 0x1u));
    }
}

static bool ot_flash_is_disabled(OtFlashState *s)
{
    return s->regs[R_DIS] != OT_MULTIBITBOOL4_FALSE;
}

static bool ot_flash_regs_is_wr_enabled(OtFlashState *s, unsigned regwen)
{
    return (bool)(s->regs[regwen] & REGWEN_EN_MASK);
}

static void ot_flash_op_signal(void *opaque)
{
    OtFlashState *s = opaque;

    switch (s->op.kind) {
    case OP_INIT:
        s->regs[R_STATUS] = FIELD_DP32(s->regs[R_STATUS], STATUS, INIT_WIP, 0u);
        s->regs[R_STATUS] =
            FIELD_DP32(s->regs[R_STATUS], STATUS, INITIALIZED, 1u);
        s->regs[R_PHY_STATUS] =
            FIELD_DP32(s->regs[R_PHY_STATUS], PHY_STATUS, INIT_WIP, 0u);
        trace_ot_flash_op_complete(s->op.kind, true);
        s->op.kind = OP_NONE;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: unexpected operation completion: %d\n", __func__,
                      s->op.kind);
        break;
    }
}

static void ot_flash_initialize(OtFlashState *s)
{
    if (s->op.kind != OP_NONE) {
        qemu_log_mask(LOG_GUEST_ERROR, "cannot initialize while in op");
        return;
    }

    s->op.kind = OP_INIT;
    trace_ot_flash_op_start(s->op.kind);
    s->regs[R_STATUS] = FIELD_DP32(s->regs[R_STATUS], STATUS, INIT_WIP, 1u);
    s->regs[R_PHY_STATUS] =
        FIELD_DP32(s->regs[R_PHY_STATUS], PHY_STATUS, INIT_WIP, 0u);
    timer_mod(s->op_delay,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + OP_INIT_DURATION_NS);
}

static bool ot_flash_fifo_in_reset(OtFlashState *s)
{
    return (bool)s->regs[R_FIFO_RST];
}

static void ot_flash_op_complete(OtFlashState *s, uint32_t ebit, uint32_t eaddr)
{
    /*
     * done is always signalled, even on error - at least this is what is
     * implemented in the OT flash_ctrl.c wait_for_done()
     */
    s->regs[R_OP_STATUS] |= R_OP_STATUS_DONE_MASK;
    s->regs[R_INTR_STATE] |= INTR_OP_DONE_MASK;
    if (ebit) {
        s->regs[R_OP_STATUS] |= R_OP_STATUS_ERR_MASK;
        s->regs[R_INTR_STATE] |= INTR_CORR_ERR_MASK;
        s->regs[R_ERR_ADDR] = FIELD_DP32(0, ERR_ADDR, ERR_ADDR, eaddr);
        s->regs[R_ERR_CODE] = ebit;
    }
    trace_ot_flash_op_complete(s->op.kind, !ebit);
    s->op.kind = OP_NONE;
    ot_flash_update_irqs(s);
}

static void ot_flash_op_read(OtFlashState *s)
{
    if (ot_fifo32_is_full(&s->rd_fifo)) {
        xtrace_ot_flash_error("read while RD FIFO full");
        return;
    }
    unsigned max_size;
    unsigned offset;
    unsigned address;
    OtFlashStorage *storage = &s->flash;
    uint32_t *src;

    if (s->op.info_part) {
        if (s->op.info_sel >= storage->info_part_count) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid info partition: %u\n",
                          __func__, s->op.info_sel);
            ot_flash_op_complete(s, R_ERR_CODE_MP_ERR_MASK, s->op.address);
            return;
        }
        max_size = storage->info_size;
        /* relative storage offset in the info storage */
        offset = storage->info_parts[s->op.info_sel].offset;
        unsigned bank_size = storage->data_size;
        /* extract the bank from the address */
        unsigned bank = s->op.address / bank_size;
        if (bank >= storage->bank_count) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid bank: %d\n", __func__,
                          bank);
            ot_flash_op_complete(s, R_ERR_CODE_MP_ERR_MASK, s->op.address);
            return;
        }
        /* get the adress relative to the bank */
        address = s->op.address % bank_size;
        if (address + s->op.count * sizeof(uint32_t) >=
            storage->info_parts[s->op.info_sel].size) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: invalid address in partition: %u %u\n", __func__,
                          address, s->op.info_sel);
            ot_flash_op_complete(s, R_ERR_CODE_MP_ERR_MASK, s->op.address);
            return;
        }
        /* add the bank offset of the first byte of info part */
        address += bank * storage->info_size;
        /* add the offset of the partition in the current bank */
        address += offset;
        src = storage->info;
        trace_ot_flash_info_part(s->op.address, bank, s->op.info_sel, address);
    } else {
        max_size = storage->data_size;
        address = s->op.address;
        src = storage->data;
    }

    /* sanity check */
    if (address >= max_size * storage->bank_count) {
        xtrace_ot_flash_error("read address out of bound");
        g_assert_not_reached();
    }

    /* convert to word address */
    address /= sizeof(uint32_t);

    while (s->op.count) {
        uint32_t word = src[address++];
        s->op.count--;
        if (!ot_flash_fifo_in_reset(s)) {
            ot_fifo32_push(&s->rd_fifo, word);
            s->regs[R_STATUS] &= ~R_STATUS_RD_EMPTY_MASK;
        }
        if (ot_fifo32_is_full(&s->rd_fifo)) {
            s->regs[R_STATUS] |= R_STATUS_RD_FULL_MASK;
            s->regs[R_INTR_STATE] |= INTR_RD_FULL_MASK;
            ot_flash_update_irqs(s);
            break;
        }
    }

    if (!s->op.count) {
        ot_flash_op_complete(s, 0u, 0u);
    }
}

static void ot_flash_op_execute(OtFlashState *s)
{
    switch (s->op.kind) {
    case OP_READ:
        trace_ot_flash_op_start(s->op.kind);
        ot_flash_op_read(s);
        break;
    default:
        xtrace_ot_flash_error("unsupported");
        break;
    }
}

static uint64_t ot_flash_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtFlashState *s = opaque;
    uint32_t val32;

    if (ot_flash_is_disabled(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: flash has been disabled\n",
                      __func__);
        return 0u;
    }

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
        val32 = s->regs[reg];
        break;
    case R_STATUS:
        val32 = FIELD_DP32(s->regs[reg], STATUS, RD_FULL,
                           (uint32_t)ot_fifo32_is_full(&s->rd_fifo));
        val32 = FIELD_DP32(val32, STATUS, RD_EMPTY,
                           (uint32_t)ot_fifo32_is_empty(&s->rd_fifo));
        break;
    case R_RD_FIFO:
        if (!ot_fifo32_is_empty(&s->rd_fifo)) {
            val32 = ot_fifo32_pop(&s->rd_fifo);
            s->regs[R_STATUS] &= ~R_STATUS_RD_FULL_MASK;
            s->regs[R_INTR_STATE] &= ~INTR_RD_FULL_MASK;
            if (ot_fifo32_is_empty(&s->rd_fifo)) {
                s->regs[R_STATUS] |= R_STATUS_RD_EMPTY_MASK;
            }
            ot_flash_update_irqs(s);
            if (s->op.count) {
                ot_flash_op_execute(s);
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: Read empty FIFO\n", __func__);
            val32 = 0;
        }
        break;
    case R_CURR_FIFO_LVL:
        val32 = ot_fifo32_num_used(&s->rd_fifo) << FIFO_LVL_RD_SHIFT;
        break;
    case R_ALERT_TEST:
    case R_PROG_FIFO:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "W/O register 0x%03" HWADDR_PRIx " (%s)\n", addr,
                      REG_NAME(reg));
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        val32 = 0;
        break;
    }

    uint64_t pc = ibex_get_current_pc();
    trace_ot_flash_io_read_out((unsigned)addr, REG_NAME(reg), (uint64_t)val32,
                               pc);

    return (uint64_t)val32;
};

static void ot_flash_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                                unsigned size)
{
    OtFlashState *s = opaque;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint64_t pc = ibex_get_current_pc();
    trace_ot_flash_io_write((unsigned)addr, REG_NAME(reg), val64, pc);

    if (ot_flash_is_disabled(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: flash has been disabled\n",
                      __func__);
        return;
    }

    switch (reg) {
    case R_INTR_STATE:
        val32 &= INTR_MASK;
        s->regs[R_INTR_STATE] &= ~val32; /* RW1C */
        ot_flash_update_irqs(s);
        break;
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
        if (val32) {
            for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
                ibex_irq_set(&s->alerts[ix], (int)((val32 >> ix) & 0x1u));
            }
        }
        break;
    case R_DIS:
        val32 &= R_DIS_VAL_MASK;
        s->regs[reg] &= val32; /* rw0c, multibitbool4 */
        if (ot_flash_is_disabled(s)) {
#if ABORT_ON_DISABLEMENT
            error_setg(&error_fatal, "flash controller disabled by SW");
#else
            xtrace_ot_flash_error("flash controller disabled by SW");
            memory_region_set_enabled(&s->mmio.mem, false);
            memory_region_set_enabled(&s->mmio.csrs, false);
            memory_region_set_enabled(&s->mmio.regs, false);
#endif
        }
        break;
    case R_INIT:
        val32 &= R_INIT_VAL_MASK;
        if (val32) {
            ot_flash_initialize(s);
        }
        break;
    case R_EXEC:
        s->regs[reg] = val32;
        break;
    case R_CONTROL:
        val32 &= CONTROL_MASK;
        s->regs[reg] = val32;
        bool start = (bool)FIELD_EX32(val32, CONTROL, START);
        unsigned op = (unsigned)FIELD_EX32(val32, CONTROL, OP);
        bool prog_sel = (bool)FIELD_EX32(val32, CONTROL, PROG_SEL);
        bool erase_sel = (bool)FIELD_EX32(val32, CONTROL, ERASE_SEL);
        bool part_sel = (bool)FIELD_EX32(val32, CONTROL, PARTITION_SEL);
        unsigned info_sel = (unsigned)FIELD_EX32(val32, CONTROL, INFO_SEL);
        unsigned num = (unsigned)FIELD_EX32(val32, CONTROL, NUM);
        if (start && s->op.kind == OP_NONE) {
            switch (op) {
            case 0:
                s->op.kind = OP_READ;
                s->op.address = s->regs[R_ADDR] & ~3u;
                s->op.info_part = part_sel;
                s->op.info_sel = info_sel;
                xtrace_ot_flash_info("Read from", s->op.address);
                break;
            default:
                qemu_log_mask(LOG_UNIMP, "%s: Operation %u not implemented\n",
                              __func__, op);
                ot_flash_op_complete(s, R_ERR_CODE_OP_ERR_MASK, 0u);
                return;
            }
            s->op.count = num + 1u;
        }
        ot_flash_op_execute(s);
        break;
    case R_ADDR:
        val32 &= R_ADDR_START_MASK;
        s->regs[reg] = val32;
        break;
    case R_PROG_TYPE_EN:
    case R_ERASE_SUSPEND:
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
    case R_CTRL_REGWEN:
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
        val32 &= MP_REGION_CFG_MASK;
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
    case R_OP_STATUS:
    case R_ERR_CODE:
    case R_ECC_SINGLE_ERR_CNT:
    case R_PHY_ALERT_CFG:
    case R_SCRATCH:
    case R_FIFO_LVL:
        val32 &= FIFO_LVL_PROG_MASK | FIFO_LVL_RD_MASK;
        s->regs[reg] = val32;
        break;
    case R_FIFO_RST:
        val32 &= R_FIFO_RST_EN_MASK;
        s->regs[reg] = val32;
        if (val32) {
            ot_fifo32_reset(&s->rd_fifo);
        }
    case R_PROG_FIFO:
        break;
    case R_STATUS:
    case R_DEBUG_STATE:
    case R_RD_FIFO:
    case R_STD_FAULT_STATUS:
    case R_FAULT_STATUS:
    case R_ERR_ADDR:
    case R_ECC_SINGLE_ERR_ADDR_0:
    case R_ECC_SINGLE_ERR_ADDR_1:
    case R_PHY_STATUS:
    case R_CURR_FIFO_LVL:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "R/O register 0x%03" HWADDR_PRIx " (%s)\n", addr,
                      REG_NAME(reg));
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
    uint32_t val32;

    if (ot_flash_is_disabled(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: flash has been disabled\n",
                      __func__);
        return 0u;
    }

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

    uint64_t pc = ibex_get_current_pc();
    trace_ot_flash_io_read_out((unsigned)addr, CSR_NAME(csr), (uint64_t)val32,
                               pc);

    return (uint64_t)val32;
};

static void ot_flash_csrs_write(void *opaque, hwaddr addr, uint64_t val64,
                                unsigned size)
{
    OtFlashState *s = opaque;
    uint32_t val32 = (uint32_t)val64;

    hwaddr csr = R32_OFF(addr);

    uint64_t pc = ibex_get_current_pc();
    trace_ot_flash_io_write((unsigned)addr, CSR_NAME(csr), val64, pc);

    if (ot_flash_is_disabled(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: flash has been disabled\n",
                      __func__);
        return;
    }

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

static Property ot_flash_properties[] = {
    DEFINE_PROP_DRIVE("drive", OtFlashState, blk),
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

static void ot_flash_reset(DeviceState *dev)
{
    OtFlashState *s = OT_FLASH(dev);

    timer_del(s->op_delay);
    s->op.kind = OP_NONE;

    memset(s->regs, 0, REGS_SIZE);
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

    ot_flash_update_irqs(s);
    for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
        ibex_irq_set(&s->alerts[ix], 0);
    }

    ot_fifo32_reset(&s->rd_fifo);
}

#ifdef USE_HEXDUMP
static char dbg_hexbuf[256];
static const char *ot_flash_hexdump(const uint8_t *buf, size_t size)
{
    static const char _hex[] = "0123456789ABCDEF";

    if (size > ((sizeof(dbg_hexbuf) / 2u) - 2u)) {
        size = sizeof(dbg_hexbuf) / 2u - 2u;
    }

    char *hexstr = dbg_hexbuf;
    for (unsigned int ix = 0; ix < size; ix++) {
        hexstr[(ix * 2)] = _hex[(buf[ix] >> 4) & 0xf];
        hexstr[(ix * 2) + 1] = _hex[buf[ix] & 0xf];
    }
    hexstr[size * 2] = '\0';
    return dbg_hexbuf;
}
#endif

static void ot_flash_load(OtFlashState *s, Error **errp)
{
    /*
     * Notes:
     *   1. only support read access to the flash backend
     *   2. only data partition for now
     */

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
        rc = blk_pread(s->blk, 0, sizeof(*header), header, 0);
        if (rc < 0) {
            error_setg(errp, "failed to read the flash header content: %d", rc);
            return;
        }

        if (memcmp(header->magic, "vFSH", sizeof(header->magic))) {
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
            flash->bank_count * ELFNAME_SIZE * BIN_APP_COUNT;
        uint8_t *elfnames = blk_blockalign(s->blk, debug_trailer_size);
        rc = blk_pread(s->blk, (int64_t)offset + flash_size, debug_trailer_size,
                       elfnames, 0);
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
     */
    flash->data = (uint32_t *)(base);
    flash->info = (uint32_t *)(base + flash->bank_count * data_size);
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
        uint64_t pc = ibex_get_current_pc();
#if LOG_GPR_ON_FLASH_DATA_ACCESS
#if LOG_GPR_ON_FLASH_DATA_ACCESS != UINT32_MAX
        if (pc == (uint64_t)LOG_GPR_ON_FLASH_DATA_ACCESS)
#endif
            ibex_log_vcpu_registers(
                RV_GPR_PC | RV_GPR_T0 | RV_GPR_T1 | RV_GPR_T2 | RV_GPR_A0 |
                RV_GPR_A1 | RV_GPR_A2);
#endif /* LOG_GPR_ON_FLASH_DATA_ACCESS */
        trace_ot_flash_mem_read_out((unsigned)addr, size, val32, pc);
    } else {
        uint64_t pc = ibex_get_current_pc();
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad offset 0x%" HWADDR_PRIx ", pc=0x%x\n", __func__,
                      addr, (unsigned)pc);
        val32 = 0;
    }

    return (uint64_t)val32;
};

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

static void ot_flash_realize(DeviceState *dev, Error **errp)
{
    OtFlashState *s = OT_FLASH(dev);

    ot_flash_load(s, &error_fatal);

    uint64_t size = (uint64_t)s->flash.data_size * s->flash.bank_count;
    MemoryRegion *mr = &s->mmio.mem;

#if DATA_PART_USE_IO_OPS
    memory_region_init_io(mr, OBJECT(dev), &ot_flash_mem_ops, s,
                          TYPE_OT_FLASH "-mem", size);
#else
    /* there is no "memory_region_init_rom_ptr" - use ram_ptr variant and r/o */
    memory_region_init_ram_ptr(mr, OBJECT(dev), TYPE_OT_FLASH "-mem", size,
                               (void *)s->flash.data);
    mr->readonly = true;
#endif /* DATA_PART_USE_IO_OPS */

    sysbus_init_mmio(SYS_BUS_DEVICE(s), mr);
}

static void ot_flash_init(Object *obj)
{
    OtFlashState *s = OT_FLASH(obj);

    memory_region_init_io(&s->mmio.regs, obj, &ot_flash_regs_ops, s,
                          TYPE_OT_FLASH "-regs", REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio.regs);

    memory_region_init_io(&s->mmio.csrs, obj, &ot_flash_csrs_ops, s,
                          TYPE_OT_FLASH "-csrs", CSRS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio.csrs);

    s->regs = g_new0(uint32_t, REGS_COUNT);
    s->csrs = g_new0(uint32_t, CSRS_COUNT);
    ot_fifo32_create(&s->rd_fifo, OT_FLASH_READ_FIFO_SIZE);

    for (unsigned ix = 0; ix < PARAM_NUM_IRQS; ix++) {
        ibex_sysbus_init_irq(obj, &s->irqs[ix]);
    }
    for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
        ibex_qdev_init_irq(obj, &s->alerts[ix], OPENTITAN_DEVICE_ALERT);
    }
    s->op_delay = timer_new_ns(QEMU_CLOCK_VIRTUAL, &ot_flash_op_signal, s);
}

static void ot_flash_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = &ot_flash_reset;
    dc->realize = &ot_flash_realize;
    device_class_set_props(dc, ot_flash_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_flash_info = {
    .name = TYPE_OT_FLASH,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtFlashState),
    .instance_init = &ot_flash_init,
    .class_init = &ot_flash_class_init,
};

static void ot_flash_register_types(void)
{
    type_register_static(&ot_flash_info);
}

type_init(ot_flash_register_types)
