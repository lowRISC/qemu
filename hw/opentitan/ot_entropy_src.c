/*
 * QEMU OpenTitan Earlgrey 1.0.0 Entropy Source device
 *
 * Copyright (c) 2023-2025 Rivos, Inc.
 * Copyright (c) 2025 lowRISC contributors.
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
 * Notes:
 * - missing correct handling of ALERT_FAIL_COUNTS, currently never incremented.
 * - missing some error handling? ES_MAIN_SM_ERR is the only error that can be
 *   triggered.
 *
 * v2: based on OpenTitan 011b6ea1fe
 * v3: based on OpenTitan 5fe6fe8605
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/typedefs.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_ast_eg.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_entropy_src.h"
#include "hw/opentitan/ot_fifo32.h"
#include "hw/opentitan/ot_otp.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "tomcrypt.h"
#include "trace.h"


#define NUM_ALERTS          2u
#define NUM_IRQS            4u
#define DISTR_FIFO_V2_DEPTH 2u
#define DISTR_FIFO_V3_DEPTH 3u
#define ES_FIFO_DEPTH       3u
#define OBSERVE_FIFO_DEPTH  32u
/* only in v3 */
#define RNG_BUS_WIDTH            4u
#define RNG_BUS_BIT_SEL_WIDTH    2u
#define HEALTH_TEST_WINDOW_WIDTH 18u

/*
 * Note about the register split into different groups:
 * ENTROPY_SRC module is supported in two variants, v2 and v3
 * There are very similar, but unfortunately v2 introduces a "version register"
 * which has been subsequently removed in v3, and this register is inserted
 * between other registers. This cause the address of all the subsequent
 * registers that follow to shift down 4 bytes. In order for QEMU to support
 * both versions with the same implementation, registers are therefore split
 * into two groups, called "lo" and "hi", which are located on each side of
 * the deprecated REV register. The register groups are mapped using two
 * different memory regions, in order to simplify support for both versions.
 *
 * There are a couple of register bitfields that also differ between versions,
 * namely the CONF and RECOV_ALERT_STS register, whose handling is specific
 * to the instantiated module version.
 */

/* clang-format off */

/* first group of registers, "lo" */
REG32(INTR_STATE, 0x00u)
    SHARED_FIELD(INTR_ES_ENTROPY_VALID, 0u, 1u)
    SHARED_FIELD(INTR_ES_HEALTH_TEST_FAILED, 1u, 1u)
    SHARED_FIELD(INTR_ES_OBSERVE_FIFO_READY, 2u, 1u)
    SHARED_FIELD(INTR_ES_FATAL_ERR, 3u, 1u)
REG32(INTR_ENABLE, 0x04u)
REG32(INTR_TEST, 0x08u)
REG32(ALERT_TEST, 0x0cu)
    FIELD(ALERT_TEST, RECOV_ALERT, 0u, 1u)
    FIELD(ALERT_TEST, FATAL_ALERT, 1u, 1u)
REG32(ME_REGWEN, 0x10u)
    FIELD(ME_REGWEN, EN, 0u, 1u)
REG32(SW_REGUPD, 0x14u)
    FIELD(SW_REGUPD, VAL, 0u, 1u)
REG32(REGWEN, 0x18u)
    FIELD(REGWEN, EN, 0u, 1u)

/* second group of registers, "rev", only used with v2 */
REG32(REV, 0x00u)
    FIELD(REV, ABI_REVISION, 0u, 8u)
    FIELD(REV, HW_REVISION, 8u, 8u)
    FIELD(REV, CHIP_TYPE, 16u, 8u)

/* third group of registers, "hi" */
REG32(MODULE_ENABLE, 0x00u)
    FIELD(MODULE_ENABLE, MODULE_ENABLE, 0u, 4u)
REG32(CONF, 0x04u)
    /* common bitfields for all versions */
    FIELD(CONF, FIPS_ENABLE, 0u, 4u)
    FIELD(CONF, FIPS_FLAG, 4u, 4u)
    FIELD(CONF, RNG_FIPS, 8u, 4u)
    FIELD(CONF, RNG_BIT_ENABLE, 12u, 4u)
    /* bitfields only defined on version 2 */
    FIELD(CONF, V2_RNG_BIT_SEL, 16u, 2u)
    FIELD(CONF, V2_THRESHOLD_SCOPE, 18u, 4u)
    FIELD(CONF, V2_ENTROPY_DATA_REG_ENABLE, 22u, 4u)
    /* bitfields only defined on for version 3 */
    FIELD(CONF, V3_THRESHOLD_SCOPE, 16u, 4u)
    FIELD(CONF, V3_ENTROPY_DATA_REG_ENABLE, 20u, 4u)
    FIELD(CONF, V3_RNG_BIT_SEL, 24u, 8u)
REG32(ENTROPY_CONTROL, 0x08u)
    FIELD(ENTROPY_CONTROL, ES_ROUTE, 0u, 4u)
    FIELD(ENTROPY_CONTROL, ES_TYPE, 4u, 4u)
REG32(ENTROPY_DATA, 0x0cu)
REG32(HEALTH_TEST_WINDOWS, 0x10u)
    FIELD(HEALTH_TEST_WINDOWS, FIPS_WINDOW, 0u, 16u)
    FIELD(HEALTH_TEST_WINDOWS, BYPASS_WINDOW, 16u, 16u)
REG32(REPCNT_THRESHOLDS, 0x14u)
    SHARED_FIELD(THRESHOLDS_FIPS, 0u, 16u)
    SHARED_FIELD(THRESHOLDS_BYPASS, 16u, 16u)
REG32(REPCNTS_THRESHOLDS, 0x18u)
REG32(ADAPTP_HI_THRESHOLDS, 0x1cu)
REG32(ADAPTP_LO_THRESHOLDS, 0x20u)
REG32(BUCKET_THRESHOLDS, 0x24u)
REG32(MARKOV_HI_THRESHOLDS, 0x28u)
REG32(MARKOV_LO_THRESHOLDS, 0x2cu)
REG32(EXTHT_HI_THRESHOLDS, 0x30u)
REG32(EXTHT_LO_THRESHOLDS, 0x34u)
REG32(REPCNT_HI_WATERMARKS, 0x38u)
    SHARED_FIELD(WATERMARK_FIPS, 0u, 16u)
    SHARED_FIELD(WATERMARK_BYPASS, 16u, 16u)
REG32(REPCNTS_HI_WATERMARKS, 0x3cu)
REG32(ADAPTP_HI_WATERMARKS, 0x40u)
REG32(ADAPTP_LO_WATERMARKS, 0x44u)
REG32(EXTHT_HI_WATERMARKS, 0x48u)
REG32(EXTHT_LO_WATERMARKS, 0x4cu)
REG32(BUCKET_HI_WATERMARKS, 0x50u)
REG32(MARKOV_HI_WATERMARKS, 0x54u)
REG32(MARKOV_LO_WATERMARKS, 0x58u)
REG32(REPCNT_TOTAL_FAILS, 0x5cu)
REG32(REPCNTS_TOTAL_FAILS, 0x60u)
REG32(ADAPTP_HI_TOTAL_FAILS, 0x64u)
REG32(ADAPTP_LO_TOTAL_FAILS, 0x68u)
REG32(BUCKET_TOTAL_FAILS, 0x6cu)
REG32(MARKOV_HI_TOTAL_FAILS, 0x70u)
REG32(MARKOV_LO_TOTAL_FAILS, 0x74u)
REG32(EXTHT_HI_TOTAL_FAILS, 0x78u)
REG32(EXTHT_LO_TOTAL_FAILS, 0x7cu)
REG32(ALERT_THRESHOLD, 0x80u)
    FIELD(ALERT_THRESHOLD, VAL, 0u, 16u)
    FIELD(ALERT_THRESHOLD, INV, 16u, 16u)
REG32(ALERT_SUMMARY_FAIL_COUNTS, 0x84u)
    FIELD(ALERT_SUMMARY_FAIL_COUNTS, ANY_FAIL_COUNT, 0u, 16u)
REG32(ALERT_FAIL_COUNTS, 0x88u)
    FIELD(ALERT_FAIL_COUNTS, REPCNT_FAIL_COUNT, 4u, 4u)
    FIELD(ALERT_FAIL_COUNTS, ADAPTP_HI_FAIL_COUNT, 8u, 4u)
    FIELD(ALERT_FAIL_COUNTS, ADAPTP_LO_FAIL_COUNT, 12u, 4u)
    FIELD(ALERT_FAIL_COUNTS, BUCKET_FAIL_COUNT, 16u, 4u)
    FIELD(ALERT_FAIL_COUNTS, MARKOV_HI_FAIL_COUNT, 20u, 4u)
    FIELD(ALERT_FAIL_COUNTS, MARKOV_LO_FAIL_COUNT, 24u, 4u)
    FIELD(ALERT_FAIL_COUNTS, REPCNTS_FAIL_COUNT, 28u, 4u)
REG32(EXTHT_FAIL_COUNTS, 0x8cu)
    FIELD(EXTHT_FAIL_COUNTS, EXTHT_HI_FAIL_COUNT, 0u, 4u)
    FIELD(EXTHT_FAIL_COUNTS, EXTHT_LO_FAIL_COUNT, 4u, 4u)
REG32(FW_OV_CONTROL, 0x90u)
    FIELD(FW_OV_CONTROL, FW_OV_MODE, 0u, 4u)
    FIELD(FW_OV_CONTROL, FW_OV_ENTROPY_INSERT, 4u, 4u)
REG32(FW_OV_SHA3_START, 0x94u)
    FIELD(FW_OV_SHA3_START, FW_OV_INSERT_START, 0u, 4u)
REG32(FW_OV_WR_FIFO_FULL, 0x98u)
    FIELD(FW_OV_WR_FIFO_FULL, VAL, 0u, 1u)
REG32(FW_OV_RD_FIFO_OVERFLOW, 0x9cu)
    FIELD(FW_OV_RD_FIFO_OVERFLOW, VAL, 0u, 1u)
REG32(FW_OV_RD_DATA, 0xa0u)
REG32(FW_OV_WR_DATA, 0xa4u)
REG32(OBSERVE_FIFO_THRESH, 0xa8u)
    FIELD(OBSERVE_FIFO_THRESH, VAL, 0u, 6u)
REG32(OBSERVE_FIFO_DEPTH, 0xacu)
    FIELD(OBSERVE_FIFO_DEPTH, VAL, 0u, 6u)
REG32(DEBUG_STATUS, 0xb0u)
    FIELD(DEBUG_STATUS, ENTROPY_FIFO_DEPTH, 0u, 2u)
    FIELD(DEBUG_STATUS, SHA3_FSM, 3u, 3u)
    FIELD(DEBUG_STATUS, SHA3_BLOCK_PR, 6u, 1u)
    FIELD(DEBUG_STATUS, SHA3_SQUEEZING, 7u, 1u)
    FIELD(DEBUG_STATUS, SHA3_ABSORBED, 8u, 1u)
    FIELD(DEBUG_STATUS, SHA3_ERR, 9u, 1u)
    FIELD(DEBUG_STATUS, MAIN_SM_IDLE, 16u, 1u)
    FIELD(DEBUG_STATUS, MAIN_SM_BOOT_DONE, 17u, 1u)
REG32(RECOV_ALERT_STS, 0xb4u)
    FIELD(RECOV_ALERT_STS, FIPS_ENABLE_FIELD_ALERT, 0u, 1u)
    FIELD(RECOV_ALERT_STS, ENTROPY_DATA_REG_ENABLE_FIELD_ALERT, 1u, 1u)
    FIELD(RECOV_ALERT_STS, MODULE_ENABLE_FIELD_ALERT, 2u, 1u)
    FIELD(RECOV_ALERT_STS, THRESHOLD_SCOPE_FIELD_ALERT, 3u, 1u)
    FIELD(RECOV_ALERT_STS, RNG_BIT_ENABLE_FIELD_ALERT, 5u, 1u)
    FIELD(RECOV_ALERT_STS, FW_OV_SHA3_START_FIELD_ALERT, 7u, 1u)
    FIELD(RECOV_ALERT_STS, FW_OV_MODE_FIELD_ALERT, 8u, 1u)
    FIELD(RECOV_ALERT_STS, FW_OV_ENTROPY_INSERT_FIELD_ALERT, 9u, 1u)
    FIELD(RECOV_ALERT_STS, ES_ROUTE_FIELD_ALERT, 10u, 1u)
    FIELD(RECOV_ALERT_STS, ES_TYPE_FIELD_ALERT, 11u, 1u)
    FIELD(RECOV_ALERT_STS, ES_MAIN_SM_ALERT, 12u, 1u)
    FIELD(RECOV_ALERT_STS, ES_BUS_CMP_ALERT, 13u, 1u)
    FIELD(RECOV_ALERT_STS, ES_THRESH_CFG_ALERT, 14u, 1u)
    FIELD(RECOV_ALERT_STS, ES_FW_OV_WR_ALERT, 15u, 1u)
    FIELD(RECOV_ALERT_STS, ES_FW_OV_DISABLE_ALERT, 16u, 1u)
    FIELD(RECOV_ALERT_STS, FIPS_FLAG_FIELD_ALERT, 17u, 1u)
    FIELD(RECOV_ALERT_STS, RNG_FIPS_FIELD_ALERT, 18u, 1u)
    FIELD(RECOV_ALERT_STS, POSTHT_ENTROPY_DROP_ALERT, 31u, 1u)
REG32(ERR_CODE, 0xb8u)
    FIELD(ERR_CODE, SFIFO_ESRNG_ERR, 0u, 1u)
    FIELD(ERR_CODE, SFIFO_DISTR_ERR, 1u, 1u)
    FIELD(ERR_CODE, SFIFO_OBSERVE_ERR, 2u, 1u)
    FIELD(ERR_CODE, SFIFO_ESFINAL_ERR, 3u, 1u)
    FIELD(ERR_CODE, ES_ACK_SM_ERR, 20u, 1u)
    FIELD(ERR_CODE, ES_MAIN_SM_ERR, 21u, 1u)
    FIELD(ERR_CODE, ES_CNTR_ERR, 22u, 1u)
    FIELD(ERR_CODE, SHA3_STATE_ERR, 23u, 1u)
    FIELD(ERR_CODE, SHA3_RST_STORAGE_ERR, 24u, 1u)
    FIELD(ERR_CODE, FIFO_WRITE_ERR, 28u, 1u)
    FIELD(ERR_CODE, FIFO_READ_ERR, 29u, 1u)
    FIELD(ERR_CODE, FIFO_STATE_ERR, 30u, 1u)
REG32(ERR_CODE_TEST, 0xbcu)
    FIELD(ERR_CODE_TEST, VAL, 0u, 5u)
REG32(MAIN_SM_STATE, 0xc0u)
    FIELD(MAIN_SM_STATE, VAL, 0u, 9u)
/* clang-format on */

#define INTR_WMASK \
    (INTR_ES_ENTROPY_VALID_MASK | INTR_ES_HEALTH_TEST_FAILED_MASK | \
     INTR_ES_OBSERVE_FIFO_READY_MASK | INTR_ES_FATAL_ERR_MASK)
#define ALERT_TEST_WMASK \
    (R_ALERT_TEST_RECOV_ALERT_MASK | R_ALERT_TEST_FATAL_ALERT_MASK)
#define CONF_V2_WMASK \
    (R_CONF_FIPS_ENABLE_MASK | R_CONF_FIPS_FLAG_MASK | R_CONF_RNG_FIPS_MASK | \
     R_CONF_RNG_BIT_ENABLE_MASK | R_CONF_V2_RNG_BIT_SEL_MASK | \
     R_CONF_V2_THRESHOLD_SCOPE_MASK | R_CONF_V2_ENTROPY_DATA_REG_ENABLE_MASK)
#define CONF_V3_WMASK UINT32_MAX
#define ENTROPY_CONTROL_WMASK \
    (R_ENTROPY_CONTROL_ES_ROUTE_MASK | R_ENTROPY_CONTROL_ES_TYPE_MASK)
#define FW_OV_CONTROL_WMASK \
    (R_FW_OV_CONTROL_FW_OV_MODE_MASK | \
     R_FW_OV_CONTROL_FW_OV_ENTROPY_INSERT_MASK)
#define RECOV_ALERT_STS_WMASK \
    (R_RECOV_ALERT_STS_FIPS_ENABLE_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_ENTROPY_DATA_REG_ENABLE_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_MODULE_ENABLE_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_THRESHOLD_SCOPE_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_RNG_BIT_ENABLE_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_FW_OV_SHA3_START_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_FW_OV_MODE_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_FW_OV_ENTROPY_INSERT_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_ES_ROUTE_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_ES_TYPE_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_ES_MAIN_SM_ALERT_MASK | \
     R_RECOV_ALERT_STS_ES_BUS_CMP_ALERT_MASK | \
     R_RECOV_ALERT_STS_ES_THRESH_CFG_ALERT_MASK | \
     R_RECOV_ALERT_STS_ES_FW_OV_WR_ALERT_MASK | \
     R_RECOV_ALERT_STS_ES_FW_OV_DISABLE_ALERT_MASK | \
     R_RECOV_ALERT_STS_FIPS_FLAG_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_RNG_FIPS_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_POSTHT_ENTROPY_DROP_ALERT_MASK)
#define ERR_CODE_MASK \
    (R_ERR_CODE_SFIFO_ESRNG_ERR_MASK | R_ERR_CODE_SFIFO_DISTR_ERR_MASK | \
     R_ERR_CODE_SFIFO_OBSERVE_ERR_MASK | R_ERR_CODE_SFIFO_ESFINAL_ERR_MASK | \
     R_ERR_CODE_ES_ACK_SM_ERR_MASK | R_ERR_CODE_ES_MAIN_SM_ERR_MASK | \
     R_ERR_CODE_ES_CNTR_ERR_MASK | R_ERR_CODE_SHA3_STATE_ERR_MASK | \
     R_ERR_CODE_SHA3_RST_STORAGE_ERR_MASK | R_ERR_CODE_FIFO_WRITE_ERR_MASK | \
     R_ERR_CODE_FIFO_READ_ERR_MASK | R_ERR_CODE_FIFO_STATE_ERR_MASK)
#define ERR_CODE_FATAL_ERROR_MASK \
    (R_ERR_CODE_ES_ACK_SM_ERR_MASK | R_ERR_CODE_ES_MAIN_SM_ERR_MASK | \
     R_ERR_CODE_ES_CNTR_ERR_MASK | R_ERR_CODE_SHA3_STATE_ERR_MASK | \
     R_ERR_CODE_SHA3_RST_STORAGE_ERR_MASK)

/*
 * this is an alias for the CHECK_MULTIBOOT macro, as the RECOV_ALERT_STS
 * bit for the FW_OV_SHA3_START register is the only register bit that does
 * not follow the same naming as its relative register
 * */
#define R_RECOV_ALERT_STS_FW_OV_INSERT_START_FIELD_ALERT_SHIFT \
    R_RECOV_ALERT_STS_FW_OV_SHA3_START_FIELD_ALERT_SHIFT

#define ALERT_STATUS_BIT(_x_) R_RECOV_ALERT_STS_##_x_##_FIELD_ALERT_SHIFT

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_LO_REG (R_REGWEN)
#define REGS_LO_COUNT (R_LAST_LO_REG + 1u)
#define REGS_LO_SIZE  (REGS_LO_COUNT * sizeof(uint32_t))
#define REGS_LO_BASE  0x00u
#define REG_LO_NAME(_reg_) \
    ((((_reg_) <= REGS_LO_COUNT) && REG_LO_NAMES[_reg_]) ? \
         REG_LO_NAMES[_reg_] : \
         "?")

/* only used in version 2 */
#define R_LAST_REV_REG (R_REV)
#define REGS_REV_COUNT (R_LAST_REV_REG + 1u)
#define REGS_REV_SIZE  (REGS_REV_COUNT * sizeof(uint32_t))
#define REGS_REV_BASE  0x1cu
#define REG_REV_NAME(_reg_) \
    ((((_reg_) <= REGS_REV_COUNT) && REG_REV_NAMES[_reg_]) ? \
         REG_REV_NAMES[_reg_] : \
         "?")

#define R_LAST_HI_REG   (R_MAIN_SM_STATE)
#define REGS_HI_COUNT   (R_LAST_HI_REG + 1u)
#define REGS_HI_SIZE    (REGS_HI_COUNT * sizeof(uint32_t))
#define REGS_HI_V2_BASE 0x20u
#define REGS_HI_V3_BASE 0x1cu
#define REG_HI_NAME(_reg_) \
    ((((_reg_) <= REGS_HI_COUNT) && REG_HI_NAMES[_reg_]) ? \
         REG_HI_NAMES[_reg_] : \
         "?")

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_LO_NAMES[REGS_LO_COUNT] = {
    /* clang-format off */
    REG_NAME_ENTRY(INTR_STATE),
    REG_NAME_ENTRY(INTR_ENABLE),
    REG_NAME_ENTRY(INTR_TEST),
    REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(ME_REGWEN),
    REG_NAME_ENTRY(SW_REGUPD),
    REG_NAME_ENTRY(REGWEN),
    /* clang-format on */
};

/* only used in version 2 */
static const char *REG_REV_NAMES[REGS_REV_COUNT] = {
    /* clang-format off */
    REG_NAME_ENTRY(REV),
    /* clang-format on */
};

static const char *REG_HI_NAMES[REGS_HI_COUNT] = {
    /* clang-format off */
    REG_NAME_ENTRY(MODULE_ENABLE),
    REG_NAME_ENTRY(CONF),
    REG_NAME_ENTRY(ENTROPY_CONTROL),
    REG_NAME_ENTRY(ENTROPY_DATA),
    REG_NAME_ENTRY(HEALTH_TEST_WINDOWS),
    REG_NAME_ENTRY(REPCNT_THRESHOLDS),
    REG_NAME_ENTRY(REPCNTS_THRESHOLDS),
    REG_NAME_ENTRY(ADAPTP_HI_THRESHOLDS),
    REG_NAME_ENTRY(ADAPTP_LO_THRESHOLDS),
    REG_NAME_ENTRY(BUCKET_THRESHOLDS),
    REG_NAME_ENTRY(MARKOV_HI_THRESHOLDS),
    REG_NAME_ENTRY(MARKOV_LO_THRESHOLDS),
    REG_NAME_ENTRY(EXTHT_HI_THRESHOLDS),
    REG_NAME_ENTRY(EXTHT_LO_THRESHOLDS),
    REG_NAME_ENTRY(REPCNT_HI_WATERMARKS),
    REG_NAME_ENTRY(REPCNTS_HI_WATERMARKS),
    REG_NAME_ENTRY(ADAPTP_HI_WATERMARKS),
    REG_NAME_ENTRY(ADAPTP_LO_WATERMARKS),
    REG_NAME_ENTRY(EXTHT_HI_WATERMARKS),
    REG_NAME_ENTRY(EXTHT_LO_WATERMARKS),
    REG_NAME_ENTRY(BUCKET_HI_WATERMARKS),
    REG_NAME_ENTRY(MARKOV_HI_WATERMARKS),
    REG_NAME_ENTRY(MARKOV_LO_WATERMARKS),
    REG_NAME_ENTRY(REPCNT_TOTAL_FAILS),
    REG_NAME_ENTRY(REPCNTS_TOTAL_FAILS),
    REG_NAME_ENTRY(ADAPTP_HI_TOTAL_FAILS),
    REG_NAME_ENTRY(ADAPTP_LO_TOTAL_FAILS),
    REG_NAME_ENTRY(BUCKET_TOTAL_FAILS),
    REG_NAME_ENTRY(MARKOV_HI_TOTAL_FAILS),
    REG_NAME_ENTRY(MARKOV_LO_TOTAL_FAILS),
    REG_NAME_ENTRY(EXTHT_HI_TOTAL_FAILS),
    REG_NAME_ENTRY(EXTHT_LO_TOTAL_FAILS),
    REG_NAME_ENTRY(ALERT_THRESHOLD),
    REG_NAME_ENTRY(ALERT_SUMMARY_FAIL_COUNTS),
    REG_NAME_ENTRY(ALERT_FAIL_COUNTS),
    REG_NAME_ENTRY(EXTHT_FAIL_COUNTS),
    REG_NAME_ENTRY(FW_OV_CONTROL),
    REG_NAME_ENTRY(FW_OV_SHA3_START),
    REG_NAME_ENTRY(FW_OV_WR_FIFO_FULL),
    REG_NAME_ENTRY(FW_OV_RD_FIFO_OVERFLOW),
    REG_NAME_ENTRY(FW_OV_RD_DATA),
    REG_NAME_ENTRY(FW_OV_WR_DATA),
    REG_NAME_ENTRY(OBSERVE_FIFO_THRESH),
    REG_NAME_ENTRY(OBSERVE_FIFO_DEPTH),
    REG_NAME_ENTRY(DEBUG_STATUS),
    REG_NAME_ENTRY(RECOV_ALERT_STS),
    REG_NAME_ENTRY(ERR_CODE),
    REG_NAME_ENTRY(ERR_CODE_TEST),
    REG_NAME_ENTRY(MAIN_SM_STATE),
    /* clang-format on */
};
#undef REG_NAME_ENTRY

/**
 * Use a 128-bit incoming packet size (HW uses 4-bit packet) in order to limit
 * feed rate to ~0.7 ms max. 128-bit packet can be divided down to 32-bit
 * FIFO packets. They are assembled into either 384-bit or 2048-bit packets.
 */
#define ES_FILL_BITS        128u
#define ES_FINAL_FIFO_DEPTH 4u
#define ES_FILL_RATE_NS \
    ((NANOSECONDS_PER_SECOND * ES_FILL_BITS) / \
     ((uint64_t)OT_AST_EG_RANDOM_4BIT_RATE * 4u))
#define OT_ENTROPY_SRC_FILL_WORD_COUNT (ES_FILL_BITS / (8u * sizeof(uint32_t)))
#define ES_WORD_COUNT                  (OT_ENTROPY_SRC_WORD_COUNT)
#define ES_SWREAD_FIFO_WORD_COUNT      ES_WORD_COUNT
#define ES_FINAL_FIFO_WORD_COUNT       (ES_WORD_COUNT * ES_FINAL_FIFO_DEPTH)
#define ES_HEXBUF_SIZE                 ((8U * 2u + 1u) * ES_WORD_COUNT + 4u)

/*
 * see hw/ip/edn/doc/#multiple-edns-in-boot-time-request-mode
 * reduce initial delay in QEMU since it takes time to manage the entropy
 */
#define OT_ENTROPY_SRC_BOOT_DELAY_NS 500000LL /* 500 us */
/*
 * default delay to pace the entropy src client (CSRNG) when no entropy is
 * available. A better implementation would compute the remaining time before
 * the next available entropy packet.
 */
#define OT_ENTROPY_SRC_WAIT_DELAY_NS 2000LL /* 2 us */

enum {
    ALERT_RECOVERABLE,
    ALERT_FATAL,
    ALERT_COUNT,
};

static_assert(ALERT_COUNT == NUM_ALERTS, "Invalid alert count");

typedef enum {
    ENTROPY_SRC_IDLE,
    ENTROPY_SRC_BOOT_HT_RUNNING,
    ENTROPY_SRC_BOOT_POST_HT_CHK,
    ENTROPY_SRC_BOOT_PHASE_DONE,
    ENTROPY_SRC_STARTUP_HT_START,
    ENTROPY_SRC_STARTUP_PHASE1,
    ENTROPY_SRC_STARTUP_PASS1,
    ENTROPY_SRC_STARTUP_FAIL1,
    ENTROPY_SRC_CONT_HT_START,
    ENTROPY_SRC_CONT_HT_RUNNING,
    ENTROPY_SRC_FW_INSERT_START,
    ENTROPY_SRC_FW_INSERT_MSG,
    ENTROPY_SRC_SHA3_MSG_DONE,
    ENTROPY_SRC_SHA3_PROCESS,
    ENTROPY_SRC_SHA3_VALID,
    ENTROPY_SRC_SHA3_DONE,
    ENTROPY_SRC_ALERT_STATE,
    ENTROPY_SRC_ALERT_HANG,
    ENTROPY_SRC_ERROR,
} OtEntropySrcFsmState;

struct OtEntropySrcState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    MemoryRegion mmio_lo;
    MemoryRegion mmio_rev; /* only in v2 */
    MemoryRegion mmio_hi;
    IbexIRQ irqs[NUM_IRQS];
    IbexIRQ alerts[NUM_ALERTS];
    QEMUTimer *scheduler;

    uint32_t *regs_lo;
    uint32_t *regs_rev; /* only on v2 */
    uint32_t *regs_hi;

    OtFifo32 input_fifo; /* not in real HW, used to reduce feed rate */
    OtFifo32 precon_fifo; /* 32-to-64 SHA3 input packer */
    OtFifo32 bypass_fifo; /* 32-to-384 packer */
    OtFifo32 observe_fifo;
    OtFifo32 swread_fifo;
    OtFifo32 final_fifo; /* output FIFO */
    hash_state sha3_state; /* libtomcrypt hash state */
    OtEntropySrcFsmState state;
    unsigned cond_word; /* count of words processed with SHA3 till hash */
    unsigned noise_count; /* count of consumed noise words since enabled */
    unsigned packet_count; /* count of output packets since enabled */
    bool obs_fifo_en; /* observe FIFO accept incoming data */

    char *ot_id;
    unsigned version; /* emulated version */
    OtASTEgState *ast;
    OtOTPState *otp_ctrl;
};

static const uint16_t OtEDNFsmStateCode[] = {
    [ENTROPY_SRC_IDLE] = 0b011110101,
    [ENTROPY_SRC_BOOT_HT_RUNNING] = 0b111010010,
    [ENTROPY_SRC_BOOT_POST_HT_CHK] = 0b101101110,
    [ENTROPY_SRC_BOOT_PHASE_DONE] = 0b010001110,
    [ENTROPY_SRC_STARTUP_HT_START] = 0b000101100,
    [ENTROPY_SRC_STARTUP_PHASE1] = 0b100000001,
    [ENTROPY_SRC_STARTUP_PASS1] = 0b110100101,
    [ENTROPY_SRC_STARTUP_FAIL1] = 0b000010111,
    [ENTROPY_SRC_CONT_HT_START] = 0b001000000,
    [ENTROPY_SRC_CONT_HT_RUNNING] = 0b110100010,
    [ENTROPY_SRC_FW_INSERT_START] = 0b011000011,
    [ENTROPY_SRC_FW_INSERT_MSG] = 0b001011001,
    [ENTROPY_SRC_SHA3_MSG_DONE] = 0b100001111,
    [ENTROPY_SRC_SHA3_PROCESS] = 0b011111000,
    [ENTROPY_SRC_SHA3_VALID] = 0b010111111,
    [ENTROPY_SRC_SHA3_DONE] = 0b110011000,
    [ENTROPY_SRC_ALERT_STATE] = 0b111001101,
    [ENTROPY_SRC_ALERT_HANG] = 0b111111011,
    [ENTROPY_SRC_ERROR] = 0b001110011,
};

#define STATE_NAME_ENTRY(_st_) [ENTROPY_SRC_##_st_] = stringify(_st_)
static const char *STATE_NAMES[] = {
    STATE_NAME_ENTRY(IDLE),
    STATE_NAME_ENTRY(BOOT_HT_RUNNING),
    STATE_NAME_ENTRY(BOOT_POST_HT_CHK),
    STATE_NAME_ENTRY(BOOT_PHASE_DONE),
    STATE_NAME_ENTRY(STARTUP_HT_START),
    STATE_NAME_ENTRY(STARTUP_PHASE1),
    STATE_NAME_ENTRY(STARTUP_PASS1),
    STATE_NAME_ENTRY(STARTUP_FAIL1),
    STATE_NAME_ENTRY(CONT_HT_START),
    STATE_NAME_ENTRY(CONT_HT_RUNNING),
    STATE_NAME_ENTRY(FW_INSERT_START),
    STATE_NAME_ENTRY(FW_INSERT_MSG),
    STATE_NAME_ENTRY(SHA3_MSG_DONE),
    STATE_NAME_ENTRY(SHA3_PROCESS),
    STATE_NAME_ENTRY(SHA3_VALID),
    STATE_NAME_ENTRY(SHA3_DONE),
    STATE_NAME_ENTRY(ALERT_STATE),
    STATE_NAME_ENTRY(ALERT_HANG),
    STATE_NAME_ENTRY(ERROR),
};
#undef STATE_NAME_ENTRY
#define STATE_NAME(_st_) \
    ((_st_) >= 0 && (_st_) < ARRAY_SIZE(STATE_NAMES) ? STATE_NAMES[(_st_)] : \
                                                       "?")
#define REG_MB4_IS_TRUE(_s_, _g_, _reg_, _fld_) \
    (FIELD_EX32((_s_)->regs##_##_g_[R_##_reg_], _reg_, _fld_) == \
     OT_MULTIBITBOOL4_TRUE)
#define REG_MB4_IS_FALSE(_s_, _g_, _reg_, _fld_) \
    (FIELD_EX32((_s_)->regs##_##_g_[R_##_reg_], _reg_, _fld_) == \
     OT_MULTIBITBOOL4_FALSE)

#define xtrace_ot_entropy_src_show_buffer(_s_, _msg_, _buf_, _len_) \
    ot_entropy_src_show_buffer(_s_, __func__, __LINE__, _msg_, _buf_, _len_)

static bool ot_entropy_src_is_module_enabled(const OtEntropySrcState *s);
static bool ot_entropy_src_is_fips_enabled(const OtEntropySrcState *s);
static bool ot_entropy_src_is_hw_route(const OtEntropySrcState *s);
static bool ot_entropy_src_is_fips_capable(const OtEntropySrcState *s);
static void ot_entropy_src_update_alerts(OtEntropySrcState *s);
static void ot_entropy_src_update_filler(OtEntropySrcState *s);

static int ot_entropy_src_get_entropy(
    OtEntropySrcState *ess, uint64_t random[OT_ENTROPY_SRC_DWORD_COUNT],
    bool *fips)
{
    if (!ot_entropy_src_is_module_enabled(ess)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: entropy_src is down\n", __func__);
        return -1;
    }

    bool fips_compliant;

    switch (ess->state) {
    case ENTROPY_SRC_BOOT_PHASE_DONE:
        fips_compliant = false;
        break;
    case ENTROPY_SRC_CONT_HT_RUNNING:
    case ENTROPY_SRC_CONT_HT_START:
    case ENTROPY_SRC_SHA3_MSG_DONE:
    case ENTROPY_SRC_SHA3_PROCESS:
    case ENTROPY_SRC_SHA3_VALID:
    case ENTROPY_SRC_SHA3_DONE:
        fips_compliant = true;
        break;
    case ENTROPY_SRC_BOOT_HT_RUNNING:
    case ENTROPY_SRC_BOOT_POST_HT_CHK:
    case ENTROPY_SRC_STARTUP_HT_START:
    case ENTROPY_SRC_STARTUP_PHASE1:
    case ENTROPY_SRC_STARTUP_PASS1:
    case ENTROPY_SRC_STARTUP_FAIL1: {
        int64_t wait_ns;
        if (timer_pending(ess->scheduler)) {
            /* computed delay fits into a 31-bit value */
            wait_ns = ((int64_t)timer_expire_time_ns(ess->scheduler)) -
                      qemu_clock_get_ns(OT_VIRTUAL_CLOCK);
            wait_ns = MAX(wait_ns, OT_ENTROPY_SRC_WAIT_DELAY_NS);
        } else {
            wait_ns = OT_ENTROPY_SRC_WAIT_DELAY_NS;
        }
        trace_ot_entropy_src_init_ongoing(ess->ot_id, STATE_NAME(ess->state),
                                          ess->state, (int)wait_ns);
        /* not ready */
        return (int)wait_ns;
    }
    case ENTROPY_SRC_IDLE:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: module is not enabled\n", __func__);
        return -1;
    case ENTROPY_SRC_FW_INSERT_START:
    case ENTROPY_SRC_FW_INSERT_MSG:
    case ENTROPY_SRC_ALERT_STATE:
    case ENTROPY_SRC_ALERT_HANG:
    case ENTROPY_SRC_ERROR:
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid state: [%s:%d]\n", __func__,
                      STATE_NAME(ess->state), ess->state);
        return -1;
    }

    if (!ot_entropy_src_is_hw_route(ess)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: HW route not selected\n", __func__);
        return -1;
    }

    if (ot_fifo32_num_used(&ess->final_fifo) < ES_WORD_COUNT) {
        trace_ot_entropy_src_no_entropy(ess->ot_id,
                                        ot_fifo32_num_used(&ess->final_fifo));
        return OT_ENTROPY_SRC_WAIT_DELAY_NS;
    }

    uint32_t *randu32 = (uint32_t *)random;
    size_t pos = 0;
    while (pos < ES_WORD_COUNT) {
        g_assert(!ot_fifo32_is_empty(&ess->final_fifo));
        randu32[pos++] = ot_fifo32_pop(&ess->final_fifo);
    }

    bool fips_capable = ot_entropy_src_is_fips_capable(ess);

    /* note: fips compliancy is only simulated here for now */
    *fips = fips_compliant && fips_capable;

    trace_ot_entropy_src_get_random_fips(
        ess->ot_id, STATE_NAME(ess->state), ot_entropy_src_is_fips_enabled(ess),
        REG_MB4_IS_TRUE(ess, hi, ENTROPY_CONTROL, ES_ROUTE),
        REG_MB4_IS_TRUE(ess, hi, ENTROPY_CONTROL, ES_TYPE),
        REG_MB4_IS_FALSE(ess, hi, CONF, RNG_BIT_ENABLE), fips_capable,
        fips_compliant, *fips);

    if (ot_fifo32_num_used(&ess->final_fifo) < ES_WORD_COUNT) {
        ot_entropy_src_update_filler(ess);
    }

    return 0;
}

static void ot_entropy_src_show_buffer(
    const OtEntropySrcState *s, const char *func, int line, const char *msg,
    const void *buf, unsigned size)
{
    if (trace_event_get_state(TRACE_OT_ENTROPY_SRC_SHOW_BUFFER) &&
        qemu_loglevel_mask(LOG_TRACE)) {
        static const char _hex[] = "0123456789ABCDEF";
        char hexstr[ES_HEXBUF_SIZE];
        unsigned len = MIN(size, ES_HEXBUF_SIZE / 2u - 4u);
        const uint8_t *pbuf = (const uint8_t *)buf;
        memset(hexstr, 0, sizeof(hexstr));
        unsigned hix = 0;
        for (unsigned ix = 0u; ix < len; ix++) {
            if (ix && !(ix & 0x3u)) {
                hexstr[hix++] = '-';
            }
            hexstr[hix++] = _hex[(pbuf[ix] >> 4u) & 0xfu];
            hexstr[hix++] = _hex[pbuf[ix] & 0xfu];
        }
        if (len < size) {
            hexstr[hix++] = '.';
            hexstr[hix++] = '.';
            hexstr[hix++] = '.';
        }

        trace_ot_entropy_src_show_buffer(s->ot_id, func, line, msg, hexstr);
    }
}

static inline uint32_t ot_entropy_src_hi_reg_base(const OtEntropySrcState *s)
{
    return (s->version < 3) ? REGS_HI_V2_BASE : REGS_HI_V3_BASE;
}

static inline uint32_t
ot_entropy_src_hi_reg_addr(const OtEntropySrcState *s, uint32_t addr)
{
    return ot_entropy_src_hi_reg_base(s) + addr;
}

static bool ot_entropy_src_is_module_enabled(const OtEntropySrcState *s)
{
    return REG_MB4_IS_TRUE(s, hi, MODULE_ENABLE, MODULE_ENABLE);
}

static bool ot_entropy_src_is_module_disabled(const OtEntropySrcState *s)
{
    return REG_MB4_IS_FALSE(s, hi, MODULE_ENABLE, MODULE_ENABLE);
}

static bool ot_entropy_src_is_fips_enabled(const OtEntropySrcState *s)
{
    return REG_MB4_IS_TRUE(s, hi, CONF, FIPS_ENABLE);
}

static void ot_entropy_src_update_irqs(OtEntropySrcState *s)
{
    uint32_t levels = s->regs_lo[R_INTR_STATE] & s->regs_lo[R_INTR_ENABLE];
    for (unsigned ix = 0; ix < NUM_IRQS; ix++) {
        ibex_irq_set(&s->irqs[ix], (int)((levels >> ix) & 0x1u));
    }
}

static bool
ot_entropy_src_is_final_fifo_slot_available(const OtEntropySrcState *s)
{
    return ot_fifo32_num_free(&s->final_fifo) >= ES_WORD_COUNT;
}

static bool ot_entropy_src_is_hw_route(const OtEntropySrcState *s)
{
    return REG_MB4_IS_FALSE(s, hi, ENTROPY_CONTROL, ES_ROUTE);
}

static bool ot_entropy_src_is_fw_route(const OtEntropySrcState *s)
{
    return REG_MB4_IS_TRUE(s, hi, ENTROPY_CONTROL, ES_ROUTE);
}

static bool ot_entropy_src_is_bypass_mode(const OtEntropySrcState *s)
{
    return !ot_entropy_src_is_fips_enabled(s) ||
           (ot_entropy_src_is_fw_route(s) &&
            REG_MB4_IS_TRUE(s, hi, ENTROPY_CONTROL, ES_TYPE));
}

static bool ot_entropy_src_is_fw_ov_mode(const OtEntropySrcState *s)
{
    return REG_MB4_IS_TRUE(s, hi, FW_OV_CONTROL, FW_OV_MODE);
}

static bool ot_entropy_src_is_fw_ov_entropy_insert(const OtEntropySrcState *s)
{
    return REG_MB4_IS_TRUE(s, hi, FW_OV_CONTROL, FW_OV_ENTROPY_INSERT);
}

static bool ot_entropy_src_is_fips_capable(const OtEntropySrcState *s)
{
    return ot_entropy_src_is_fips_enabled(s) &&
           !(REG_MB4_IS_TRUE(s, hi, ENTROPY_CONTROL, ES_ROUTE) &&
             REG_MB4_IS_TRUE(s, hi, ENTROPY_CONTROL, ES_TYPE)) &&
           REG_MB4_IS_FALSE(s, hi, CONF, RNG_BIT_ENABLE);
}

static unsigned ot_alert_get_alert_fail_count(const OtEntropySrcState *s)
{
    unsigned count;

    count = FIELD_EX32(s->regs_hi[R_ALERT_FAIL_COUNTS], ALERT_FAIL_COUNTS,
                       REPCNT_FAIL_COUNT);
    count += FIELD_EX32(s->regs_hi[R_ALERT_FAIL_COUNTS], ALERT_FAIL_COUNTS,
                        ADAPTP_HI_FAIL_COUNT);
    count += FIELD_EX32(s->regs_hi[R_ALERT_FAIL_COUNTS], ALERT_FAIL_COUNTS,
                        ADAPTP_LO_FAIL_COUNT);
    count += FIELD_EX32(s->regs_hi[R_ALERT_FAIL_COUNTS], ALERT_FAIL_COUNTS,
                        BUCKET_FAIL_COUNT);
    count += FIELD_EX32(s->regs_hi[R_ALERT_FAIL_COUNTS], ALERT_FAIL_COUNTS,
                        MARKOV_HI_FAIL_COUNT);
    count += FIELD_EX32(s->regs_hi[R_ALERT_FAIL_COUNTS], ALERT_FAIL_COUNTS,
                        MARKOV_LO_FAIL_COUNT);
    count += FIELD_EX32(s->regs_hi[R_ALERT_FAIL_COUNTS], ALERT_FAIL_COUNTS,
                        REPCNTS_FAIL_COUNT);

    return count;
}

static void ot_entropy_src_change_state_line(
    OtEntropySrcState *s, OtEntropySrcFsmState state, int line)
{
    OtEntropySrcFsmState old_state = s->state;

    switch (s->state) {
    case ENTROPY_SRC_ALERT_STATE:
        s->state = ENTROPY_SRC_ALERT_HANG;
        break;
    case ENTROPY_SRC_ALERT_HANG:
        if ((state == ENTROPY_SRC_IDLE) &&
            ot_entropy_src_is_module_disabled(s)) {
            s->state = state;
        }
        break;
    default:
        s->state = state;
        break;
    }

    if (old_state != s->state) {
        trace_ot_entropy_src_change_state(s->ot_id, line, STATE_NAME(old_state),
                                          old_state, STATE_NAME(s->state),
                                          s->state);
    }

    if (s->state == ENTROPY_SRC_ERROR) {
        s->regs_hi[R_ERR_CODE] |= R_ERR_CODE_ES_MAIN_SM_ERR_MASK;
        ot_entropy_src_update_alerts(s);
    }
}

#define ot_entropy_src_change_state(_s_, _st_) \
    ot_entropy_src_change_state_line(_s_, _st_, __LINE__)

static void ot_entropy_src_update_alerts(OtEntropySrcState *s)
{
    unsigned alert_threshold =
        FIELD_EX32(s->regs_hi[R_ALERT_THRESHOLD], ALERT_THRESHOLD, VAL);
    unsigned alert_count = ot_alert_get_alert_fail_count(s);
    bool recoverable = (bool)s->regs_hi[R_RECOV_ALERT_STS];
    uint32_t level = s->regs_lo[R_ALERT_TEST];
    if (alert_count >= alert_threshold || recoverable) {
        level |= 1u << ALERT_RECOVERABLE;
    }
    uint32_t fatal_alert = s->regs_hi[R_ERR_CODE] & ERR_CODE_FATAL_ERROR_MASK;
    fatal_alert |=
        (1u << s->regs_hi[R_ERR_CODE_TEST]) & ERR_CODE_FATAL_ERROR_MASK;
    if (fatal_alert) {
        level |= 1u << ALERT_FATAL;
    }

    for (unsigned ix = 0; ix < NUM_ALERTS; ix++) {
        ibex_irq_set(&s->alerts[ix], (int)((level >> ix) & 0x1u));
    }
}

static bool ot_entropy_src_check_multibitboot(
    OtEntropySrcState *s, uint8_t mbbool, uint32_t alert_bit)
{
    switch (mbbool) {
    case OT_MULTIBITBOOL4_TRUE:
    case OT_MULTIBITBOOL4_FALSE:
        return true;
    default:
        break;
    }

    s->regs_hi[R_RECOV_ALERT_STS] |= 1u << alert_bit;
    ot_entropy_src_update_alerts(s);
    return false;
}

static bool ot_entropy_src_can_consume_entropy(const OtEntropySrcState *s)
{
    return ot_entropy_src_is_module_enabled(s) &&
           !(ot_entropy_src_is_fw_ov_entropy_insert(s) &&
             !ot_entropy_src_is_fw_ov_mode(s));
}

static void ot_entropy_src_update_filler(OtEntropySrcState *s)
{
    /* fill granule is OT_ENTROPY_SRC_FILL_WORD_COUNT bits */
    bool input =
        ot_fifo32_num_free(&s->input_fifo) >= OT_ENTROPY_SRC_FILL_WORD_COUNT;
    bool output = ot_fifo32_num_free(&s->final_fifo) >= ES_WORD_COUNT;
    bool process = ot_entropy_src_can_consume_entropy(s);

    bool accept_entropy = input && output && process;
    trace_ot_entropy_src_update_filler(s->ot_id, input, output, process,
                                       accept_entropy);

    if (!accept_entropy) {
        /* if cannot accept entropy, stop the entropy scheduler */
        if (timer_pending(s->scheduler)) {
            trace_ot_entropy_src_info(s->ot_id, "stop scheduler");
            timer_del(s->scheduler);
        }
    } else {
        /*
         * if entropy can be handled, start the entropy scheduler if
         * it is not already active
         */
        if (!timer_pending(s->scheduler)) {
            trace_ot_entropy_src_info(s->ot_id, "reschedule");
            uint64_t now = qemu_clock_get_ns(OT_VIRTUAL_CLOCK);
            timer_mod(s->scheduler, (int64_t)(now + (uint64_t)ES_FILL_RATE_NS));
        }
    }
}

static bool ot_entropy_src_can_condition_entropy(const OtEntropySrcState *s)
{
    if (!ot_fifo32_is_full(&s->precon_fifo)) {
        /* room in preconditioner packer */
        return true;
    }
    if (ot_entropy_src_is_final_fifo_slot_available(s)) {
        /* room in output FIFO */
        return true;
    }

    return false;
}

static bool ot_entropy_src_can_bypass_entropy(const OtEntropySrcState *s)
{
    if (!ot_fifo32_is_full(&s->bypass_fifo)) {
        /* room in bypass packer */
        return true;
    }
    if (ot_entropy_src_is_final_fifo_slot_available(s)) {
        /* room in output FIFO */
        return true;
    }

    return false;
}

static bool
ot_entropy_src_push_entropy_to_conditioner(OtEntropySrcState *s, uint32_t word)
{
    int res;
    if (s->cond_word == 0) {
        res = sha3_384_init(&s->sha3_state);
        g_assert(res == CRYPT_OK);
    }

    g_assert(!ot_fifo32_is_full(&s->precon_fifo));

    ot_fifo32_push(&s->precon_fifo, word);

    if (!ot_fifo32_is_full(&s->precon_fifo)) {
        return false;
    }

    ot_entropy_src_change_state(s, ENTROPY_SRC_SHA3_PROCESS);

    uint32_t size;
    const uint32_t *buf;
    buf = ot_fifo32_peek_buf(&s->precon_fifo, s->precon_fifo.num, &size);
    g_assert(size == s->precon_fifo.num);
    xtrace_ot_entropy_src_show_buffer(s, "sha3 in", buf,
                                      size * sizeof(uint32_t));
    res = sha3_process(&s->sha3_state, (const uint8_t *)buf,
                       size * sizeof(uint32_t));
    g_assert(res == CRYPT_OK);
    s->cond_word += size;
    ot_fifo32_reset(&s->precon_fifo);

    return true;
}

static bool ot_entropy_src_can_hash(const OtEntropySrcState *s)
{
    return ot_fifo32_is_empty(&s->precon_fifo) &&
           (s->cond_word >= (2048 / (8u * sizeof(uint32_t))));
}

static void ot_entropy_src_perform_hash(OtEntropySrcState *s)
{
    uint32_t hash[OT_ENTROPY_SRC_WORD_COUNT];
    int res;
    res = sha3_done(&s->sha3_state, (uint8_t *)hash);
    g_assert(res == CRYPT_OK);
    s->cond_word = 0;

    xtrace_ot_entropy_src_show_buffer(s, "sha3 md", hash,
                                      OT_ENTROPY_SRC_WORD_COUNT *
                                          sizeof(uint32_t));

    ot_entropy_src_change_state(s, ENTROPY_SRC_SHA3_MSG_DONE);

    for (unsigned ix = 0; ix < OT_ENTROPY_SRC_WORD_COUNT; ix++) {
        g_assert(!ot_fifo32_is_full(&s->final_fifo));
        ot_fifo32_push(&s->final_fifo, hash[ix]);
    }
    s->packet_count += 1u;

    ot_entropy_src_change_state(s, REG_MB4_IS_TRUE(s, hi, FW_OV_SHA3_START,
                                                   FW_OV_INSERT_START) ?
                                       ENTROPY_SRC_IDLE :
                                       ENTROPY_SRC_CONT_HT_START);
}

static bool
ot_entropy_src_push_bypass_entropy(OtEntropySrcState *s, uint32_t word)
{
    g_assert(!ot_fifo32_is_full(&s->bypass_fifo));

    ot_fifo32_push(&s->bypass_fifo, word);
    if (!ot_fifo32_is_full(&s->bypass_fifo)) {
        /* need a whole OT_ENTROPY_SRC_PACKET_SIZE_BITS packet to move on */
        return false;
    }

    /* bypass conditioner full/ready, empty it into the final FIFO */
    while (!ot_fifo32_is_empty(&s->bypass_fifo)) {
        g_assert(!ot_fifo32_is_full(&s->final_fifo));
        ot_fifo32_push(&s->final_fifo, ot_fifo32_pop(&s->bypass_fifo));
    }
    s->packet_count += 1u;

    trace_ot_entropy_src_push_bypass_entropy(s->ot_id,
                                             ot_fifo32_num_used(
                                                 &s->final_fifo) /
                                                 OT_ENTROPY_SRC_WORD_COUNT);

    return true;
}

static void ot_entropy_src_update_fw_route(OtEntropySrcState *s)
{
    if (ot_fifo32_num_used(&s->final_fifo) >= ES_WORD_COUNT) {
        trace_ot_entropy_src_info(s->ot_id, "FW ROUTE");
        if (ot_fifo32_is_empty(&s->swread_fifo)) {
            /* refill swread FIFO */
            unsigned count = ES_WORD_COUNT;
            while (count--) {
                ot_fifo32_push(&s->swread_fifo, ot_fifo32_pop(&s->final_fifo));
            }
            s->regs_lo[R_INTR_STATE] |= INTR_ES_ENTROPY_VALID_MASK;
            trace_ot_entropy_src_available(s->ot_id, STATE_NAME(s->state),
                                           s->state);
            ot_entropy_src_update_filler(s);
        }
    }
}

static bool ot_entropy_src_consume_entropy(OtEntropySrcState *s, uint32_t word)
{
    bool fill_obs_fifo = ot_entropy_src_is_fw_ov_mode(s);
    bool hw_path = !ot_entropy_src_is_fw_ov_entropy_insert(s);
    bool bypass = ot_entropy_src_is_bypass_mode(s);

    if (hw_path) {
        /* check that HW accept data */
        hw_path = bypass ? ot_entropy_src_can_bypass_entropy(s) :
                           ot_entropy_src_can_condition_entropy(s);
    }

    if (!(fill_obs_fifo || hw_path)) {
        /* no way to consume noise, stop here */
        trace_ot_entropy_src_info(s->ot_id, "cannot consume noise for now");
        return false;
    }

    s->noise_count += 1u;
    trace_ot_entropy_src_consume_entropy(s->ot_id, fill_obs_fifo, bypass,
                                         hw_path, s->noise_count);

    if (fill_obs_fifo) {
        if (ot_fifo32_is_full(&s->observe_fifo)) {
            trace_ot_entropy_src_error(s->ot_id, "observe FIFO overflow",
                                       STATE_NAME(s->state), s->state);
            s->regs_hi[R_FW_OV_RD_FIFO_OVERFLOW] |=
                R_FW_OV_RD_FIFO_OVERFLOW_VAL_MASK;
        } else {
            if (s->obs_fifo_en) {
                unsigned threshold = s->regs_hi[R_OBSERVE_FIFO_THRESH];
                ot_fifo32_push(&s->observe_fifo, word);
                trace_ot_entropy_src_obs_fifo(s->ot_id,
                                              ot_fifo32_num_used(
                                                  &s->observe_fifo),
                                              threshold);
                if (ot_fifo32_is_full(&s->observe_fifo)) {
                    /* can only be enabled back once the FIFO is emptied */
                    trace_ot_entropy_src_info(s->ot_id, "observe FIFO is full");
                    s->obs_fifo_en = false;
                }
                /* is it > or >= ? */
                if (ot_fifo32_num_used(&s->observe_fifo) >= threshold) {
                    s->regs_lo[R_INTR_STATE] |= INTR_ES_OBSERVE_FIFO_READY_MASK;
                }
            } else {
                trace_ot_entropy_src_info(s->ot_id, "observe FIFO not enabled");
            }
        }
    }

    if (hw_path) {
        if (ot_entropy_src_is_bypass_mode(s)) {
            ot_entropy_src_push_bypass_entropy(s, word);
        } else {
            if (ot_entropy_src_push_entropy_to_conditioner(s, word)) {
                if (ot_entropy_src_can_hash(s)) {
                    trace_ot_entropy_src_info(s->ot_id, "can hash");
                    ot_entropy_src_perform_hash(s);
                }
            }
        }
    }

    if (ot_entropy_src_is_fw_route(s)) {
        ot_entropy_src_update_fw_route(s);
    }

    return true;
}

static uint32_t ot_entropy_src_get_entropy_data(OtEntropySrcState *s)
{
    bool is_entropy_data_enabled =
        (s->version < 3) ?
            REG_MB4_IS_TRUE(s, hi, CONF, V2_ENTROPY_DATA_REG_ENABLE) :
            REG_MB4_IS_TRUE(s, hi, CONF, V3_ENTROPY_DATA_REG_ENABLE);

    if (ot_entropy_src_is_module_enabled(s) && is_entropy_data_enabled &&
        ot_entropy_src_is_fw_route(s)) {
        if (!ot_fifo32_is_empty(&s->swread_fifo)) {
            return ot_fifo32_pop(&s->swread_fifo);
        }
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Entropy data not available\n",
                      __func__);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Entropy data not configured\n",
                      __func__);
    }

    return 0u;
}

static bool ot_entropy_src_fill_noise(OtEntropySrcState *s)
{
    unsigned count = ot_fifo32_num_free(&s->input_fifo);
    if (count < OT_ENTROPY_SRC_FILL_WORD_COUNT) {
        /* no room left, should be resheduled */
        return false;
    }

    uint32_t buffer[OT_ENTROPY_SRC_FILL_WORD_COUNT];
    /* synchronous read */
    ot_ast_eg_getrandom(buffer, sizeof(buffer));

    /* push the whole entropy buffer into the input FIFO */
    unsigned pos = 0;
    while (!ot_fifo32_is_full(&s->input_fifo) && pos < ARRAY_SIZE(buffer)) {
        ot_fifo32_push(&s->input_fifo, buffer[pos++]);
    }

    trace_ot_entropy_src_fill_noise(s->ot_id, count,
                                    ot_fifo32_num_used(&s->input_fifo));

    for (unsigned ix = 0;
         ix < ES_WORD_COUNT && !ot_fifo32_is_empty(&s->input_fifo); ix++) {
        if (!ot_entropy_src_consume_entropy(s, ot_fifo32_pop(&s->input_fifo))) {
            break;
        }
    }

    ot_entropy_src_update_irqs(s);

    return true;
}

static void ot_entropy_src_noise_refill(void *opaque)
{
    OtEntropySrcState *s = opaque;

    if (!ot_entropy_src_fill_noise(s)) {
        trace_ot_entropy_src_info(s->ot_id, "FIFO already filled up");
        return;
    }

    switch (s->state) {
    case ENTROPY_SRC_BOOT_HT_RUNNING:
        if (s->packet_count > 0) {
            ot_entropy_src_change_state(s, ENTROPY_SRC_BOOT_PHASE_DONE);
        }
        break;
    case ENTROPY_SRC_STARTUP_HT_START:
        ot_entropy_src_change_state(s, ENTROPY_SRC_CONT_HT_RUNNING);
        break;
    case ENTROPY_SRC_CONT_HT_RUNNING:
    case ENTROPY_SRC_CONT_HT_START:
    case ENTROPY_SRC_BOOT_PHASE_DONE:
    case ENTROPY_SRC_SHA3_VALID:
    case ENTROPY_SRC_SHA3_PROCESS:
    case ENTROPY_SRC_SHA3_DONE:
    case ENTROPY_SRC_SHA3_MSG_DONE:
        break;
    default:
        trace_ot_entropy_src_error(s->ot_id, "unexpected state",
                                   STATE_NAME(s->state), s->state);
        break;
    }

    ot_entropy_src_update_filler(s);
}

static void ot_entropy_src_scheduler(void *opaque)
{
    OtEntropySrcState *s = opaque;

    switch (s->state) {
    case ENTROPY_SRC_BOOT_HT_RUNNING:
    case ENTROPY_SRC_BOOT_PHASE_DONE:
    case ENTROPY_SRC_STARTUP_HT_START:
    case ENTROPY_SRC_CONT_HT_START:
    case ENTROPY_SRC_CONT_HT_RUNNING:
    case ENTROPY_SRC_SHA3_VALID:
    case ENTROPY_SRC_SHA3_PROCESS:
    case ENTROPY_SRC_SHA3_DONE:
    case ENTROPY_SRC_SHA3_MSG_DONE:
        ot_entropy_src_noise_refill(s);
        break;
    case ENTROPY_SRC_IDLE:
        break;
    case ENTROPY_SRC_BOOT_POST_HT_CHK:
    case ENTROPY_SRC_STARTUP_PHASE1:
    case ENTROPY_SRC_STARTUP_PASS1:
    case ENTROPY_SRC_STARTUP_FAIL1:
    case ENTROPY_SRC_FW_INSERT_START:
    case ENTROPY_SRC_FW_INSERT_MSG:
    case ENTROPY_SRC_ALERT_STATE:
    case ENTROPY_SRC_ALERT_HANG:
    case ENTROPY_SRC_ERROR:
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid state: [%s:%d]\n", __func__,
                      STATE_NAME(s->state), s->state);
    }

    ot_entropy_src_update_alerts(s);
    ot_entropy_src_update_irqs(s);
}

static uint64_t
ot_entropy_src_lo_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtEntropySrcState *s = opaque;
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_INTR_STATE:
    case R_INTR_ENABLE:
    case R_ME_REGWEN:
    case R_SW_REGUPD:
        val32 = s->regs_lo[reg];
        break;
    case R_REGWEN:
        val32 = (uint32_t)(s->regs_lo[R_SW_REGUPD] == R_SW_REGUPD_VAL_MASK &&
                           ot_entropy_src_is_module_disabled(s));
        break;
    case R_INTR_TEST:
    case R_ALERT_TEST:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: W/O register 0x%02" HWADDR_PRIx " (%s)\n",
                      __func__, s->ot_id, addr, REG_LO_NAME(reg));
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Bad offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, addr);
        val32 = 0;
        break;
    }
    uint32_t pc = ibex_get_current_pc();
    trace_ot_entropy_src_io_read_out(s->ot_id, (uint32_t)addr, REG_LO_NAME(reg),
                                     val32, pc);

    return (uint64_t)val32;
}

static uint64_t
ot_entropy_src_rev_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtEntropySrcState *s = opaque;
    (void)size;
    uint32_t val32;

    g_assert(s->version < 3 && s->regs_rev);

    hwaddr reg = R32_OFF(addr);

    /* NOLINTNEXTLINE(hicpp-multiway-paths-covered) */
    switch (reg) {
    case R_REV:
        val32 = s->regs_rev[reg];
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Bad offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, addr + REGS_REV_BASE);
        val32 = 0;
        break;
    }
    uint32_t pc = ibex_get_current_pc();
    trace_ot_entropy_src_io_read_out(s->ot_id, (uint32_t)addr + REGS_REV_BASE,
                                     REG_REV_NAME(reg), val32, pc);

    return (uint64_t)val32;
}

static uint64_t
ot_entropy_src_hi_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtEntropySrcState *s = opaque;
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_MODULE_ENABLE:
    case R_CONF:
    case R_ENTROPY_CONTROL:
    case R_HEALTH_TEST_WINDOWS:
    case R_REPCNT_THRESHOLDS:
    case R_REPCNTS_THRESHOLDS:
    case R_ADAPTP_HI_THRESHOLDS:
    case R_ADAPTP_LO_THRESHOLDS:
    case R_BUCKET_THRESHOLDS:
    case R_MARKOV_HI_THRESHOLDS:
    case R_MARKOV_LO_THRESHOLDS:
    case R_EXTHT_HI_THRESHOLDS:
    case R_EXTHT_LO_THRESHOLDS:
    case R_REPCNT_HI_WATERMARKS:
    case R_REPCNTS_HI_WATERMARKS:
    case R_ADAPTP_HI_WATERMARKS:
    case R_ADAPTP_LO_WATERMARKS:
    case R_EXTHT_HI_WATERMARKS:
    case R_EXTHT_LO_WATERMARKS:
    case R_BUCKET_HI_WATERMARKS:
    case R_MARKOV_HI_WATERMARKS:
    case R_MARKOV_LO_WATERMARKS:
    case R_REPCNT_TOTAL_FAILS:
    case R_REPCNTS_TOTAL_FAILS:
    case R_ADAPTP_HI_TOTAL_FAILS:
    case R_ADAPTP_LO_TOTAL_FAILS:
    case R_BUCKET_TOTAL_FAILS:
    case R_MARKOV_HI_TOTAL_FAILS:
    case R_MARKOV_LO_TOTAL_FAILS:
    case R_EXTHT_HI_TOTAL_FAILS:
    case R_EXTHT_LO_TOTAL_FAILS:
    case R_ALERT_THRESHOLD:
    case R_ALERT_FAIL_COUNTS:
    case R_EXTHT_FAIL_COUNTS:
    case R_FW_OV_CONTROL:
    case R_FW_OV_SHA3_START:
    case R_FW_OV_RD_FIFO_OVERFLOW:
    case R_OBSERVE_FIFO_THRESH:
    case R_RECOV_ALERT_STS:
    case R_ERR_CODE:
    case R_ERR_CODE_TEST:
        val32 = s->regs_hi[reg];
        break;
    case R_DEBUG_STATUS:
        /* SHA3 block reporting is not supported */
        val32 = FIELD_DP32(0, DEBUG_STATUS, ENTROPY_FIFO_DEPTH,
                           ot_fifo32_num_used(&s->final_fifo));
        val32 = FIELD_DP32(val32, DEBUG_STATUS, MAIN_SM_IDLE,
                           (uint32_t)(s->state == ENTROPY_SRC_IDLE));
        val32 = FIELD_DP32(val32, DEBUG_STATUS, MAIN_SM_BOOT_DONE,
                           (uint32_t)(s->state == ENTROPY_SRC_BOOT_PHASE_DONE));
        break;
    case R_MAIN_SM_STATE:
        if (s->state < ARRAY_SIZE(OtEDNFsmStateCode)) {
            val32 = OtEDNFsmStateCode[s->state];
        } else {
            val32 = OtEDNFsmStateCode[ENTROPY_SRC_ERROR];
        }
        break;
    case R_ALERT_SUMMARY_FAIL_COUNTS:
        val32 = (uint32_t)ot_alert_get_alert_fail_count(s);
        break;
    case R_ENTROPY_DATA:
        val32 = ot_entropy_src_get_entropy_data(s);
        break;
    case R_FW_OV_WR_FIFO_FULL: {
        bool can_write;
        if (ot_entropy_src_is_fw_ov_mode(s) &&
            ot_entropy_src_is_fw_ov_entropy_insert(s)) {
            if (ot_entropy_src_is_bypass_mode(s)) {
                can_write = ot_entropy_src_can_bypass_entropy(s);
            } else {
                can_write = ot_entropy_src_can_condition_entropy(s);
            }
        } else {
            can_write = false;
        }
        val32 = can_write ? 0u : R_FW_OV_WR_FIFO_FULL_VAL_MASK;
    } break;
    case R_FW_OV_RD_DATA:
        if (ot_entropy_src_is_fw_ov_mode(s)) {
            if (!ot_fifo32_is_empty(&s->observe_fifo)) {
                val32 = ot_fifo32_pop(&s->observe_fifo);
            } else {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "%s: Read from empty observe FIFO\n", __func__);
                val32 = 0;
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: FW override mode not active\n",
                          __func__);
            val32 = 0;
        }
        break;
    case R_OBSERVE_FIFO_DEPTH:
        val32 = ot_fifo32_num_used(&s->observe_fifo);
        break;
    case R_FW_OV_WR_DATA:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: W/O register 0x%02x (%s)\n",
                      __func__, s->ot_id, ot_entropy_src_hi_reg_addr(s, addr),
                      REG_HI_NAME(reg));
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Bad offset 0x%x\n", __func__,
                      s->ot_id, ot_entropy_src_hi_reg_addr(s, addr));
        val32 = 0;
        break;
    }
    uint32_t pc = ibex_get_current_pc();
    trace_ot_entropy_src_io_read_out(s->ot_id,
                                     ot_entropy_src_hi_reg_addr(s, addr),
                                     REG_HI_NAME(reg), val32, pc);

    return (uint64_t)val32;
}

#define CHECK_MULTIBOOT(_s_, _g_, _r_, _b_) \
    do { \
        if (!ot_entropy_src_check_multibitboot( \
                (_s_), FIELD_EX32(s->regs##_##_g_[R_##_r_], _r_, _b_), \
                ALERT_STATUS_BIT(_b_))) { \
            qemu_log_mask(LOG_GUEST_ERROR, \
                          "%s: %s: invalid multiboot value 0x%1x\n", __func__, \
                          (_s_)->ot_id, \
                          FIELD_EX32((_s_)->regs##_##_g_[R_##_r_], _r_, _b_)); \
        } \
    } while (0)

#define CHECK_MULTIBOOT_VER(_s_, _g_, _r_, _v_, _b_) \
    do { \
        if (!ot_entropy_src_check_multibitboot( \
                (_s_), FIELD_EX32(s->regs##_##_g_[R_##_r_], _r_, _v_##_##_b_), \
                ALERT_STATUS_BIT(_b_))) { \
            qemu_log_mask(LOG_GUEST_ERROR, \
                          "%s: %s: invalid multiboot value 0x%1x\n", __func__, \
                          (_s_)->ot_id, \
                          FIELD_EX32((_s_)->regs##_##_g_[R_##_r_], _r_, \
                                     _v_##_##_b_)); \
        } \
    } while (0)

static void ot_entropy_src_lo_regs_write(void *opaque, hwaddr addr,
                                         uint64_t val64, unsigned size)
{
    OtEntropySrcState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;
    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_entropy_src_io_write(s->ot_id, (uint32_t)addr, REG_LO_NAME(reg),
                                  val32, pc);

    switch (reg) {
    case R_INTR_STATE:
        val32 &= INTR_WMASK;
        s->regs_lo[reg] &= ~val32; /* RW1C */
        /* not sure about this behavior, seems to be what the tests expect... */
        if (!ot_fifo32_is_empty(&s->swread_fifo)) {
            s->regs_lo[R_INTR_STATE] |= INTR_ES_ENTROPY_VALID_MASK;
        }
        if (!ot_fifo32_is_empty(&s->observe_fifo)) {
            s->regs_lo[R_INTR_STATE] |= INTR_ES_OBSERVE_FIFO_READY_MASK;
        }
        ot_entropy_src_update_irqs(s);
        break;
    case R_INTR_ENABLE:
        val32 &= INTR_WMASK;
        s->regs_lo[reg] = val32;
        ot_entropy_src_update_irqs(s);
        break;
    case R_INTR_TEST:
        val32 &= INTR_WMASK;
        s->regs_lo[R_INTR_STATE] |= val32;
        ot_entropy_src_update_irqs(s);
        break;
    case R_ALERT_TEST:
        val32 &= ALERT_TEST_WMASK;
        s->regs_lo[reg] = val32;
        ot_entropy_src_update_alerts(s);
        break;
    case R_ME_REGWEN:
        val32 &= R_ME_REGWEN_EN_MASK;
        s->regs_lo[reg] &= val32; /* RW0C */
        break;
    case R_SW_REGUPD:
        val32 &= R_SW_REGUPD_VAL_MASK;
        s->regs_lo[reg] &= val32; /* RW0C */
        break;
    case R_REGWEN:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: R/O register 0x%02" HWADDR_PRIx " (%s)\n",
                      __func__, s->ot_id, addr, REG_LO_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Bad offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, addr);
        break;
    }
};

static void ot_entropy_src_rev_regs_write(void *opaque, hwaddr addr,
                                          uint64_t val64, unsigned size)
{
    OtEntropySrcState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;
    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_entropy_src_io_write(s->ot_id, (uint32_t)addr + REGS_REV_BASE,
                                  REG_REV_NAME(reg), val32, pc);

    /* NOLINTNEXTLINE(hicpp-multiway-paths-covered) */
    switch (reg) {
    case R_REV:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: R/O register 0x%02" HWADDR_PRIx " (%s)\n",
                      __func__, s->ot_id, addr + REGS_REV_BASE,
                      REG_REV_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Bad offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, addr + REGS_REV_BASE);
        break;
    }
};

static void ot_entropy_src_hi_regs_write(void *opaque, hwaddr addr,
                                         uint64_t val64, unsigned size)
{
    OtEntropySrcState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_entropy_src_io_write(s->ot_id, ot_entropy_src_hi_reg_addr(s, addr),
                                  REG_HI_NAME(reg), val32, pc);

    switch (reg) {
    case R_MODULE_ENABLE:
        if (s->regs_lo[R_ME_REGWEN]) {
            uint32_t old = s->regs_hi[reg];
            val32 &= R_MODULE_ENABLE_MODULE_ENABLE_MASK;
            s->regs_hi[reg] = val32;
            CHECK_MULTIBOOT(s, hi, MODULE_ENABLE, MODULE_ENABLE);
            if (ot_entropy_src_is_module_disabled(s)) {
                /* reset takes care of cancelling the scheduler timer */
                resettable_reset(OBJECT(s), RESET_TYPE_COLD);
                break;
            }
            if ((old ^ s->regs_hi[reg]) &&
                ot_entropy_src_is_module_enabled(s)) {
                if (ot_entropy_src_is_fips_enabled(s)) {
                    /* start up phase */
                    ot_entropy_src_change_state(s,
                                                ENTROPY_SRC_STARTUP_HT_START);
                } else {
                    /* boot phase */
                    ot_entropy_src_change_state(s, ENTROPY_SRC_BOOT_HT_RUNNING);
                }
                uint64_t now = qemu_clock_get_ns(OT_VIRTUAL_CLOCK);
                timer_mod(s->scheduler,
                          (int64_t)(now +
                                    (uint64_t)OT_ENTROPY_SRC_BOOT_DELAY_NS));
            }
            break;
        }
        qemu_log_mask(LOG_GUEST_ERROR, "%s: ME_REGWEN not enabled\n", __func__);
        break;
    case R_CONF:
        if (s->regs_lo[R_REGWEN]) {
            val32 &= (s->version < 3) ? CONF_V2_WMASK : CONF_V3_WMASK;
            s->regs_hi[reg] = val32;
            CHECK_MULTIBOOT(s, hi, CONF, FIPS_ENABLE);
            CHECK_MULTIBOOT(s, hi, CONF, RNG_BIT_ENABLE);
            if (s->version < 3) {
                CHECK_MULTIBOOT_VER(s, hi, CONF, V2, ENTROPY_DATA_REG_ENABLE);
                CHECK_MULTIBOOT_VER(s, hi, CONF, V2, THRESHOLD_SCOPE);
            } else {
                CHECK_MULTIBOOT_VER(s, hi, CONF, V3, ENTROPY_DATA_REG_ENABLE);
                CHECK_MULTIBOOT_VER(s, hi, CONF, V3, THRESHOLD_SCOPE);
            }
        }
        break;
    case R_ENTROPY_CONTROL:
        if (s->regs_lo[R_REGWEN]) {
            val32 &= ENTROPY_CONTROL_WMASK;
            s->regs_hi[reg] = val32;
            CHECK_MULTIBOOT(s, hi, ENTROPY_CONTROL, ES_ROUTE);
            CHECK_MULTIBOOT(s, hi, ENTROPY_CONTROL, ES_TYPE);
        }
        break;
    case R_HEALTH_TEST_WINDOWS:
        if (s->regs_lo[R_REGWEN]) {
            s->regs_hi[reg] = val32;
        }
        break;
    case R_REPCNT_THRESHOLDS:
    case R_REPCNTS_THRESHOLDS:
    case R_ADAPTP_HI_THRESHOLDS:
    case R_ADAPTP_LO_THRESHOLDS:
    case R_BUCKET_THRESHOLDS:
    case R_MARKOV_HI_THRESHOLDS:
    case R_MARKOV_LO_THRESHOLDS:
    case R_EXTHT_HI_THRESHOLDS:
    case R_EXTHT_LO_THRESHOLDS:
        if (s->regs_lo[R_REGWEN]) {
            s->regs_hi[reg] = val32;
            ot_entropy_src_update_alerts(s);
        }
        break;
    case R_ALERT_THRESHOLD:
        if (s->regs_lo[R_REGWEN]) {
            if ((uint16_t)(val32) != (uint16_t)(~(val32 >> 16u))) {
                s->regs_hi[R_RECOV_ALERT_STS] |=
                    R_RECOV_ALERT_STS_ES_THRESH_CFG_ALERT_MASK;
            } else {
                s->regs_hi[reg] = val32;
            }
            ot_entropy_src_update_alerts(s);
        }
        break;
    case R_FW_OV_CONTROL:
        if (s->regs_lo[R_REGWEN]) {
            val32 &= FW_OV_CONTROL_WMASK;
            s->regs_hi[reg] = val32;
            CHECK_MULTIBOOT(s, hi, FW_OV_CONTROL, FW_OV_MODE);
            CHECK_MULTIBOOT(s, hi, FW_OV_CONTROL, FW_OV_ENTROPY_INSERT);
            s->obs_fifo_en = ot_entropy_src_is_fw_ov_mode(s);
        }
        break;
    case R_FW_OV_SHA3_START:
        if (!ot_entropy_src_is_module_enabled(s)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: module not enabled\n",
                          __func__);
            break;
        }
        val32 &= R_FW_OV_SHA3_START_FW_OV_INSERT_START_MASK;
        s->regs_hi[reg] = val32;
        CHECK_MULTIBOOT(s, hi, FW_OV_SHA3_START, FW_OV_INSERT_START);
        if (REG_MB4_IS_TRUE(s, hi, FW_OV_SHA3_START, FW_OV_INSERT_START)) {
            OtEntropySrcFsmState new_state;
            new_state = ot_entropy_src_is_bypass_mode(s) ?
                            ENTROPY_SRC_IDLE :
                            ENTROPY_SRC_FW_INSERT_START;
            ot_entropy_src_change_state(s, new_state);
        } else { /* default to false */
            if (s->state == ENTROPY_SRC_SHA3_PROCESS) {
                /* handle SHA3 processing */
                if (ot_fifo32_is_empty(&s->precon_fifo)) {
                    ot_entropy_src_perform_hash(s);
                    if (ot_entropy_src_is_fw_route(s)) {
                        ot_entropy_src_update_fw_route(s);
                    }
                } else {
                    qemu_log_mask(LOG_GUEST_ERROR, "%s: need 1 more word\n",
                                  __func__);
                }
            } else {
                OtEntropySrcFsmState new_state;
                new_state = ot_entropy_src_is_bypass_mode(s) ?
                                ENTROPY_SRC_BOOT_HT_RUNNING :
                                ENTROPY_SRC_STARTUP_HT_START;
                ot_entropy_src_change_state(s, new_state);
            }
        }
        break;
    case R_FW_OV_RD_FIFO_OVERFLOW:
        val32 &= R_FW_OV_RD_FIFO_OVERFLOW_VAL_MASK;
        s->regs_hi[reg] &= val32; /* RW0C */
        break;
    case R_FW_OV_WR_DATA:
        if (!ot_entropy_src_is_module_enabled(s)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: module not enabled\n",
                          __func__);
            break;
        }
        if (ot_entropy_src_is_fw_ov_mode(s) &&
            ot_entropy_src_is_fw_ov_entropy_insert(s)) {
            bool can_write;
            if (ot_entropy_src_is_bypass_mode(s)) {
                can_write = ot_entropy_src_can_bypass_entropy(s);
                if (can_write) {
                    ot_entropy_src_push_bypass_entropy(s, val32);
                }
            } else {
                can_write = ot_entropy_src_can_condition_entropy(s);
                if (can_write) {
                    ot_entropy_src_push_entropy_to_conditioner(s, val32);
                }
            }
            if (!can_write) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: FW override: FIFO full\n",
                              __func__);
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: FW override mode not active\n",
                          __func__);
        }
        break;
    case R_OBSERVE_FIFO_THRESH:
        if (s->regs_lo[R_REGWEN]) {
            val32 &= R_OBSERVE_FIFO_THRESH_VAL_MASK;
            s->regs_hi[reg] = val32;
            ot_entropy_src_update_irqs(s);
        }
        break;
    case R_RECOV_ALERT_STS:
        val32 &= RECOV_ALERT_STS_WMASK;
        s->regs_hi[reg] &= val32; /* RW0C */
        break;
    case R_ERR_CODE_TEST:
        val32 &= R_ERR_CODE_TEST_VAL_MASK;
        s->regs_hi[R_ERR_CODE_TEST] = val32;
        ot_entropy_src_update_irqs(s);
        ot_entropy_src_update_alerts(s);
        break;
    case R_ENTROPY_DATA:
    case R_REPCNT_HI_WATERMARKS:
    case R_REPCNTS_HI_WATERMARKS:
    case R_ADAPTP_HI_WATERMARKS:
    case R_ADAPTP_LO_WATERMARKS:
    case R_EXTHT_HI_WATERMARKS:
    case R_EXTHT_LO_WATERMARKS:
    case R_BUCKET_HI_WATERMARKS:
    case R_MARKOV_HI_WATERMARKS:
    case R_MARKOV_LO_WATERMARKS:
    case R_REPCNT_TOTAL_FAILS:
    case R_REPCNTS_TOTAL_FAILS:
    case R_ADAPTP_HI_TOTAL_FAILS:
    case R_ADAPTP_LO_TOTAL_FAILS:
    case R_BUCKET_TOTAL_FAILS:
    case R_MARKOV_HI_TOTAL_FAILS:
    case R_MARKOV_LO_TOTAL_FAILS:
    case R_EXTHT_HI_TOTAL_FAILS:
    case R_EXTHT_LO_TOTAL_FAILS:
    case R_ALERT_SUMMARY_FAIL_COUNTS:
    case R_ALERT_FAIL_COUNTS:
    case R_EXTHT_FAIL_COUNTS:
    case R_FW_OV_WR_FIFO_FULL:
    case R_FW_OV_RD_DATA:
    case R_OBSERVE_FIFO_DEPTH:
    case R_DEBUG_STATUS:
    case R_ERR_CODE:
    case R_MAIN_SM_STATE:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: R/O register 0x%02x (%s)\n",
                      __func__, ot_entropy_src_hi_reg_addr(s, addr),
                      REG_HI_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%02x\n", __func__,
                      ot_entropy_src_hi_reg_addr(s, addr));
        break;
    }
};

static const MemoryRegionOps ot_entropy_src_lo_ops = {
    .read = &ot_entropy_src_lo_regs_read,
    .write = &ot_entropy_src_lo_regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4u,
        .max_access_size = 4u,
    },
};

static const MemoryRegionOps ot_entropy_src_rev_ops = {
    .read = &ot_entropy_src_rev_regs_read,
    .write = &ot_entropy_src_rev_regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4u,
        .max_access_size = 4u,
    },
};

static const MemoryRegionOps ot_entropy_src_hi_ops = {
    .read = &ot_entropy_src_hi_regs_read,
    .write = &ot_entropy_src_hi_regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4u,
        .max_access_size = 4u,
    },
};

static Property ot_entropy_src_properties[] = {
    DEFINE_PROP_STRING(OT_COMMON_DEV_ID, OtEntropySrcState, ot_id),
    DEFINE_PROP_UINT32("version", OtEntropySrcState, version, 0),

    DEFINE_PROP_LINK("ast", OtEntropySrcState, ast, TYPE_OT_AST_EG,
                     OtASTEgState *),
    DEFINE_PROP_LINK("otp_ctrl", OtEntropySrcState, otp_ctrl, TYPE_OT_OTP,
                     OtOTPState *),
    DEFINE_PROP_END_OF_LIST(),
};

static void ot_entropy_src_reset_enter(Object *obj, ResetType type)
{
    OtEntropySrcClass *c = OT_ENTROPY_SRC_GET_CLASS(obj);
    OtEntropySrcState *s = OT_ENTROPY_SRC(obj);

    trace_ot_entropy_src_reset(s->ot_id);

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    memset(s->regs_lo, 0, REGS_LO_SIZE);
    if (s->version < 3) {
        memset(s->regs_rev, 0, REGS_REV_SIZE);
    }
    memset(s->regs_hi, 0, REGS_HI_SIZE);

    s->regs_lo[R_ME_REGWEN] = 0x00000001u;
    s->regs_lo[R_SW_REGUPD] = 0x00000001u;
    s->regs_lo[R_REGWEN] = 0x00000001u;
    if (s->version < 3) {
        s->regs_rev[R_REV] = 0x10303u;
        s->regs_hi[R_CONF] = 0x2649999u;
        s->regs_hi[R_HEALTH_TEST_WINDOWS] = 0x600200u;
    } else {
        s->regs_hi[R_CONF] = 0x999999u;
        s->regs_hi[R_HEALTH_TEST_WINDOWS] = 0x1800200u;
    }

    s->regs_hi[R_MODULE_ENABLE] = 0x9u;
    s->regs_hi[R_ENTROPY_CONTROL] = 0x99u;
    s->regs_hi[R_REPCNT_THRESHOLDS] = 0xffffffffu;
    s->regs_hi[R_REPCNTS_THRESHOLDS] = 0xffffffffu;
    s->regs_hi[R_ADAPTP_HI_THRESHOLDS] = 0xffffffffu;
    s->regs_hi[R_BUCKET_THRESHOLDS] = 0xffffffffu;
    s->regs_hi[R_MARKOV_HI_THRESHOLDS] = 0xffffffffu;
    s->regs_hi[R_EXTHT_HI_THRESHOLDS] = 0xffffffffu;
    s->regs_hi[R_ADAPTP_LO_WATERMARKS] = 0xffffffffu;
    s->regs_hi[R_EXTHT_LO_WATERMARKS] = 0xffffffffu;
    s->regs_hi[R_MARKOV_LO_WATERMARKS] = 0xffffffffu;
    s->regs_hi[R_ALERT_THRESHOLD] = 0xfffd0002u;
    s->regs_hi[R_FW_OV_CONTROL] = 0x99u;
    s->regs_hi[R_FW_OV_SHA3_START] = 0x9u;
    s->regs_hi[R_OBSERVE_FIFO_THRESH] = 0x10u;
    s->regs_hi[R_DEBUG_STATUS] = 0x10000u;
    s->regs_hi[R_MAIN_SM_STATE] = 0xf5u;
    ot_fifo32_reset(&s->input_fifo);
    ot_fifo32_reset(&s->precon_fifo);
    ot_fifo32_reset(&s->bypass_fifo);
    ot_fifo32_reset(&s->observe_fifo);
    ot_fifo32_reset(&s->swread_fifo);
    ot_fifo32_reset(&s->final_fifo);

    s->cond_word = 0u;
    s->noise_count = 0u;
    s->packet_count = 0u;
    s->obs_fifo_en = false;

    ot_entropy_src_update_irqs(s);
    for (unsigned ix = 0; ix < NUM_ALERTS; ix++) {
        ibex_irq_set(&s->alerts[ix], 0);
    }

    OtOTPClass *oc = OBJECT_GET_CLASS(OtOTPClass, s->otp_ctrl, TYPE_OT_OTP);
    const OtOTPEntropyCfg *entropy_cfg = oc->get_entropy_cfg(s->otp_ctrl);
    g_assert(entropy_cfg);

    ot_entropy_src_change_state(s, ENTROPY_SRC_IDLE);
}

static void ot_entropy_src_realize(DeviceState *dev, Error **errp)
{
    (void)errp;

    OtEntropySrcState *s = OT_ENTROPY_SRC(dev);

    /* emulated version should be specified */
    g_assert(s->version > 0);
    g_assert(s->ast);
    g_assert(s->otp_ctrl);
}

static void ot_entropy_src_init(Object *obj)
{
    OtEntropySrcState *s = OT_ENTROPY_SRC(obj);

#define OT_ENTROPY_SRC_APERTURE 0x100u

    memory_region_init(&s->mmio, obj, TYPE_OT_ENTROPY_SRC "-regs",
                       OT_ENTROPY_SRC_APERTURE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    memory_region_init_io(&s->mmio_lo, obj, &ot_entropy_src_lo_ops, s,
                          TYPE_OT_ENTROPY_SRC "-regs-lo", REGS_LO_SIZE);
    memory_region_add_subregion(&s->mmio, REGS_LO_BASE, &s->mmio_lo);

    if (s->version < 3) {
        memory_region_init_io(&s->mmio_rev, obj, &ot_entropy_src_rev_ops, s,
                              TYPE_OT_ENTROPY_SRC "-regs-rev", REGS_REV_SIZE);
        memory_region_add_subregion(&s->mmio, REGS_REV_BASE, &s->mmio_rev);
    }

    memory_region_init_io(&s->mmio_hi, obj, &ot_entropy_src_hi_ops, s,
                          TYPE_OT_ENTROPY_SRC "-regs-hi", REGS_HI_SIZE);
    memory_region_add_subregion(&s->mmio, ot_entropy_src_hi_reg_base(s),
                                &s->mmio_hi);

    s->regs_lo = g_new0(uint32_t, REGS_LO_COUNT);
    if (s->version < 3) {
        s->regs_rev = g_new0(uint32_t, REGS_REV_COUNT);
    }
    s->regs_hi = g_new0(uint32_t, REGS_HI_COUNT);

    for (unsigned ix = 0; ix < NUM_IRQS; ix++) {
        ibex_sysbus_init_irq(obj, &s->irqs[ix]);
    }

    for (unsigned ix = 0; ix < NUM_ALERTS; ix++) {
        ibex_qdev_init_irq(obj, &s->alerts[ix], OT_DEVICE_ALERT);
    }

    ot_fifo32_create(&s->input_fifo, OT_ENTROPY_SRC_FILL_WORD_COUNT * 2u);
    ot_fifo32_create(&s->precon_fifo, sizeof(uint64_t) / sizeof(uint32_t));
    ot_fifo32_create(&s->bypass_fifo, ES_WORD_COUNT);
    ot_fifo32_create(&s->observe_fifo, OBSERVE_FIFO_DEPTH);
    ot_fifo32_create(&s->swread_fifo, ES_SWREAD_FIFO_WORD_COUNT);
    ot_fifo32_create(&s->final_fifo, ES_FINAL_FIFO_WORD_COUNT);

    s->scheduler = timer_new_ns(OT_VIRTUAL_CLOCK, &ot_entropy_src_scheduler, s);
}

static void ot_entropy_src_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->realize = &ot_entropy_src_realize;
    device_class_set_props(dc, ot_entropy_src_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    ResettableClass *rc = RESETTABLE_CLASS(klass);
    OtEntropySrcClass *ec = OT_ENTROPY_SRC_CLASS(klass);
    ec->get_entropy = &ot_entropy_src_get_entropy;
    resettable_class_set_parent_phases(rc, &ot_entropy_src_reset_enter, NULL,
                                       NULL, &ec->parent_phases);
}

static const TypeInfo ot_entropy_src_info = {
    .name = TYPE_OT_ENTROPY_SRC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtEntropySrcState),
    .instance_init = &ot_entropy_src_init,
    .class_size = sizeof(OtEntropySrcClass),
    .class_init = &ot_entropy_src_class_init,
};

static void ot_entropy_src_register_types(void)
{
    type_register_static(&ot_entropy_src_info);
}

type_init(ot_entropy_src_register_types);
