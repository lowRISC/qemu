/*
 * QEMU OpenTitan EarlGrey One Time Programmable (OTP) implementation
 *
 * Copyright (c) 2023-2025 Rivos, Inc.
 * Copyright (c) 2025 lowRISC contributors.
 *
 * Author(s):
 *  Emmanuel Blot <eblot@rivosinc.com>
 *  Loïc Lefort <loic@rivosinc.com>
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
 */

#define OT_OTP_COMPORTABLE_REGS

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qom/object.h"
#include "hw/opentitan/ot_otp_eg.h"
#include "hw/opentitan/ot_otp_engine.h"
#include "hw/opentitan/ot_otp_impl_if.h"
#include "trace.h"

#define NUM_ERROR_ENTRIES       13u
#define NUM_PART                11u
#define NUM_PART_BUF            6u
#define NUM_PART_UNBUF          5u
#define NUM_SRAM_KEY_REQ_SLOTS  4u
#define NUM_SW_CFG_WINDOW_WORDS 512u

#define OTP_BYTE_ADDR_WIDTH 11u

/* clang-format off */
/* Core registers */
REG32(STATUS, 0x10u)
    FIELD(STATUS, VENDOR_TEST_ERROR, 0u, 1u)
    FIELD(STATUS, CREATOR_SW_CFG_ERROR, 1u, 1u)
    FIELD(STATUS, OWNER_SW_CFG_ERROR, 2u, 1u)
    FIELD(STATUS, ROT_CREATOR_AUTH_CODESIGN_ERROR, 3u, 1u)
    FIELD(STATUS, ROT_CREATOR_AUTH_STATE_ERROR, 4u, 1u)
    FIELD(STATUS, HW_CFG0_ERROR, 5u, 1u)
    FIELD(STATUS, HW_CFG1_ERROR, 6u, 1u)
    FIELD(STATUS, SECRET0_ERROR, 7u, 1u)
    FIELD(STATUS, SECRET1_ERROR, 8u, 1u)
    FIELD(STATUS, SECRET2_ERROR, 9u, 1u)
    FIELD(STATUS, LIFE_CYCLE_ERROR, 10u, 1u)
    FIELD(STATUS, DAI_ERROR, 11u, 1u)
    FIELD(STATUS, LCI_ERROR, 12u, 1u)
    FIELD(STATUS, TIMEOUT_ERROR, 13u, 1u)
    FIELD(STATUS, LFSR_FSM_ERROR, 14u, 1u)
    FIELD(STATUS, SCRAMBLING_FSM_ERROR, 15u, 1u)
    FIELD(STATUS, KEY_DERIV_FSM_ERROR, 16u, 1u)
    FIELD(STATUS, BUS_INTEG_ERROR, 17u, 1u)
    FIELD(STATUS, DAI_IDLE, 18u, 1u)
    FIELD(STATUS, CHECK_PENDING, 19u, 1u)
REG32(ERR_CODE_0, 0x14u)
REG32(ERR_CODE_1, 0x18u)
REG32(ERR_CODE_2, 0x1cu)
REG32(ERR_CODE_3, 0x20u)
REG32(ERR_CODE_4, 0x24u)
REG32(ERR_CODE_5, 0x28u)
REG32(ERR_CODE_6, 0x2cu)
REG32(ERR_CODE_7, 0x30u)
REG32(ERR_CODE_8, 0x34u)
REG32(ERR_CODE_9, 0x38u)
REG32(ERR_CODE_10, 0x3cu)
REG32(ERR_CODE_11, 0x40u)
REG32(ERR_CODE_12, 0x44u)
REG32(DIRECT_ACCESS_REGWEN, 0x48u)
    FIELD(DIRECT_ACCESS_REGWEN, REGWEN, 0u, 1u)
REG32(DIRECT_ACCESS_CMD, 0x4cu)
REG32(DIRECT_ACCESS_ADDRESS, 0x50u)
    FIELD(DIRECT_ACCESS_ADDRESS, ADDRESS, 0, 11u)
REG32(DIRECT_ACCESS_WDATA_0, 0x54u)
REG32(DIRECT_ACCESS_WDATA_1, 0x58u)
REG32(DIRECT_ACCESS_RDATA_0, 0x5cu)
REG32(DIRECT_ACCESS_RDATA_1, 0x60u)
REG32(CHECK_TRIGGER_REGWEN, 0x64u)
    FIELD(CHECK_TRIGGER_REGWEN, REGWEN, 0u, 1u)
REG32(CHECK_TRIGGER, 0x68u)
    FIELD(CHECK_TRIGGER, INTEGRITY, 0u, 1u)
    FIELD(CHECK_TRIGGER, CONSISTENCY, 1u, 1u)
REG32(CHECK_REGWEN, 0x6cu)
    FIELD(CHECK_REGWEN, REGWEN, 0u, 1u)
REG32(CHECK_TIMEOUT, 0x70u)
REG32(INTEGRITY_CHECK_PERIOD, 0x74u)
REG32(CONSISTENCY_CHECK_PERIOD, 0x78u)
REG32(VENDOR_TEST_READ_LOCK, 0x7cu)
REG32(CREATOR_SW_CFG_READ_LOCK, 0x80u)
REG32(OWNER_SW_CFG_READ_LOCK, 0x84u)
REG32(ROT_CREATOR_AUTH_CODESIGN_READ_LOCK, 0x88u)
REG32(ROT_CREATOR_AUTH_STATE_READ_LOCK, 0x8cu)
REG32(VENDOR_TEST_DIGEST_0, 0x90u)
REG32(VENDOR_TEST_DIGEST_1, 0x94u)
REG32(CREATOR_SW_CFG_DIGEST_0, 0x98u)
REG32(CREATOR_SW_CFG_DIGEST_1, 0x9cu)
REG32(OWNER_SW_CFG_DIGEST_0, 0xa0u)
REG32(OWNER_SW_CFG_DIGEST_1, 0xa4u)
REG32(ROT_CREATOR_AUTH_CODESIGN_DIGEST_0, 0xa8u)
REG32(ROT_CREATOR_AUTH_CODESIGN_DIGEST_1, 0xacu)
REG32(ROT_CREATOR_AUTH_STATE_DIGEST_0, 0xb0u)
REG32(ROT_CREATOR_AUTH_STATE_DIGEST_1, 0xb4u)
REG32(HW_CFG0_DIGEST_0, 0xb8u)
REG32(HW_CFG0_DIGEST_1, 0xbcu)
REG32(HW_CFG1_DIGEST_0, 0xc0u)
REG32(HW_CFG1_DIGEST_1, 0xc4u)
REG32(SECRET0_DIGEST_0, 0xc8u)
REG32(SECRET0_DIGEST_1, 0xccu)
REG32(SECRET1_DIGEST_0, 0xd0u)
REG32(SECRET1_DIGEST_1, 0xd4u)
REG32(SECRET2_DIGEST_0, 0xd8u)
REG32(SECRET2_DIGEST_1, 0xdcu)

/* Software Config Window registers (at offset SW_CFG_WINDOW_OFFSET) */
REG32(VENDOR_TEST_SCRATCH, 0u)
REG32(VENDOR_TEST_DIGEST, 56u)
REG32(CREATOR_SW_CFG_AST_CFG, 64u)
REG32(CREATOR_SW_CFG_AST_INIT_EN, 220u)
REG32(CREATOR_SW_CFG_ROM_EXT_SKU, 224u)
REG32(CREATOR_SW_CFG_SIGVERIFY_SPX_EN, 228u)
REG32(CREATOR_SW_CFG_FLASH_DATA_DEFAULT_CFG, 232u)
REG32(CREATOR_SW_CFG_FLASH_INFO_BOOT_DATA_CFG, 236u)
REG32(CREATOR_SW_CFG_FLASH_HW_INFO_CFG_OVERRIDE, 240u)
REG32(CREATOR_SW_CFG_RNG_EN, 244u)
REG32(CREATOR_SW_CFG_JITTER_EN, 248u)
REG32(CREATOR_SW_CFG_RET_RAM_RESET_MASK, 252u)
REG32(CREATOR_SW_CFG_MANUF_STATE, 256u)
REG32(CREATOR_SW_CFG_ROM_EXEC_EN, 260u)
REG32(CREATOR_SW_CFG_CPUCTRL, 264u)
REG32(CREATOR_SW_CFG_MIN_SEC_VER_ROM_EXT, 268u)
REG32(CREATOR_SW_CFG_MIN_SEC_VER_BL0, 272u)
REG32(CREATOR_SW_CFG_DEFAULT_BOOT_DATA_IN_PROD_EN, 276u)
REG32(CREATOR_SW_CFG_RMA_SPIN_EN, 280u)
REG32(CREATOR_SW_CFG_RMA_SPIN_CYCLES, 284u)
REG32(CREATOR_SW_CFG_RNG_REPCNT_THRESHOLDS, 288u)
REG32(CREATOR_SW_CFG_RNG_REPCNTS_THRESHOLDS, 292u)
REG32(CREATOR_SW_CFG_RNG_ADAPTP_HI_THRESHOLDS, 296u)
REG32(CREATOR_SW_CFG_RNG_ADAPTP_LO_THRESHOLDS, 300u)
REG32(CREATOR_SW_CFG_RNG_BUCKET_THRESHOLDS, 304u)
REG32(CREATOR_SW_CFG_RNG_MARKOV_HI_THRESHOLDS, 308u)
REG32(CREATOR_SW_CFG_RNG_MARKOV_LO_THRESHOLDS, 312u)
REG32(CREATOR_SW_CFG_RNG_EXTHT_HI_THRESHOLDS, 316u)
REG32(CREATOR_SW_CFG_RNG_EXTHT_LO_THRESHOLDS, 320u)
REG32(CREATOR_SW_CFG_RNG_ALERT_THRESHOLD, 324u)
REG32(CREATOR_SW_CFG_RNG_HEALTH_CONFIG_DIGEST, 328u)
REG32(CREATOR_SW_CFG_SRAM_KEY_RENEW_EN, 332u)
REG32(CREATOR_SW_CFG_IMMUTABLE_ROM_EXT_EN, 336u)
REG32(CREATOR_SW_CFG_IMMUTABLE_ROM_EXT_START_OFFSET, 340u)
REG32(CREATOR_SW_CFG_IMMUTABLE_ROM_EXT_LENGTH, 344u)
REG32(CREATOR_SW_CFG_IMMUTABLE_ROM_EXT_SHA256_HASH, 348u)
REG32(CREATOR_SW_CFG_RESERVED, 380u)
REG32(CREATOR_SW_CFG_DIGEST, 424u)
REG32(OWNER_SW_CFG_ROM_ERROR_REPORTING, 432u)
REG32(OWNER_SW_CFG_ROM_BOOTSTRAP_DIS, 436u)
REG32(OWNER_SW_CFG_ROM_ALERT_CLASS_EN, 440u)
REG32(OWNER_SW_CFG_ROM_ALERT_ESCALATION, 444u)
REG32(OWNER_SW_CFG_ROM_ALERT_CLASSIFICATION, 448u)
REG32(OWNER_SW_CFG_ROM_LOCAL_ALERT_CLASSIFICATION, 768u)
REG32(OWNER_SW_CFG_ROM_ALERT_ACCUM_THRESH, 832u)
REG32(OWNER_SW_CFG_ROM_ALERT_TIMEOUT_CYCLES, 848u)
REG32(OWNER_SW_CFG_ROM_ALERT_PHASE_CYCLES, 864u)
REG32(OWNER_SW_CFG_ROM_ALERT_DIGEST_PROD, 928u)
REG32(OWNER_SW_CFG_ROM_ALERT_DIGEST_PROD_END, 932u)
REG32(OWNER_SW_CFG_ROM_ALERT_DIGEST_DEV, 936u)
REG32(OWNER_SW_CFG_ROM_ALERT_DIGEST_RMA, 940u)
REG32(OWNER_SW_CFG_ROM_WATCHDOG_BITE_THRESHOLD_CYCLES, 944u)
REG32(OWNER_SW_CFG_ROM_KEYMGR_OTP_MEAS_EN, 948u)
REG32(OWNER_SW_CFG_MANUF_STATE, 952u)
REG32(OWNER_SW_CFG_ROM_RSTMGR_INFO_EN, 956u)
REG32(OWNER_SW_CFG_ROM_EXT_BOOTSTRAP_EN, 960u)
REG32(OWNER_SW_CFG_ROM_SENSOR_CTRL_ALERT_CFG, 964u)
REG32(OWNER_SW_CFG_ROM_SRAM_READBACK_EN, 976u)
REG32(OWNER_SW_CFG_ROM_PRESERVE_RESET_REASON_EN, 980u)
REG32(OWNER_SW_CFG_ROM_RESET_REASON_CHECK_VALUE, 984u)
REG32(OWNER_SW_CFG_ROM_BANNER_EN, 988u)
REG32(OWNER_SW_CFG_ROM_FLASH_ECC_EXC_HANDLER_EN, 992u)
REG32(OWNER_SW_CFG_RESERVED, 996u)
REG32(OWNER_SW_CFG_DIGEST, 1136u)
REG32(ROT_CREATOR_AUTH_CODESIGN_ECDSA_KEY_TYPE0, 1144u)
REG32(ROT_CREATOR_AUTH_CODESIGN_ECDSA_KEY0, 1148u)
REG32(ROT_CREATOR_AUTH_CODESIGN_ECDSA_KEY_TYPE1, 1212u)
REG32(ROT_CREATOR_AUTH_CODESIGN_ECDSA_KEY1, 1216u)
REG32(ROT_CREATOR_AUTH_CODESIGN_ECDSA_KEY_TYPE2, 1280u)
REG32(ROT_CREATOR_AUTH_CODESIGN_ECDSA_KEY2, 1284u)
REG32(ROT_CREATOR_AUTH_CODESIGN_ECDSA_KEY_TYPE3, 1348u)
REG32(ROT_CREATOR_AUTH_CODESIGN_ECDSA_KEY3, 1352u)
REG32(ROT_CREATOR_AUTH_CODESIGN_SPX_KEY_TYPE0, 1416u)
REG32(ROT_CREATOR_AUTH_CODESIGN_SPX_KEY0, 1420u)
REG32(ROT_CREATOR_AUTH_CODESIGN_SPX_KEY_CONFIG0, 1452u)
REG32(ROT_CREATOR_AUTH_CODESIGN_SPX_KEY_TYPE1, 1456u)
REG32(ROT_CREATOR_AUTH_CODESIGN_SPX_KEY1, 1460u)
REG32(ROT_CREATOR_AUTH_CODESIGN_SPX_KEY_CONFIG1, 1492u)
REG32(ROT_CREATOR_AUTH_CODESIGN_SPX_KEY_TYPE2, 1496u)
REG32(ROT_CREATOR_AUTH_CODESIGN_SPX_KEY2, 1500u)
REG32(ROT_CREATOR_AUTH_CODESIGN_SPX_KEY_CONFIG2, 1532u)
REG32(ROT_CREATOR_AUTH_CODESIGN_SPX_KEY_TYPE3, 1536u)
REG32(ROT_CREATOR_AUTH_CODESIGN_SPX_KEY3, 1540u)
REG32(ROT_CREATOR_AUTH_CODESIGN_SPX_KEY_CONFIG3, 1572u)
REG32(ROT_CREATOR_AUTH_CODESIGN_BLOCK_SHA2_256_HASH, 1576u)
REG32(ROT_CREATOR_AUTH_CODESIGN_DIGEST, 1608u)
REG32(ROT_CREATOR_AUTH_STATE_ECDSA_KEY0, 1616u)
REG32(ROT_CREATOR_AUTH_STATE_ECDSA_KEY1, 1620u)
REG32(ROT_CREATOR_AUTH_STATE_ECDSA_KEY2, 1624u)
REG32(ROT_CREATOR_AUTH_STATE_ECDSA_KEY3, 1628u)
REG32(ROT_CREATOR_AUTH_STATE_SPX_KEY0, 1632u)
REG32(ROT_CREATOR_AUTH_STATE_SPX_KEY1, 1636u)
REG32(ROT_CREATOR_AUTH_STATE_SPX_KEY2, 1640u)
REG32(ROT_CREATOR_AUTH_STATE_SPX_KEY3, 1644u)
REG32(ROT_CREATOR_AUTH_STATE_DIGEST, 1648u)
REG32(HW_CFG0_DEVICE_ID, 1656u)
REG32(HW_CFG0_MANUF_STATE, 1688u)
REG32(HW_CFG0_DIGEST, 1720u)
REG32(HW_CFG1_EN_SRAM_IFETCH, 1728u)
REG32(HW_CFG1_EN_CSRNG_SW_APP_READ, 1729u)
REG32(HW_CFG1_DIS_RV_DM_LATE_DEBUG, 1730u)
REG32(HW_CFG1_DIGEST, 1736u)
REG32(SECRET0_TEST_UNLOCK_TOKEN, 1744u)
REG32(SECRET0_TEST_EXIT_TOKEN, 1760u)
REG32(SECRET0_DIGEST, 1776u)
REG32(SECRET1_FLASH_ADDR_KEY_SEED, 1784u)
REG32(SECRET1_FLASH_DATA_KEY_SEED, 1816u)
REG32(SECRET1_SRAM_DATA_KEY_SEED, 1848u)
REG32(SECRET1_DIGEST, 1864u)
REG32(SECRET2_RMA_TOKEN, 1872u)
REG32(SECRET2_CREATOR_ROOT_KEY_SHARE0, 1888u)
REG32(SECRET2_CREATOR_ROOT_KEY_SHARE1, 1920u)
REG32(SECRET2_DIGEST, 1952u)
REG32(LC_TRANSITION_CNT, 1960u)
REG32(LC_STATE, 2008u)
/* clang-format on */

#define VENDOR_TEST_SCRATCH_SIZE                             56u
#define CREATOR_SW_CFG_AST_CFG_SIZE                          156u
#define CREATOR_SW_CFG_AST_INIT_EN_SIZE                      4u
#define CREATOR_SW_CFG_ROM_EXT_SKU_SIZE                      4u
#define CREATOR_SW_CFG_SIGVERIFY_SPX_EN_SIZE                 4u
#define CREATOR_SW_CFG_FLASH_DATA_DEFAULT_CFG_SIZE           4u
#define CREATOR_SW_CFG_FLASH_INFO_BOOT_DATA_CFG_SIZE         4u
#define CREATOR_SW_CFG_FLASH_HW_INFO_CFG_OVERRIDE_SIZE       4u
#define CREATOR_SW_CFG_RNG_EN_SIZE                           4u
#define CREATOR_SW_CFG_JITTER_EN_SIZE                        4u
#define CREATOR_SW_CFG_RET_RAM_RESET_MASK_SIZE               4u
#define CREATOR_SW_CFG_MANUF_STATE_SIZE                      4u
#define CREATOR_SW_CFG_ROM_EXEC_EN_SIZE                      4u
#define CREATOR_SW_CFG_CPUCTRL_SIZE                          4u
#define CREATOR_SW_CFG_MIN_SEC_VER_ROM_EXT_SIZE              4u
#define CREATOR_SW_CFG_MIN_SEC_VER_BL0_SIZE                  4u
#define CREATOR_SW_CFG_DEFAULT_BOOT_DATA_IN_PROD_EN_SIZE     4u
#define CREATOR_SW_CFG_RMA_SPIN_EN_SIZE                      4u
#define CREATOR_SW_CFG_RMA_SPIN_CYCLES_SIZE                  4u
#define CREATOR_SW_CFG_RNG_REPCNT_THRESHOLDS_SIZE            4u
#define CREATOR_SW_CFG_RNG_REPCNTS_THRESHOLDS_SIZE           4u
#define CREATOR_SW_CFG_RNG_ADAPTP_HI_THRESHOLDS_SIZE         4u
#define CREATOR_SW_CFG_RNG_ADAPTP_LO_THRESHOLDS_SIZE         4u
#define CREATOR_SW_CFG_RNG_BUCKET_THRESHOLDS_SIZE            4u
#define CREATOR_SW_CFG_RNG_MARKOV_HI_THRESHOLDS_SIZE         4u
#define CREATOR_SW_CFG_RNG_MARKOV_LO_THRESHOLDS_SIZE         4u
#define CREATOR_SW_CFG_RNG_EXTHT_HI_THRESHOLDS_SIZE          4u
#define CREATOR_SW_CFG_RNG_EXTHT_LO_THRESHOLDS_SIZE          4u
#define CREATOR_SW_CFG_RNG_ALERT_THRESHOLD_SIZE              4u
#define CREATOR_SW_CFG_SRAM_KEY_RENEW_EN_SIZE                4u
#define CREATOR_SW_CFG_IMMUTABLE_ROM_EXT_EN_SIZE             4u
#define CREATOR_SW_CFG_IMMUTABLE_ROM_EXT_START_OFFSET_SIZE   4u
#define CREATOR_SW_CFG_IMMUTABLE_ROM_EXT_LENGTH_SIZE         4u
#define CREATOR_SW_CFG_IMMUTABLE_ROM_EXT_SHA256_HASH_SIZE    32u
#define CREATOR_SW_CFG_RESERVED_SIZE                         32u
#define OWNER_SW_CFG_ROM_ERROR_REPORTING_SIZE                4u
#define OWNER_SW_CFG_ROM_BOOTSTRAP_DIS_SIZE                  4u
#define OWNER_SW_CFG_ROM_ALERT_CLASS_EN_SIZE                 4u
#define OWNER_SW_CFG_ROM_ALERT_ESCALATION_SIZE               4u
#define OWNER_SW_CFG_ROM_ALERT_CLASSIFICATION_SIZE           320u
#define OWNER_SW_CFG_ROM_LOCAL_ALERT_CLASSIFICATION_SIZE     64u
#define OWNER_SW_CFG_ROM_ALERT_ACCUM_THRESH_SIZE             16u
#define OWNER_SW_CFG_ROM_ALERT_TIMEOUT_CYCLES_SIZE           16u
#define OWNER_SW_CFG_ROM_ALERT_PHASE_CYCLES_SIZE             64u
#define OWNER_SW_CFG_ROM_ALERT_DIGEST_PROD_SIZE              4u
#define OWNER_SW_CFG_ROM_ALERT_DIGEST_PROD_END_SIZE          4u
#define OWNER_SW_CFG_ROM_ALERT_DIGEST_DEV_SIZE               4u
#define OWNER_SW_CFG_ROM_ALERT_DIGEST_RMA_SIZE               4u
#define OWNER_SW_CFG_ROM_WATCHDOG_BITE_THRESHOLD_CYCLES_SIZE 4u
#define OWNER_SW_CFG_ROM_KEYMGR_OTP_MEAS_EN_SIZE             4u
#define OWNER_SW_CFG_MANUF_STATE_SIZE                        4u
#define OWNER_SW_CFG_ROM_RSTMGR_INFO_EN_SIZE                 4u
#define OWNER_SW_CFG_ROM_EXT_BOOTSTRAP_EN_SIZE               4u
#define OWNER_SW_CFG_ROM_SENSOR_CTRL_ALERT_CFG_SIZE          12u
#define OWNER_SW_CFG_ROM_SRAM_READBACK_EN_SIZE               4u
#define OWNER_SW_CFG_ROM_PRESERVE_RESET_REASON_EN_SIZE       4u
#define OWNER_SW_CFG_ROM_RESET_REASON_CHECK_VALUE_SIZE       4u
#define OWNER_SW_CFG_ROM_BANNER_EN_SIZE                      4u
#define OWNER_SW_CFG_ROM_FLASH_ECC_EXC_HANDLER_EN_SIZE       4u
#define OWNER_SW_CFG_RESERVED_SIZE                           128u
#define ROT_CREATOR_AUTH_CODESIGN_ECDSA_KEY_TYPE0_SIZE       4u
#define ROT_CREATOR_AUTH_CODESIGN_ECDSA_KEY0_SIZE            64u
#define ROT_CREATOR_AUTH_CODESIGN_ECDSA_KEY_TYPE1_SIZE       4u
#define ROT_CREATOR_AUTH_CODESIGN_ECDSA_KEY1_SIZE            64u
#define ROT_CREATOR_AUTH_CODESIGN_ECDSA_KEY_TYPE2_SIZE       4u
#define ROT_CREATOR_AUTH_CODESIGN_ECDSA_KEY2_SIZE            64u
#define ROT_CREATOR_AUTH_CODESIGN_ECDSA_KEY_TYPE3_SIZE       4u
#define ROT_CREATOR_AUTH_CODESIGN_ECDSA_KEY3_SIZE            64u
#define ROT_CREATOR_AUTH_CODESIGN_SPX_KEY_TYPE0_SIZE         4u
#define ROT_CREATOR_AUTH_CODESIGN_SPX_KEY0_SIZE              32u
#define ROT_CREATOR_AUTH_CODESIGN_SPX_KEY_CONFIG0_SIZE       4u
#define ROT_CREATOR_AUTH_CODESIGN_SPX_KEY_TYPE1_SIZE         4u
#define ROT_CREATOR_AUTH_CODESIGN_SPX_KEY1_SIZE              32u
#define ROT_CREATOR_AUTH_CODESIGN_SPX_KEY_CONFIG1_SIZE       4u
#define ROT_CREATOR_AUTH_CODESIGN_SPX_KEY_TYPE2_SIZE         4u
#define ROT_CREATOR_AUTH_CODESIGN_SPX_KEY2_SIZE              32u
#define ROT_CREATOR_AUTH_CODESIGN_SPX_KEY_CONFIG2_SIZE       4u
#define ROT_CREATOR_AUTH_CODESIGN_SPX_KEY_TYPE3_SIZE         4u
#define ROT_CREATOR_AUTH_CODESIGN_SPX_KEY3_SIZE              32u
#define ROT_CREATOR_AUTH_CODESIGN_SPX_KEY_CONFIG3_SIZE       4u
#define ROT_CREATOR_AUTH_CODESIGN_BLOCK_SHA2_256_HASH_SIZE   32u
#define ROT_CREATOR_AUTH_STATE_ECDSA_KEY0_SIZE               4u
#define ROT_CREATOR_AUTH_STATE_ECDSA_KEY1_SIZE               4u
#define ROT_CREATOR_AUTH_STATE_ECDSA_KEY2_SIZE               4u
#define ROT_CREATOR_AUTH_STATE_ECDSA_KEY3_SIZE               4u
#define ROT_CREATOR_AUTH_STATE_SPX_KEY0_SIZE                 4u
#define ROT_CREATOR_AUTH_STATE_SPX_KEY1_SIZE                 4u
#define ROT_CREATOR_AUTH_STATE_SPX_KEY2_SIZE                 4u
#define ROT_CREATOR_AUTH_STATE_SPX_KEY3_SIZE                 4u
#define HW_CFG0_DEVICE_ID_SIZE                               32u
#define HW_CFG0_MANUF_STATE_SIZE                             32u
#define HW_CFG1_EN_SRAM_IFETCH_SIZE                          1u
#define HW_CFG1_EN_CSRNG_SW_APP_READ_SIZE                    1u
#define HW_CFG1_DIS_RV_DM_LATE_DEBUG_SIZE                    1u
#define SECRET0_TEST_UNLOCK_TOKEN_SIZE                       16u
#define SECRET0_TEST_EXIT_TOKEN_SIZE                         16u
#define SECRET1_FLASH_ADDR_KEY_SEED_SIZE                     32u
#define SECRET1_FLASH_DATA_KEY_SEED_SIZE                     32u
#define SECRET1_SRAM_DATA_KEY_SEED_SIZE                      16u
#define SECRET2_RMA_TOKEN_SIZE                               16u
#define SECRET2_CREATOR_ROOT_KEY_SHARE0_SIZE                 32u
#define SECRET2_CREATOR_ROOT_KEY_SHARE1_SIZE                 32u

#define SW_CFG_WINDOW_OFFSET 0x800u
#define SW_CFG_WINDOW_SIZE   (NUM_SW_CFG_WINDOW_WORDS * sizeof(uint32_t))

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_SECRET2_DIGEST_1)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

/* note: useless casts are required for GCC linter */
static_assert((unsigned)R_STATUS == (unsigned)R_OTP_FIRST_IMPL_REG,
              "Invalid register address");

typedef enum {
    OTP_PART_VENDOR_TEST,
    OTP_PART_CREATOR_SW_CFG,
    OTP_PART_OWNER_SW_CFG,
    OTP_PART_ROT_CREATOR_AUTH_CODESIGN,
    OTP_PART_ROT_CREATOR_AUTH_STATE,
    OTP_PART_HW_CFG0,
    OTP_PART_HW_CFG1,
    OTP_PART_SECRET0,
    OTP_PART_SECRET1,
    OTP_PART_SECRET2,
    OTP_PART_LIFE_CYCLE,
    OTP_PART_OTP_COUNT, /* count of "real" OTP partitions */
    OTP_ENTRY_DAI = OTP_PART_OTP_COUNT, /* Fake partitions for error (...) */
    OTP_ENTRY_KDI, /* Key derivation issue, not really OTP */
    OTP_ENTRY_COUNT,
} OtOTPPartitionType;

static_assert(OTP_PART_OTP_COUNT == NUM_PART, "Invalid partition count");

#define OT_OTP_EG_PARTS

/* NOLINTNEXTLINE */
#include "ot_otp_eg_parts.c"

static_assert(OTP_PART_OTP_COUNT == OTP_PART_COUNT, "Invalid partition count");
static_assert(OTP_PART_COUNT <= 64, "Maximum part count reached");

static_assert(OTP_BYTE_ADDR_WIDTH == R_DIRECT_ACCESS_ADDRESS_ADDRESS_LENGTH,
              "OTP byte address width mismatch");
struct OtOTPEgState {
    OtOTPEngineState parent_obj;

    struct {
        MemoryRegion ctrl;
        struct {
            MemoryRegion regs;
            MemoryRegion swcfg;
        } sub;
    } mmio;
};

struct OtOTPEgClass {
    OtOTPEngineClass parent_class;
    ResettablePhases parent_phases;
};

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    /* clang-format off */
    REG_NAME_ENTRY(INTR_STATE),
    REG_NAME_ENTRY(INTR_ENABLE),
    REG_NAME_ENTRY(INTR_TEST),
    REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(STATUS),
    REG_NAME_ENTRY(ERR_CODE_0),
    REG_NAME_ENTRY(ERR_CODE_1),
    REG_NAME_ENTRY(ERR_CODE_2),
    REG_NAME_ENTRY(ERR_CODE_3),
    REG_NAME_ENTRY(ERR_CODE_4),
    REG_NAME_ENTRY(ERR_CODE_5),
    REG_NAME_ENTRY(ERR_CODE_6),
    REG_NAME_ENTRY(ERR_CODE_7),
    REG_NAME_ENTRY(ERR_CODE_8),
    REG_NAME_ENTRY(ERR_CODE_9),
    REG_NAME_ENTRY(ERR_CODE_10),
    REG_NAME_ENTRY(ERR_CODE_11),
    REG_NAME_ENTRY(ERR_CODE_12),
    REG_NAME_ENTRY(DIRECT_ACCESS_REGWEN),
    REG_NAME_ENTRY(DIRECT_ACCESS_CMD),
    REG_NAME_ENTRY(DIRECT_ACCESS_ADDRESS),
    REG_NAME_ENTRY(DIRECT_ACCESS_WDATA_0),
    REG_NAME_ENTRY(DIRECT_ACCESS_WDATA_1),
    REG_NAME_ENTRY(DIRECT_ACCESS_RDATA_0),
    REG_NAME_ENTRY(DIRECT_ACCESS_RDATA_1),
    REG_NAME_ENTRY(CHECK_TRIGGER_REGWEN),
    REG_NAME_ENTRY(CHECK_TRIGGER),
    REG_NAME_ENTRY(CHECK_REGWEN),
    REG_NAME_ENTRY(CHECK_TIMEOUT),
    REG_NAME_ENTRY(INTEGRITY_CHECK_PERIOD),
    REG_NAME_ENTRY(CONSISTENCY_CHECK_PERIOD),
    REG_NAME_ENTRY(VENDOR_TEST_READ_LOCK),
    REG_NAME_ENTRY(CREATOR_SW_CFG_READ_LOCK),
    REG_NAME_ENTRY(OWNER_SW_CFG_READ_LOCK),
    REG_NAME_ENTRY(ROT_CREATOR_AUTH_CODESIGN_READ_LOCK),
    REG_NAME_ENTRY(ROT_CREATOR_AUTH_STATE_READ_LOCK),
    REG_NAME_ENTRY(VENDOR_TEST_DIGEST_0),
    REG_NAME_ENTRY(VENDOR_TEST_DIGEST_1),
    REG_NAME_ENTRY(CREATOR_SW_CFG_DIGEST_0),
    REG_NAME_ENTRY(CREATOR_SW_CFG_DIGEST_1),
    REG_NAME_ENTRY(OWNER_SW_CFG_DIGEST_0),
    REG_NAME_ENTRY(OWNER_SW_CFG_DIGEST_1),
    REG_NAME_ENTRY(ROT_CREATOR_AUTH_CODESIGN_DIGEST_0),
    REG_NAME_ENTRY(ROT_CREATOR_AUTH_CODESIGN_DIGEST_1),
    REG_NAME_ENTRY(ROT_CREATOR_AUTH_STATE_DIGEST_0),
    REG_NAME_ENTRY(ROT_CREATOR_AUTH_STATE_DIGEST_1),
    REG_NAME_ENTRY(HW_CFG0_DIGEST_0),
    REG_NAME_ENTRY(HW_CFG0_DIGEST_1),
    REG_NAME_ENTRY(HW_CFG1_DIGEST_0),
    REG_NAME_ENTRY(HW_CFG1_DIGEST_1),
    REG_NAME_ENTRY(SECRET0_DIGEST_0),
    REG_NAME_ENTRY(SECRET0_DIGEST_1),
    REG_NAME_ENTRY(SECRET1_DIGEST_0),
    REG_NAME_ENTRY(SECRET1_DIGEST_1),
    REG_NAME_ENTRY(SECRET2_DIGEST_0),
    REG_NAME_ENTRY(SECRET2_DIGEST_1),
    /* clang-format on */
};
#undef REG_NAME_ENTRY

#define OT_OTP_NAME_ENTRY(_st_) [OT_OTP_##_st_] = stringify(OT_OTP_##_st_)

static const char *OTP_TOKEN_NAMES[] = {
    /* clang-format off */
    OT_OTP_NAME_ENTRY(TOKEN_TEST_UNLOCK),
    OT_OTP_NAME_ENTRY(TOKEN_TEST_EXIT),
    OT_OTP_NAME_ENTRY(TOKEN_RMA),
    /* clang-format on */
};

#define OTP_TOKEN_NAME(_tk_) \
    ((unsigned)(_tk_) < ARRAY_SIZE(OTP_TOKEN_NAMES) ? \
         OTP_TOKEN_NAMES[(_tk_)] : \
         "?")

static uint32_t ot_otp_eg_get_status(const OtOTPEngineState *s)
{
    uint32_t status;

    status = FIELD_DP32(s->regs[R_STATUS], STATUS, DAI_IDLE,
                        !ot_otp_engine_dai_is_busy(s));

    return status;
}

static uint64_t ot_otp_eg_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    OtOTPEngineState *s = OT_OTP_ENGINE(opaque);
    OtOTPEngineClass *c = OT_OTP_ENGINE_GET_CLASS(s);
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);
    switch (reg) {
    case R_INTR_STATE:
    case R_INTR_ENABLE:
    case R_ERR_CODE_0 ... R_ERR_CODE_12:
    case R_DIRECT_ACCESS_WDATA_0:
    case R_DIRECT_ACCESS_WDATA_1:
    case R_DIRECT_ACCESS_RDATA_0:
    case R_DIRECT_ACCESS_RDATA_1:
    case R_DIRECT_ACCESS_ADDRESS:
    case R_VENDOR_TEST_READ_LOCK ... R_ROT_CREATOR_AUTH_STATE_READ_LOCK:
    case R_CHECK_TRIGGER_REGWEN:
    case R_CHECK_REGWEN:
        val32 = s->regs[reg];
        break;
    case R_STATUS:
        val32 = ot_otp_eg_get_status(s);
        break;
    case R_DIRECT_ACCESS_REGWEN:
        /* disabled either if SW locked, or if DAI is busy. */
        val32 = s->regs[reg];
        val32 &= FIELD_DP32(0u, DIRECT_ACCESS_REGWEN, REGWEN,
                            (uint32_t)!ot_otp_engine_dai_is_busy(s));
        break;
    /* NOLINTNEXTLINE */
    case R_DIRECT_ACCESS_CMD:
    case R_CHECK_TRIGGER:
        val32 = 0; /* R0W1C */
        break;
    case R_CHECK_TIMEOUT:
    case R_INTEGRITY_CHECK_PERIOD:
    case R_CONSISTENCY_CHECK_PERIOD:
        /* @todo: not yet implemented, but these are R/W registers */
        qemu_log_mask(LOG_UNIMP, "%s: %s: %s is not supported\n", __func__,
                      s->ot_id, REG_NAME(reg));
        val32 = s->regs[reg];
        break;
    case R_VENDOR_TEST_DIGEST_0 ... R_SECRET2_DIGEST_1:
        /*
         * In all partitions with a digest, the digest itself is ALWAYS
         * readable.
         */
        val32 = c->get_part_digest_reg(s, reg - R_VENDOR_TEST_DIGEST_0);
        break;
    case R_INTR_TEST:
    case R_ALERT_TEST:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: W/O register 0x02%" HWADDR_PRIx " (%s)\n",
                      __func__, s->ot_id, addr, REG_NAME(reg));
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
    trace_ot_otp_io_reg_read_out(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32,
                                 pc);

    return (uint64_t)val32;
}

static void ot_otp_eg_reg_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    OtOTPEngineState *s = OT_OTP_ENGINE(opaque);
    OtOTPEngineClass *c = OT_OTP_ENGINE_GET_CLASS(s);
    (void)size;
    uint32_t val32 = (uint32_t)value;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();

    trace_ot_otp_io_reg_write(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32,
                              pc);

    switch (reg) {
    case R_DIRECT_ACCESS_CMD:
    case R_DIRECT_ACCESS_ADDRESS:
    case R_DIRECT_ACCESS_WDATA_0:
    case R_DIRECT_ACCESS_WDATA_1:
    case R_VENDOR_TEST_READ_LOCK ... R_ROT_CREATOR_AUTH_STATE_READ_LOCK:
        if (!(s->regs[R_DIRECT_ACCESS_REGWEN] &
              R_DIRECT_ACCESS_REGWEN_REGWEN_MASK)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: %s is not enabled, %s is protected\n",
                          __func__, s->ot_id, REG_NAME(R_DIRECT_ACCESS_REGWEN),
                          REG_NAME(reg));
            return;
        }
        break;
    case R_CHECK_TRIGGER:
        if (!(s->regs[R_CHECK_TRIGGER_REGWEN] &
              R_CHECK_TRIGGER_REGWEN_REGWEN_MASK)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: %s is not enabled, %s is protected\n",
                          __func__, s->ot_id, REG_NAME(R_CHECK_TRIGGER_REGWEN),
                          REG_NAME(reg));
            return;
        }
        break;
    case R_CHECK_TIMEOUT:
    case R_INTEGRITY_CHECK_PERIOD:
    case R_CONSISTENCY_CHECK_PERIOD:
        if (!(s->regs[R_CHECK_REGWEN] & R_CHECK_REGWEN_REGWEN_MASK)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: %s is not enabled, %s is protected\n",
                          __func__, s->ot_id, REG_NAME(R_CHECK_REGWEN),
                          REG_NAME(reg));
            return;
        }
        break;
    case R_STATUS:
    case R_ERR_CODE_0 ... R_ERR_CODE_12:
    case R_DIRECT_ACCESS_RDATA_0:
    case R_DIRECT_ACCESS_RDATA_1:
    case R_VENDOR_TEST_DIGEST_0 ... R_SECRET2_DIGEST_1:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: R/O register 0x%03" HWADDR_PRIx " (%s)\n",
                      __func__, s->ot_id, addr, REG_NAME(reg));
        return;
    default:
        break;
    }

    switch (reg) {
    case R_INTR_STATE:
        val32 &= INTR_WMASK;
        s->regs[R_INTR_STATE] &= ~val32; /* RW1C */
        c->update_irqs(s);
        break;
    case R_INTR_ENABLE:
        val32 &= INTR_WMASK;
        s->regs[R_INTR_ENABLE] = val32;
        c->update_irqs(s);
        break;
    case R_INTR_TEST:
        val32 &= INTR_WMASK;
        s->regs[R_INTR_STATE] = val32;
        c->update_irqs(s);
        break;
    case R_ALERT_TEST:
        val32 &= ALERT_WMASK;
        s->regs[reg] = val32;
        c->update_alerts(s);
        break;
    case R_DIRECT_ACCESS_REGWEN:
        val32 &= R_DIRECT_ACCESS_REGWEN_REGWEN_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_DIRECT_ACCESS_CMD:
        if (FIELD_EX32(val32, OT_OTP_DIRECT_ACCESS_CMD, RD)) {
            c->dai_read(s);
        } else if (FIELD_EX32(val32, OT_OTP_DIRECT_ACCESS_CMD, WR)) {
            c->dai_write(s);
        } else if (FIELD_EX32(val32, OT_OTP_DIRECT_ACCESS_CMD, DIGEST)) {
            c->dai_digest(s);
        }
        break;
    case R_DIRECT_ACCESS_ADDRESS:
        val32 &= R_DIRECT_ACCESS_ADDRESS_ADDRESS_MASK;
        s->regs[reg] = val32;
        break;
    case R_DIRECT_ACCESS_WDATA_0:
    case R_DIRECT_ACCESS_WDATA_1:
        s->regs[reg] = val32;
        break;
    case R_VENDOR_TEST_READ_LOCK ... R_ROT_CREATOR_AUTH_STATE_READ_LOCK:
        val32 &= OT_OTP_READ_LOCK_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_CHECK_TRIGGER_REGWEN:
        val32 &= R_CHECK_TRIGGER_REGWEN_REGWEN_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_CHECK_REGWEN:
        val32 &= R_CHECK_REGWEN_REGWEN_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_CHECK_TRIGGER:
    case R_CHECK_TIMEOUT:
    case R_INTEGRITY_CHECK_PERIOD:
    case R_CONSISTENCY_CHECK_PERIOD:
        qemu_log_mask(LOG_UNIMP, "%s: %s: %s is not supported\n", __func__,
                      s->ot_id, REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Bad offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, addr);
        break;
    }
}

static const char *ot_otp_eg_swcfg_reg_name(unsigned swreg)
{
#define CASE_BYTE(_reg_) \
    case A_##_reg_: \
        return stringify(_reg_)
#define CASE_SUB(_reg_, _sz_) \
    case A_##_reg_...(A_##_reg_ + (_sz_) - 1u): \
        return stringify(_reg_)
#define CASE_REG(_reg_) \
    case A_##_reg_...(A_##_reg_ + 3u): \
        return stringify(_reg_)
#define CASE_WIDE(_reg_) \
    case A_##_reg_...(A_##_reg_ + 7u): \
        return stringify(_reg_)
#define CASE_RANGE(_reg_) \
    case A_##_reg_...(A_##_reg_ + (_reg_##_SIZE) - 1u): \
        return stringify(_reg_)

    switch (swreg) {
        CASE_RANGE(VENDOR_TEST_SCRATCH);
        CASE_WIDE(VENDOR_TEST_DIGEST);
        CASE_RANGE(CREATOR_SW_CFG_AST_CFG);
        CASE_REG(CREATOR_SW_CFG_AST_INIT_EN);
        CASE_REG(CREATOR_SW_CFG_ROM_EXT_SKU);
        CASE_REG(CREATOR_SW_CFG_SIGVERIFY_SPX_EN);
        CASE_REG(CREATOR_SW_CFG_FLASH_DATA_DEFAULT_CFG);
        CASE_REG(CREATOR_SW_CFG_FLASH_INFO_BOOT_DATA_CFG);
        CASE_REG(CREATOR_SW_CFG_FLASH_HW_INFO_CFG_OVERRIDE);
        CASE_REG(CREATOR_SW_CFG_RNG_EN);
        CASE_REG(CREATOR_SW_CFG_JITTER_EN);
        CASE_REG(CREATOR_SW_CFG_RET_RAM_RESET_MASK);
        CASE_REG(CREATOR_SW_CFG_MANUF_STATE);
        CASE_REG(CREATOR_SW_CFG_ROM_EXEC_EN);
        CASE_REG(CREATOR_SW_CFG_CPUCTRL);
        CASE_REG(CREATOR_SW_CFG_MIN_SEC_VER_ROM_EXT);
        CASE_REG(CREATOR_SW_CFG_MIN_SEC_VER_BL0);
        CASE_REG(CREATOR_SW_CFG_DEFAULT_BOOT_DATA_IN_PROD_EN);
        CASE_REG(CREATOR_SW_CFG_RMA_SPIN_EN);
        CASE_REG(CREATOR_SW_CFG_RMA_SPIN_CYCLES);
        CASE_REG(CREATOR_SW_CFG_RNG_REPCNT_THRESHOLDS);
        CASE_REG(CREATOR_SW_CFG_RNG_REPCNTS_THRESHOLDS);
        CASE_REG(CREATOR_SW_CFG_RNG_ADAPTP_HI_THRESHOLDS);
        CASE_REG(CREATOR_SW_CFG_RNG_ADAPTP_LO_THRESHOLDS);
        CASE_REG(CREATOR_SW_CFG_RNG_BUCKET_THRESHOLDS);
        CASE_REG(CREATOR_SW_CFG_RNG_MARKOV_HI_THRESHOLDS);
        CASE_REG(CREATOR_SW_CFG_RNG_MARKOV_LO_THRESHOLDS);
        CASE_REG(CREATOR_SW_CFG_RNG_EXTHT_HI_THRESHOLDS);
        CASE_REG(CREATOR_SW_CFG_RNG_EXTHT_LO_THRESHOLDS);
        CASE_REG(CREATOR_SW_CFG_RNG_ALERT_THRESHOLD);
        CASE_REG(CREATOR_SW_CFG_RNG_HEALTH_CONFIG_DIGEST);
        CASE_REG(CREATOR_SW_CFG_SRAM_KEY_RENEW_EN);
        CASE_REG(CREATOR_SW_CFG_IMMUTABLE_ROM_EXT_EN);
        CASE_REG(CREATOR_SW_CFG_IMMUTABLE_ROM_EXT_START_OFFSET);
        CASE_REG(CREATOR_SW_CFG_IMMUTABLE_ROM_EXT_LENGTH);
        CASE_RANGE(CREATOR_SW_CFG_IMMUTABLE_ROM_EXT_SHA256_HASH);
        CASE_RANGE(CREATOR_SW_CFG_RESERVED);
        CASE_WIDE(CREATOR_SW_CFG_DIGEST);
        CASE_REG(OWNER_SW_CFG_ROM_ERROR_REPORTING);
        CASE_REG(OWNER_SW_CFG_ROM_BOOTSTRAP_DIS);
        CASE_REG(OWNER_SW_CFG_ROM_ALERT_CLASS_EN);
        CASE_REG(OWNER_SW_CFG_ROM_ALERT_ESCALATION);
        CASE_RANGE(OWNER_SW_CFG_ROM_ALERT_CLASSIFICATION);
        CASE_RANGE(OWNER_SW_CFG_ROM_LOCAL_ALERT_CLASSIFICATION);
        CASE_RANGE(OWNER_SW_CFG_ROM_ALERT_ACCUM_THRESH);
        CASE_RANGE(OWNER_SW_CFG_ROM_ALERT_TIMEOUT_CYCLES);
        CASE_RANGE(OWNER_SW_CFG_ROM_ALERT_PHASE_CYCLES);
        CASE_REG(OWNER_SW_CFG_ROM_ALERT_DIGEST_PROD);
        CASE_REG(OWNER_SW_CFG_ROM_ALERT_DIGEST_PROD_END);
        CASE_REG(OWNER_SW_CFG_ROM_ALERT_DIGEST_DEV);
        CASE_REG(OWNER_SW_CFG_ROM_ALERT_DIGEST_RMA);
        CASE_REG(OWNER_SW_CFG_ROM_WATCHDOG_BITE_THRESHOLD_CYCLES);
        CASE_REG(OWNER_SW_CFG_ROM_KEYMGR_OTP_MEAS_EN);
        CASE_REG(OWNER_SW_CFG_MANUF_STATE);
        CASE_REG(OWNER_SW_CFG_ROM_RSTMGR_INFO_EN);
        CASE_REG(OWNER_SW_CFG_ROM_EXT_BOOTSTRAP_EN);
        CASE_RANGE(OWNER_SW_CFG_ROM_SENSOR_CTRL_ALERT_CFG);
        CASE_REG(OWNER_SW_CFG_ROM_SRAM_READBACK_EN);
        CASE_REG(OWNER_SW_CFG_ROM_PRESERVE_RESET_REASON_EN);
        CASE_REG(OWNER_SW_CFG_ROM_RESET_REASON_CHECK_VALUE);
        CASE_REG(OWNER_SW_CFG_ROM_BANNER_EN);
        CASE_REG(OWNER_SW_CFG_ROM_FLASH_ECC_EXC_HANDLER_EN);
        CASE_RANGE(OWNER_SW_CFG_RESERVED);
        CASE_WIDE(OWNER_SW_CFG_DIGEST);
        CASE_REG(ROT_CREATOR_AUTH_CODESIGN_ECDSA_KEY_TYPE0);
        CASE_RANGE(ROT_CREATOR_AUTH_CODESIGN_ECDSA_KEY0);
        CASE_REG(ROT_CREATOR_AUTH_CODESIGN_ECDSA_KEY_TYPE1);
        CASE_RANGE(ROT_CREATOR_AUTH_CODESIGN_ECDSA_KEY1);
        CASE_REG(ROT_CREATOR_AUTH_CODESIGN_ECDSA_KEY_TYPE2);
        CASE_RANGE(ROT_CREATOR_AUTH_CODESIGN_ECDSA_KEY2);
        CASE_REG(ROT_CREATOR_AUTH_CODESIGN_ECDSA_KEY_TYPE3);
        CASE_RANGE(ROT_CREATOR_AUTH_CODESIGN_ECDSA_KEY3);
        CASE_REG(ROT_CREATOR_AUTH_CODESIGN_SPX_KEY_TYPE0);
        CASE_RANGE(ROT_CREATOR_AUTH_CODESIGN_SPX_KEY0);
        CASE_REG(ROT_CREATOR_AUTH_CODESIGN_SPX_KEY_CONFIG0);
        CASE_REG(ROT_CREATOR_AUTH_CODESIGN_SPX_KEY_TYPE1);
        CASE_RANGE(ROT_CREATOR_AUTH_CODESIGN_SPX_KEY1);
        CASE_REG(ROT_CREATOR_AUTH_CODESIGN_SPX_KEY_CONFIG1);
        CASE_REG(ROT_CREATOR_AUTH_CODESIGN_SPX_KEY_TYPE2);
        CASE_RANGE(ROT_CREATOR_AUTH_CODESIGN_SPX_KEY2);
        CASE_REG(ROT_CREATOR_AUTH_CODESIGN_SPX_KEY_CONFIG2);
        CASE_REG(ROT_CREATOR_AUTH_CODESIGN_SPX_KEY_TYPE3);
        CASE_RANGE(ROT_CREATOR_AUTH_CODESIGN_SPX_KEY3);
        CASE_REG(ROT_CREATOR_AUTH_CODESIGN_SPX_KEY_CONFIG3);
        CASE_RANGE(ROT_CREATOR_AUTH_CODESIGN_BLOCK_SHA2_256_HASH);
        CASE_WIDE(ROT_CREATOR_AUTH_CODESIGN_DIGEST);
        CASE_REG(ROT_CREATOR_AUTH_STATE_ECDSA_KEY0);
        CASE_REG(ROT_CREATOR_AUTH_STATE_ECDSA_KEY1);
        CASE_REG(ROT_CREATOR_AUTH_STATE_ECDSA_KEY2);
        CASE_REG(ROT_CREATOR_AUTH_STATE_ECDSA_KEY3);
        CASE_REG(ROT_CREATOR_AUTH_STATE_SPX_KEY0);
        CASE_REG(ROT_CREATOR_AUTH_STATE_SPX_KEY1);
        CASE_REG(ROT_CREATOR_AUTH_STATE_SPX_KEY2);
        CASE_REG(ROT_CREATOR_AUTH_STATE_SPX_KEY3);
        CASE_WIDE(ROT_CREATOR_AUTH_STATE_DIGEST);
        CASE_RANGE(HW_CFG0_DEVICE_ID);
        CASE_RANGE(HW_CFG0_MANUF_STATE);
        CASE_WIDE(HW_CFG0_DIGEST);
        CASE_BYTE(HW_CFG1_EN_SRAM_IFETCH);
        CASE_BYTE(HW_CFG1_EN_CSRNG_SW_APP_READ);
        CASE_BYTE(HW_CFG1_DIS_RV_DM_LATE_DEBUG);
        CASE_WIDE(HW_CFG1_DIGEST);
        CASE_RANGE(SECRET0_TEST_UNLOCK_TOKEN);
        CASE_RANGE(SECRET0_TEST_EXIT_TOKEN);
        CASE_WIDE(SECRET0_DIGEST);
        CASE_RANGE(SECRET1_FLASH_ADDR_KEY_SEED);
        CASE_RANGE(SECRET1_FLASH_DATA_KEY_SEED);
        CASE_RANGE(SECRET1_SRAM_DATA_KEY_SEED);
        CASE_WIDE(SECRET1_DIGEST);
        CASE_RANGE(SECRET2_RMA_TOKEN);
        CASE_RANGE(SECRET2_CREATOR_ROOT_KEY_SHARE0);
        CASE_RANGE(SECRET2_CREATOR_ROOT_KEY_SHARE1);
        CASE_WIDE(SECRET2_DIGEST);
        CASE_RANGE(LC_TRANSITION_CNT);
        CASE_RANGE(LC_STATE);
    default:
        return "<?>";
    }

#undef CASE_BYTE
#undef CASE_SUB
#undef CASE_REG
#undef CASE_RANGE
#undef CASE_DIGEST
}

static MemTxResult ot_otp_eg_swcfg_read_with_attrs(
    void *opaque, hwaddr addr, uint64_t *data, unsigned size, MemTxAttrs attrs)
{
    OtOTPEngineState *s = OT_OTP_ENGINE(opaque);
    OtOTPEngineClass *c = OT_OTP_ENGINE_GET_CLASS(s);
    (void)size;
    (void)attrs;

    hwaddr reg = R32_OFF(addr);
    int partition = c->get_part_from_address(s, addr);

    if (partition < 0) {
        trace_ot_otp_access_error_on(s->ot_id, partition, addr, "invalid");
        *data = 0ull;

        return MEMTX_DECODE_ERROR;
    }

    unsigned part_ix = (unsigned)partition;

    if (ot_otp_engine_is_buffered(s, part_ix)) {
        trace_ot_otp_access_error_on(s->ot_id, partition, addr, "buffered");
        c->set_error(s, part_ix, OT_OTP_ACCESS_ERROR);

        /* real HW seems to stall the Tile Link bus in this case */
        return MEMTX_ACCESS_ERROR;
    }

    bool is_readable = c->is_readable(s, part_ix);
    bool is_digest = ot_otp_engine_is_part_digest_offset(s, part_ix, addr);
    bool is_zer = ot_otp_engine_is_part_zer_offset(s, part_ix, addr);

    if (!is_readable && !(is_digest || is_zer)) {
        trace_ot_otp_access_error_on(s->ot_id, partition, addr, "not readable");
        c->set_error(s, part_ix, OT_OTP_ACCESS_ERROR);

        return MEMTX_DECODE_ERROR;
    }

    uint32_t val32 = s->otp->data[reg];
    c->set_error(s, part_ix, OT_OTP_NO_ERROR);

    uint64_t pc;

    pc = ibex_get_current_pc();
    trace_ot_otp_io_swcfg_read_out(s->ot_id, (uint32_t)addr,
                                   ot_otp_eg_swcfg_reg_name(addr), val32, pc);

    *data = (uint64_t)val32;

    return MEMTX_OK;
}

static void ot_otp_eg_get_lc_info(
    const OtOTPIf *dev, uint16_t *lc_tcount, uint16_t *lc_state,
    uint8_t *lc_valid, uint8_t *secret_valid, const OtOTPTokens **tokens)
{
    const OtOTPEngineState *s = OT_OTP_ENGINE(dev);
    const OtOTPStorage *otp = s->otp;

    if (lc_tcount) {
        memcpy(lc_tcount, &otp->data[R_LC_TRANSITION_CNT],
               LC_TRANSITION_CNT_SIZE);
    }

    if (lc_state) {
        memcpy(lc_state, &otp->data[R_LC_STATE], LC_STATE_SIZE);
    }

    if (lc_valid) {
        *lc_valid = !(s->part_ctrls[OTP_PART_SECRET0].failed ||
                      s->part_ctrls[OTP_PART_SECRET2].failed ||
                      s->part_ctrls[s->part_lc_num].failed) ?
                        OT_MULTIBITBOOL_LC4_TRUE :
                        OT_MULTIBITBOOL_LC4_FALSE;
    }
    if (secret_valid) {
        *secret_valid = (!s->part_ctrls[OTP_PART_SECRET2].failed &&
                         s->part_ctrls[OTP_PART_SECRET2].locked) ?
                            OT_MULTIBITBOOL_LC4_TRUE :
                            OT_MULTIBITBOOL_LC4_FALSE;
    }
    if (tokens) {
        *tokens = s->tokens;
    }
}
static void ot_otp_eg_get_keymgr_secret(
    OtOTPIf *dev, OtOTPKeyMgrSecretType type, OtOTPKeyMgrSecret *secret)
{
    OtOTPEngineState *s = OT_OTP_ENGINE(dev);
    int partition;
    size_t offset;

    switch (type) {
    case OT_OTP_KEYMGR_SECRET_CREATOR_ROOT_KEY_SHARE0:
        partition = OTP_PART_SECRET2;
        offset =
            A_SECRET2_CREATOR_ROOT_KEY_SHARE0 - s->part_descs[partition].offset;
        break;
    case OT_OTP_KEYMGR_SECRET_CREATOR_ROOT_KEY_SHARE1:
        partition = OTP_PART_SECRET2;
        offset =
            A_SECRET2_CREATOR_ROOT_KEY_SHARE1 - s->part_descs[partition].offset;
        break;
    case OT_OTP_KEYMGR_SECRET_CREATOR_SEED:
    case OT_OTP_KEYMGR_SECRET_OWNER_SEED:
    default:
        error_report("%s: %s: invalid OTP keymgr secret type: %d", __func__,
                     s->ot_id, type);
        secret->valid = false;
        memset(secret->secret, 0, OT_OTP_KEYMGR_SECRET_SIZE);
        return;
    }

    unsigned part_ix = (unsigned)partition;
    g_assert(ot_otp_engine_is_buffered(s, part_ix));

    const uint8_t *data_ptr;
    if (s->lc_broadcast.current_level & BIT(OT_OTP_LC_SEED_HW_RD_EN)) {
        data_ptr = (const uint8_t *)s->part_ctrls[part_ix].buffer.data;
    } else {
        /* source data from PartInvDefault instead of real buffer */
        OtOTPPartController *pctrl = &s->part_ctrls[part_ix];
        data_ptr = pctrl->inv_default_data;
    }

    secret->valid = s->part_ctrls[part_ix].digest != 0;
    memcpy(secret->secret, &data_ptr[offset], OT_OTP_KEYMGR_SECRET_SIZE);
}

static void
ot_otp_eg_update_status_error(OtOTPImplIf *dev, OtOTPStatus error, bool set)
{
    const OtOTPEngineState *s = OT_OTP_ENGINE(dev);

    uint32_t mask;
    switch (error) {
    case OT_OTP_STATUS_DAI:
        mask = R_STATUS_DAI_ERROR_MASK;
        break;
    case OT_OTP_STATUS_LCI:
        mask = R_STATUS_LCI_ERROR_MASK;
        break;
    default:
        g_assert_not_reached();
        return;
    }

    if (set) {
        s->regs[R_STATUS] |= mask;
    } else {
        s->regs[R_STATUS] &= ~mask;
    }
}

static void ot_otp_eg_pwr_load_hw_cfg(OtOTPEngineState *s)
{
    OtOTPImplIfClass *ic = OT_OTP_IMPL_IF_GET_CLASS(s);

    const OtOTPPartDesc *pdesc0 = &ic->part_descs[OTP_PART_HW_CFG0];
    const OtOTPPartDesc *pdesc1 = &ic->part_descs[OTP_PART_HW_CFG1];
    const OtOTPPartController *pctrl0 = &s->part_ctrls[OTP_PART_HW_CFG0];
    const OtOTPPartController *pctrl1 = &s->part_ctrls[OTP_PART_HW_CFG1];
    const uint8_t *pdata0 = (const uint8_t *)pctrl0->buffer.data;
    const uint8_t *pdata1 = (const uint8_t *)pctrl1->buffer.data;

    OtOTPHWCfg *hw_cfg = s->hw_cfg;

    memcpy(hw_cfg->device_id, &pdata0[A_HW_CFG0_DEVICE_ID - pdesc0->offset],
           sizeof(hw_cfg->device_id));
    memcpy(hw_cfg->manuf_state, &pdata0[A_HW_CFG0_MANUF_STATE - pdesc0->offset],
           sizeof(hw_cfg->manuf_state));
    /* do not prevent execution from SRAM if no OTP configuration is loaded */
    hw_cfg->en_sram_ifetch_mb8 =
        s->blk ? pdata1[A_HW_CFG1_EN_SRAM_IFETCH - pdesc1->offset] :
                 OT_MULTIBITBOOL8_TRUE;
    /* do not prevent CSRNG app reads if no OTP configuration is loaded */
    hw_cfg->en_csrng_sw_app_read_mb8 =
        s->blk ? pdata1[A_HW_CFG1_EN_CSRNG_SW_APP_READ - pdesc1->offset] :
                 OT_MULTIBITBOOL8_TRUE;
}

static void ot_otp_eg_pwr_load_tokens(OtOTPEngineState *s)
{
    memset(s->tokens, 0, sizeof(*s->tokens));

    OtOTPTokens *tokens = s->tokens;

    static_assert(sizeof(OtOTPTokenValue) == 16u, "Invalid token size");

    for (unsigned tkx = 0; tkx < OT_OTP_TOKEN_COUNT; tkx++) {
        unsigned partition;
        uint32_t secret_addr;

        switch (tkx) {
        case OT_OTP_TOKEN_TEST_UNLOCK:
            partition = (unsigned)OTP_PART_SECRET0;
            secret_addr = A_SECRET0_TEST_UNLOCK_TOKEN;
            break;
        case OT_OTP_TOKEN_TEST_EXIT:
            partition = (unsigned)OTP_PART_SECRET0;
            secret_addr = A_SECRET0_TEST_EXIT_TOKEN;
            break;
        case OT_OTP_TOKEN_RMA:
            partition = (unsigned)OTP_PART_SECRET2;
            secret_addr = A_SECRET2_RMA_TOKEN;
            break;
        default:
            g_assert_not_reached();
            break;
        }

        OtOTPPartController *pctrl = &s->part_ctrls[partition];
        g_assert(pctrl->buffer.data != NULL);

        /* byte offset of the secret within the partition */
        unsigned secret_offset =
            secret_addr - ot_otp_engine_part_data_offset(s, partition);
        g_assert(secret_offset + sizeof(OtOTPTokenValue) <=
                 OT_OTP_PART_DESCS[partition].size);

        OtOTPTokenValue value;
        memcpy(&value, &pctrl->buffer.data[secret_offset / sizeof(uint32_t)],
               sizeof(OtOTPTokenValue));

        if (s->part_ctrls[partition].locked) {
            tokens->values[tkx] = value;
            tokens->valid_bm |= 1u << tkx;
        }
        trace_ot_otp_load_token(s->ot_id, OTP_TOKEN_NAME(tkx), tkx, value.hi,
                                value.lo,
                                (s->tokens->valid_bm & (1u << tkx)) ? "" :
                                                                      "in");
    }
}

static void ot_otp_eg_signal_pwr_sequence(OtOTPImplIf *dev)
{
    OtOTPEngineState *s = OT_OTP_ENGINE(dev);

    ot_otp_eg_pwr_load_hw_cfg(s);
    ot_otp_eg_pwr_load_tokens(s);
}

static const MemoryRegionOps ot_otp_eg_reg_ops = {
    .read = &ot_otp_eg_reg_read,
    .write = &ot_otp_eg_reg_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static const MemoryRegionOps ot_otp_eg_swcfg_ops = {
    .read_with_attrs = &ot_otp_eg_swcfg_read_with_attrs,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void ot_otp_eg_reset_enter(Object *obj, ResetType type)
{
    OtOTPEngineState *s = OT_OTP_ENGINE(obj);
    OtOTPEgClass *dc = OT_OTP_EG_GET_CLASS(obj);

    memset(s->regs, 0, REGS_SIZE);

    s->regs[R_DIRECT_ACCESS_REGWEN] = 0x1u;
    s->regs[R_CHECK_TRIGGER_REGWEN] = 0x1u;
    s->regs[R_CHECK_REGWEN] = 0x1u;
    s->regs[R_VENDOR_TEST_READ_LOCK] = 0x1u;
    s->regs[R_CREATOR_SW_CFG_READ_LOCK] = 0x1u;
    s->regs[R_OWNER_SW_CFG_READ_LOCK] = 0x1u;
    s->regs[R_ROT_CREATOR_AUTH_CODESIGN_READ_LOCK] = 0x1u;
    s->regs[R_ROT_CREATOR_AUTH_STATE_READ_LOCK] = 0x1u;

    if (dc->parent_phases.enter) {
        /* OtOTPEngineState cleanup */
        dc->parent_phases.enter(obj, type);
    }
}

static void ot_otp_eg_init(Object *obj)
{
    OtOTPEgState *s = OT_OTP_EG(obj);
    OtOTPEngineState *es = OT_OTP_ENGINE(obj);

    /* note: device realization is implemented in OtOTPEngineState */

    es->regs = g_new0(uint32_t, REGS_COUNT);
    es->reg_offset.dai_base = R_DIRECT_ACCESS_REGWEN;
    es->reg_offset.err_code_base = R_ERR_CODE_0;
    es->reg_offset.read_lock_base = R_VENDOR_TEST_READ_LOCK;

    /*
     * "ctrl" region covers two sub-regions:
     *   - "regs", registers:
     *     offset 0, size REGS_SIZE
     *   - "swcfg", software config window
     *     offset SW_CFG_WINDOW_OFFSET, size SW_CFG_WINDOW_SIZE
     */
    memory_region_init(&s->mmio.ctrl, obj, TYPE_OT_OTP_EG "-ctrl",
                       SW_CFG_WINDOW_OFFSET + SW_CFG_WINDOW_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio.ctrl);

    memory_region_init_io(&s->mmio.sub.regs, obj, &ot_otp_eg_reg_ops, s,
                          TYPE_OT_OTP_EG "-regs", REGS_SIZE);
    memory_region_add_subregion(&s->mmio.ctrl, 0u, &s->mmio.sub.regs);

    /* @todo it might be worthwhile to use a ROM-kind here */
    memory_region_init_io(&s->mmio.sub.swcfg, obj, &ot_otp_eg_swcfg_ops, s,
                          TYPE_OT_OTP_EG "-swcfg", SW_CFG_WINDOW_SIZE);
    memory_region_add_subregion(&s->mmio.ctrl, SW_CFG_WINDOW_OFFSET,
                                &s->mmio.sub.swcfg);
}

static void ot_otp_eg_class_init(ObjectClass *klass, void *data)
{
    (void)data;

    ResettableClass *rc = RESETTABLE_CLASS(klass);
    OtOTPEgClass *dc = OT_OTP_EG_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_otp_eg_reset_enter, NULL, NULL,
                                       &dc->parent_phases);

    OtOTPEngineClass *ec = OT_OTP_ENGINE_CLASS(klass);
    OtOTPIfClass *oc = OT_OTP_IF_CLASS(klass);
    oc->get_lc_info = &ot_otp_eg_get_lc_info;
    oc->get_hw_cfg = ec->get_hw_cfg;
    oc->get_otp_key = ec->get_otp_key;
    oc->get_keymgr_secret = &ot_otp_eg_get_keymgr_secret;
    oc->program_req = ec->program_req;

    OtOTPImplIfClass *ic = OT_OTP_IMPL_IF_CLASS(klass);
    ic->signal_pwr_sequence = &ot_otp_eg_signal_pwr_sequence;
    ic->update_status_error = &ot_otp_eg_update_status_error;

    ic->part_descs = OT_OTP_PART_DESCS;
    ic->part_count = (unsigned)OTP_PART_COUNT;
    ic->part_lc_num = (unsigned)OTP_PART_LIFE_CYCLE;
    ic->sram_key_req_slot_count = NUM_SRAM_KEY_REQ_SLOTS;

    ic->key_seeds = OT_OTP_KEY_SEEDS;
    ic->has_flash_support = true;
    ic->has_zer_support = false;
    for (unsigned part_ix = 0; part_ix < ic->part_count; part_ix++) {
        g_assert(!ic->part_descs[part_ix].zeroizable &&
                 ic->part_descs[part_ix].zer_offset == UINT16_MAX);
    }

    g_assert(OT_OTP_KEY_SEEDS[OT_OTP_KEY_FLASH_ADDR].size ==
             SECRET1_FLASH_ADDR_KEY_SEED_SIZE);
    g_assert(OT_OTP_KEY_SEEDS[OT_OTP_KEY_FLASH_DATA].size ==
             SECRET1_FLASH_DATA_KEY_SEED_SIZE);
    g_assert(OT_OTP_KEY_SEEDS[OT_OTP_KEY_SRAM].size ==
             SECRET1_SRAM_DATA_KEY_SEED_SIZE);
}

static const TypeInfo ot_otp_eg_info = {
    .name = TYPE_OT_OTP_EG,
    .parent = TYPE_OT_OTP_ENGINE,
    .instance_size = sizeof(OtOTPEgState),
    .instance_init = &ot_otp_eg_init,
    .class_size = sizeof(OtOTPEgClass),
    .class_init = &ot_otp_eg_class_init,
    .interfaces =
        (InterfaceInfo[]){
            { TYPE_OT_OTP_IF }, /* public OTP API */
            { TYPE_OT_OTP_IMPL_IF }, /* private OTP API for OTP engine */
            {},
        },
};

static void ot_otp_eg_register_types(void)
{
    type_register_static(&ot_otp_eg_info);
}

type_init(ot_otp_eg_register_types);
