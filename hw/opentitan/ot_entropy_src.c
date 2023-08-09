/*
 * QEMU OpenTitan Entropy Source device
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
 * Note: for now, only a minimalist subset of Entropy source device is
 *       implemented in order to enable OpenTitan's ROM boot to progress.
 */

#include "qemu/osdep.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_ast.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_entropy_src.h"
#include "hw/opentitan/ot_fifo32.h"
#include "hw/opentitan/ot_otp.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "trace.h"

#define PARAM_NUM_IRQS           4u
#define PARAM_NUM_ALERTS         2u
#define PARAM_OBSERVE_FIFO_DEPTH 64u

/* clang-format off */
REG32(INTR_STATE, 0x0u)
    SHARED_FIELD(INTR_ES_ENTROPY_VALID, 0u, 1u)
    SHARED_FIELD(INTR_ES_HEALTH_TEST_FAILED, 1u, 1u)
    SHARED_FIELD(INTR_ES_OBSERVE_FIFO_READY, 2u, 1u)
    SHARED_FIELD(INTR_ES_FATAL_ERR, 3u, 1u)
REG32(INTR_ENABLE, 0x4u)
REG32(INTR_TEST, 0x8u)
REG32(ALERT_TEST, 0xcu)
    FIELD(ALERT_TEST, RECOV_ALERT, 0u, 1u)
    FIELD(ALERT_TEST, FATAL_ALERT, 1u, 1u)
REG32(ME_REGWEN, 0x10u)
    FIELD(ME_REGWEN, EN, 0u, 1u)
REG32(SW_REGUPD, 0x14u)
    FIELD(SW_REGUPD, UPD, 0u, 1u)
REG32(REGWEN, 0x18u)
    FIELD(REGWEN, EN, 0u, 1u)
REG32(REV, 0x1cu)
    FIELD(REV, ABI_REVISION, 0u, 8u)
    FIELD(REV, HW_REVISION, 8u, 8u)
    FIELD(REV, CHIP_TYPE, 16u, 8u)
REG32(MODULE_ENABLE, 0x20u)
    FIELD(MODULE_ENABLE, MODULE_ENABLE, 0u, 4u)
REG32(CONF, 0x24u)
    FIELD(CONF, FIPS_ENABLE, 0u, 4u)
    FIELD(CONF, ENTROPY_DATA_REG_ENABLE, 4u, 4u)
    FIELD(CONF, THRESHOLD_SCOPE, 12u, 4u)
    FIELD(CONF, RNG_BIT_ENABLE, 20u, 4u)
    FIELD(CONF, RNG_BIT_SEL, 24u, 2u)
REG32(ENTROPY_CONTROL, 0x28u)
    FIELD(ENTROPY_CONTROL, ES_ROUTE, 0u, 4u)
    FIELD(ENTROPY_CONTROL, ES_TYPE, 4u, 4u)
REG32(ENTROPY_DATA, 0x2cu)
REG32(HEALTH_TEST_WINDOWS, 0x30u)
    FIELD(HEALTH_TEST_WINDOWS, FIPS_WINDOW, 0u, 16u)
    FIELD(HEALTH_TEST_WINDOWS, BYPASS_WINDOW, 16u, 16u)
REG32(REPCNT_THRESHOLDS, 0x34u)
    SHARED_FIELD(THRESHOLDS_FIPS, 0u, 16u)
    SHARED_FIELD(THRESHOLDS_BYPASS, 16u, 16u)
REG32(REPCNTS_THRESHOLDS, 0x38u)
REG32(ADAPTP_HI_THRESHOLDS, 0x3cu)
REG32(ADAPTP_LO_THRESHOLDS, 0x40u)
REG32(BUCKET_THRESHOLDS, 0x44u)
REG32(MARKOV_HI_THRESHOLDS, 0x48u)
REG32(MARKOV_LO_THRESHOLDS, 0x4cu)
REG32(EXTHT_HI_THRESHOLDS, 0x50u)
REG32(EXTHT_LO_THRESHOLDS, 0x54u)
REG32(REPCNT_HI_WATERMARKS, 0x58u)
    SHARED_FIELD(WATERMARK_FIPS, 0u, 16u)
    SHARED_FIELD(WATERMARK_BYPASS, 16u, 16u)
REG32(REPCNTS_HI_WATERMARKS, 0x5cu)
REG32(ADAPTP_HI_WATERMARKS, 0x60u)
REG32(ADAPTP_LO_WATERMARKS, 0x64u)
REG32(EXTHT_HI_WATERMARKS, 0x68u)
REG32(EXTHT_LO_WATERMARKS, 0x6cu)
REG32(BUCKET_HI_WATERMARKS, 0x70u)
REG32(MARKOV_HI_WATERMARKS, 0x74u)
REG32(MARKOV_LO_WATERMARKS, 0x78u)
REG32(REPCNT_TOTAL_FAILS, 0x7cu)
REG32(REPCNTS_TOTAL_FAILS, 0x80u)
REG32(ADAPTP_HI_TOTAL_FAILS, 0x84u)
REG32(ADAPTP_LO_TOTAL_FAILS, 0x88u)
REG32(BUCKET_TOTAL_FAILS, 0x8cu)
REG32(MARKOV_HI_TOTAL_FAILS, 0x90u)
REG32(MARKOV_LO_TOTAL_FAILS, 0x94u)
REG32(EXTHT_HI_TOTAL_FAILS, 0x98u)
REG32(EXTHT_LO_TOTAL_FAILS, 0x9cu)
REG32(ALERT_THRESHOLD, 0xa0u)
    FIELD(ALERT_THRESHOLD, ALERT_THRESHOLD, 0u, 16u)
    FIELD(ALERT_THRESHOLD, ALERT_THRESHOLD_INV, 16u, 16u)
REG32(ALERT_SUMMARY_FAIL_COUNTS, 0xa4u)
    FIELD(ALERT_SUMMARY_FAIL_COUNTS, ANY_FAIL_COUNT, 0u, 16u)
REG32(ALERT_FAIL_COUNTS, 0xa8u)
    FIELD(ALERT_FAIL_COUNTS, REPCNT_FAIL_COUNT, 4u, 4u)
    FIELD(ALERT_FAIL_COUNTS, ADAPTP_HI_FAIL_COUNT, 8u, 4u)
    FIELD(ALERT_FAIL_COUNTS, ADAPTP_LO_FAIL_COUNT, 12u, 4u)
    FIELD(ALERT_FAIL_COUNTS, BUCKET_FAIL_COUNT, 16u, 4u)
    FIELD(ALERT_FAIL_COUNTS, MARKOV_HI_FAIL_COUNT, 20u, 4u)
    FIELD(ALERT_FAIL_COUNTS, MARKOV_LO_FAIL_COUNT, 24u, 4u)
    FIELD(ALERT_FAIL_COUNTS, REPCNTS_FAIL_COUNT, 28u, 4u)
REG32(EXTHT_FAIL_COUNTS, 0xacu)
    FIELD(EXTHT_FAIL_COUNTS, EXTHT_HI_FAIL_COUNT, 0u, 4u)
    FIELD(EXTHT_FAIL_COUNTS, EXTHT_LO_FAIL_COUNT, 4u, 4u)
REG32(FW_OV_CONTROL, 0xb0u)
    FIELD(FW_OV_CONTROL, FW_OV_MODE, 0u, 4u)
    FIELD(FW_OV_CONTROL, FW_OV_ENTROPY_INSERT, 4u, 4u)
REG32(FW_OV_SHA3_START, 0xb4u)
    FIELD(FW_OV_SHA3_START, FW_OV_INSERT_START, 0u, 4u)
REG32(FW_OV_WR_FIFO_FULL, 0xb8u)
    FIELD(FW_OV_WR_FIFO_FULL, VAL, 0u, 1u)
REG32(FW_OV_RD_FIFO_OVERFLOW, 0xbcu)
    FIELD(FW_OV_RD_FIFO_OVERFLOW, VAL, 0u, 1u)
REG32(FW_OV_RD_DATA, 0xc0u)
REG32(FW_OV_WR_DATA, 0xc4u)
REG32(OBSERVE_FIFO_THRESH, 0xc8u)
    FIELD(OBSERVE_FIFO_THRESH, VAL, 0u, 7u)
REG32(OBSERVE_FIFO_DEPTH, 0xccu)
    FIELD(OBSERVE_FIFO_DEPTH, VAL, 0u, 7u)
REG32(DEBUG_STATUS, 0xd0u)
    FIELD(DEBUG_STATUS, ENTROPY_FIFO_DEPTH, 0u, 7u)
    FIELD(DEBUG_STATUS, SHA3_FSM, 3u, 7u)
    FIELD(DEBUG_STATUS, SHA3_BLOCK_PR, 6u, 1u)
    FIELD(DEBUG_STATUS, SHA3_SQUEEZING, 7u, 1u)
    FIELD(DEBUG_STATUS, SHA3_ABSORBED, 8u, 1u)
    FIELD(DEBUG_STATUS, SHA3_ERR, 9u, 1u)
    FIELD(DEBUG_STATUS, MAIN_SM_IDLE, 16u, 1u)
    FIELD(DEBUG_STATUS, MAIN_SM_BOOT_DONE, 17u, 1u)
REG32(RECOV_ALERT_STS, 0xd4u)
    FIELD(RECOV_ALERT_STS, FIPS_ENABLE_FIELD_ALERT, 0u, 1u)
    FIELD(RECOV_ALERT_STS, ENTROPY_DATA_REG_ENABLE_FIELD_ALERT, 1u, 1u)
    FIELD(RECOV_ALERT_STS, MODULE_ENABLE_FIELD_ALERT, 2u, 1u)
    FIELD(RECOV_ALERT_STS, THRESHOLD_SCOPE_FIELD_ALERT, 3u, 1u)
    FIELD(RECOV_ALERT_STS, RNG_BIT_ENABLE_FIELD_ALERT, 5u, 1u)
    FIELD(RECOV_ALERT_STS, FW_OV_INSERT_START_FIELD_ALERT, 7u, 1u)
    FIELD(RECOV_ALERT_STS, FW_OV_MODE_FIELD_ALERT, 8u, 1u)
    FIELD(RECOV_ALERT_STS, FW_OV_ENTROPY_INSERT_FIELD_ALERT, 9u, 1u)
    FIELD(RECOV_ALERT_STS, ES_ROUTE_FIELD_ALERT, 10u, 1u)
    FIELD(RECOV_ALERT_STS, ES_TYPE_FIELD_ALERT, 11u, 1u)
    FIELD(RECOV_ALERT_STS, ES_MAIN_SM_ALERT, 12u, 1u)
    FIELD(RECOV_ALERT_STS, ES_BUS_CMP_ALERT, 13u, 1u)
    FIELD(RECOV_ALERT_STS, ES_THRESH_CFG_ALERT, 14u, 1u)
    FIELD(RECOV_ALERT_STS, ES_FW_OV_WR_ALERT, 15u, 1u)
    FIELD(RECOV_ALERT_STS, ES_FW_OV_DISABLE_ALERT, 16u, 1u)
REG32(ERR_CODE, 0xd8u)
    FIELD(ERR_CODE, SFIFO_ESRNG_ERR, 0u, 1u)
    FIELD(ERR_CODE, SFIFO_OBSERVE_ERR, 1u, 1u)
    FIELD(ERR_CODE, SFIFO_ESFINAL_ERR, 2u, 1u)
    FIELD(ERR_CODE, ES_ACK_SM_ERR, 20u, 1u)
    FIELD(ERR_CODE, ES_MAIN_SM_ERR, 21u, 1u)
    FIELD(ERR_CODE, ES_CNTR_ERR, 22u, 1u)
    FIELD(ERR_CODE, SHA3_STATE_ERR, 23u, 1u)
    FIELD(ERR_CODE, SHA3_RST_STORAGE_ERR, 24u, 1u)
    FIELD(ERR_CODE, FIFO_WRITE_ERR, 28u, 1u)
    FIELD(ERR_CODE, FIFO_READ_ERR, 29u, 1u)
    FIELD(ERR_CODE, FIFO_STATE_ERR, 30u, 1u)
REG32(ERR_CODE_TEST, 0xdcu)
    FIELD(ERR_CODE_TEST, VAL, 0u, 5u)
REG32(MAIN_SM_STATE, 0xe0u)
    FIELD(MAIN_SM_STATE, VAL, 0u, 9u)
/* clang-format on */

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_MAIN_SM_STATE)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define INTR_MASK \
    (INTR_ES_ENTROPY_VALID_MASK | INTR_ES_HEALTH_TEST_FAILED_MASK | \
     INTR_ES_OBSERVE_FIFO_READY_MASK | INTR_ES_FATAL_ERR_MASK)
#define ALERT_TEST_MASK \
    (R_ALERT_TEST_RECOV_ALERT_MASK | R_ALERT_TEST_FATAL_ALERT_MASK)
#define CONF_MASK \
    (R_CONF_FIPS_ENABLE_MASK | R_CONF_ENTROPY_DATA_REG_ENABLE_MASK | \
     R_CONF_THRESHOLD_SCOPE_MASK | R_CONF_RNG_BIT_ENABLE_MASK | \
     R_CONF_RNG_BIT_SEL_MASK)
#define ENTROPY_CONTROL_MASK \
    (R_ENTROPY_CONTROL_ES_ROUTE_MASK | R_ENTROPY_CONTROL_ES_TYPE_MASK)
#define FW_OV_CONTROL_MASK \
    (R_FW_OV_CONTROL_FW_OV_MODE_MASK | \
     R_FW_OV_CONTROL_FW_OV_ENTROPY_INSERT_MASK)
#define RECOV_ALERT_STS_MASK \
    (R_RECOV_ALERT_STS_FIPS_ENABLE_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_ENTROPY_DATA_REG_ENABLE_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_MODULE_ENABLE_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_THRESHOLD_SCOPE_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_RNG_BIT_ENABLE_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_FW_OV_INSERT_START_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_FW_OV_MODE_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_FW_OV_ENTROPY_INSERT_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_ES_ROUTE_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_ES_TYPE_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_ES_MAIN_SM_ALERT_MASK | \
     R_RECOV_ALERT_STS_ES_BUS_CMP_ALERT_MASK | \
     R_RECOV_ALERT_STS_ES_THRESH_CFG_ALERT_MASK | \
     R_RECOV_ALERT_STS_ES_FW_OV_WR_ALERT_MASK | \
     R_RECOV_ALERT_STS_ES_FW_OV_DISABLE_ALERT_MASK)
#define ERR_CODE_MASK \
    (R_ERR_CODE_SFIFO_ESRNG_ERR_MASK | R_ERR_CODE_SFIFO_OBSERVE_ERR_MASK | \
     R_ERR_CODE_SFIFO_ESFINAL_ERR_MASK | R_ERR_CODE_ES_ACK_SM_ERR_MASK | \
     R_ERR_CODE_ES_MAIN_SM_ERR_MASK | R_ERR_CODE_ES_CNTR_ERR_MASK | \
     R_ERR_CODE_SHA3_STATE_ERR_MASK | R_ERR_CODE_SHA3_RST_STORAGE_ERR_MASK | \
     R_ERR_CODE_FIFO_WRITE_ERR_MASK | R_ERR_CODE_FIFO_READ_ERR_MASK | \
     R_ERR_CODE_FIFO_STATE_ERR_MASK)
#define ERR_CODE_FATAL_ERROR_MASK \
    (R_ERR_CODE_ES_ACK_SM_ERR_MASK | R_ERR_CODE_ES_MAIN_SM_ERR_MASK | \
     R_ERR_CODE_ES_CNTR_ERR_MASK | R_ERR_CODE_SHA3_STATE_ERR_MASK | \
     R_ERR_CODE_SHA3_RST_STORAGE_ERR_MASK)

#define ALERT_STATUS_BIT(_x_) R_RECOV_ALERT_STS_##_x_##_FIELD_ALERT_MASK

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    REG_NAME_ENTRY(INTR_STATE),
    REG_NAME_ENTRY(INTR_ENABLE),
    REG_NAME_ENTRY(INTR_TEST),
    REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(ME_REGWEN),
    REG_NAME_ENTRY(SW_REGUPD),
    REG_NAME_ENTRY(REGWEN),
    REG_NAME_ENTRY(REV),
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
};
#undef REG_NAME_ENTRY

/* use a 384-bit buffer to slow down feed rate down to ~2 ms */
#define OT_ENTROPY_SRC_FILL_RATE_NS \
    ((NANOSECONDS_PER_SECOND * OT_ENTROPY_SRC_PACKET_SIZE_BITS) / \
     (OT_AST_RANDOM_4BIT_RATE * 4u))

enum {
    ALERT_RECOVERABLE,
    ALERT_FATAL,
};

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
    ENTROPY_SRC_SHA3_MSGDONE,
    ENTROPY_SRC_SHA3_PREP,
    ENTROPY_SRC_SHA3_PROCESS,
    ENTROPY_SRC_SHA3_VALID,
    ENTROPY_SRC_SHA3_DONE,
    ENTROPY_SRC_SHA3_QUIESCE,
    ENTROPY_SRC_ALERT_STATE,
    ENTROPY_SRC_ALERT_HANG,
    ENTROPY_SRC_ERROR,
} OtEntropySrcFsmState;

struct OtEntropySrcState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    IbexIRQ irqs[PARAM_NUM_IRQS];
    IbexIRQ alerts[PARAM_NUM_ALERTS];
    QEMUTimer *scheduler;

    uint32_t *regs;
    unsigned gennum;
    OtEntropySrcFsmState state;
    OtFifo32 hw_fifo;
    OtFifo32 observe_fifo;
    bool otp_fw_read;
    bool otp_fw_over;

    OtASTState *ast;
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
    [ENTROPY_SRC_SHA3_MSGDONE] = 0b100001111,
    [ENTROPY_SRC_SHA3_PREP] = 0b011111000,
    [ENTROPY_SRC_SHA3_PROCESS] = 0b010111111,
    [ENTROPY_SRC_SHA3_VALID] = 0b101110001,
    [ENTROPY_SRC_SHA3_DONE] = 0b110011000,
    [ENTROPY_SRC_SHA3_QUIESCE] = 0b111001101,
    [ENTROPY_SRC_ALERT_STATE] = 0b111111011,
    [ENTROPY_SRC_ALERT_HANG] = 0b101011100,
    [ENTROPY_SRC_ERROR] = 0b100111101,
};

#define STATE_NAME_ENTRY(_st_) [_st_] = stringify(_st_)
static const char *STATE_NAMES[] = {
    STATE_NAME_ENTRY(ENTROPY_SRC_IDLE),
    STATE_NAME_ENTRY(ENTROPY_SRC_BOOT_HT_RUNNING),
    STATE_NAME_ENTRY(ENTROPY_SRC_BOOT_POST_HT_CHK),
    STATE_NAME_ENTRY(ENTROPY_SRC_BOOT_PHASE_DONE),
    STATE_NAME_ENTRY(ENTROPY_SRC_STARTUP_HT_START),
    STATE_NAME_ENTRY(ENTROPY_SRC_STARTUP_PHASE1),
    STATE_NAME_ENTRY(ENTROPY_SRC_STARTUP_PASS1),
    STATE_NAME_ENTRY(ENTROPY_SRC_STARTUP_FAIL1),
    STATE_NAME_ENTRY(ENTROPY_SRC_CONT_HT_START),
    STATE_NAME_ENTRY(ENTROPY_SRC_CONT_HT_RUNNING),
    STATE_NAME_ENTRY(ENTROPY_SRC_FW_INSERT_START),
    STATE_NAME_ENTRY(ENTROPY_SRC_FW_INSERT_MSG),
    STATE_NAME_ENTRY(ENTROPY_SRC_SHA3_MSGDONE),
    STATE_NAME_ENTRY(ENTROPY_SRC_SHA3_PREP),
    STATE_NAME_ENTRY(ENTROPY_SRC_SHA3_PROCESS),
    STATE_NAME_ENTRY(ENTROPY_SRC_SHA3_VALID),
    STATE_NAME_ENTRY(ENTROPY_SRC_SHA3_DONE),
    STATE_NAME_ENTRY(ENTROPY_SRC_SHA3_QUIESCE),
    STATE_NAME_ENTRY(ENTROPY_SRC_ALERT_STATE),
    STATE_NAME_ENTRY(ENTROPY_SRC_ALERT_HANG),
    STATE_NAME_ENTRY(ENTROPY_SRC_ERROR),
};
#undef STATE_NAME_ENTRY
#define STATE_NAME(_st_) \
    ((_st_) >= 0 && (_st_) < ARRAY_SIZE(STATE_NAMES) ? STATE_NAMES[(_st_)] : \
                                                       "?")
#define REG_MB4_IS_TRUE(_s_, _reg_, _fld_) \
    (FIELD_EX32((_s_)->regs[R_##_reg_], _reg_, _fld_) == OT_MULTIBITBOOL4_TRUE)
#define REG_MB4_IS_FALSE(_s_, _reg_, _fld_) \
    (FIELD_EX32((_s_)->regs[R_##_reg_], _reg_, _fld_) == OT_MULTIBITBOOL4_FALSE)

static bool ot_entropy_src_is_module_enabled(OtEntropySrcState *s);
static bool ot_entropy_src_is_hw_route(OtEntropySrcState *s);
static bool ot_entropy_src_is_fips_capable(OtEntropySrcState *s);
static void ot_entropy_src_reset(DeviceState *dev);
static void ot_entropy_src_update_alerts(OtEntropySrcState *s);
static void ot_entropy_src_update_filler(OtEntropySrcState *s);

/* -------------------------------------------------------------------------- */
/* Public API */
/* -------------------------------------------------------------------------- */

int ot_entropy_src_get_generation(OtEntropySrcState *s)
{
    return ot_entropy_src_is_module_enabled(s) ? (int)s->gennum : 0;
}

int ot_entropy_src_get_random(OtEntropySrcState *s, int genid,
                              uint64_t random[OT_ENTROPY_SRC_DWORD_COUNT],
                              bool *fips)
{
    if (!ot_entropy_src_is_module_enabled(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: entropy_src is down\n", __func__);
        return -2;
    }

    if (genid != (int)s->gennum) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: entropy_src gennum mismatch req:%d cur:%u\n",
                      __func__, genid, s->gennum);
        return -2;
    }

    bool fips_compliant;

    switch (s->state) {
    case ENTROPY_SRC_BOOT_PHASE_DONE:
        fips_compliant = false;
        break;
    case ENTROPY_SRC_CONT_HT_RUNNING:
        fips_compliant = true;
        break;
    case ENTROPY_SRC_BOOT_HT_RUNNING:
    case ENTROPY_SRC_BOOT_POST_HT_CHK:
    case ENTROPY_SRC_STARTUP_HT_START:
    case ENTROPY_SRC_STARTUP_PHASE1:
    case ENTROPY_SRC_STARTUP_PASS1:
    case ENTROPY_SRC_STARTUP_FAIL1:
    case ENTROPY_SRC_CONT_HT_START:
    case ENTROPY_SRC_SHA3_MSGDONE:
    case ENTROPY_SRC_SHA3_PREP:
    case ENTROPY_SRC_SHA3_PROCESS:
    case ENTROPY_SRC_SHA3_VALID:
    case ENTROPY_SRC_SHA3_DONE:
    case ENTROPY_SRC_SHA3_QUIESCE:
        trace_ot_entropy_src_init_ongoing(STATE_NAME(s->state), s->state);
        return 1; /* not ready */
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
                      STATE_NAME(s->state), s->state);
        return -1;
    }

    if (!ot_entropy_src_is_hw_route(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: HW route not selected\n", __func__);
        return -1;
    }

    if (ot_fifo32_num_used(&s->hw_fifo) < OT_ENTROPY_SRC_WORD_COUNT) {
        trace_ot_entropy_src_no_entropy(ot_fifo32_num_used(&s->hw_fifo));
        return 1;
    }

    uint32_t *randu32 = (uint32_t *)random;
    size_t pos = 0;
    while (pos < OT_ENTROPY_SRC_WORD_COUNT) {
        assert(!ot_fifo32_is_empty(&s->hw_fifo));
        randu32[pos++] = ot_fifo32_pop(&s->hw_fifo);
    }

    /* note: fips compliancy is only simulated here for now */
    *fips = fips_compliant && ot_entropy_src_is_fips_capable(s);

    if (ot_fifo32_num_used(&s->hw_fifo) < OT_ENTROPY_SRC_WORD_COUNT) {
        ot_entropy_src_update_filler(s);
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
/* Private implementation */
/* -------------------------------------------------------------------------- */

static bool ot_entropy_src_is_module_enabled(OtEntropySrcState *s)
{
    return REG_MB4_IS_TRUE(s, MODULE_ENABLE, MODULE_ENABLE);
}

static bool ot_entropy_src_is_module_disabled(OtEntropySrcState *s)
{
    return REG_MB4_IS_FALSE(s, MODULE_ENABLE, MODULE_ENABLE);
}

static bool ot_entropy_src_is_fips_enabled(OtEntropySrcState *s)
{
    return REG_MB4_IS_TRUE(s, CONF, FIPS_ENABLE);
}

static void ot_entropy_src_update_irqs(OtEntropySrcState *s)
{
    uint32_t level = s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE];
    for (unsigned ix = 0; ix < PARAM_NUM_IRQS; ix++) {
        ibex_irq_set(&s->irqs[ix], (int)((level >> ix) & 0x1u));
    }
}

static bool ot_entropy_src_is_hw_route(OtEntropySrcState *s)
{
    return REG_MB4_IS_FALSE(s, ENTROPY_CONTROL, ES_ROUTE);
}

static bool ot_entropy_src_is_fw_route(OtEntropySrcState *s)
{
    return REG_MB4_IS_TRUE(s, ENTROPY_CONTROL, ES_ROUTE);
}

static bool ot_entropy_src_is_fw_ov_mode(OtEntropySrcState *s)
{
    return REG_MB4_IS_TRUE(s, FW_OV_CONTROL, FW_OV_MODE) && s->otp_fw_over;
}

static bool ot_entropy_src_is_fips_capable(OtEntropySrcState *s)
{
    bool fips_capable =
        ot_entropy_src_is_fips_enabled(s) &&
        !(REG_MB4_IS_TRUE(s, ENTROPY_CONTROL, ES_ROUTE) &&
          REG_MB4_IS_TRUE(s, ENTROPY_CONTROL, ES_TYPE)) &&
        REG_MB4_IS_FALSE(s, CONF, RNG_BIT_ENABLE);
    trace_ot_entropy_src_is_fips_capable(
        ot_entropy_src_is_fips_enabled(s),
        REG_MB4_IS_TRUE(s, ENTROPY_CONTROL, ES_ROUTE),
        REG_MB4_IS_TRUE(s, ENTROPY_CONTROL, ES_TYPE),
        REG_MB4_IS_FALSE(s, CONF, RNG_BIT_ENABLE), fips_capable);
    return fips_capable;
}

static unsigned ot_alert_get_alert_fail_count(OtEntropySrcState *s)
{
    unsigned count;

    count = FIELD_EX32(s->regs[R_ALERT_FAIL_COUNTS], ALERT_FAIL_COUNTS,
                       REPCNT_FAIL_COUNT);
    count += FIELD_EX32(s->regs[R_ALERT_FAIL_COUNTS], ALERT_FAIL_COUNTS,
                        ADAPTP_HI_FAIL_COUNT);
    count += FIELD_EX32(s->regs[R_ALERT_FAIL_COUNTS], ALERT_FAIL_COUNTS,
                        ADAPTP_LO_FAIL_COUNT);
    count += FIELD_EX32(s->regs[R_ALERT_FAIL_COUNTS], ALERT_FAIL_COUNTS,
                        BUCKET_FAIL_COUNT);
    count += FIELD_EX32(s->regs[R_ALERT_FAIL_COUNTS], ALERT_FAIL_COUNTS,
                        MARKOV_HI_FAIL_COUNT);
    count += FIELD_EX32(s->regs[R_ALERT_FAIL_COUNTS], ALERT_FAIL_COUNTS,
                        MARKOV_LO_FAIL_COUNT);
    count += FIELD_EX32(s->regs[R_ALERT_FAIL_COUNTS], ALERT_FAIL_COUNTS,
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

    trace_ot_entropy_src_change_state(line, STATE_NAME(old_state), old_state,
                                      STATE_NAME(s->state), s->state);

    if (s->state == ENTROPY_SRC_ERROR) {
        s->regs[R_ERR_CODE] |= R_ERR_CODE_ES_MAIN_SM_ERR_MASK;
        ot_entropy_src_update_alerts(s);
    }
}

#define ot_entropy_src_change_state(_s_, _st_) \
    ot_entropy_src_change_state_line(_s_, _st_, __LINE__)

static void ot_entropy_src_update_alerts(OtEntropySrcState *s)
{
    unsigned alert_threshold = FIELD_EX32(s->regs[R_ALERT_THRESHOLD],
                                          ALERT_THRESHOLD, ALERT_THRESHOLD);
    unsigned alert_count = ot_alert_get_alert_fail_count(s);
    bool recoverable = (bool)s->regs[R_RECOV_ALERT_STS];
    if (alert_count >= alert_threshold || recoverable) {
        ibex_irq_set(&s->alerts[ALERT_RECOVERABLE], 1);
        if (s->state != ENTROPY_SRC_ERROR) {
            ot_entropy_src_change_state(s, ENTROPY_SRC_ALERT_STATE);
        }
    }
    uint32_t fatal_alert = s->regs[R_ERR_CODE] & ERR_CODE_FATAL_ERROR_MASK;
    if (fatal_alert) {
        ibex_irq_set(&s->alerts[ALERT_FATAL], 1);
        if (s->state != ENTROPY_SRC_ERROR) {
            ot_entropy_src_change_state(s, ENTROPY_SRC_ERROR);
        }
    }
}

static void ot_entropy_src_check_multibitboot(
    OtEntropySrcState *s, uint8_t mbbool, uint32_t alert_bit)
{
    switch (mbbool) {
    case OT_MULTIBITBOOL4_TRUE:
    case OT_MULTIBITBOOL4_FALSE:
        return;
    default:
        break;
    }

    s->regs[R_RECOV_ALERT_STS] |= 1u << alert_bit;
    ot_entropy_src_update_alerts(s);
}

static void ot_entropy_src_update_filler(OtEntropySrcState *s)
{
    if (ot_fifo32_num_free(&s->hw_fifo) < OT_ENTROPY_SRC_WORD_COUNT) {
        /* fill granule is 384 bits */
        /* if the entropy HW FIFO is full, stop the entropy scheduler */
        if (timer_pending(s->scheduler)) {
            trace_ot_entropy_src_info("stop scheduler");
            timer_del(s->scheduler);
        }
    } else {
        /*
         * if the entropy HW FIFO is not full, start the entropy scheduler if
         * it is not already active
         */
        if (!timer_pending(s->scheduler)) {
            trace_ot_entropy_src_info("reschedule");
            uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            timer_mod(s->scheduler,
                      now + (uint64_t)OT_ENTROPY_SRC_FILL_RATE_NS);
        }
    }
}

static bool ot_entropy_src_hwfifo_fill_entropy(OtEntropySrcState *s)
{
    unsigned count = ot_fifo32_num_free(&s->hw_fifo);
    if (!count) {
        return false;
    }

    trace_ot_entropy_src_hwfifo_fill_entropy(count);

    uint32_t buffer[OT_ENTROPY_SRC_WORD_COUNT];
    ot_ast_getrandom(buffer, sizeof(buffer));

    unsigned pos = 0;
    while (!ot_fifo32_is_full(&s->hw_fifo) && pos < OT_ENTROPY_SRC_WORD_COUNT) {
        ot_fifo32_push(&s->hw_fifo, buffer[pos++]);
    }

    if (ot_fifo32_num_free(&s->hw_fifo) >= OT_ENTROPY_SRC_WORD_COUNT) {
        s->regs[R_INTR_STATE] |= INTR_ES_ENTROPY_VALID_MASK;
        trace_ot_entropy_src_available(STATE_NAME(s->state), s->state);
    }

    if (ot_entropy_src_is_fw_ov_mode(s)) {
        unsigned fw_pos = 0;
        while (fw_pos < pos)
            if (!ot_fifo32_is_full(&s->observe_fifo)) {
                ot_fifo32_push(&s->observe_fifo, buffer[fw_pos++]);
            }
        if (fw_pos < pos) {
            s->regs[R_FW_OV_RD_FIFO_OVERFLOW] |=
                R_FW_OV_RD_FIFO_OVERFLOW_VAL_MASK;
        }
        unsigned threshold = s->regs[R_OBSERVE_FIFO_THRESH];
        /* is it > or >= ? */
        if (ot_fifo32_num_used(&s->observe_fifo) >= threshold) {
            s->regs[R_INTR_STATE] |= INTR_ES_OBSERVE_FIFO_READY_MASK;
        }
    }

    ot_entropy_src_update_irqs(s);

    return true;
}

static void ot_entropy_src_hwfifo_refill(void *opaque)
{
    OtEntropySrcState *s = opaque;

    if (!ot_entropy_src_hwfifo_fill_entropy(s)) {
        trace_ot_entropy_src_info("FIFO already filled up");
        return;
    }

    switch (s->state) {
    case ENTROPY_SRC_BOOT_HT_RUNNING:
        ot_entropy_src_change_state(s, ENTROPY_SRC_BOOT_PHASE_DONE);
        break;
    case ENTROPY_SRC_STARTUP_HT_START:
        ot_entropy_src_change_state(s, ENTROPY_SRC_CONT_HT_RUNNING);
        break;
    case ENTROPY_SRC_CONT_HT_RUNNING:
    case ENTROPY_SRC_BOOT_PHASE_DONE:
        break;
    default:
        trace_ot_entropy_src_error("unexpected state", STATE_NAME(s->state),
                                   s->state);
        break;
    }

    if (ot_fifo32_num_used(&s->hw_fifo) < OT_ENTROPY_SRC_WORD_COUNT) {
        ot_entropy_src_update_filler(s);
    }
}

static void ot_entropy_src_scheduler(void *opaque)
{
    OtEntropySrcState *s = opaque;

    switch (s->state) {
    case ENTROPY_SRC_BOOT_HT_RUNNING:
        /* skip HT tests */
    case ENTROPY_SRC_BOOT_PHASE_DONE:
        ot_entropy_src_hwfifo_refill(s);
        break;
    case ENTROPY_SRC_STARTUP_HT_START:
        /* skip Sha3 and HT tests */
    case ENTROPY_SRC_CONT_HT_RUNNING:
        ot_entropy_src_hwfifo_refill(s);
        break;
    case ENTROPY_SRC_IDLE:
        break;
    case ENTROPY_SRC_BOOT_POST_HT_CHK:
    case ENTROPY_SRC_STARTUP_PHASE1:
    case ENTROPY_SRC_STARTUP_PASS1:
    case ENTROPY_SRC_STARTUP_FAIL1:
    case ENTROPY_SRC_CONT_HT_START:
    case ENTROPY_SRC_FW_INSERT_START:
    case ENTROPY_SRC_FW_INSERT_MSG:
    case ENTROPY_SRC_SHA3_MSGDONE:
    case ENTROPY_SRC_SHA3_PREP:
    case ENTROPY_SRC_SHA3_PROCESS:
    case ENTROPY_SRC_SHA3_VALID:
    case ENTROPY_SRC_SHA3_DONE:
    case ENTROPY_SRC_SHA3_QUIESCE:
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
ot_entropy_src_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtEntropySrcState *s = opaque;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_INTR_STATE:
    case R_INTR_ENABLE:
    case R_ME_REGWEN:
    case R_SW_REGUPD:
    case R_REV:
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
    case R_OBSERVE_FIFO_THRESH:
    case R_DEBUG_STATUS:
    case R_RECOV_ALERT_STS:
    case R_ERR_CODE:
    case R_ERR_CODE_TEST:
        val32 = s->regs[reg];
        break;
    case R_MAIN_SM_STATE:
        if (s->state < ARRAY_SIZE(OtEDNFsmStateCode)) {
            val32 = OtEDNFsmStateCode[s->state];
        } else {
            val32 = OtEDNFsmStateCode[ENTROPY_SRC_ERROR];
        }
        break;
    case R_REGWEN:
        val32 = (uint32_t)(s->regs[R_SW_REGUPD] == R_SW_REGUPD_UPD_MASK &&
                           ot_entropy_src_is_module_disabled(s));
        break;
    case R_ALERT_SUMMARY_FAIL_COUNTS:
        val32 = (uint32_t)ot_alert_get_alert_fail_count(s);
        break;
    case R_ENTROPY_DATA:
        if (ot_entropy_src_is_module_enabled(s) &&
            REG_MB4_IS_TRUE(s, CONF, ENTROPY_DATA_REG_ENABLE) &&
            ot_entropy_src_is_fw_route(s) && s->otp_fw_read) {
            if (!ot_fifo32_is_empty(&s->hw_fifo)) {
                val32 = ot_fifo32_pop(&s->hw_fifo);
                ot_entropy_src_update_filler(s);
            } else {
                qemu_log_mask(LOG_GUEST_ERROR, "Entropy data not available");
                val32 = 0;
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "Entropy data not configured");
            val32 = 0;
        }
        break;
    case R_FW_OV_WR_FIFO_FULL:
        val32 =
            ot_fifo32_is_full(&s->hw_fifo) ? R_FW_OV_WR_FIFO_FULL_VAL_MASK : 0u;
        break;
    case R_FW_OV_RD_DATA:
        if (ot_entropy_src_is_fw_ov_mode(s)) {
            if (!ot_fifo32_is_empty(&s->observe_fifo)) {
                val32 = ot_fifo32_pop(&s->observe_fifo);
            } else {
                qemu_log_mask(LOG_GUEST_ERROR, "Read from empty observe FIFO");
                val32 = 0;
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "FW override mode not active");
            val32 = 0;
        }
        break;
    case R_OBSERVE_FIFO_DEPTH:
        val32 = ot_fifo32_num_used(&s->observe_fifo);
        break;
    case R_INTR_TEST:
    case R_ALERT_TEST:
    case R_FW_OV_WR_DATA:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "W/O register 0x02%" HWADDR_PRIx " (%s)\n", addr,
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
    trace_ot_entropy_src_io_read_out((unsigned)addr, REG_NAME(reg),
                                     (uint64_t)val32, pc);

    return (uint64_t)val32;
};

#define CHECK_MULTIBOOT(_s_, _r_, _b_) \
    ot_entropy_src_check_multibitboot((_s_), \
                                      FIELD_EX32(s->regs[R_##_r_], _r_, _b_), \
                                      ALERT_STATUS_BIT(_b_));

static void ot_entropy_src_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                                      unsigned size)
{
    OtEntropySrcState *s = opaque;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint64_t pc = ibex_get_current_pc();
    trace_ot_entropy_src_io_write((unsigned)addr, REG_NAME(reg), val64, pc);

    switch (reg) {
    case R_INTR_STATE:
        val32 &= INTR_MASK;
        s->regs[reg] &= ~val32; /* RW1C */
        ot_entropy_src_update_irqs(s);
        break;
    case R_INTR_ENABLE:
        val32 &= INTR_MASK;
        s->regs[reg] = val32;
        ot_entropy_src_update_irqs(s);
        break;
    case R_INTR_TEST:
        val32 &= INTR_MASK;
        s->regs[R_INTR_STATE] |= val32;
        ot_entropy_src_update_irqs(s);
        break;
    case R_ALERT_TEST:
        val32 &= ALERT_TEST_MASK;
        if (val32) {
            for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
                ibex_irq_set(&s->alerts[ix], (int)((val32 >> ix) & 0x1u));
            }
        }
        break;
    case R_ME_REGWEN:
        val32 &= R_ME_REGWEN_EN_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_SW_REGUPD:
        val32 &= R_SW_REGUPD_UPD_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_MODULE_ENABLE:
        if (s->regs[R_ME_REGWEN]) {
            uint32_t old = s->regs[reg];
            val32 &= R_MODULE_ENABLE_MODULE_ENABLE_MASK;
            s->regs[reg] = val32;
            CHECK_MULTIBOOT(s, MODULE_ENABLE, MODULE_ENABLE);
            if (ot_entropy_src_is_module_disabled(s)) {
                /* change state in disable mode can discard an error state */
                ot_entropy_src_change_state(s, ENTROPY_SRC_IDLE);
                /* reset takes care of cancelling the scheduler timer */
                ot_entropy_src_reset(DEVICE(s));
                break;
            }
            if ((old ^ s->regs[reg]) && ot_entropy_src_is_module_enabled(s)) {
                s->gennum += 1;
                if (ot_entropy_src_is_fips_enabled(s)) {
                    /* start up phase */
                    ot_entropy_src_change_state(s,
                                                ENTROPY_SRC_STARTUP_HT_START);
                } else {
                    /* boot phase */
                    ot_entropy_src_change_state(s, ENTROPY_SRC_BOOT_HT_RUNNING);
                }
                uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                timer_mod(s->scheduler,
                          now + (uint64_t)OT_ENTROPY_SRC_BOOT_DELAY_NS);
            }
            break;
        }
        qemu_log_mask(LOG_GUEST_ERROR, "%s: ME_REGWEN not enabled\n", __func__);
        break;
    case R_CONF:
        if (s->regs[R_REGWEN]) {
            val32 &= CONF_MASK;
            s->regs[reg] = val32;
            CHECK_MULTIBOOT(s, CONF, FIPS_ENABLE);
            CHECK_MULTIBOOT(s, CONF, ENTROPY_DATA_REG_ENABLE);
            CHECK_MULTIBOOT(s, CONF, THRESHOLD_SCOPE);
            CHECK_MULTIBOOT(s, CONF, RNG_BIT_ENABLE);
        }
        break;
    case R_ENTROPY_CONTROL:
        if (s->regs[R_REGWEN]) {
            val32 &= ENTROPY_CONTROL_MASK;
            s->regs[reg] = val32;
            CHECK_MULTIBOOT(s, ENTROPY_CONTROL, ES_ROUTE);
            CHECK_MULTIBOOT(s, ENTROPY_CONTROL, ES_TYPE);
        }
        break;
    case R_HEALTH_TEST_WINDOWS:
        if (s->regs[R_REGWEN]) {
            s->regs[reg] = val32;
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
        if (s->regs[R_REGWEN]) {
            s->regs[reg] = val32;
            ot_entropy_src_update_alerts(s);
        }
        break;
    case R_ALERT_THRESHOLD:
        if (s->regs[R_REGWEN]) {
            if ((uint16_t)(val32) != (uint16_t)(~(val32 >> 16u))) {
                s->regs[R_RECOV_ALERT_STS] |=
                    R_RECOV_ALERT_STS_ES_THRESH_CFG_ALERT_MASK;
            } else {
                s->regs[reg] = val32;
            }
            ot_entropy_src_update_alerts(s);
        }
        break;
    case R_FW_OV_CONTROL:
        if (s->regs[R_REGWEN]) {
            val32 &= FW_OV_CONTROL_MASK;
            s->regs[reg] = val32;
            CHECK_MULTIBOOT(s, FW_OV_CONTROL, FW_OV_MODE);
            CHECK_MULTIBOOT(s, FW_OV_CONTROL, FW_OV_ENTROPY_INSERT);
        }
        break;
    case R_FW_OV_SHA3_START:
        val32 &= R_FW_OV_SHA3_START_FW_OV_INSERT_START_MASK;
        s->regs[reg] = val32;
        CHECK_MULTIBOOT(s, FW_OV_SHA3_START, FW_OV_INSERT_START);
        qemu_log_mask(LOG_UNIMP,
                      "FW override 0x02%" HWADDR_PRIx
                      " (%s) is not supported\n",
                      addr, REG_NAME(reg));
        break;
    case R_FW_OV_RD_FIFO_OVERFLOW:
        val32 &= R_FW_OV_RD_FIFO_OVERFLOW_VAL_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_FW_OV_WR_DATA:
        if (ot_entropy_src_is_fw_ov_mode(s) &&
            REG_MB4_IS_TRUE(s, FW_OV_CONTROL, FW_OV_ENTROPY_INSERT)) {
            if (!ot_fifo32_is_full(&s->hw_fifo)) {
                ot_fifo32_push(&s->hw_fifo, val32);
            } else {
                qemu_log_mask(LOG_GUEST_ERROR, "FW override: FIFO full");
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "FW override mode not active");
        }
        break;
    case R_OBSERVE_FIFO_THRESH:
        if (s->regs[R_REGWEN]) {
            val32 &= R_OBSERVE_FIFO_THRESH_VAL_MASK;
            s->regs[reg] = val32;
            ot_entropy_src_update_irqs(s);
        }
        break;
    case R_RECOV_ALERT_STS:
        val32 &= RECOV_ALERT_STS_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_ERR_CODE_TEST:
        val32 &= R_ERR_CODE_TEST_VAL_MASK;
        val32 = 1u << val32;
        val32 &= ERR_CODE_MASK;
        s->regs[R_ERR_CODE] = val32;
        ot_entropy_src_update_irqs(s);
        ot_entropy_src_update_alerts(s);
        break;
    case R_REGWEN:
    case R_REV:
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
        qemu_log_mask(LOG_GUEST_ERROR,
                      "R/O register 0x02%" HWADDR_PRIx " (%s)\n", addr,
                      REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
};

static Property ot_entropy_src_properties[] = {
    DEFINE_PROP_LINK("ast", OtEntropySrcState, ast, TYPE_OT_AST, OtASTState *),
    DEFINE_PROP_LINK("otp_ctrl", OtEntropySrcState, otp_ctrl, TYPE_OT_OTP,
                     OtOTPState *),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_entropy_src_regs_ops = {
    .read = &ot_entropy_src_regs_read,
    .write = &ot_entropy_src_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_entropy_src_reset(DeviceState *dev)
{
    OtEntropySrcState *s = OT_ENTROPY_SRC(dev);

    assert(s->ast);
    assert(s->otp_ctrl);

    timer_del(s->scheduler);

    memset(s->regs, 0, REGS_SIZE);

    s->regs[R_ME_REGWEN] = 0x1u;
    s->regs[R_SW_REGUPD] = 0x1u;
    s->regs[R_REGWEN] = 0x1u;
    s->regs[R_REV] = 0x10303u;
    s->regs[R_MODULE_ENABLE] = 0x9u;
    s->regs[R_CONF] = 0x909099u;
    s->regs[R_ENTROPY_CONTROL] = 0x99u;
    s->regs[R_HEALTH_TEST_WINDOWS] = 0x600200u;
    s->regs[R_REPCNT_THRESHOLDS] = 0xffffffffu;
    s->regs[R_REPCNTS_THRESHOLDS] = 0xffffffffu;
    s->regs[R_ADAPTP_HI_THRESHOLDS] = 0xffffffffu;
    s->regs[R_BUCKET_THRESHOLDS] = 0xffffffffu;
    s->regs[R_MARKOV_HI_THRESHOLDS] = 0xffffffffu;
    s->regs[R_EXTHT_HI_THRESHOLDS] = 0xffffffffu;
    s->regs[R_ADAPTP_LO_WATERMARKS] = 0xffffffffu;
    s->regs[R_EXTHT_LO_WATERMARKS] = 0xffffffffu;
    s->regs[R_MARKOV_LO_WATERMARKS] = 0xffffffffu;
    s->regs[R_ALERT_THRESHOLD] = 0xfffd0002u;
    s->regs[R_FW_OV_CONTROL] = 0x99u;
    s->regs[R_FW_OV_SHA3_START] = 0x9u;
    s->regs[R_OBSERVE_FIFO_THRESH] = 0x20u;
    s->regs[R_DEBUG_STATUS] = 0x10000u;

    ot_fifo32_reset(&s->observe_fifo);
    ot_fifo32_reset(&s->hw_fifo);

    ot_entropy_src_update_irqs(s);
    for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
        ibex_irq_set(&s->alerts[ix], 0);
    }

    const OtOTPHWCfg *hw_cfg;
    hw_cfg = ot_otp_ctrl_get_hw_cfg(s->otp_ctrl);

    s->otp_fw_read = hw_cfg->en_entropy_src_fw_read == OT_MULTIBITBOOL8_TRUE;
    s->otp_fw_over = hw_cfg->en_entropy_src_fw_over == OT_MULTIBITBOOL8_TRUE;

    trace_ot_entropy_src_otp_conf(s->otp_fw_read, s->otp_fw_over);

    ot_entropy_src_change_state(s, ENTROPY_SRC_IDLE);
}

static void ot_entropy_src_init(Object *obj)
{
    OtEntropySrcState *s = OT_ENTROPY_SRC(obj);

    memory_region_init_io(&s->mmio, obj, &ot_entropy_src_regs_ops, s,
                          TYPE_OT_ENTROPY_SRC, REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    s->regs = g_new0(uint32_t, REGS_COUNT);
    for (unsigned ix = 0; ix < PARAM_NUM_IRQS; ix++) {
        ibex_sysbus_init_irq(obj, &s->irqs[ix]);
    }
    for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
        ibex_qdev_init_irq(obj, &s->alerts[ix], OPENTITAN_DEVICE_ALERT);
    }

    /*
     * should be large enough to store a full output entropy packet + a new
     * entropy buffer from AST. Real HW use a 4-bit input FIFO, QEMU use a
     * 384-bit FIFO to decrease timer. Moreover the 384-bit entropy packet
     * constraint is due to the limited emulation of entropy src: the same
     * buffer is used for entropy packet output (towards CSRNG) as internal
     * entropy src engine (SHA3 etc.) is not implemented */
    ot_fifo32_create(&s->hw_fifo, OT_ENTROPY_SRC_WORD_COUNT * 2u);
    ot_fifo32_create(&s->observe_fifo, PARAM_OBSERVE_FIFO_DEPTH);

    s->scheduler =
        timer_new_ns(QEMU_CLOCK_VIRTUAL, &ot_entropy_src_scheduler, s);
}

static void ot_entropy_src_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = &ot_entropy_src_reset;
    device_class_set_props(dc, ot_entropy_src_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_entropy_src_info = {
    .name = TYPE_OT_ENTROPY_SRC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtEntropySrcState),
    .instance_init = &ot_entropy_src_init,
    .class_init = &ot_entropy_src_class_init,
};

static void ot_entropy_src_register_types(void)
{
    type_register_static(&ot_entropy_src_info);
}

type_init(ot_entropy_src_register_types)
