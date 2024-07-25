/*
 * QEMU OpenTitan Darjeeling One Time Programmable (OTP) memory controller
 *
 * Copyright (c) 2023-2024 Rivos, Inc.
 *
 * Author(s):
 *  Emmanuel Blot <eblot@rivosinc.com>
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
 *
 * Constants are based on what can be extracted from
 *    https://github.com/lowRISC/opentitan-integrated/commit/eaf699f001
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_edn.h"
#include "hw/opentitan/ot_fifo32.h"
#include "hw/opentitan/ot_lc_ctrl.h"
#include "hw/opentitan/ot_otp_be_if.h"
#include "hw/opentitan/ot_otp_dj.h"
#include "hw/opentitan/ot_present.h"
#include "hw/opentitan/ot_prng.h"
#include "hw/opentitan/ot_pwrmgr.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "sysemu/block-backend.h"
#include "trace.h"

#undef OT_OTP_DEBUG

#define NUM_IRQS                2u
#define NUM_ALERTS              5u
#define NUM_SRAM_KEY_REQ_SLOTS  4u
#define NUM_ERROR_ENTRIES       24u
#define NUM_DAI_WORDS           2u
#define NUM_DIGEST_WORDS        2u
#define NUM_SW_CFG_WINDOW_WORDS 4096u
#define NUM_PART                22u
#define NUM_PART_UNBUF          15u
#define NUM_PART_BUF            7u
#define OTP_BYTE_ADDR_WIDTH     14u

/* clang-format off */
/* Core registers */
REG32(INTR_STATE, 0x0u)
    SHARED_FIELD(INTR_OTP_OPERATION_DONE, 0u, 1u)
    SHARED_FIELD(INTR_OTP_ERROR, 1u, 1u)
REG32(INTR_ENABLE, 0x4u)
REG32(INTR_TEST, 0x8u)
REG32(ALERT_TEST, 0xcu)
    SHARED_FIELD(ALERT_FATAL_MACRO_ERROR, 0u, 1u)
    SHARED_FIELD(ALERT_FATAL_CHECK_ERROR, 1u, 1u)
    SHARED_FIELD(ALERT_FATAL_BUS_INTEG_ERROR, 2u, 1u)
    SHARED_FIELD(ALERT_FATAL_PRIM_OTP_ALERT, 3u, 1u)
    SHARED_FIELD(ALERT_RECOV_PRIM_OTP_ALERT, 4u, 1u)
REG32(STATUS, 0x10u)
    FIELD(STATUS, VENDOR_TEST_ERROR, 0u, 1u)
    FIELD(STATUS, CREATOR_SW_CFG_ERROR, 1u, 1u)
    FIELD(STATUS, OWNER_SW_CFG_ERROR, 2u, 1u)
    FIELD(STATUS, OWNERSHIP_SLOT_STATE_ERROR, 3u, 1u)
    FIELD(STATUS, ROT_CREATOR_AUTH_ERROR, 4u, 1u)
    FIELD(STATUS, ROT_OWNER_AUTH_SLOT0_ERROR, 5u, 1u)
    FIELD(STATUS, ROT_OWNER_AUTH_SLOT1_ERROR, 6u, 1u)
    FIELD(STATUS, PLAT_INTEG_AUTH_SLOT0_ERROR, 7u, 1u)
    FIELD(STATUS, PLAT_INTEG_AUTH_SLOT1_ERROR, 8u, 1u)
    FIELD(STATUS, PLAT_OWNER_AUTH_SLOT0_ERROR, 9u, 1u)
    FIELD(STATUS, PLAT_OWNER_AUTH_SLOT1_ERROR, 10u, 1u)
    FIELD(STATUS, PLAT_OWNER_AUTH_SLOT2_ERROR, 11u, 1u)
    FIELD(STATUS, PLAT_OWNER_AUTH_SLOT3_ERROR, 12u, 1u)
    FIELD(STATUS, EXT_NVM_ERROR, 13u, 1u)
    FIELD(STATUS, ROM_PATCH_ERROR, 14u, 1u)
    FIELD(STATUS, HW_CFG0_ERROR, 15u, 1u)
    FIELD(STATUS, HW_CFG1_ERROR, 16u, 1u)
    FIELD(STATUS, SECRET0_ERROR, 17u, 1u)
    FIELD(STATUS, SECRET1_ERROR, 18u, 1u)
    FIELD(STATUS, SECRET2_ERROR, 19u, 1u)
    FIELD(STATUS, SECRET3_ERROR, 20u, 1u)
    FIELD(STATUS, LIFE_CYCLE_ERROR, 21u, 1u)
    FIELD(STATUS, DAI_ERROR, 22u, 1u)
    FIELD(STATUS, LCI_ERROR, 23u, 1u)
    FIELD(STATUS, TIMEOUT_ERROR, 24u, 1u)
    FIELD(STATUS, LFSR_FSM_ERROR, 25u, 1u)
    FIELD(STATUS, SCRAMBLING_FSM_ERROR, 26u, 1u)
    FIELD(STATUS, KEY_DERIV_FSM_ERROR, 27u, 1u)
    FIELD(STATUS, BUS_INTEG_ERROR, 28u, 1u)
    FIELD(STATUS, DAI_IDLE, 29u, 1u)
    FIELD(STATUS, CHECK_PENDING, 30u, 1u)
    FIELD(STATUS, RESET_ALLOWED, 31u, 1u)
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
REG32(ERR_CODE_13, 0x48u)
REG32(ERR_CODE_14, 0x4cu)
REG32(ERR_CODE_15, 0x50u)
REG32(ERR_CODE_16, 0x54u)
REG32(ERR_CODE_17, 0x58u)
REG32(ERR_CODE_18, 0x5cu)
REG32(ERR_CODE_19, 0x60u)
REG32(ERR_CODE_20, 0x64u)
REG32(ERR_CODE_21, 0x68u)
REG32(ERR_CODE_22, 0x6cu)
REG32(ERR_CODE_23, 0x70u)
REG32(DIRECT_ACCESS_REGWEN, 0x74u)
    FIELD(DIRECT_ACCESS_REGWEN, REGWEN, 0u, 1u)
REG32(DIRECT_ACCESS_CMD, 0x78u)
    FIELD(DIRECT_ACCESS_CMD, RD, 0u, 1u)
    FIELD(DIRECT_ACCESS_CMD, WR, 1u, 1u)
    FIELD(DIRECT_ACCESS_CMD, DIGEST, 2u, 1u)
REG32(DIRECT_ACCESS_ADDRESS, 0x7cu)
    FIELD(DIRECT_ACCESS_ADDRESS, ADDRESS, 0, 14u)
REG32(DIRECT_ACCESS_WDATA_0, 0x80u)
REG32(DIRECT_ACCESS_WDATA_1, 0x84u)
REG32(DIRECT_ACCESS_RDATA_0, 0x88u)
REG32(DIRECT_ACCESS_RDATA_1, 0x8cu)
REG32(CHECK_TRIGGER_REGWEN, 0x90u)
    FIELD(CHECK_TRIGGER_REGWEN, REGWEN, 0u, 1u)
REG32(CHECK_TRIGGER, 0x94u)
    FIELD(CHECK_TRIGGER, INTEGRITY, 0u, 1u)
    FIELD(CHECK_TRIGGER, CONSISTENCY, 1u, 1u)
REG32(CHECK_REGWEN, 0x98u)
    FIELD(CHECK_REGWEN, REGWEN, 0u, 1u)
REG32(CHECK_TIMEOUT, 0x9cu)
REG32(INTEGRITY_CHECK_PERIOD, 0xa0u)
REG32(CONSISTENCY_CHECK_PERIOD, 0xa4u)
REG32(VENDOR_TEST_READ_LOCK, 0xa8u)
    SHARED_FIELD(READ_LOCK,  0u, 1u)
REG32(CREATOR_SW_CFG_READ_LOCK, 0xacu)
REG32(OWNER_SW_CFG_READ_LOCK, 0xb0u)
REG32(OWNERSHIP_SLOT_STATE_READ_LOCK, 0xb4u)
REG32(ROT_CREATOR_AUTH_READ_LOCK, 0xb8u)
REG32(ROT_OWNER_AUTH_SLOT0_READ_LOCK, 0xbcu)
REG32(ROT_OWNER_AUTH_SLOT1_READ_LOCK, 0xc0u)
REG32(PLAT_INTEG_AUTH_SLOT0_READ_LOCK, 0xc4u)
REG32(PLAT_INTEG_AUTH_SLOT1_READ_LOCK, 0xc8u)
REG32(PLAT_OWNER_AUTH_SLOT0_READ_LOCK, 0xccu)
REG32(PLAT_OWNER_AUTH_SLOT1_READ_LOCK, 0xd0u)
REG32(PLAT_OWNER_AUTH_SLOT2_READ_LOCK, 0xd4u)
REG32(PLAT_OWNER_AUTH_SLOT3_READ_LOCK, 0xd8u)
REG32(EXT_NVM_READ_LOCK, 0xdcu)
REG32(ROM_PATCH_READ_LOCK, 0xe0u)
REG32(VENDOR_TEST_DIGEST_0, 0xe4u)
REG32(VENDOR_TEST_DIGEST_1, 0xe8u)
REG32(CREATOR_SW_CFG_DIGEST_0, 0xecu)
REG32(CREATOR_SW_CFG_DIGEST_1, 0xf0u)
REG32(OWNER_SW_CFG_DIGEST_0, 0xf4u)
REG32(OWNER_SW_CFG_DIGEST_1, 0xf8u)
REG32(ROT_CREATOR_AUTH_DIGEST_0, 0xfcu)
REG32(ROT_CREATOR_AUTH_DIGEST_1, 0x100u)
REG32(ROT_OWNER_AUTH_SLOT0_DIGEST_0, 0x104)
REG32(ROT_OWNER_AUTH_SLOT0_DIGEST_1, 0x108u)
REG32(ROT_OWNER_AUTH_SLOT1_DIGEST_0, 0x10cu)
REG32(ROT_OWNER_AUTH_SLOT1_DIGEST_1, 0x110u)
REG32(PLAT_INTEG_AUTH_SLOT0_DIGEST_0, 0x114u)
REG32(PLAT_INTEG_AUTH_SLOT0_DIGEST_1, 0x118u)
REG32(PLAT_INTEG_AUTH_SLOT1_DIGEST_0, 0x11cu)
REG32(PLAT_INTEG_AUTH_SLOT1_DIGEST_1, 0x120u)
REG32(PLAT_OWNER_AUTH_SLOT0_DIGEST_0, 0x124u)
REG32(PLAT_OWNER_AUTH_SLOT0_DIGEST_1, 0x128u)
REG32(PLAT_OWNER_AUTH_SLOT1_DIGEST_0, 0x12cu)
REG32(PLAT_OWNER_AUTH_SLOT1_DIGEST_1, 0x130u)
REG32(PLAT_OWNER_AUTH_SLOT2_DIGEST_0, 0x134u)
REG32(PLAT_OWNER_AUTH_SLOT2_DIGEST_1, 0x138u)
REG32(PLAT_OWNER_AUTH_SLOT3_DIGEST_0, 0x13cu)
REG32(PLAT_OWNER_AUTH_SLOT3_DIGEST_1, 0x140u)
REG32(ROM_PATCH_DIGEST_0, 0x144u)
REG32(ROM_PATCH_DIGEST_1, 0x148u)
REG32(HW_CFG0_DIGEST_0, 0x14cu)
REG32(HW_CFG0_DIGEST_1, 0x150u)
REG32(HW_CFG1_DIGEST_0, 0x154u)
REG32(HW_CFG1_DIGEST_1, 0x158u)
REG32(SECRET0_DIGEST_0, 0x15cu)
REG32(SECRET0_DIGEST_1, 0x160u)
REG32(SECRET1_DIGEST_0, 0x164u)
REG32(SECRET1_DIGEST_1, 0x168u)
REG32(SECRET2_DIGEST_0, 0x16cu)
REG32(SECRET2_DIGEST_1, 0x170u)
REG32(SECRET3_DIGEST_0, 0x174u)
REG32(SECRET3_DIGEST_1, 0x178u)
/* Software Config Window registers (at offset SW_CFG_WINDOW = +0x4000) */
REG32(VENDOR_TEST_SCRATCH, 0u)
REG32(VENDOR_TEST_DIGEST, 56u)
REG32(CREATOR_SW_CFG_DIO_ATTR, 64u)
REG32(CREATOR_SW_CFG_AST_CFG, 140u)
REG32(CREATOR_SW_CFG_AST_SPARES, 160u)
REG32(CREATOR_SW_CFG_AST_AVGSFUSECTL, 176u)
REG32(CREATOR_SW_CFG_AST_AVGSHDRCFG, 180u)
REG32(CREATOR_SW_CFG_AST_RINGOSC_TRIM_CTL, 184u)
REG32(CREATOR_SW_CFG_AST_RINGOSC_FREQ_COUNT_CTL, 188u)
REG32(CREATOR_SW_CFG_AST_RINGOSC_FREQ_TH_SLOW, 192u)
REG32(CREATOR_SW_CFG_AST_RINGOSC_FREQ_TH_FAST, 196u)
REG32(CREATOR_SW_CFG_AST_INIT_EN, 200u)
REG32(CREATOR_SW_CFG_OVERRIDES, 204u)
REG32(CREATOR_SW_CFG_ROM_EXT_SKU, 236u)
REG32(CREATOR_SW_CFG_SIGVERIFY_RSA_MOD_EXP_IBEX_EN, 240u)
REG32(CREATOR_SW_CFG_SIGVERIFY_RSA_KEY_EN, 244u)
REG32(CREATOR_SW_CFG_SIGVERIFY_SPX_EN, 252u)
REG32(CREATOR_SW_CFG_SIGVERIFY_SPX_KEY_EN, 256u)
REG32(CREATOR_SW_CFG_FLASH_DATA_DEFAULT_CFG, 264u)
REG32(CREATOR_SW_CFG_FLASH_INFO_BOOT_DATA_CFG, 268u)
REG32(CREATOR_SW_CFG_FLASH_HW_INFO_CFG_OVERRIDE, 272u)
REG32(CREATOR_SW_CFG_RNG_EN, 276u)
REG32(CREATOR_SW_CFG_JITTER_EN, 280u)
REG32(CREATOR_SW_CFG_RET_RAM_RESET_MASK, 284u)
REG32(CREATOR_SW_CFG_MANUF_STATE, 288u)
REG32(CREATOR_SW_CFG_ROM_EXEC_EN, 292u)
REG32(CREATOR_SW_CFG_CPUCTRL, 296u)
REG32(CREATOR_SW_CFG_MIN_SEC_VER_ROM_EXT, 300u)
REG32(CREATOR_SW_CFG_MIN_SEC_VER_BL0, 304u)
REG32(CREATOR_SW_CFG_DEFAULT_BOOT_DATA_IN_PROD_EN, 308u)
REG32(CREATOR_SW_CFG_RMA_SPIN_EN, 312u)
REG32(CREATOR_SW_CFG_RMA_SPIN_CYCLES, 316u)
REG32(CREATOR_SW_CFG_RNG_REPCNT_THRESHOLDS, 320u)
REG32(CREATOR_SW_CFG_RNG_REPCNTS_THRESHOLDS, 324u)
REG32(CREATOR_SW_CFG_RNG_ADAPTP_HI_THRESHOLDS, 328u)
REG32(CREATOR_SW_CFG_RNG_ADAPTP_LO_THRESHOLDS, 332u)
REG32(CREATOR_SW_CFG_RNG_BUCKET_THRESHOLDS, 336u)
REG32(CREATOR_SW_CFG_RNG_MARKOV_HI_THRESHOLDS, 340u)
REG32(CREATOR_SW_CFG_RNG_MARKOV_LO_THRESHOLDS, 344u)
REG32(CREATOR_SW_CFG_RNG_EXTHT_HI_THRESHOLDS, 348u)
REG32(CREATOR_SW_CFG_RNG_EXTHT_LO_THRESHOLDS, 352u)
REG32(CREATOR_SW_CFG_RNG_ALERT_THRESHOLD, 356u)
REG32(CREATOR_SW_CFG_RNG_HEALTH_CONFIG_DIGEST, 360u)
REG32(CREATOR_SW_CFG_SRAM_KEY_RENEW_EN, 364u)
REG32(CREATOR_SW_CFG_DIGEST, 376u)
REG32(OWNER_SW_CFG_ROM_ERROR_REPORTING, 384u)
REG32(OWNER_SW_CFG_ROM_BOOTSTRAP_DIS, 388u)
REG32(OWNER_SW_CFG_ROM_ALERT_CLASS_EN, 392u)
REG32(OWNER_SW_CFG_ROM_ALERT_ESCALATION, 396u)
REG32(OWNER_SW_CFG_ROM_ALERT_CLASSIFICATION, 400u)
REG32(OWNER_SW_CFG_ROM_LOCAL_ALERT_CLASSIFICATION, 800u)
REG32(OWNER_SW_CFG_ROM_ALERT_ACCUM_THRESH, 864u)
REG32(OWNER_SW_CFG_ROM_ALERT_TIMEOUT_CYCLES, 880u)
REG32(OWNER_SW_CFG_ROM_ALERT_PHASE_CYCLES, 896u)
REG32(OWNER_SW_CFG_ROM_ALERT_DIGEST_PROD, 960u)
REG32(OWNER_SW_CFG_ROM_ALERT_DIGEST_PROD_END, 964u)
REG32(OWNER_SW_CFG_ROM_ALERT_DIGEST_DEV, 968u)
REG32(OWNER_SW_CFG_ROM_ALERT_DIGEST_RMA, 972u)
REG32(OWNER_SW_CFG_ROM_WATCHDOG_BITE_THRESHOLD_CYCLES, 976u)
REG32(OWNER_SW_CFG_ROM_KEYMGR_ROM_EXT_MEAS_EN, 980u)
REG32(OWNER_SW_CFG_MANUF_STATE, 984u)
REG32(OWNER_SW_CFG_ROM_RSTMGR_INFO_EN, 988u)
REG32(OWNER_SW_CFG_DIGEST, 1008u)
REG32(OWNERSHIP_SLOT_STATE_ROT_OWNER_AUTH, 1016u)
REG32(OWNERSHIP_SLOT_STATE_PLAT_INTEG_AUTH, 1032u)
REG32(OWNERSHIP_SLOT_STATE_PLAT_OWNER_AUTH, 1048u)
REG32(ROT_CREATOR_AUTH_NON_RAW_MFW_CODESIGN_KEY, 1064u)
REG32(ROT_CREATOR_AUTH_ROM2_PATCH_SIGVERIFY_KEY, 1240u)
REG32(ROT_CREATOR_AUTH_KEYMANIFEST_KEY, 1416u)
REG32(ROT_CREATOR_AUTH_IDENTITY_CERT, 1592u)
REG32(ROT_CREATOR_AUTH_IDENTITY_CERT_CMAC, 2360u)
REG32(ROT_CREATOR_AUTH_DIGEST, 2480u)
REG32(ROT_OWNER_AUTH_SLOT0_KEYMANIFEST_KEY, 2488u)
REG32(ROT_OWNER_AUTH_SLOT0_UNLOCK4XFER_KEY, 2552u)
REG32(ROT_OWNER_AUTH_SLOT0_DIGEST, 2808u)
REG32(ROT_OWNER_AUTH_SLOT1_KEYMANIFEST_KEY, 2816u)
REG32(ROT_OWNER_AUTH_SLOT1_UNLOCK4XFER_KEY, 2880u)
REG32(ROT_OWNER_AUTH_SLOT1_DIGEST, 3136u)
REG32(PLAT_INTEG_AUTH_SLOT0_KEYMANIFEST_KEY, 3144u)
REG32(PLAT_INTEG_AUTH_SLOT0_UNLOCK4XFER_KEY, 3208u)
REG32(PLAT_INTEG_AUTH_SLOT0_DIGEST, 3464u)
REG32(PLAT_INTEG_AUTH_SLOT1_KEYMANIFEST_KEY, 3472u)
REG32(PLAT_INTEG_AUTH_SLOT1_UNLOCK4XFER_KEY, 3536u)
REG32(PLAT_INTEG_AUTH_SLOT1_DIGEST, 3792u)
REG32(PLAT_OWNER_AUTH_SLOT0_KEYMANIFEST_KEY, 3800u)
REG32(PLAT_OWNER_AUTH_SLOT0_UNLOCK4XFER_KEY, 3864u)
REG32(PLAT_OWNER_AUTH_SLOT0_DIGEST, 4120u)
REG32(PLAT_OWNER_AUTH_SLOT1_KEYMANIFEST_KEY, 4128u)
REG32(PLAT_OWNER_AUTH_SLOT1_UNLOCK4XFER_KEY, 4192u)
REG32(PLAT_OWNER_AUTH_SLOT1_DIGEST, 4448u)
REG32(PLAT_OWNER_AUTH_SLOT2_KEYMANIFEST_KEY, 4456u)
REG32(PLAT_OWNER_AUTH_SLOT2_UNLOCK4XFER_KEY, 4520u)
REG32(PLAT_OWNER_AUTH_SLOT2_DIGEST, 4776u)
REG32(PLAT_OWNER_AUTH_SLOT3_KEYMANIFEST_KEY, 4784u)
REG32(PLAT_OWNER_AUTH_SLOT3_UNLOCK4XFER_KEY, 4848u)
REG32(PLAT_OWNER_AUTH_SLOT3_DIGEST, 5104u)
REG32(EXT_NVM_ANTIREPLAY_FRESHNESS_CNT, 5112u)
REG32(ROM_PATCH_DATA, 6136u)
REG32(ROM_PATCH_DIGEST, 15912u)
REG32(HW_CFG0_DEVICE_ID, 15920u)
REG32(HW_CFG0_MANUF_STATE, 15952u)
REG32(HW_CFG0_DIGEST, 15984u)
REG32(HW_CFG1_SOC_DBG_STATE, 15992u)
REG32(HW_CFG1_EN_SRAM_IFETCH, 15996u)
REG32(HW_CFG1_DIGEST, 16000u)
REG32(SECRET0_TEST_UNLOCK_TOKEN, 16008u)
REG32(SECRET0_TEST_EXIT_TOKEN, 16024u)
REG32(SECRET0_DIGEST, 16040u)
REG32(SECRET1_FLASH_ADDR_KEY_SEED, 16048u)
REG32(SECRET1_FLASH_DATA_KEY_SEED, 16080u)
REG32(SECRET1_SRAM_DATA_KEY_SEED, 16112u)
REG32(SECRET1_DIGEST, 16128u)
REG32(SECRET2_RMA_TOKEN, 16136u)
REG32(SECRET2_CREATOR_ROOT_KEY_SHARE0, 16152u)
REG32(SECRET2_CREATOR_ROOT_KEY_SHARE1, 16184u)
REG32(SECRET2_CREATOR_SEED, 16216u)
REG32(SECRET2_DIGEST, 16248u)
REG32(SECRET3_OWNER_SEED, 16256u)
REG32(SECRET3_DIGEST, 16288u)
REG32(LC_TRANSITION_CNT, 16296u)
REG32(LC_STATE, 16344u)
/* clang-format on */

#define VENDOR_TEST_SCRATCH_SIZE                             56u
#define CREATOR_SW_CFG_DIO_ATTR_SIZE                         76u
#define CREATOR_SW_CFG_AST_CFG_SIZE                          20u
#define CREATOR_SW_CFG_AST_SPARES_SIZE                       14u
#define CREATOR_SW_CFG_AST_AVGSFUSECTL_SIZE                  4u
#define CREATOR_SW_CFG_AST_AVGSHDRCFG_SIZE                   3u
#define CREATOR_SW_CFG_AST_RINGOSC_TRIM_CTL_SIZE             1u
#define CREATOR_SW_CFG_AST_RINGOSC_FREQ_COUNT_CTL_SIZE       2u
#define CREATOR_SW_CFG_AST_RINGOSC_FREQ_TH_SLOW_SIZE         2u
#define CREATOR_SW_CFG_AST_RINGOSC_FREQ_TH_FAST_SIZE         2u
#define CREATOR_SW_CFG_AST_INIT_EN_SIZE                      4u
#define CREATOR_SW_CFG_OVERRIDES_SIZE                        32u
#define CREATOR_SW_CFG_ROM_EXT_SKU_SIZE                      4u
#define CREATOR_SW_CFG_SIGVERIFY_RSA_MOD_EXP_IBEX_EN_SIZE    4u
#define CREATOR_SW_CFG_SIGVERIFY_RSA_KEY_EN_SIZE             8u
#define CREATOR_SW_CFG_SIGVERIFY_SPX_EN_SIZE                 4u
#define CREATOR_SW_CFG_SIGVERIFY_SPX_KEY_EN_SIZE             8u
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
#define OWNER_SW_CFG_ROM_ERROR_REPORTING_SIZE                4u
#define OWNER_SW_CFG_ROM_BOOTSTRAP_DIS_SIZE                  4u
#define OWNER_SW_CFG_ROM_ALERT_CLASS_EN_SIZE                 4u
#define OWNER_SW_CFG_ROM_ALERT_ESCALATION_SIZE               4u
#define OWNER_SW_CFG_ROM_ALERT_CLASSIFICATION_SIZE           400u
#define OWNER_SW_CFG_ROM_LOCAL_ALERT_CLASSIFICATION_SIZE     64u
#define OWNER_SW_CFG_ROM_ALERT_ACCUM_THRESH_SIZE             16u
#define OWNER_SW_CFG_ROM_ALERT_TIMEOUT_CYCLES_SIZE           16u
#define OWNER_SW_CFG_ROM_ALERT_PHASE_CYCLES_SIZE             64u
#define OWNER_SW_CFG_ROM_WATCHDOG_BITE_THRESHOLD_CYCLES_SIZE 4u
#define OWNER_SW_CFG_ROM_KEYMGR_ROM_EXT_MEAS_EN_SIZE         4u
#define OWNER_SW_CFG_MANUF_STATE_SIZE                        4u
#define OWNER_SW_CFG_ROM_RSTMGR_INFO_EN_SIZE                 4u
#define OWNERSHIP_SLOT_STATE_ROT_OWNER_AUTH_SIZE             16u
#define OWNERSHIP_SLOT_STATE_PLAT_INTEG_AUTH_SIZE            16u
#define OWNERSHIP_SLOT_STATE_PLAT_OWNER_AUTH_SIZE            16u
#define ROT_CREATOR_AUTH_NON_RAW_MFW_CODESIGN_KEY_SIZE       176u
#define ROT_CREATOR_AUTH_ROM2_PATCH_SIGVERIFY_KEY_SIZE       176u
#define ROT_CREATOR_AUTH_KEYMANIFEST_KEY_SIZE                176u
#define ROT_CREATOR_AUTH_IDENTITY_CERT_SIZE                  768u
#define ROT_CREATOR_AUTH_IDENTITY_CERT_CMAC_SIZE             16u
#define ROT_OWNER_AUTH_SLOT0_KEYMANIFEST_KEY_SIZE            64u
#define ROT_OWNER_AUTH_SLOT0_UNLOCK4XFER_KEY_SIZE            64u
#define ROT_OWNER_AUTH_SLOT1_KEYMANIFEST_KEY_SIZE            64u
#define ROT_OWNER_AUTH_SLOT1_UNLOCK4XFER_KEY_SIZE            64u
#define PLAT_INTEG_AUTH_SLOT0_KEYMANIFEST_KEY_SIZE           64u
#define PLAT_INTEG_AUTH_SLOT0_UNLOCK4XFER_KEY_SIZE           64u
#define PLAT_INTEG_AUTH_SLOT1_KEYMANIFEST_KEY_SIZE           64u
#define PLAT_INTEG_AUTH_SLOT1_UNLOCK4XFER_KEY_SIZE           64u
#define PLAT_OWNER_AUTH_SLOT0_KEYMANIFEST_KEY_SIZE           64u
#define PLAT_OWNER_AUTH_SLOT0_UNLOCK4XFER_KEY_SIZE           64u
#define PLAT_OWNER_AUTH_SLOT1_KEYMANIFEST_KEY_SIZE           64u
#define PLAT_OWNER_AUTH_SLOT1_UNLOCK4XFER_KEY_SIZE           64u
#define PLAT_OWNER_AUTH_SLOT2_KEYMANIFEST_KEY_SIZE           64u
#define PLAT_OWNER_AUTH_SLOT2_UNLOCK4XFER_KEY_SIZE           64u
#define PLAT_OWNER_AUTH_SLOT3_KEYMANIFEST_KEY_SIZE           64u
#define PLAT_OWNER_AUTH_SLOT3_UNLOCK4XFER_KEY_SIZE           64u
#define EXT_NVM_ANTIREPLAY_FRESHNESS_CNT_SIZE                1024u
#define ROM_PATCH_DATA_SIZE                                  9192u
#define HW_CFG0_DEVICE_ID_SIZE                               32u
#define HW_CFG0_MANUF_STATE_SIZE                             32u
#define HW_CFG1_SOC_DBG_STATE_SIZE                           4u
#define HW_CFG1_EN_SRAM_IFETCH_SIZE                          1u
#define SECRET0_TEST_UNLOCK_TOKEN_SIZE                       16u
#define SECRET0_TEST_EXIT_TOKEN_SIZE                         16u
#define SECRET1_FLASH_ADDR_KEY_SEED_SIZE                     32u
#define SECRET1_FLASH_DATA_KEY_SEED_SIZE                     32u
#define SECRET1_SRAM_DATA_KEY_SEED_SIZE                      16u
#define SECRET2_RMA_TOKEN_SIZE                               16u
#define SECRET2_CREATOR_ROOT_KEY_SHARE0_SIZE                 32u
#define SECRET2_CREATOR_ROOT_KEY_SHARE1_SIZE                 32u
#define SECRET2_CREATOR_SEED_SIZE                            32u
#define SECRET3_OWNER_SEED_SIZE                              32u
#define LC_TRANSITION_CNT_SIZE                               48u
#define LC_STATE_SIZE                                        40u

#define INTR_MASK (INTR_OTP_OPERATION_DONE_MASK | INTR_OTP_ERROR_MASK)
#define ALERT_TEST_MASK \
    (ALERT_FATAL_MACRO_ERROR_MASK | ALERT_FATAL_CHECK_ERROR_MASK | \
     ALERT_FATAL_BUS_INTEG_ERROR_MASK | ALERT_FATAL_PRIM_OTP_ALERT_MASK | \
     ALERT_RECOV_PRIM_OTP_ALERT_MASK)


#define SW_CFG_WINDOW      0x4000u
#define SW_CFG_WINDOW_SIZE (NUM_SW_CFG_WINDOW_WORDS * sizeof(uint32_t))

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_SECRET3_DIGEST_1)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

/* the following delays are arbitrary for now */
#define DAI_READ_DELAY_NS   100000u /* 100us */
#define DAI_WRITE_DELAY_NS  1000000u /* 1ms */
#define DAI_DIGEST_DELAY_NS 5000000u /* 5ms */
#define LCI_PROG_DELAY_NS   500000u /* 500us*/

#define SRAM_KEY_SEED_WIDTH (SRAM_DATA_KEY_SEED_SIZE * 8u)
#define KEY_MGR_KEY_WIDTH   256u
#define SRAM_KEY_WIDTH      128u
#define SRAM_NONCE_WIDTH    128u
#define OTBN_KEY_WIDTH      128u
#define OTBN_NONCE_WIDTH    64u

#define SRAM_KEY_WORDS   ((SRAM_KEY_WIDTH) / 32u)
#define SRAM_NONCE_WORDS ((SRAM_NONCE_WIDTH) / 32u)

/* Need 128 bits of entropy to compute each 64-bit key part */
#define OTP_ENTROPY_PRESENT_BITS \
    (((NUM_SRAM_KEY_REQ_SLOTS * SRAM_KEY_WIDTH) + OTBN_KEY_WIDTH) * 128u / 64u)
#define OTP_ENTROPY_PRESENT_WORDS (OTP_ENTROPY_PRESENT_BITS / 32u)
#define OTP_ENTROPY_NONCE_BITS \
    (NUM_SRAM_KEY_REQ_SLOTS * SRAM_NONCE_WIDTH + OTBN_NONCE_WIDTH)
#define OTP_ENTROPY_NONCE_WORDS (OTP_ENTROPY_NONCE_BITS / 32u)
#define OTP_ENTROPY_BUF_COUNT \
    (OTP_ENTROPY_PRESENT_WORDS + OTP_ENTROPY_NONCE_WORDS)

typedef enum {
    OTP_PART_VENDOR_TEST,
    OTP_PART_CREATOR_SW_CFG,
    OTP_PART_OWNER_SW_CFG,
    OTP_PART_OWNERSHIP_SLOT_STATE,
    OTP_PART_ROT_CREATOR_AUTH,
    OTP_PART_ROT_OWNER_AUTH_SLOT0,
    OTP_PART_ROT_OWNER_AUTH_SLOT1,
    OTP_PART_PLAT_INTEG_AUTH_SLOT0,
    OTP_PART_PLAT_INTEG_AUTH_SLOT1,
    OTP_PART_PLAT_OWNER_AUTH_SLOT0,
    OTP_PART_PLAT_OWNER_AUTH_SLOT1,
    OTP_PART_PLAT_OWNER_AUTH_SLOT2,
    OTP_PART_PLAT_OWNER_AUTH_SLOT3,
    OTP_PART_EXT_NVM,
    OTP_PART_ROM_PATCH,
    OTP_PART_HW_CFG0,
    OTP_PART_HW_CFG1,
    OTP_PART_SECRET0,
    OTP_PART_SECRET1,
    OTP_PART_SECRET2,
    OTP_PART_SECRET3,
    OTP_PART_LIFE_CYCLE,
    OTP_PART_COUNT,
    OTP_ENTRY_DAI = OTP_PART_COUNT, /* Fake partitions for error (...) */
    OTP_ENTRY_KDI, /* Key derivation issue, not really OTP */
    OTP_ENTRY_COUNT,
} OtOTPPartitionType;

/* Error code (compliant with ERR_CODE registers) */
typedef enum {
    OTP_NO_ERROR,
    OTP_MACRO_ERROR,
    OTP_MACRO_ECC_CORR_ERROR, /* This is NOT an error */
    OTP_MACRO_ECC_UNCORR_ERROR,
    OTP_MACRO_WRITE_BLANK_ERROR,
    OTP_ACCESS_ERROR,
    OTP_CHECK_FAIL_ERROR, /* Digest error */
    OTP_FSM_STATE_ERROR,
} OtOTPError;

/* States of an unbuffered partition FSM */
typedef enum {
    OTP_UNBUF_RESET,
    OTP_UNBUF_INIT,
    OTP_UNBUF_INIT_WAIT,
    OTP_UNBUF_IDLE,
    OTP_UNBUF_READ,
    OTP_UNBUF_READ_WAIT,
    OTP_UNBUF_ERROR,
} OtOTPUnbufState;

/* States of a buffered partition FSM */
typedef enum {
    OTP_BUF_RESET,
    OTP_BUF_INIT,
    OTP_BUF_INIT_WAIT,
    OTP_BUF_INIT_DESCR,
    OTP_BUF_INIT_DESCR_WAIT,
    OTP_BUF_IDLE,
    OTP_BUF_INTEG_SCR,
    OTP_BUF_INTEG_SCR_WAIT,
    OTP_BUF_INTEG_DIG_CLR,
    OTP_BUF_INTEG_DIG,
    OTP_BUF_INTEG_DIG_PAD,
    OTP_BUF_INTEG_DIG_FIN,
    OTP_BUF_INTEG_DIG_WAIT,
    OTP_BUF_CNSTY_READ,
    OTP_BUF_CNSTY_READ_WAIT,
    OTP_BUF_ERROR,
} OtOTPBufState;

typedef enum {
    OTP_DAI_RESET,
    OTP_DAI_INIT_OTP,
    OTP_DAI_INIT_PART,
    OTP_DAI_IDLE,
    OTP_DAI_ERROR,
    OTP_DAI_READ,
    OTP_DAI_READ_WAIT,
    OTP_DAI_DESCR,
    OTP_DAI_DESCR_WAIT,
    OTP_DAI_WRITE,
    OTP_DAI_WRITE_WAIT,
    OTP_DAI_SCR,
    OTP_DAI_SCR_WAIT,
    OTP_DAI_DIG_CLR,
    OTP_DAI_DIG_READ,
    OTP_DAI_DIG_READ_WAIT,
    OTP_DAI_DIG,
    OTP_DAI_DIG_PAD,
    OTP_DAI_DIG_FIN,
    OTP_DAI_DIG_WAIT,
} OtOTPDAIState;

typedef enum {
    OTP_LCI_RESET,
    OTP_LCI_IDLE,
    OTP_LCI_WRITE,
    OTP_LCI_WRITE_WAIT,
    OTP_LCI_ERROR,
} OtOTPLCIState;

// TODO: wr and rd lock need to be rewritten (not simple boolean)

typedef struct {
    uint16_t size;
    uint16_t offset;
    uint16_t digest_offset;
    uint16_t hw_digest : 1;
    uint16_t sw_digest : 1;
    uint16_t secret : 1;
    uint16_t buffered : 1;
    uint16_t write_lock : 1;
    uint16_t read_lock : 1;
    uint16_t read_lock_csr : 1;
    uint16_t integrity : 1;
    uint16_t iskeymgr_creator : 1;
    uint16_t iskeymgr_owner : 1;
} OtOTPPartDesc;

#define OT_OTP_DJ_PARTS

#define OTP_PART_LIFE_CYCLE_SIZE 88u

/* NOLINTNEXTLINE */
#include "ot_otp_dj_parts.c"
/* NOLINTNEXTLINE */
#include "ot_otp_dj_lcvalues.c"

static_assert(OTP_PART_COUNT == NUM_PART, "Invalid partition definitions");
static_assert(OTP_PART_COUNT == OTP_PART_COUNT,
              "Invalid partition definitions");
static_assert(NUM_PART_UNBUF + NUM_PART_BUF == NUM_PART, "Invalid partitions");
static_assert(NUM_ERROR_ENTRIES == OTP_ENTRY_COUNT, "Invalid entries");
static_assert(NUM_PART <= 64, "Maximum part count reached");

#define OTP_DIGEST_ADDR_MASK (sizeof(uint64_t) - 1u)

typedef struct {
    union {
        OtOTPBufState b;
        OtOTPUnbufState u;
    } state;
    struct {
        uint32_t *data; /* size, see OtOTPPartDescs; w/o digest data */
        uint64_t digest;
        uint64_t next_digest; /* computed HW digest to store into OTP cell */
    } buffer; /* only meaningful for buffered partitions */
    bool locked;
    bool failed;
    bool read_lock;
    bool write_lock;
} OtOTPPartController;

typedef struct {
    QEMUTimer *delay; /* simulate delayed access completion */
    QEMUBH *digest_bh; /* write computed digest to OTP cell */
    OtOTPDAIState state;
    int partition; /* current partition being worked on or -1 */
} OtOTPDAIController;

typedef struct {
    QEMUTimer *prog_delay; /* OTP cell prog delay */
    QEMUBH *prog_bh; /* OTP prog trigger */
    OtOTPLCIState state;
    OtOTPError error;
    struct {
        uint32_t state;
        unsigned tcount;
    } lc;
    ot_otp_program_ack_fn ack_fn;
    void *ack_data;
    uint16_t data[OTP_PART_LIFE_CYCLE_SIZE / sizeof(uint16_t)];
    unsigned hpos; /* current offset in data */
} OtOTPLCIController;

typedef struct {
    uint32_t *storage; /* overall buffer for the storage backend */
    uint32_t *data; /* data buffer (all partitions) */
    uint32_t *ecc; /* ecc buffer for date */
    unsigned size; /* overall storage size in bytes */
    unsigned data_size; /* data buffer size in bytes */
    unsigned ecc_size; /* ecc buffer size in bytes */
    unsigned ecc_bit_count; /* count of ECC bit for each data granule */
    unsigned ecc_granule; /* size of a granule in bytes */
    uint64_t digest_iv; /* Present digest IV */
    uint8_t digest_constant[16u]; /* Present digest finalization constant */
} OtOTPStorage;

typedef struct {
    QEMUBH *bh;
    uint16_t signal; /* each bit tells if signal needs to be handled */
    uint16_t level; /* level of the matching signal */
} OtOTPLcBroadcast;

static_assert(OT_OTP_LC_BROADCAST_COUNT < 8 * sizeof(uint16_t),
              "Invalid OT_OTP_LC_BROADCAST_COUNT");

typedef struct {
    QEMUBH *entropy_bh;
    OtPresentState *present;
    OtPrngState *prng;
    OtFifo32 entropy_buf;
    bool edn_sched;
} OtOTPKeyGen;

struct OtOTPDjState {
    OtOTPState parent_obj;

    struct {
        MemoryRegion ctrl;
        struct {
            MemoryRegion regs;
            MemoryRegion swcfg;
        } sub;
    } mmio;
    QEMUBH *pwr_otp_bh;
    IbexIRQ irqs[NUM_IRQS];
    IbexIRQ alerts[NUM_ALERTS];
    IbexIRQ pwc_otp_rsp;

    uint32_t *regs;
    uint32_t alert_bm;

    OtOTPLcBroadcast lc_broadcast;
    OtOTPDAIController *dai;
    OtOTPLCIController *lci;
    OtOTPPartController *partctrls;
    OtOTPKeyGen *keygen;

    OtOTPStorage *otp;
    OtOTPHWCfg *hw_cfg;
    OtOTPTokens *tokens;

    char *ot_id;
    BlockBackend *blk; /* OTP host backend */
    OtOtpBeIf *otp_backend; /* may be NULL */
    OtEDNState *edn;
    uint8_t edn_ep;
};

typedef struct {
    uint8_t key[SRAM_KEY_WIDTH / 8u];
    uint8_t nonce[SRAM_NONCE_WIDTH / 8u];
} OtOTPScrmblKeyInit;

static const OtOTPScrmblKeyInit RND_CNST_SCRMBL_KEY_INIT = {
    .key = {
        0xceu, 0xbeu, 0xb9u, 0x6fu, 0xfeu, 0x0eu, 0xceu, 0xd7u,
        0x95u, 0xf8u, 0xb2u, 0xcfu, 0xe2u, 0x3cu, 0x1eu, 0x51u,
    },
    .nonce = {
        0x9eu, 0x4fu, 0xa0u, 0x80u, 0x47u, 0xa6u, 0xbcu, 0xfbu,
        0x81u, 0x1bu, 0x04u, 0xf0u, 0xa4u, 0x79u, 0x00u, 0x6eu,
    },
};

/* TODO: need to find real values from the RTL spaghetti */
static const uint8_t RND_CNST_DIGEST_CONST[SRAM_KEY_WIDTH / 8u] = {
    0xe0, 0x48, 0xb6, 0x57, 0x39, 0x6b, 0x4b, 0x83,
    0x27, 0x71, 0x95, 0xfc, 0x47, 0x1e, 0x4b, 0x26
};

static const uint64_t RND_CNST_DIGEST_IV = 0x4d5a89aa9109294aull;

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
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
    REG_NAME_ENTRY(ERR_CODE_13),
    REG_NAME_ENTRY(ERR_CODE_14),
    REG_NAME_ENTRY(ERR_CODE_15),
    REG_NAME_ENTRY(ERR_CODE_16),
    REG_NAME_ENTRY(ERR_CODE_17),
    REG_NAME_ENTRY(ERR_CODE_18),
    REG_NAME_ENTRY(ERR_CODE_19),
    REG_NAME_ENTRY(ERR_CODE_20),
    REG_NAME_ENTRY(ERR_CODE_21),
    REG_NAME_ENTRY(ERR_CODE_22),
    REG_NAME_ENTRY(ERR_CODE_23),
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
    REG_NAME_ENTRY(OWNERSHIP_SLOT_STATE_READ_LOCK),
    REG_NAME_ENTRY(ROT_CREATOR_AUTH_READ_LOCK),
    REG_NAME_ENTRY(ROT_OWNER_AUTH_SLOT0_READ_LOCK),
    REG_NAME_ENTRY(ROT_OWNER_AUTH_SLOT1_READ_LOCK),
    REG_NAME_ENTRY(PLAT_INTEG_AUTH_SLOT0_READ_LOCK),
    REG_NAME_ENTRY(PLAT_INTEG_AUTH_SLOT1_READ_LOCK),
    REG_NAME_ENTRY(PLAT_OWNER_AUTH_SLOT0_READ_LOCK),
    REG_NAME_ENTRY(PLAT_OWNER_AUTH_SLOT1_READ_LOCK),
    REG_NAME_ENTRY(PLAT_OWNER_AUTH_SLOT2_READ_LOCK),
    REG_NAME_ENTRY(PLAT_OWNER_AUTH_SLOT3_READ_LOCK),
    REG_NAME_ENTRY(EXT_NVM_READ_LOCK),
    REG_NAME_ENTRY(ROM_PATCH_READ_LOCK),
    REG_NAME_ENTRY(VENDOR_TEST_DIGEST_0),
    REG_NAME_ENTRY(VENDOR_TEST_DIGEST_1),
    REG_NAME_ENTRY(CREATOR_SW_CFG_DIGEST_0),
    REG_NAME_ENTRY(CREATOR_SW_CFG_DIGEST_1),
    REG_NAME_ENTRY(OWNER_SW_CFG_DIGEST_0),
    REG_NAME_ENTRY(OWNER_SW_CFG_DIGEST_1),
    REG_NAME_ENTRY(ROT_CREATOR_AUTH_DIGEST_0),
    REG_NAME_ENTRY(ROT_CREATOR_AUTH_DIGEST_1),
    REG_NAME_ENTRY(ROT_OWNER_AUTH_SLOT0_DIGEST_0),
    REG_NAME_ENTRY(ROT_OWNER_AUTH_SLOT0_DIGEST_1),
    REG_NAME_ENTRY(ROT_OWNER_AUTH_SLOT1_DIGEST_0),
    REG_NAME_ENTRY(ROT_OWNER_AUTH_SLOT1_DIGEST_1),
    REG_NAME_ENTRY(PLAT_INTEG_AUTH_SLOT0_DIGEST_0),
    REG_NAME_ENTRY(PLAT_INTEG_AUTH_SLOT0_DIGEST_1),
    REG_NAME_ENTRY(PLAT_INTEG_AUTH_SLOT1_DIGEST_0),
    REG_NAME_ENTRY(PLAT_INTEG_AUTH_SLOT1_DIGEST_1),
    REG_NAME_ENTRY(PLAT_OWNER_AUTH_SLOT0_DIGEST_0),
    REG_NAME_ENTRY(PLAT_OWNER_AUTH_SLOT0_DIGEST_1),
    REG_NAME_ENTRY(PLAT_OWNER_AUTH_SLOT1_DIGEST_0),
    REG_NAME_ENTRY(PLAT_OWNER_AUTH_SLOT1_DIGEST_1),
    REG_NAME_ENTRY(PLAT_OWNER_AUTH_SLOT2_DIGEST_0),
    REG_NAME_ENTRY(PLAT_OWNER_AUTH_SLOT2_DIGEST_1),
    REG_NAME_ENTRY(PLAT_OWNER_AUTH_SLOT3_DIGEST_0),
    REG_NAME_ENTRY(PLAT_OWNER_AUTH_SLOT3_DIGEST_1),
    REG_NAME_ENTRY(ROM_PATCH_DIGEST_0),
    REG_NAME_ENTRY(ROM_PATCH_DIGEST_1),
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
    REG_NAME_ENTRY(SECRET3_DIGEST_0),
    REG_NAME_ENTRY(SECRET3_DIGEST_1),
};
#undef REG_NAME_ENTRY

#define OTP_NAME_ENTRY(_st_) [_st_] = stringify(_st_)

static const char *DAI_STATE_NAMES[] = {
    OTP_NAME_ENTRY(OTP_DAI_RESET),      OTP_NAME_ENTRY(OTP_DAI_INIT_OTP),
    OTP_NAME_ENTRY(OTP_DAI_INIT_PART),  OTP_NAME_ENTRY(OTP_DAI_IDLE),
    OTP_NAME_ENTRY(OTP_DAI_ERROR),      OTP_NAME_ENTRY(OTP_DAI_READ),
    OTP_NAME_ENTRY(OTP_DAI_READ_WAIT),  OTP_NAME_ENTRY(OTP_DAI_DESCR),
    OTP_NAME_ENTRY(OTP_DAI_DESCR_WAIT), OTP_NAME_ENTRY(OTP_DAI_WRITE),
    OTP_NAME_ENTRY(OTP_DAI_WRITE_WAIT), OTP_NAME_ENTRY(OTP_DAI_SCR),
    OTP_NAME_ENTRY(OTP_DAI_SCR_WAIT),   OTP_NAME_ENTRY(OTP_DAI_DIG_CLR),
    OTP_NAME_ENTRY(OTP_DAI_DIG_READ),   OTP_NAME_ENTRY(OTP_DAI_DIG_READ_WAIT),
    OTP_NAME_ENTRY(OTP_DAI_DIG),        OTP_NAME_ENTRY(OTP_DAI_DIG_PAD),
    OTP_NAME_ENTRY(OTP_DAI_DIG_FIN),    OTP_NAME_ENTRY(OTP_DAI_DIG_WAIT),
};

static const char *LCI_STATE_NAMES[] = {
    OTP_NAME_ENTRY(OTP_LCI_RESET), OTP_NAME_ENTRY(OTP_LCI_IDLE),
    OTP_NAME_ENTRY(OTP_LCI_WRITE), OTP_NAME_ENTRY(OTP_LCI_WRITE_WAIT),
    OTP_NAME_ENTRY(OTP_LCI_ERROR),
};

static const char *OTP_TOKEN_NAMES[] = {
    OTP_NAME_ENTRY(OTP_TOKEN_TEST_UNLOCK),
    OTP_NAME_ENTRY(OTP_TOKEN_TEST_EXIT),
    OTP_NAME_ENTRY(OTP_TOKEN_RMA),
};

static const char *PART_NAMES[] = {
    OTP_NAME_ENTRY(OTP_PART_VENDOR_TEST),
    OTP_NAME_ENTRY(OTP_PART_CREATOR_SW_CFG),
    OTP_NAME_ENTRY(OTP_PART_OWNER_SW_CFG),
    OTP_NAME_ENTRY(OTP_PART_OWNERSHIP_SLOT_STATE),
    OTP_NAME_ENTRY(OTP_PART_ROT_CREATOR_AUTH),
    OTP_NAME_ENTRY(OTP_PART_ROT_OWNER_AUTH_SLOT0),
    OTP_NAME_ENTRY(OTP_PART_ROT_OWNER_AUTH_SLOT1),
    OTP_NAME_ENTRY(OTP_PART_PLAT_INTEG_AUTH_SLOT0),
    OTP_NAME_ENTRY(OTP_PART_PLAT_INTEG_AUTH_SLOT1),
    OTP_NAME_ENTRY(OTP_PART_PLAT_OWNER_AUTH_SLOT0),
    OTP_NAME_ENTRY(OTP_PART_PLAT_OWNER_AUTH_SLOT1),
    OTP_NAME_ENTRY(OTP_PART_PLAT_OWNER_AUTH_SLOT2),
    OTP_NAME_ENTRY(OTP_PART_PLAT_OWNER_AUTH_SLOT3),
    OTP_NAME_ENTRY(OTP_PART_EXT_NVM),
    OTP_NAME_ENTRY(OTP_PART_ROM_PATCH),
    OTP_NAME_ENTRY(OTP_PART_HW_CFG0),
    OTP_NAME_ENTRY(OTP_PART_HW_CFG1),
    OTP_NAME_ENTRY(OTP_PART_SECRET0),
    OTP_NAME_ENTRY(OTP_PART_SECRET1),
    OTP_NAME_ENTRY(OTP_PART_SECRET2),
    OTP_NAME_ENTRY(OTP_PART_SECRET3),
    OTP_NAME_ENTRY(OTP_PART_LIFE_CYCLE),
};

static const char *ERR_CODE_NAMES[] = {
    OTP_NAME_ENTRY(OTP_NO_ERROR),
    OTP_NAME_ENTRY(OTP_MACRO_ERROR),
    OTP_NAME_ENTRY(OTP_MACRO_ECC_CORR_ERROR),
    OTP_NAME_ENTRY(OTP_MACRO_ECC_UNCORR_ERROR),
    OTP_NAME_ENTRY(OTP_MACRO_WRITE_BLANK_ERROR),
    OTP_NAME_ENTRY(OTP_ACCESS_ERROR),
    OTP_NAME_ENTRY(OTP_CHECK_FAIL_ERROR),
    OTP_NAME_ENTRY(OTP_FSM_STATE_ERROR),
};
/* clang-format on */

#undef OTP_NAME_ENTRY

#define BUF_STATE_NAME(_st_) \
    ((unsigned)(_st_) < ARRAY_SIZE(BUF_STATE_NAMES) ? \
         BUF_STATE_NAMES[(_st_)] : \
         "?")
#define UNBUF_STATE_NAME(_st_) \
    ((unsigned)(_st_) < ARRAY_SIZE(UNBUF_STATE_NAMES) ? \
         UNBUF_STATE_NAMES[(_st_)] : \
         "?")
#define DAI_STATE_NAME(_st_) \
    ((unsigned)(_st_) < ARRAY_SIZE(DAI_STATE_NAMES) ? \
         DAI_STATE_NAMES[(_st_)] : \
         "?")
#define LCI_STATE_NAME(_st_) \
    ((unsigned)(_st_) < ARRAY_SIZE(LCI_STATE_NAMES) ? \
         LCI_STATE_NAMES[(_st_)] : \
         "?")
#define OTP_TOKEN_NAME(_tk_) \
    ((unsigned)(_tk_) < ARRAY_SIZE(OTP_TOKEN_NAMES) ? \
         OTP_TOKEN_NAMES[(_tk_)] : \
         "?")
#define PART_NAME(_pt_) \
    (((unsigned)(_pt_)) < ARRAY_SIZE(PART_NAMES) ? PART_NAMES[(_pt_)] : "?")
#define ERR_CODE_NAME(_err_) \
    (((unsigned)(_err_)) < ARRAY_SIZE(ERR_CODE_NAMES) ? \
         ERR_CODE_NAMES[(_err_)] : \
         "?")

static void ot_otp_dj_dai_set_error(OtOTPDjState *s, OtOTPError err);

static void
ot_otp_dj_dai_change_state_line(OtOTPDjState *s, OtOTPDAIState state, int line);

#define DAI_CHANGE_STATE(_s_, _st_) \
    ot_otp_dj_dai_change_state_line(_s_, _st_, __LINE__)

static void
ot_otp_dj_lci_change_state_line(OtOTPDjState *s, OtOTPLCIState state, int line);

#define LCI_CHANGE_STATE(_s_, _st_) \
    ot_otp_dj_lci_change_state_line(_s_, _st_, __LINE__)

#define OT_OTP_PART_DATA_OFFSET(_pix_) \
    ((unsigned)(OtOTPPartDescs[(_pix_)].offset))
#define OT_OTP_PART_DATA_BYTE_SIZE(_pix_) \
    ((unsigned)(OtOTPPartDescs[(_pix_)].size - \
                sizeof(uint32_t) * NUM_DIGEST_WORDS))

#define LC_TRANSITION_COUNT_MAX 24u
#define LC_STATE_BIT_WIDTH      5u
#define LC_ENCODE_STATE(_x_) \
    (((_x_) << (LC_STATE_BIT_WIDTH * 0)) | \
     ((_x_) << (LC_STATE_BIT_WIDTH * 1u)) | \
     ((_x_) << (LC_STATE_BIT_WIDTH * 2u)) | \
     ((_x_) << (LC_STATE_BIT_WIDTH * 3u)) | \
     ((_x_) << (LC_STATE_BIT_WIDTH * 4u)) | \
     ((_x_) << (LC_STATE_BIT_WIDTH * 5u)))
#define LC_STATE_BITS(_elc_) ((_elc_) & ((1u << LC_STATE_BIT_WIDTH) - 1u))

#ifdef OT_OTP_DEBUG
#define TRACE_OTP(msg, ...) qemu_log("%s: " msg "\n", __func__, ##__VA_ARGS__);
#else
#define TRACE_OTP(msg, ...)
#endif

#ifdef OT_OTP_DEBUG
static char hexbuf[256u];
static const char *ot_otp_hexdump(const void *data, size_t size)
{
    static const char _hex[] = "0123456789abcdef";
    const uint8_t *buf = (const uint8_t *)data;

    if (size > ((sizeof(hexbuf) / 2u) - 2u)) {
        size = sizeof(hexbuf) / 2u - 2u;
    }

    char *hexstr = hexbuf;
    for (size_t ix = 0; ix < size; ix++) {
        hexstr[(ix * 2u)] = _hex[(buf[ix] >> 4u) & 0xfu];
        hexstr[(ix * 2u) + 1u] = _hex[buf[ix] & 0xfu];
    }
    hexstr[size * 2u] = '\0';
    return hexbuf;
}
#endif

static void ot_otp_dj_update_irqs(OtOTPDjState *s)
{
    uint32_t level = s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE];

    level |= s->alert_bm;

    for (unsigned ix = 0; ix < ARRAY_SIZE(s->irqs); ix++) {
        ibex_irq_set(&s->irqs[ix], (int)((level >> ix) & 0x1));
    }
}

static void ot_otp_dj_update_alerts(OtOTPDjState *s)
{
    uint32_t level = s->regs[R_ALERT_TEST];

    for (unsigned ix = 0; ix < ARRAY_SIZE(s->alerts); ix++) {
        ibex_irq_set(&s->alerts[ix], (int)((level >> ix) & 0x1u));
    }
}

static bool ot_otp_dj_is_wide_granule(int partition, unsigned address)
{
    if ((unsigned)partition < OTP_PART_COUNT) {
        if (OtOTPPartDescs[partition].secret) {
            return true;
        }

        if (OtOTPPartDescs[partition].digest_offset ==
            (address & OTP_DIGEST_ADDR_MASK)) {
            return true;
        }
    }

    return false;
}

static bool ot_otp_dj_is_buffered(int partition)
{
    if (partition >= 0 && partition < OTP_PART_COUNT) {
        return OtOTPPartDescs[partition].buffered;
    }

    return false;
}

static bool ot_otp_dj_is_backend_ecc_enabled(const OtOTPDjState *s)
{
    if (!s->otp_backend) {
        return true;
    }

    OtOtpBeIfClass *bec = OT_OTP_BE_IF_GET_CLASS(s->otp_backend);
    if (!bec->is_ecc_enabled) {
        return true;
    }

    return bec->is_ecc_enabled(s->otp_backend);
}

static bool ot_otp_dj_is_ecc_enabled(const OtOTPDjState *s)
{
    return s->otp->ecc_granule == sizeof(uint16_t) &&
           ot_otp_dj_is_backend_ecc_enabled(s);
}

static bool ot_otp_dj_has_digest(unsigned partition)
{
    return OtOTPPartDescs[partition].hw_digest ||
           OtOTPPartDescs[partition].sw_digest;
}

static void ot_otp_dj_set_error(OtOTPDjState *s, unsigned part, OtOTPError err)
{
    /* This is it NUM_ERROR_ENTRIES */
    g_assert(part < NUM_ERROR_ENTRIES);

    uint32_t errval = ((uint32_t)err) & 0x7;
    if (errval || errval != s->regs[R_ERR_CODE_0 + part]) {
        trace_ot_otp_set_error(s->ot_id, PART_NAME(part), part,
                               ERR_CODE_NAME(err), err);
    }
    s->regs[R_ERR_CODE_0 + part] = errval;

    switch (err) {
    case OTP_MACRO_ERROR:
    case OTP_MACRO_ECC_UNCORR_ERROR:
        s->alert_bm |= ALERT_FATAL_MACRO_ERROR_MASK;
        ot_otp_dj_update_alerts(s);
        break;
    /* NOLINTNEXTLINE */
    case OTP_MACRO_ECC_CORR_ERROR:
        /*
         * "The corresponding controller automatically recovers from this error
         *  when issuing a new command."
         */
        break;
    case OTP_MACRO_WRITE_BLANK_ERROR:
        break;
    case OTP_ACCESS_ERROR:
        s->regs[R_STATUS] |= R_STATUS_DAI_ERROR_MASK;
        break;
    case OTP_CHECK_FAIL_ERROR:
    case OTP_FSM_STATE_ERROR:
        s->alert_bm |= ALERT_FATAL_CHECK_ERROR_MASK;
        ot_otp_dj_update_alerts(s);
        break;
    default:
        break;
    }

    if (err != OTP_NO_ERROR) {
        s->regs[R_INTR_STATE] |= INTR_OTP_ERROR_MASK;
        ot_otp_dj_update_irqs(s);
    }
}

static uint32_t ot_otp_dj_dai_is_busy(const OtOTPDjState *s)
{
    return s->dai->state != OTP_DAI_IDLE;
}

static uint32_t ot_otp_dj_get_status(const OtOTPDjState *s)
{
    uint32_t status;

    status = FIELD_DP32(s->regs[R_STATUS], STATUS, DAI_IDLE,
                        !ot_otp_dj_dai_is_busy(s));
    status = FIELD_DP32(status, STATUS, RESET_ALLOWED, 1u);

    return status;
}

static int ot_otp_dj_get_part_from_address(const OtOTPDjState *s, hwaddr addr)
{
    for (unsigned ix = 0; ix < OTP_PART_COUNT; ix++) {
        const OtOTPPartDesc *part = &OtOTPPartDescs[ix];
        if ((addr >= part->offset) &&
            ((addr + sizeof(uint32_t)) <= (part->offset + part->size))) {
            trace_ot_otp_addr_to_part(s->ot_id, (uint32_t)addr, PART_NAME(ix),
                                      ix);
            return (OtOTPPartitionType)ix;
        }
    }

    return -1;
}

static uint16_t ot_otp_dj_get_part_digest_offset(int part)
{
    switch (part) {
    case OTP_PART_VENDOR_TEST:
    case OTP_PART_CREATOR_SW_CFG:
    case OTP_PART_OWNER_SW_CFG:
    case OTP_PART_OWNERSHIP_SLOT_STATE:
    case OTP_PART_ROT_CREATOR_AUTH:
    case OTP_PART_ROT_OWNER_AUTH_SLOT0:
    case OTP_PART_ROT_OWNER_AUTH_SLOT1:
    case OTP_PART_PLAT_INTEG_AUTH_SLOT0:
    case OTP_PART_PLAT_INTEG_AUTH_SLOT1:
    case OTP_PART_PLAT_OWNER_AUTH_SLOT0:
    case OTP_PART_PLAT_OWNER_AUTH_SLOT1:
    case OTP_PART_PLAT_OWNER_AUTH_SLOT2:
    case OTP_PART_PLAT_OWNER_AUTH_SLOT3:
    case OTP_PART_EXT_NVM:
    case OTP_PART_ROM_PATCH:
    case OTP_PART_HW_CFG0:
    case OTP_PART_HW_CFG1:
    case OTP_PART_SECRET0:
    case OTP_PART_SECRET1:
    case OTP_PART_SECRET2:
    case OTP_PART_SECRET3:
    case OTP_PART_LIFE_CYCLE:
        return OtOTPPartDescs[part].digest_offset;
    default:
        return UINT16_MAX;
    }
}

static uint8_t ot_otp_dj_compute_ecc_u16(uint16_t data)
{
    uint32_t data_o = (uint32_t)data;

    data_o |= __builtin_parity(data_o & 0x00ad5bu) << 16u;
    data_o |= __builtin_parity(data_o & 0x00366du) << 17u;
    data_o |= __builtin_parity(data_o & 0x00c78eu) << 18u;
    data_o |= __builtin_parity(data_o & 0x0007f0u) << 19u;
    data_o |= __builtin_parity(data_o & 0x00f800u) << 20u;
    data_o |= __builtin_parity(data_o & 0x1fffffu) << 21u;

    return (uint8_t)(data_o >> 16u);
}

static uint16_t ot_otp_dj_compute_ecc_u32(uint32_t data)
{
    uint16_t data_lo = (uint16_t)(data & UINT16_MAX);
    uint16_t data_hi = (uint16_t)(data >> 16u);

    uint16_t ecc_lo = (uint16_t)ot_otp_dj_compute_ecc_u16(data_lo);
    uint16_t ecc_hi = (uint16_t)ot_otp_dj_compute_ecc_u16(data_hi);

    return (ecc_hi << 8u) | ecc_lo;
}

static uint32_t ot_otp_dj_compute_ecc_u64(uint64_t data)
{
    uint32_t data_lo = (uint32_t)(data & UINT32_MAX);
    uint32_t data_hi = (uint32_t)(data >> 32u);

    uint32_t ecc_lo = (uint32_t)ot_otp_dj_compute_ecc_u32(data_lo);
    uint32_t ecc_hi = (uint32_t)ot_otp_dj_compute_ecc_u32(data_hi);

    return (ecc_hi << 16u) | ecc_lo;
}

static uint32_t ot_otp_dj_verify_ecc_22_16_u16(const OtOTPDjState *s,
                                               uint32_t data_i, unsigned *err_o)
{
    unsigned syndrome = 0u;

    syndrome |= __builtin_parity(data_i & 0x01ad5bu) << 0u;
    syndrome |= __builtin_parity(data_i & 0x02366du) << 1u;
    syndrome |= __builtin_parity(data_i & 0x04c78eu) << 2u;
    syndrome |= __builtin_parity(data_i & 0x0807f0u) << 3u;
    syndrome |= __builtin_parity(data_i & 0x10f800u) << 4u;
    syndrome |= __builtin_parity(data_i & 0x3fffffu) << 5u;

    unsigned err = (syndrome >> 5u) & 1u;
    if (!err && (syndrome & 0x1fu)) {
        err = 2u;
    }

    *err_o = err;

    if (!err) {
        return data_i & UINT16_MAX;
    }

    uint32_t data_o = 0;

#define OTP_ECC_RECOVER(_sy_, _di_, _ix_) \
    ((unsigned)((syndrome == (_sy_)) ^ (bool)((_di_) & (1u << (_ix_)))) \
     << (_ix_))

    data_o |= OTP_ECC_RECOVER(0x23u, data_i, 0u);
    data_o |= OTP_ECC_RECOVER(0x25u, data_i, 1u);
    data_o |= OTP_ECC_RECOVER(0x26u, data_i, 2u);
    data_o |= OTP_ECC_RECOVER(0x27u, data_i, 3u);
    data_o |= OTP_ECC_RECOVER(0x29u, data_i, 4u);
    data_o |= OTP_ECC_RECOVER(0x2au, data_i, 5u);
    data_o |= OTP_ECC_RECOVER(0x2bu, data_i, 6u);
    data_o |= OTP_ECC_RECOVER(0x2cu, data_i, 7u);
    data_o |= OTP_ECC_RECOVER(0x2du, data_i, 8u);
    data_o |= OTP_ECC_RECOVER(0x2eu, data_i, 9u);
    data_o |= OTP_ECC_RECOVER(0x2fu, data_i, 10u);
    data_o |= OTP_ECC_RECOVER(0x31u, data_i, 11u);
    data_o |= OTP_ECC_RECOVER(0x32u, data_i, 12u);
    data_o |= OTP_ECC_RECOVER(0x33u, data_i, 13u);
    data_o |= OTP_ECC_RECOVER(0x34u, data_i, 14u);
    data_o |= OTP_ECC_RECOVER(0x35u, data_i, 15u);

#undef OTP_ECC_RECOVER

    if (err > 1u) {
        trace_ot_otp_ecc_unrecoverable_error(s->ot_id, data_i & UINT16_MAX);
    } else {
        if ((data_i & UINT16_MAX) != data_o) {
            trace_ot_otp_ecc_recovered_error(s->ot_id, data_i & UINT16_MAX,
                                             data_o);
        } else {
            /* ECC bit is corrupted */
            trace_ot_otp_ecc_parity_error(s->ot_id, data_i & UINT16_MAX,
                                          data_i >> 16u);
        }
    }

    return data_o;
}

static uint32_t ot_otp_dj_verify_ecc(const OtOTPDjState *s, uint32_t data,
                                     uint32_t ecc, unsigned *err)
{
    uint32_t data_lo_i, data_lo_o, data_hi_i, data_hi_o;
    unsigned err_lo, err_hi;

    data_lo_i = (data & 0xffffu) | ((ecc & 0xffu) << 16u);
    data_lo_o = ot_otp_dj_verify_ecc_22_16_u16(s, data_lo_i, &err_lo);

    data_hi_i = (data >> 16u) | (((ecc >> 8u) & 0xffu) << 16u);
    data_hi_o = ot_otp_dj_verify_ecc_22_16_u16(s, data_hi_i, &err_hi);

    *err |= err_lo | err_hi;

    return (data_hi_o << 16u) | data_lo_o;
}

static uint64_t ot_otd_dj_verify_digest(OtOTPDjState *s, unsigned partition,
                                        uint64_t digest, uint32_t ecc)
{
    uint32_t dig_lo = (uint32_t)(digest & UINT32_MAX);
    uint32_t dig_hi = (uint32_t)(digest >> 32u);

    unsigned err = 0;
    if (ot_otp_dj_is_ecc_enabled(s)) {
        dig_lo = ot_otp_dj_verify_ecc(s, dig_lo, ecc & 0xffffu, &err);
        dig_hi = ot_otp_dj_verify_ecc(s, dig_hi, ecc >> 16u, &err);
    }

    digest = (((uint64_t)dig_hi) << 32u) | ((uint64_t)dig_lo);

    if (err) {
        OtOTPError otp_err =
            (err > 1) ? OTP_MACRO_ECC_UNCORR_ERROR : OTP_MACRO_ECC_CORR_ERROR;
        // Note: need to check if any caller could override the error/state
        // in this case
        ot_otp_dj_set_error(s, partition, otp_err);
    }

    return digest;
}

static int ot_otp_dj_apply_ecc(OtOTPDjState *s, unsigned partition)
{
    g_assert(ot_otp_dj_is_ecc_enabled(s));

    unsigned start = OtOTPPartDescs[partition].offset >> 2u;
    unsigned end =
        (ot_otp_dj_is_buffered((int)partition) &&
         ot_otp_dj_has_digest(partition)) ?
            (unsigned)(OtOTPPartDescs[partition].digest_offset >> 2u) :
            start + (unsigned)(OtOTPPartDescs[partition].size >> 2u);

    g_assert(start < end && (end / sizeof(uint32_t)) < s->otp->data_size);
    for (unsigned ix = start; ix < end; ix++) {
        unsigned err = 0;
        uint32_t *word = &s->otp->data[ix];
        uint16_t ecc = ((const uint16_t *)s->otp->ecc)[ix];
        *word = ot_otp_dj_verify_ecc(s, *word, (uint32_t)ecc, &err);
        if (err) {
            OtOTPError otp_err = (err > 1) ? OTP_MACRO_ECC_UNCORR_ERROR :
                                             OTP_MACRO_ECC_CORR_ERROR;
            // Note: need to check if any caller could override the error/state
            // in this case
            ot_otp_dj_set_error(s, partition, otp_err);
            if (err > 1) {
                trace_ot_otp_ecc_init_error(s->ot_id, PART_NAME(partition),
                                            partition, ix << 2u, *word, ecc);
                s->partctrls[partition].failed = true;
                return -1;
            }
        }
    }

    return 0;
}

static uint64_t ot_otp_dj_get_part_digest(OtOTPDjState *s, int part)
{
    g_assert(!ot_otp_dj_is_buffered(part));

    uint16_t offset = ot_otp_dj_get_part_digest_offset(part);

    if (offset == UINT16_MAX) {
        return 0u;
    }

    const uint8_t *data = (const uint8_t *)s->otp->data;
    uint64_t digest = ldq_le_p(data + offset);

    if (part != OTP_PART_VENDOR_TEST && ot_otp_dj_is_ecc_enabled(s)) {
        unsigned waddr = offset >> 2u;
        unsigned ewaddr = waddr >> 1u;
        g_assert(ewaddr < s->otp->ecc_size);
        uint32_t ecc = s->otp->ecc[ewaddr];
        digest = ot_otd_dj_verify_digest(s, (unsigned)part, digest, ecc);
    }

    return digest;
}

static uint64_t ot_otp_dj_get_buffered_part_digest(OtOTPDjState *s, int part)
{
    g_assert(ot_otp_dj_is_buffered(part));

    OtOTPPartController *pctrl = &s->partctrls[part];

    return pctrl->buffer.digest;
}

static bool ot_otp_dj_is_part_digest_offset(int part, hwaddr addr)
{
    uint16_t offset = ot_otp_dj_get_part_digest_offset(part);

    return (offset != UINT16_MAX) && ((addr & ~OTP_DIGEST_ADDR_MASK) == offset);
}

static bool ot_otp_dj_is_readable(OtOTPDjState *s, int partition)
{
    if (OtOTPPartDescs[partition].secret) {
        /* secret partitions are only readable if digest is not yet set. */
        return ot_otp_dj_get_buffered_part_digest(s, partition) == 0u;
    }

    uint32_t reg;

    switch (partition) {
    case OTP_PART_VENDOR_TEST:
        reg = R_VENDOR_TEST_READ_LOCK;
        break;
    case OTP_PART_CREATOR_SW_CFG:
        reg = R_CREATOR_SW_CFG_READ_LOCK;
        break;
    case OTP_PART_OWNER_SW_CFG:
        reg = R_OWNER_SW_CFG_READ_LOCK;
        break;
    case OTP_PART_OWNERSHIP_SLOT_STATE:
        reg = R_OWNERSHIP_SLOT_STATE_READ_LOCK;
        break;
    case OTP_PART_ROT_CREATOR_AUTH:
        reg = R_ROT_CREATOR_AUTH_READ_LOCK;
        break;
    case OTP_PART_ROT_OWNER_AUTH_SLOT0:
        reg = R_ROT_OWNER_AUTH_SLOT0_READ_LOCK;
        break;
    case OTP_PART_ROT_OWNER_AUTH_SLOT1:
        reg = R_ROT_OWNER_AUTH_SLOT1_READ_LOCK;
        break;
    case OTP_PART_PLAT_INTEG_AUTH_SLOT0:
        reg = R_PLAT_INTEG_AUTH_SLOT0_READ_LOCK;
        break;
    case OTP_PART_PLAT_INTEG_AUTH_SLOT1:
        reg = R_PLAT_INTEG_AUTH_SLOT1_READ_LOCK;
        break;
    case OTP_PART_PLAT_OWNER_AUTH_SLOT0:
        reg = R_PLAT_OWNER_AUTH_SLOT0_READ_LOCK;
        break;
    case OTP_PART_PLAT_OWNER_AUTH_SLOT1:
        reg = R_PLAT_OWNER_AUTH_SLOT1_READ_LOCK;
        break;
    case OTP_PART_PLAT_OWNER_AUTH_SLOT2:
        reg = R_PLAT_OWNER_AUTH_SLOT2_READ_LOCK;
        break;
    case OTP_PART_PLAT_OWNER_AUTH_SLOT3:
        reg = R_PLAT_OWNER_AUTH_SLOT3_READ_LOCK;
        break;
    case OTP_PART_EXT_NVM:
        reg = R_EXT_NVM_READ_LOCK;
        break;
    case OTP_PART_ROM_PATCH:
        reg = R_ROM_PATCH_READ_LOCK;
        break;
    case OTP_PART_HW_CFG0:
    case OTP_PART_HW_CFG1:
    case OTP_PART_SECRET0:
    case OTP_PART_SECRET1:
    case OTP_PART_SECRET2:
    case OTP_PART_SECRET3:
        reg = UINT32_MAX;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid partition: %d\n", __func__,
                      partition);
        return false;
    }

    if (OtOTPPartDescs[partition].read_lock_csr) {
        if (reg == UINT32_MAX) {
            error_setg(&error_fatal, "CSR register not defined");
            g_assert_not_reached();
        }
        return (bool)SHARED_FIELD_EX32(s->regs[reg], READ_LOCK);
    }

    if (reg != UINT32_MAX) {
        error_setg(&error_fatal, "Unexpected CSR register");
        g_assert_not_reached();
    }

    if (!OtOTPPartDescs[partition].read_lock) {
        /* read lock is not supported for this partition */
        return true;
    }

    /* hw read lock, not locked */
    return !s->partctrls[partition].read_lock;
}

static void
ot_otp_dj_dai_change_state_line(OtOTPDjState *s, OtOTPDAIState state, int line)
{
    trace_ot_otp_dai_change_state(s->ot_id, line, DAI_STATE_NAME(s->dai->state),
                                  s->dai->state, DAI_STATE_NAME(state), state);

    s->dai->state = state;
}

static void
ot_otp_dj_lci_change_state_line(OtOTPDjState *s, OtOTPLCIState state, int line)
{
    trace_ot_otp_lci_change_state(s->ot_id, line, LCI_STATE_NAME(s->lci->state),
                                  s->lci->state, LCI_STATE_NAME(state), state);

    s->lci->state = state;
}

static void ot_otp_dj_lc_broadcast_recv(void *opaque, int n, int level)
{
    OtOTPDjState *s = opaque;
    OtOTPLcBroadcast *bcast = &s->lc_broadcast;

    g_assert((unsigned)n < OT_OTP_LC_BROADCAST_COUNT);

    uint16_t bit = 1u << (unsigned)n;
    bcast->signal |= bit;
    /*
     * as these signals are only used to change permissions, it is valid to
     * override a signal value that has not been processed yet
     */
    if (level) {
        bcast->level |= bit;
    } else {
        bcast->level &= ~bit;
    }

    /* use a BH to decouple IRQ signaling from actual handling */
    qemu_bh_schedule(s->lc_broadcast.bh);
}

static void ot_otp_dj_lc_broadcast_bh(void *opaque)
{
    OtOTPDjState *s = opaque;
    OtOTPLcBroadcast *bcast = &s->lc_broadcast;

    /* handle all flagged signals */
    while (bcast->signal) {
        /* pick and clear */
        unsigned sig = ctz16(bcast->signal);
        uint16_t bit = 1u << (unsigned)sig;
        bcast->signal &= ~bit;
        bool level = (bool)(bcast->level & bit);

        trace_ot_otp_lc_broadcast(s->ot_id, sig, level);

        switch ((int)sig) {
        case OT_OTP_LC_DFT_EN:
            qemu_log_mask(LOG_UNIMP, "%s: DFT feature not supported\n",
                          __func__);
            break;
        case OT_OTP_LC_ESCALATE_EN:
            if (level) {
                DAI_CHANGE_STATE(s, OTP_DAI_ERROR);
                LCI_CHANGE_STATE(s, OTP_LCI_ERROR);
                // TODO: manage other FSMs
                qemu_log_mask(LOG_UNIMP, "%s: ESCALATE partially implemented\n",
                              __func__);
            }
            break;
        case OT_OTP_LC_CHECK_BYP_EN:
            qemu_log_mask(LOG_UNIMP, "%s: Bypass is ignored\n", __func__);
            break;
        case OT_OTP_LC_CREATOR_SEED_SW_RW_EN:
            for (unsigned ix = 0; ix < OTP_PART_COUNT; ix++) {
                if (OtOTPPartDescs[OTP_PART_SECRET2].iskeymgr_creator) {
                    s->partctrls[OTP_PART_SECRET2].read_lock = !level;
                    s->partctrls[OTP_PART_SECRET2].write_lock = !level;
                }
            }
            break;
        case OT_OTP_LC_OWNER_SEED_SW_RW_EN:
            for (unsigned ix = 0; ix < OTP_PART_COUNT; ix++) {
                if (OtOTPPartDescs[OTP_PART_SECRET2].iskeymgr_owner) {
                    s->partctrls[OTP_PART_SECRET2].read_lock = !level;
                    s->partctrls[OTP_PART_SECRET2].write_lock = !level;
                }
            }
            break;
        case OT_OTP_LC_SEED_HW_RD_EN:
            qemu_log_mask(LOG_UNIMP, "%s: Seed HW read is ignored\n", __func__);
            break;
        default:
            error_setg(&error_fatal, "%s: unexpected LC broadcast %d\n",
                       __func__, sig);
            g_assert_not_reached();
            break;
        }
    }
}

static uint64_t ot_otp_dj_compute_partition_digest(
    OtOTPDjState *s, const uint8_t *base, unsigned size)
{
    OtPresentState *ps = ot_present_new();

    uint8_t buf[sizeof(uint64_t) * 2u];
    uint64_t state = s->otp->digest_iv;
    uint64_t out;
    for (unsigned off = 0; off < size; off += sizeof(buf)) {
        const uint8_t *chunk;
        if (off + sizeof(buf) > size) {
            memcpy(buf, base + off, sizeof(uint64_t));
            memcpy(&buf[sizeof(uint64_t)], base + off + sizeof(uint64_t),
                   sizeof(uint64_t));
            chunk = buf;
        } else {
            chunk = base + off;
        }

        ot_present_init(ps, chunk);
        ot_present_encrypt(ps, state, &out);
        state ^= out;
    }

    ot_present_init(ps, s->otp->digest_constant);
    ot_present_encrypt(ps, state, &out);
    state ^= out;

    ot_present_free(ps);

    return state;
}

static uint64_t
ot_otp_dj_load_partition_digest(OtOTPDjState *s, unsigned partition)
{
    unsigned digoff = (unsigned)OtOTPPartDescs[partition].digest_offset;

    if ((digoff + sizeof(uint64_t)) > s->otp->data_size) {
        error_setg(&error_fatal, "Partition located outside storage?\n");
        /* linter doest not know the above call never returns */
        return 0u;
    }

    const uint8_t *data = (const uint8_t *)s->otp->data;
    uint64_t digest = ldq_le_p(data + digoff);

    if (ot_otp_dj_is_ecc_enabled(s)) {
        unsigned ewaddr = (digoff >> 3u);
        g_assert(ewaddr < s->otp->ecc_size);
        uint32_t ecc = s->otp->ecc[ewaddr];
        digest = ot_otd_dj_verify_digest(s, partition, digest, ecc);
    }

    return digest;
}

static void ot_otp_dj_bufferize_partition(OtOTPDjState *s, unsigned ix)
{
    OtOTPPartController *pctrl = &s->partctrls[ix];

    g_assert(pctrl->buffer.data != NULL);

    if (OtOTPPartDescs[ix].hw_digest) {
        pctrl->buffer.digest = ot_otp_dj_load_partition_digest(s, ix);
    } else {
        pctrl->buffer.digest = 0;
    }

    unsigned offset = (unsigned)OtOTPPartDescs[ix].offset;
    unsigned part_size = OT_OTP_PART_DATA_BYTE_SIZE(ix);

    const uint8_t *base = (const uint8_t *)s->otp->data;
    base += offset;

    memcpy(pctrl->buffer.data, base, part_size);
}

static void ot_otp_dj_check_partition_integrity(OtOTPDjState *s, unsigned ix)
{
    OtOTPPartController *pctrl = &s->partctrls[ix];

    if (!OtOTPPartDescs[ix].hw_digest || pctrl->buffer.digest == 0) {
        trace_ot_otp_skip_digest(s->ot_id, PART_NAME(ix), ix);
        s->partctrls[ix].locked = false;
        return;
    }

    pctrl->locked = true;

    unsigned part_size = OT_OTP_PART_DATA_BYTE_SIZE(ix);
    uint64_t digest =
        ot_otp_dj_compute_partition_digest(s,
                                           (const uint8_t *)pctrl->buffer.data,
                                           part_size);

    if (digest != pctrl->buffer.digest) {
        trace_ot_otp_mismatch_digest(s->ot_id, PART_NAME(ix), ix, digest,
                                     pctrl->buffer.digest);

        TRACE_OTP("%s: compute digest %016llx from %s\n", __func__, digest,
                  ot_otp_hexdump(pctrl->buffer.data, part_size));

        pctrl->failed = true;
        /* this is a fatal error */
        ot_otp_dj_set_error(s, ix, OTP_CHECK_FAIL_ERROR);
        /* TODO: revert buffered part to default */
    } else {
        trace_ot_otp_integrity_report(s->ot_id, PART_NAME(ix), ix, "digest OK");
        pctrl->failed = false;
    }
}

static void ot_otp_dj_initialize_partitions(OtOTPDjState *s)
{
    for (unsigned ix = 0; ix < OTP_PART_COUNT; ix++) {
        if (ot_otp_dj_is_ecc_enabled(s) && ix != OTP_PART_VENDOR_TEST) {
            if (ot_otp_dj_apply_ecc(s, ix)) {
                continue;
            }
        }

        if (OtOTPPartDescs[ix].sw_digest) {
            uint64_t digest = ot_otp_dj_get_part_digest(s, (int)ix);
            s->partctrls[ix].locked = digest != 0;
            continue;
        }

        if (OtOTPPartDescs[ix].buffered) {
            ot_otp_dj_bufferize_partition(s, ix);
            ot_otp_dj_check_partition_integrity(s, ix);
            continue;
        }
    }
}

static bool ot_otp_dj_is_backend_writable(OtOTPDjState *s)
{
    return (s->blk != NULL) && blk_is_writable(s->blk);
}

static inline int ot_otp_dj_write_backend(OtOTPDjState *s, const void *buffer,
                                          unsigned offset, size_t size)
{
    /*
     * the blk_pwrite API is awful, isolate it so that linter exceptions are
     * are not repeated over and over
     */
    // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
    return blk_pwrite(s->blk, (int64_t)(intptr_t)offset, (int64_t)size, buffer,
                      /* a bitfield of enum is not an enum item */
                      (BdrvRequestFlags)0);
    // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
}

static void ot_otp_dj_dai_init(OtOTPDjState *s)
{
    DAI_CHANGE_STATE(s, OTP_DAI_IDLE);
}

static void ot_otp_dj_dai_set_error(OtOTPDjState *s, OtOTPError err)
{
    ot_otp_dj_set_error(s, OTP_ENTRY_DAI, err);

    switch (err) {
    case OTP_FSM_STATE_ERROR:
    case OTP_MACRO_ERROR:
    case OTP_MACRO_ECC_UNCORR_ERROR:
        DAI_CHANGE_STATE(s, OTP_DAI_ERROR);
        break;
    default:
        DAI_CHANGE_STATE(s, OTP_DAI_IDLE);
        break;
    }
}

static void ot_otp_dj_dai_clear_error(OtOTPDjState *s)
{
    s->regs[R_STATUS] &= ~R_STATUS_DAI_ERROR_MASK;
    s->regs[R_ERR_CODE_0 + OTP_ENTRY_DAI] = 0;
}

static void ot_otp_dj_dai_read(OtOTPDjState *s)
{
    if (ot_otp_dj_dai_is_busy(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: DAI controller busy\n", __func__);
        return;
    }

    ot_otp_dj_dai_clear_error(s);

    DAI_CHANGE_STATE(s, OTP_DAI_READ);

    unsigned address = s->regs[R_DIRECT_ACCESS_ADDRESS];

    int partition = ot_otp_dj_get_part_from_address(s, address);

    if (partition < 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Invalid partition address 0x%x\n",
                      __func__, address);
        ot_otp_dj_dai_set_error(s, OTP_ACCESS_ERROR);
        return;
    }

    if (partition >= OTP_PART_LIFE_CYCLE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Life cycle partition cannot be accessed from DAI\n",
                      __func__);
        ot_otp_dj_dai_set_error(s, OTP_ACCESS_ERROR);
        return;
    }

    bool is_digest = ot_otp_dj_is_part_digest_offset(partition, address);
    bool is_readable = ot_otp_dj_is_readable(s, partition);
    bool is_wide = ot_otp_dj_is_wide_granule(partition, address);

    /* "in all partitions, the digest itself is ALWAYS readable." */
    if (!is_digest && !is_readable) {
        ot_otp_dj_dai_set_error(s, OTP_ACCESS_ERROR);
        return;
    }

    unsigned waddr = address >> 2u;
    bool do_ecc =
        (partition != OTP_PART_VENDOR_TEST) && ot_otp_dj_is_ecc_enabled(s);

    DAI_CHANGE_STATE(s, OTP_DAI_READ_WAIT);

    uint32_t data_lo, data_hi;
    unsigned err = 0;

    if (is_wide || is_digest) {
        waddr &= ~0b1u;
        data_lo = s->otp->data[waddr];
        data_hi = s->otp->data[waddr + 1u];

        if (do_ecc) {
            unsigned ewaddr = waddr >> 1u;
            g_assert(ewaddr < s->otp->ecc_size);
            uint32_t ecc = s->otp->ecc[ewaddr];
            if (ot_otp_dj_is_ecc_enabled(s)) {
                data_lo = ot_otp_dj_verify_ecc(s, data_lo, ecc & 0xffffu, &err);
                data_hi = ot_otp_dj_verify_ecc(s, data_hi, ecc >> 16u, &err);
            }
        }
    } else {
        data_lo = s->otp->data[waddr];
        data_hi = 0u;

        if (do_ecc) {
            unsigned ewaddr = waddr >> 1u;
            g_assert(ewaddr < s->otp->ecc_size);
            uint32_t ecc = s->otp->ecc[ewaddr];
            if (waddr & 1u) {
                ecc >>= 16u;
            }
            if (ot_otp_dj_is_ecc_enabled(s)) {
                data_lo = ot_otp_dj_verify_ecc(s, data_lo, ecc & 0xffffu, &err);
            }
        }
    }

    s->regs[R_DIRECT_ACCESS_RDATA_0] = data_lo;
    s->regs[R_DIRECT_ACCESS_RDATA_1] = data_hi;

    if (err) {
        OtOTPError otp_err =
            (err > 1) ? OTP_MACRO_ECC_UNCORR_ERROR : OTP_MACRO_ECC_CORR_ERROR;
        ot_otp_dj_dai_set_error(s, otp_err);
        return;
    }

    if (!ot_otp_dj_is_buffered(partition)) {
        /* fake slow access to OTP cell */
        timer_mod(s->dai->delay,
                  qemu_clock_get_ns(OT_VIRTUAL_CLOCK) + DAI_READ_DELAY_NS);
    } else {
        DAI_CHANGE_STATE(s, OTP_DAI_IDLE);
    }
}

static int ot_otp_dj_dai_write_u64(OtOTPDjState *s, unsigned address)
{
    unsigned waddr = (address / sizeof(uint32_t)) & ~1u;
    uint32_t *dst = &s->otp->data[waddr];

    uint32_t dst_lo = dst[0u];
    uint32_t dst_hi = dst[1u];

    uint32_t lo = s->regs[R_DIRECT_ACCESS_WDATA_0];
    uint32_t hi = s->regs[R_DIRECT_ACCESS_WDATA_1];

    if ((dst_lo & ~lo) || (dst_hi & ~hi)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Cannot clear OTP bits\n", __func__);
        ot_otp_dj_set_error(s, OTP_ENTRY_DAI, OTP_MACRO_WRITE_BLANK_ERROR);
    }

    dst[0u] |= lo;
    dst[1u] |= hi;

    uintptr_t offset = (uintptr_t)s->otp->data - (uintptr_t)s->otp->storage;
    if (ot_otp_dj_write_backend(s, dst,
                                (unsigned)(offset + waddr * sizeof(uint32_t)),
                                sizeof(uint64_t))) {
        error_report("%s: cannot update OTP backend", __func__);
        ot_otp_dj_dai_set_error(s, OTP_MACRO_ERROR);
        return -1;
    }

    if (ot_otp_dj_is_ecc_enabled(s)) {
        unsigned ewaddr = waddr >> 1u;
        g_assert(ewaddr < s->otp->ecc_size);
        uint32_t *edst = &s->otp->ecc[ewaddr];

        uint32_t ecc_lo = (uint32_t)ot_otp_dj_compute_ecc_u32(lo);
        uint32_t ecc_hi = (uint32_t)ot_otp_dj_compute_ecc_u32(hi);
        uint32_t ecc = (ecc_hi << 16u) | ecc_lo;

        if (*edst & ~ecc) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: Cannot clear OTP ECC bits\n",
                          __func__);
            ot_otp_dj_set_error(s, OTP_ENTRY_DAI, OTP_MACRO_WRITE_BLANK_ERROR);
        }
        *edst |= ecc;

        offset = (uintptr_t)s->otp->ecc - (uintptr_t)s->otp->storage;
        if (ot_otp_dj_write_backend(s, edst, (unsigned)(offset + (waddr << 1u)),
                                    sizeof(uint32_t))) {
            error_report("%s: cannot update OTP backend", __func__);
            ot_otp_dj_dai_set_error(s, OTP_MACRO_ERROR);
            return -1;
        }

        trace_ot_otp_dai_new_dword_ecc(s->ot_id, PART_NAME(s->dai->partition),
                                       s->dai->partition, *dst, *edst);
    }

    return 0;
}

static int ot_otp_dj_dai_write_u32(OtOTPDjState *s, unsigned address)
{
    unsigned waddr = address / sizeof(uint32_t);
    uint32_t *dst = &s->otp->data[waddr];
    uint32_t data = s->regs[R_DIRECT_ACCESS_WDATA_0];

    if (*dst & ~data) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Cannot clear OTP bits\n", __func__);
        ot_otp_dj_set_error(s, OTP_ENTRY_DAI, OTP_MACRO_WRITE_BLANK_ERROR);
    }

    *dst |= data;

    uintptr_t offset = (uintptr_t)s->otp->data - (uintptr_t)s->otp->storage;
    if (ot_otp_dj_write_backend(s, dst,
                                (unsigned)(offset + waddr * sizeof(uint32_t)),
                                sizeof(uint32_t))) {
        error_report("%s: cannot update OTP backend", __func__);
        ot_otp_dj_dai_set_error(s, OTP_MACRO_ERROR);
        return -1;
    }

    if (ot_otp_dj_is_ecc_enabled(s)) {
        g_assert((waddr >> 1u) < s->otp->ecc_size);
        uint16_t *edst = &((uint16_t *)s->otp->ecc)[waddr];
        uint16_t ecc = ot_otp_dj_compute_ecc_u32(*dst);

        if (*edst & ~ecc) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: Cannot clear OTP ECC bits\n",
                          __func__);
            ot_otp_dj_set_error(s, OTP_ENTRY_DAI, OTP_MACRO_WRITE_BLANK_ERROR);
        }
        *edst |= ecc;

        offset = (uintptr_t)s->otp->ecc - (uintptr_t)s->otp->storage;
        if (ot_otp_dj_write_backend(s, edst,
                                    (unsigned)(offset + (address >> 1u)),
                                    sizeof(uint16_t))) {
            error_report("%s: cannot update OTP backend", __func__);
            ot_otp_dj_dai_set_error(s, OTP_MACRO_ERROR);
            return -1;
        }

        trace_ot_otp_dai_new_word_ecc(s->ot_id, PART_NAME(s->dai->partition),
                                      s->dai->partition, *dst, *edst);
    }

    return 0;
}

static void ot_otp_dj_dai_write(OtOTPDjState *s)
{
    if (ot_otp_dj_dai_is_busy(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: DAI controller busy\n", __func__);
        return;
    }

    if (!ot_otp_dj_is_backend_writable(s)) {
        /* OTP backend missing or read-only; reject any write request */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: OTP backend file is missing or R/O\n", __func__);
        ot_otp_dj_dai_set_error(s, OTP_MACRO_ERROR);
        return;
    }

    DAI_CHANGE_STATE(s, OTP_DAI_WRITE);

    ot_otp_dj_dai_clear_error(s);

    unsigned address = s->regs[R_DIRECT_ACCESS_ADDRESS];

    int partition = ot_otp_dj_get_part_from_address(s, address);

    if (partition < 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Invalid partition address 0x%x\n",
                      __func__, address);
        ot_otp_dj_dai_set_error(s, OTP_ACCESS_ERROR);
        return;
    }

    if (partition >= OTP_PART_LIFE_CYCLE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Life cycle partition cannot be accessed from DAI\n",
                      __func__);
        ot_otp_dj_dai_set_error(s, OTP_ACCESS_ERROR);
        return;
    }

    OtOTPPartController *pctrl = &s->partctrls[partition];

    if (pctrl->locked) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Partition %s (%u) is locked\n",
                      __func__, PART_NAME(partition), partition);
        ot_otp_dj_dai_set_error(s, OTP_ACCESS_ERROR);
        return;
    }

    if (pctrl->write_lock) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Partition %s (%u) is write locked\n", __func__,
                      PART_NAME(partition), partition);
        ot_otp_dj_dai_set_error(s, OTP_ACCESS_ERROR);
        return;
    }

    bool is_digest = ot_otp_dj_is_part_digest_offset(partition, address);
    bool is_wide = ot_otp_dj_is_wide_granule(partition, address);

    if (is_digest) {
        if (OtOTPPartDescs[partition].hw_digest) {
            /* should have been a Digest command, not a Write command */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: Partition %s (%u) HW digest cannot be directly "
                          "written\n",
                          __func__, PART_NAME(partition), partition);
            ot_otp_dj_dai_set_error(s, OTP_ACCESS_ERROR);
            return;
        }
    }

    if (is_wide || is_digest) {
        if (ot_otp_dj_dai_write_u64(s, address)) {
            return;
        }
    } else {
        if (ot_otp_dj_dai_write_u32(s, address)) {
            return;
        }
    }

    DAI_CHANGE_STATE(s, OTP_DAI_WRITE_WAIT);

    /* fake slow access to OTP cell */
    timer_mod(s->dai->delay,
              qemu_clock_get_ns(OT_VIRTUAL_CLOCK) + DAI_WRITE_DELAY_NS);
}

static void ot_otp_dj_dai_digest(OtOTPDjState *s)
{
    if (ot_otp_dj_dai_is_busy(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: DAI controller busy\n", __func__);
        return;
    }

    if (!ot_otp_dj_is_backend_writable(s)) {
        /* OTP backend missing or read-only; reject any write request */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: OTP backend file is missing or R/O\n", __func__);
        ot_otp_dj_dai_set_error(s, OTP_MACRO_ERROR);
        return;
    }

    DAI_CHANGE_STATE(s, OTP_DAI_DIG_CLR);

    ot_otp_dj_dai_clear_error(s);

    unsigned address = s->regs[R_DIRECT_ACCESS_ADDRESS];

    int partition = ot_otp_dj_get_part_from_address(s, address);

    if (partition < 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Invalid partition address 0x%x\n",
                      __func__, address);
        ot_otp_dj_dai_set_error(s, OTP_ACCESS_ERROR);
        return;
    }

    if (partition >= OTP_PART_LIFE_CYCLE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Life cycle partition cannot be accessed from DAI\n",
                      __func__);
        ot_otp_dj_dai_set_error(s, OTP_ACCESS_ERROR);
        return;
    }

    if (!OtOTPPartDescs[partition].hw_digest) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Invalid partition, no HW digest on %s (#%u)\n",
                      __func__, PART_NAME(partition), partition);
        ot_otp_dj_dai_set_error(s, OTP_ACCESS_ERROR);
        return;
    }

    OtOTPPartController *pctrl = &s->partctrls[partition];

    if (pctrl->locked) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Partition %s (%u) is locked\n",
                      __func__, PART_NAME(partition), partition);
        ot_otp_dj_dai_set_error(s, OTP_ACCESS_ERROR);
        return;
    }

    if (pctrl->write_lock) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Partition %s (%u) is write locked\n", __func__,
                      PART_NAME(partition), partition);
        ot_otp_dj_dai_set_error(s, OTP_ACCESS_ERROR);
        return;
    }

    DAI_CHANGE_STATE(s, OTP_DAI_DIG_READ);

    const uint8_t *data;

    if (OtOTPPartDescs[partition].buffered) {
        data = ((const uint8_t *)s->otp->data) +
               OT_OTP_PART_DATA_OFFSET(partition);
    } else {
        data = (const uint8_t *)pctrl->buffer.data;
    }
    unsigned part_size = OT_OTP_PART_DATA_BYTE_SIZE(partition);

    DAI_CHANGE_STATE(s, OTP_DAI_DIG);

    pctrl->buffer.next_digest =
        ot_otp_dj_compute_partition_digest(s, data, part_size);
    s->dai->partition = partition;

    TRACE_OTP("%s: next digest %016llx from %s\n", __func__,
              pctrl->buffer.next_digest, ot_otp_hexdump(data, part_size));

    DAI_CHANGE_STATE(s, OTP_DAI_DIG_WAIT);

    /* fake slow access to OTP cell */
    timer_mod(s->dai->delay,
              qemu_clock_get_ns(OT_VIRTUAL_CLOCK) + DAI_DIGEST_DELAY_NS);
}

static void ot_otp_dj_dai_write_digest(void *opaque)
{
    OtOTPDjState *s = OT_OTP_DJ(opaque);

    g_assert((s->dai->partition >= 0) && (s->dai->partition < OTP_PART_COUNT));

    DAI_CHANGE_STATE(s, OTP_DAI_WRITE);

    OtOTPPartController *pctrl = &s->partctrls[s->dai->partition];
    unsigned address = OtOTPPartDescs[s->dai->partition].digest_offset;
    unsigned dwaddr = address / sizeof(uint64_t);
    uint64_t *dst = &((uint64_t *)s->otp->data)[dwaddr];
    uint64_t data = pctrl->buffer.next_digest;
    pctrl->buffer.next_digest = 0;

    if (*dst & ~data) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Cannot clear OTP data bits\n",
                      __func__);
        ot_otp_dj_set_error(s, OTP_ENTRY_DAI, OTP_MACRO_WRITE_BLANK_ERROR);
    }
    *dst |= data;

    uintptr_t offset;
    offset = (uintptr_t)s->otp->data - (uintptr_t)s->otp->storage;
    if (ot_otp_dj_write_backend(s, dst, (unsigned)(offset + address),
                                sizeof(uint64_t))) {
        error_report("%s: cannot update OTP backend", __func__);
        ot_otp_dj_dai_set_error(s, OTP_MACRO_ERROR);
        return;
    }

    uint32_t ecc = ot_otp_dj_compute_ecc_u64(data);

    // dwaddr is 64-bit based, convert it to 32-bit base for ECC
    unsigned ewaddr = (dwaddr << 1u) / s->otp->ecc_granule;
    g_assert(ewaddr < s->otp->ecc_size);
    uint32_t *edst = &s->otp->ecc[ewaddr];

    if (*edst & ~ecc) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Cannot clear OTP ECC bits\n",
                      __func__);
        ot_otp_dj_set_error(s, OTP_ENTRY_DAI, OTP_MACRO_WRITE_BLANK_ERROR);
    }
    *edst |= ecc;

    offset = (uintptr_t)s->otp->ecc - (uintptr_t)s->otp->storage;
    if (ot_otp_dj_write_backend(s, edst, (unsigned)(offset + (ewaddr << 2u)),
                                sizeof(uint32_t))) {
        error_report("%s: cannot update OTP backend", __func__);
        ot_otp_dj_dai_set_error(s, OTP_MACRO_ERROR);
        return;
    }

    trace_ot_otp_dai_new_digest_ecc(s->ot_id, PART_NAME(s->dai->partition),
                                    s->dai->partition, *dst, *edst);

    DAI_CHANGE_STATE(s, OTP_DAI_WRITE_WAIT);

    /* fake slow access to OTP cell */
    timer_mod(s->dai->delay,
              qemu_clock_get_ns(OT_VIRTUAL_CLOCK) + DAI_WRITE_DELAY_NS);
}

static void ot_otp_dj_dai_complete(void *opaque)
{
    OtOTPDjState *s = opaque;

    switch (s->dai->state) {
    case OTP_DAI_READ_WAIT:
        trace_ot_otp_dai_read(s->ot_id, PART_NAME(s->dai->partition),
                              s->dai->partition,
                              s->regs[R_DIRECT_ACCESS_RDATA_1],
                              s->regs[R_DIRECT_ACCESS_RDATA_1]);
        s->dai->partition = -1;
        DAI_CHANGE_STATE(s, OTP_DAI_IDLE);
        break;
    case OTP_DAI_WRITE_WAIT:
        s->regs[R_INTR_STATE] |= INTR_OTP_OPERATION_DONE_MASK;
        s->dai->partition = -1;
        DAI_CHANGE_STATE(s, OTP_DAI_IDLE);
        break;
    case OTP_DAI_DIG_WAIT:
        g_assert(s->dai->partition >= 0);
        qemu_bh_schedule(s->dai->digest_bh);
        break;
    case OTP_DAI_ERROR:
        break;
    default:
        g_assert_not_reached();
        break;
    };
}

static void ot_otp_dj_lci_init(OtOTPDjState *s)
{
    LCI_CHANGE_STATE(s, OTP_LCI_IDLE);
}

static uint64_t ot_otp_dj_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    OtOTPDjState *s = OT_OTP_DJ(opaque);
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);
    switch (reg) {
    case R_INTR_STATE:
    case R_INTR_ENABLE:
    case R_ERR_CODE_0:
    case R_ERR_CODE_1:
    case R_ERR_CODE_2:
    case R_ERR_CODE_3:
    case R_ERR_CODE_4:
    case R_ERR_CODE_5:
    case R_ERR_CODE_6:
    case R_ERR_CODE_7:
    case R_ERR_CODE_8:
    case R_ERR_CODE_9:
    case R_ERR_CODE_10:
    case R_ERR_CODE_11:
    case R_ERR_CODE_12:
    case R_ERR_CODE_13:
    case R_ERR_CODE_14:
    case R_ERR_CODE_15:
    case R_ERR_CODE_16:
    case R_ERR_CODE_17:
    case R_ERR_CODE_18:
    case R_ERR_CODE_19:
    case R_ERR_CODE_20:
    case R_ERR_CODE_21:
    case R_ERR_CODE_22:
    case R_ERR_CODE_23:
    case R_DIRECT_ACCESS_WDATA_0:
    case R_DIRECT_ACCESS_WDATA_1:
    case R_DIRECT_ACCESS_RDATA_0:
    case R_DIRECT_ACCESS_RDATA_1:
    case R_DIRECT_ACCESS_ADDRESS:
    case R_VENDOR_TEST_READ_LOCK:
    case R_CREATOR_SW_CFG_READ_LOCK:
    case R_OWNER_SW_CFG_READ_LOCK:
    case R_OWNERSHIP_SLOT_STATE_READ_LOCK:
    case R_ROT_CREATOR_AUTH_READ_LOCK:
    case R_ROT_OWNER_AUTH_SLOT0_READ_LOCK:
    case R_ROT_OWNER_AUTH_SLOT1_READ_LOCK:
    case R_PLAT_INTEG_AUTH_SLOT0_READ_LOCK:
    case R_PLAT_INTEG_AUTH_SLOT1_READ_LOCK:
    case R_PLAT_OWNER_AUTH_SLOT0_READ_LOCK:
    case R_PLAT_OWNER_AUTH_SLOT1_READ_LOCK:
    case R_PLAT_OWNER_AUTH_SLOT2_READ_LOCK:
    case R_PLAT_OWNER_AUTH_SLOT3_READ_LOCK:
    case R_EXT_NVM_READ_LOCK:
    case R_ROM_PATCH_READ_LOCK:
        val32 = s->regs[reg];
        break;
    case R_STATUS:
        val32 = ot_otp_dj_get_status(s);
        break;
    case R_DIRECT_ACCESS_REGWEN:
        val32 = FIELD_DP32(0, DIRECT_ACCESS_REGWEN, REGWEN,
                           (uint32_t)!ot_otp_dj_dai_is_busy(s));
        break;
    /* NOLINTNEXTLINE */
    case R_DIRECT_ACCESS_CMD:
        val32 = 0; /* R0W1C */
        break;
    case R_CHECK_TRIGGER_REGWEN:
    case R_CHECK_TRIGGER:
    case R_CHECK_REGWEN:
    case R_CHECK_TIMEOUT:
    case R_INTEGRITY_CHECK_PERIOD:
    case R_CONSISTENCY_CHECK_PERIOD:
        /* TODO: not yet implemented */
        val32 = 0;
        break;
    /* in all partitions, the digest itself is ALWAYS readable."*/
    case R_VENDOR_TEST_DIGEST_0:
        val32 = (uint32_t)ot_otp_dj_get_part_digest(s, OTP_PART_VENDOR_TEST);
        break;
    case R_VENDOR_TEST_DIGEST_1:
        val32 = (uint32_t)(ot_otp_dj_get_part_digest(s, OTP_PART_VENDOR_TEST) >>
                           32u);
        break;
    case R_CREATOR_SW_CFG_DIGEST_0:
        val32 = (uint32_t)ot_otp_dj_get_part_digest(s, OTP_PART_CREATOR_SW_CFG);
        break;
    case R_CREATOR_SW_CFG_DIGEST_1:
        val32 =
            (uint32_t)(ot_otp_dj_get_part_digest(s, OTP_PART_CREATOR_SW_CFG) >>
                       32u);
        break;
    case R_OWNER_SW_CFG_DIGEST_0:
        val32 = (uint32_t)ot_otp_dj_get_part_digest(s, OTP_PART_OWNER_SW_CFG);
        break;
    case R_OWNER_SW_CFG_DIGEST_1:
        val32 =
            (uint32_t)(ot_otp_dj_get_part_digest(s, OTP_PART_OWNER_SW_CFG) >>
                       32u);
        break;
    case R_ROT_CREATOR_AUTH_DIGEST_0:
        val32 =
            (uint32_t)ot_otp_dj_get_part_digest(s, OTP_PART_ROT_CREATOR_AUTH);
        break;
    case R_ROT_CREATOR_AUTH_DIGEST_1:
        val32 =
            (uint32_t)(ot_otp_dj_get_part_digest(s,
                                                 OTP_PART_ROT_CREATOR_AUTH) >>
                       32u);
        break;
    case R_ROT_OWNER_AUTH_SLOT0_DIGEST_0:
        val32 =
            (uint32_t)ot_otp_dj_get_part_digest(s,
                                                OTP_PART_ROT_OWNER_AUTH_SLOT0);
        break;
    case R_ROT_OWNER_AUTH_SLOT0_DIGEST_1:
        val32 = (uint32_t)(ot_otp_dj_get_part_digest(
                               s, OTP_PART_ROT_OWNER_AUTH_SLOT0) >>
                           32u);
        break;
    case R_ROT_OWNER_AUTH_SLOT1_DIGEST_0:
        val32 =
            (uint32_t)ot_otp_dj_get_part_digest(s,
                                                OTP_PART_ROT_OWNER_AUTH_SLOT1);
        break;
    case R_ROT_OWNER_AUTH_SLOT1_DIGEST_1:
        val32 = (uint32_t)(ot_otp_dj_get_part_digest(
                               s, OTP_PART_ROT_OWNER_AUTH_SLOT1) >>
                           32u);
        break;
    case R_PLAT_INTEG_AUTH_SLOT0_DIGEST_0:
        val32 =
            (uint32_t)ot_otp_dj_get_part_digest(s,
                                                OTP_PART_PLAT_INTEG_AUTH_SLOT0);
        break;
    case R_PLAT_INTEG_AUTH_SLOT0_DIGEST_1:
        val32 = (uint32_t)(ot_otp_dj_get_part_digest(
                               s, OTP_PART_PLAT_INTEG_AUTH_SLOT0) >>
                           32u);
        break;
    case R_PLAT_INTEG_AUTH_SLOT1_DIGEST_0:
        val32 =
            (uint32_t)ot_otp_dj_get_part_digest(s,
                                                OTP_PART_PLAT_INTEG_AUTH_SLOT1);
        break;
    case R_PLAT_INTEG_AUTH_SLOT1_DIGEST_1:
        val32 = (uint32_t)(ot_otp_dj_get_part_digest(
                               s, OTP_PART_PLAT_INTEG_AUTH_SLOT1) >>
                           32u);
        break;
    case R_PLAT_OWNER_AUTH_SLOT0_DIGEST_0:
        val32 =
            (uint32_t)ot_otp_dj_get_part_digest(s,
                                                OTP_PART_PLAT_OWNER_AUTH_SLOT0);
        break;
    case R_PLAT_OWNER_AUTH_SLOT0_DIGEST_1:
        val32 = (uint32_t)(ot_otp_dj_get_part_digest(
                               s, OTP_PART_PLAT_OWNER_AUTH_SLOT0) >>
                           32u);
        break;
    case R_PLAT_OWNER_AUTH_SLOT1_DIGEST_0:
        val32 =
            (uint32_t)ot_otp_dj_get_part_digest(s,
                                                OTP_PART_PLAT_OWNER_AUTH_SLOT1);
        break;
    case R_PLAT_OWNER_AUTH_SLOT1_DIGEST_1:
        val32 = (uint32_t)(ot_otp_dj_get_part_digest(
                               s, OTP_PART_PLAT_OWNER_AUTH_SLOT1) >>
                           32u);
        break;
    case R_PLAT_OWNER_AUTH_SLOT2_DIGEST_0:
        val32 =
            (uint32_t)ot_otp_dj_get_part_digest(s,
                                                OTP_PART_PLAT_OWNER_AUTH_SLOT2);
        break;
    case R_PLAT_OWNER_AUTH_SLOT2_DIGEST_1:
        val32 = (uint32_t)(ot_otp_dj_get_part_digest(
                               s, OTP_PART_PLAT_OWNER_AUTH_SLOT2) >>
                           32u);
        break;
    case R_PLAT_OWNER_AUTH_SLOT3_DIGEST_0:
        val32 =
            (uint32_t)ot_otp_dj_get_part_digest(s,
                                                OTP_PART_PLAT_OWNER_AUTH_SLOT3);
        break;
    case R_PLAT_OWNER_AUTH_SLOT3_DIGEST_1:
        val32 = (uint32_t)(ot_otp_dj_get_part_digest(
                               s, OTP_PART_PLAT_OWNER_AUTH_SLOT3) >>
                           32u);
        break;
    case R_ROM_PATCH_DIGEST_0:
        val32 = (uint32_t)ot_otp_dj_get_part_digest(s, OTP_PART_ROM_PATCH);
        break;
    case R_ROM_PATCH_DIGEST_1:
        val32 =
            (uint32_t)(ot_otp_dj_get_part_digest(s, OTP_PART_ROM_PATCH) >> 32u);
        break;
    case R_HW_CFG0_DIGEST_0:
        val32 =
            (uint32_t)ot_otp_dj_get_buffered_part_digest(s, OTP_PART_HW_CFG0);
        break;
    case R_HW_CFG0_DIGEST_1:
        val32 =
            (uint32_t)(ot_otp_dj_get_buffered_part_digest(s,
                                                          OTP_PART_HW_CFG0) >>
                       32u);
        break;
    case R_HW_CFG1_DIGEST_0:
        val32 =
            (uint32_t)ot_otp_dj_get_buffered_part_digest(s, OTP_PART_HW_CFG1);
        break;
    case R_HW_CFG1_DIGEST_1:
        val32 =
            (uint32_t)(ot_otp_dj_get_buffered_part_digest(s,
                                                          OTP_PART_HW_CFG1) >>
                       32u);
        break;
    case R_SECRET0_DIGEST_0:
        val32 =
            (uint32_t)ot_otp_dj_get_buffered_part_digest(s, OTP_PART_SECRET0);
        break;
    case R_SECRET0_DIGEST_1:
        val32 =
            (uint32_t)(ot_otp_dj_get_buffered_part_digest(s,
                                                          OTP_PART_SECRET0) >>
                       32u);
        break;
    case R_SECRET1_DIGEST_0:
        val32 =
            (uint32_t)ot_otp_dj_get_buffered_part_digest(s, OTP_PART_SECRET1);
        break;
    case R_SECRET1_DIGEST_1:
        val32 =
            (uint32_t)(ot_otp_dj_get_buffered_part_digest(s,
                                                          OTP_PART_SECRET1) >>
                       32u);
        break;
    case R_SECRET2_DIGEST_0:
        val32 =
            (uint32_t)ot_otp_dj_get_buffered_part_digest(s, OTP_PART_SECRET2);
        break;
    case R_SECRET2_DIGEST_1:
        val32 =
            (uint32_t)(ot_otp_dj_get_buffered_part_digest(s,
                                                          OTP_PART_SECRET2) >>
                       32u);
        break;
    case R_SECRET3_DIGEST_0:
        val32 =
            (uint32_t)ot_otp_dj_get_buffered_part_digest(s, OTP_PART_SECRET3);
        break;
    case R_SECRET3_DIGEST_1:
        val32 =
            (uint32_t)(ot_otp_dj_get_buffered_part_digest(s,
                                                          OTP_PART_SECRET3) >>
                       32u);
        break;
    case R_INTR_TEST:
    case R_ALERT_TEST:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: W/O register 0x02%" HWADDR_PRIx " (%s)\n", __func__,
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
    trace_ot_otp_io_reg_read_out(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32,
                                 pc);

    return (uint64_t)val32;
}

static void ot_otp_dj_reg_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    OtOTPDjState *s = OT_OTP_DJ(opaque);
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
    case R_VENDOR_TEST_READ_LOCK:
    case R_CREATOR_SW_CFG_READ_LOCK:
    case R_OWNER_SW_CFG_READ_LOCK:
    case R_OWNERSHIP_SLOT_STATE_READ_LOCK:
    case R_ROT_CREATOR_AUTH_READ_LOCK:
    case R_ROT_OWNER_AUTH_SLOT0_READ_LOCK:
    case R_ROT_OWNER_AUTH_SLOT1_READ_LOCK:
    case R_PLAT_INTEG_AUTH_SLOT0_READ_LOCK:
    case R_PLAT_INTEG_AUTH_SLOT1_READ_LOCK:
    case R_PLAT_OWNER_AUTH_SLOT0_READ_LOCK:
    case R_PLAT_OWNER_AUTH_SLOT1_READ_LOCK:
    case R_PLAT_OWNER_AUTH_SLOT2_READ_LOCK:
    case R_PLAT_OWNER_AUTH_SLOT3_READ_LOCK:
    case R_EXT_NVM_READ_LOCK:
    case R_ROM_PATCH_READ_LOCK:
        if (!(s->regs[R_DIRECT_ACCESS_REGWEN] &
              R_DIRECT_ACCESS_REGWEN_REGWEN_MASK)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s is not enabled, %s is protected\n", __func__,
                          REG_NAME(R_DIRECT_ACCESS_REGWEN), REG_NAME(reg));
            return;
        }
        break;
    case R_CHECK_TRIGGER:
        if (!(s->regs[R_CHECK_TRIGGER_REGWEN] &
              R_CHECK_TRIGGER_REGWEN_REGWEN_MASK)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s is not enabled, %s is protected\n", __func__,
                          REG_NAME(R_CHECK_TRIGGER_REGWEN), REG_NAME(reg));
            return;
        }
        break;
    case R_CHECK_TIMEOUT:
    case R_INTEGRITY_CHECK_PERIOD:
    case R_CONSISTENCY_CHECK_PERIOD:
        if (!(s->regs[R_CHECK_REGWEN] & R_CHECK_REGWEN_REGWEN_MASK)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s is not enabled, %s is protected\n", __func__,
                          REG_NAME(R_CHECK_REGWEN), REG_NAME(reg));
            return;
        }
        break;
    case R_STATUS:
    case R_ERR_CODE_0:
    case R_ERR_CODE_1:
    case R_ERR_CODE_2:
    case R_ERR_CODE_3:
    case R_ERR_CODE_4:
    case R_ERR_CODE_5:
    case R_ERR_CODE_6:
    case R_ERR_CODE_7:
    case R_ERR_CODE_8:
    case R_ERR_CODE_9:
    case R_ERR_CODE_10:
    case R_ERR_CODE_11:
    case R_ERR_CODE_12:
    case R_ERR_CODE_13:
    case R_ERR_CODE_14:
    case R_ERR_CODE_15:
    case R_ERR_CODE_16:
    case R_ERR_CODE_17:
    case R_ERR_CODE_18:
    case R_ERR_CODE_19:
    case R_ERR_CODE_20:
    case R_ERR_CODE_21:
    case R_ERR_CODE_22:
    case R_ERR_CODE_23:
    case R_DIRECT_ACCESS_REGWEN:
    case R_DIRECT_ACCESS_RDATA_0:
    case R_DIRECT_ACCESS_RDATA_1:
    case R_VENDOR_TEST_DIGEST_0:
    case R_VENDOR_TEST_DIGEST_1:
    case R_CREATOR_SW_CFG_DIGEST_0:
    case R_CREATOR_SW_CFG_DIGEST_1:
    case R_OWNER_SW_CFG_DIGEST_0:
    case R_OWNER_SW_CFG_DIGEST_1:
    case R_ROT_CREATOR_AUTH_DIGEST_0:
    case R_ROT_CREATOR_AUTH_DIGEST_1:
    case R_ROT_OWNER_AUTH_SLOT0_DIGEST_0:
    case R_ROT_OWNER_AUTH_SLOT0_DIGEST_1:
    case R_ROT_OWNER_AUTH_SLOT1_DIGEST_0:
    case R_ROT_OWNER_AUTH_SLOT1_DIGEST_1:
    case R_PLAT_INTEG_AUTH_SLOT0_DIGEST_0:
    case R_PLAT_INTEG_AUTH_SLOT0_DIGEST_1:
    case R_PLAT_INTEG_AUTH_SLOT1_DIGEST_0:
    case R_PLAT_INTEG_AUTH_SLOT1_DIGEST_1:
    case R_PLAT_OWNER_AUTH_SLOT0_DIGEST_0:
    case R_PLAT_OWNER_AUTH_SLOT0_DIGEST_1:
    case R_PLAT_OWNER_AUTH_SLOT1_DIGEST_0:
    case R_PLAT_OWNER_AUTH_SLOT1_DIGEST_1:
    case R_PLAT_OWNER_AUTH_SLOT2_DIGEST_0:
    case R_PLAT_OWNER_AUTH_SLOT2_DIGEST_1:
    case R_PLAT_OWNER_AUTH_SLOT3_DIGEST_0:
    case R_PLAT_OWNER_AUTH_SLOT3_DIGEST_1:
    case R_ROM_PATCH_DIGEST_0:
    case R_ROM_PATCH_DIGEST_1:
    case R_HW_CFG0_DIGEST_0:
    case R_HW_CFG0_DIGEST_1:
    case R_HW_CFG1_DIGEST_0:
    case R_HW_CFG1_DIGEST_1:
    case R_SECRET0_DIGEST_0:
    case R_SECRET0_DIGEST_1:
    case R_SECRET1_DIGEST_0:
    case R_SECRET1_DIGEST_1:
    case R_SECRET2_DIGEST_0:
    case R_SECRET2_DIGEST_1:
    case R_SECRET3_DIGEST_0:
    case R_SECRET3_DIGEST_1:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: R/O register 0x%03" HWADDR_PRIx " (%s)\n", __func__,
                      addr, REG_NAME(reg));
        return;
    default:
        break;
    }

    switch (reg) {
    case R_INTR_STATE:
        val32 &= INTR_MASK;
        s->regs[R_INTR_STATE] &= ~val32; /* RW1C */
        ot_otp_dj_update_irqs(s);
        break;
    case R_INTR_ENABLE:
        val32 &= INTR_MASK;
        s->regs[R_INTR_ENABLE] = val32;
        ot_otp_dj_update_irqs(s);
        break;
    case R_INTR_TEST:
        val32 &= INTR_MASK;
        s->regs[R_INTR_STATE] = val32;
        ot_otp_dj_update_irqs(s);
        break;
    case R_ALERT_TEST:
        val32 &= ALERT_TEST_MASK;
        s->regs[reg] = val32;
        ot_otp_dj_update_alerts(s);
        break;
    case R_DIRECT_ACCESS_CMD:
        if (FIELD_EX32(val32, DIRECT_ACCESS_CMD, RD)) {
            ot_otp_dj_dai_read(s);
        } else if (FIELD_EX32(val32, DIRECT_ACCESS_CMD, WR)) {
            ot_otp_dj_dai_write(s);
        } else if (FIELD_EX32(val32, DIRECT_ACCESS_CMD, DIGEST)) {
            ot_otp_dj_dai_digest(s);
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
    case R_VENDOR_TEST_READ_LOCK:
    case R_CREATOR_SW_CFG_READ_LOCK:
    case R_OWNER_SW_CFG_READ_LOCK:
    case R_OWNERSHIP_SLOT_STATE_READ_LOCK:
    case R_ROT_CREATOR_AUTH_READ_LOCK:
    case R_ROT_OWNER_AUTH_SLOT0_READ_LOCK:
    case R_ROT_OWNER_AUTH_SLOT1_READ_LOCK:
    case R_PLAT_INTEG_AUTH_SLOT0_READ_LOCK:
    case R_PLAT_INTEG_AUTH_SLOT1_READ_LOCK:
    case R_PLAT_OWNER_AUTH_SLOT0_READ_LOCK:
    case R_PLAT_OWNER_AUTH_SLOT1_READ_LOCK:
    case R_PLAT_OWNER_AUTH_SLOT2_READ_LOCK:
    case R_PLAT_OWNER_AUTH_SLOT3_READ_LOCK:
    case R_EXT_NVM_READ_LOCK:
    case R_ROM_PATCH_READ_LOCK:
        val32 &= READ_LOCK_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_CHECK_TRIGGER_REGWEN:
    case R_CHECK_TRIGGER:
    case R_CHECK_REGWEN:
    case R_CHECK_TIMEOUT:
    case R_INTEGRITY_CHECK_PERIOD:
    case R_CONSISTENCY_CHECK_PERIOD:
        qemu_log_mask(LOG_UNIMP, "%s: %s is not supported\n", __func__,
                      REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static const char *ot_otp_dj_swcfg_reg_name(unsigned swreg)
{
#define CASE_SCALAR(_reg_) \
    case R_##_reg_: \
        return stringify(_reg_)
#define CASE_RANGE(_reg_) \
    case R_##_reg_...(R_##_reg_ + (((_reg_##_SIZE) + 3u) / 4u) - 1u): \
        return stringify(_reg_)
#define CASE_DIGEST(_reg_) \
    case R_##_reg_...(R_##_reg_ + 1u): \
        return stringify(_reg_)

    switch (swreg) {
        CASE_RANGE(VENDOR_TEST_SCRATCH);
        CASE_DIGEST(VENDOR_TEST_DIGEST);
        CASE_RANGE(CREATOR_SW_CFG_DIO_ATTR);
        CASE_RANGE(CREATOR_SW_CFG_AST_CFG);
        CASE_RANGE(CREATOR_SW_CFG_AST_SPARES);
        CASE_SCALAR(CREATOR_SW_CFG_AST_AVGSFUSECTL);
        CASE_SCALAR(CREATOR_SW_CFG_AST_AVGSHDRCFG);
        CASE_SCALAR(CREATOR_SW_CFG_AST_RINGOSC_TRIM_CTL);
        CASE_SCALAR(CREATOR_SW_CFG_AST_RINGOSC_FREQ_COUNT_CTL);
        CASE_SCALAR(CREATOR_SW_CFG_AST_RINGOSC_FREQ_TH_SLOW);
        CASE_SCALAR(CREATOR_SW_CFG_AST_RINGOSC_FREQ_TH_FAST);
        CASE_SCALAR(CREATOR_SW_CFG_AST_INIT_EN);
        CASE_RANGE(CREATOR_SW_CFG_OVERRIDES);
        CASE_SCALAR(CREATOR_SW_CFG_ROM_EXT_SKU);
        CASE_SCALAR(CREATOR_SW_CFG_SIGVERIFY_RSA_MOD_EXP_IBEX_EN);
        CASE_RANGE(CREATOR_SW_CFG_SIGVERIFY_RSA_KEY_EN);
        CASE_SCALAR(CREATOR_SW_CFG_SIGVERIFY_SPX_EN);
        CASE_RANGE(CREATOR_SW_CFG_SIGVERIFY_SPX_KEY_EN);
        CASE_SCALAR(CREATOR_SW_CFG_FLASH_DATA_DEFAULT_CFG);
        CASE_SCALAR(CREATOR_SW_CFG_FLASH_INFO_BOOT_DATA_CFG);
        CASE_SCALAR(CREATOR_SW_CFG_FLASH_HW_INFO_CFG_OVERRIDE);
        CASE_SCALAR(CREATOR_SW_CFG_RNG_EN);
        CASE_SCALAR(CREATOR_SW_CFG_JITTER_EN);
        CASE_SCALAR(CREATOR_SW_CFG_RET_RAM_RESET_MASK);
        CASE_SCALAR(CREATOR_SW_CFG_MANUF_STATE);
        CASE_SCALAR(CREATOR_SW_CFG_ROM_EXEC_EN);
        CASE_SCALAR(CREATOR_SW_CFG_CPUCTRL);
        CASE_SCALAR(CREATOR_SW_CFG_MIN_SEC_VER_ROM_EXT);
        CASE_SCALAR(CREATOR_SW_CFG_MIN_SEC_VER_BL0);
        CASE_SCALAR(CREATOR_SW_CFG_DEFAULT_BOOT_DATA_IN_PROD_EN);
        CASE_SCALAR(CREATOR_SW_CFG_RMA_SPIN_EN);
        CASE_SCALAR(CREATOR_SW_CFG_RMA_SPIN_CYCLES);
        CASE_SCALAR(CREATOR_SW_CFG_RNG_REPCNT_THRESHOLDS);
        CASE_SCALAR(CREATOR_SW_CFG_RNG_REPCNTS_THRESHOLDS);
        CASE_SCALAR(CREATOR_SW_CFG_RNG_ADAPTP_HI_THRESHOLDS);
        CASE_SCALAR(CREATOR_SW_CFG_RNG_ADAPTP_LO_THRESHOLDS);
        CASE_SCALAR(CREATOR_SW_CFG_RNG_BUCKET_THRESHOLDS);
        CASE_SCALAR(CREATOR_SW_CFG_RNG_MARKOV_HI_THRESHOLDS);
        CASE_SCALAR(CREATOR_SW_CFG_RNG_MARKOV_LO_THRESHOLDS);
        CASE_SCALAR(CREATOR_SW_CFG_RNG_EXTHT_HI_THRESHOLDS);
        CASE_SCALAR(CREATOR_SW_CFG_RNG_EXTHT_LO_THRESHOLDS);
        CASE_SCALAR(CREATOR_SW_CFG_RNG_ALERT_THRESHOLD);
        CASE_SCALAR(CREATOR_SW_CFG_RNG_HEALTH_CONFIG_DIGEST);
        CASE_SCALAR(CREATOR_SW_CFG_SRAM_KEY_RENEW_EN);
        CASE_DIGEST(CREATOR_SW_CFG_DIGEST);
        CASE_SCALAR(OWNER_SW_CFG_ROM_ERROR_REPORTING);
        CASE_SCALAR(OWNER_SW_CFG_ROM_BOOTSTRAP_DIS);
        CASE_SCALAR(OWNER_SW_CFG_ROM_ALERT_CLASS_EN);
        CASE_SCALAR(OWNER_SW_CFG_ROM_ALERT_ESCALATION);
        CASE_RANGE(OWNER_SW_CFG_ROM_ALERT_CLASSIFICATION);
        CASE_RANGE(OWNER_SW_CFG_ROM_LOCAL_ALERT_CLASSIFICATION);
        CASE_RANGE(OWNER_SW_CFG_ROM_ALERT_ACCUM_THRESH);
        CASE_RANGE(OWNER_SW_CFG_ROM_ALERT_TIMEOUT_CYCLES);
        CASE_RANGE(OWNER_SW_CFG_ROM_ALERT_PHASE_CYCLES);
        CASE_SCALAR(OWNER_SW_CFG_ROM_ALERT_DIGEST_PROD);
        CASE_SCALAR(OWNER_SW_CFG_ROM_ALERT_DIGEST_PROD_END);
        CASE_SCALAR(OWNER_SW_CFG_ROM_ALERT_DIGEST_DEV);
        CASE_SCALAR(OWNER_SW_CFG_ROM_ALERT_DIGEST_RMA);
        CASE_SCALAR(OWNER_SW_CFG_ROM_WATCHDOG_BITE_THRESHOLD_CYCLES);
        CASE_SCALAR(OWNER_SW_CFG_ROM_KEYMGR_ROM_EXT_MEAS_EN);
        CASE_SCALAR(OWNER_SW_CFG_MANUF_STATE);
        CASE_SCALAR(OWNER_SW_CFG_ROM_RSTMGR_INFO_EN);
        CASE_DIGEST(OWNER_SW_CFG_DIGEST);
        CASE_RANGE(OWNERSHIP_SLOT_STATE_ROT_OWNER_AUTH);
        CASE_RANGE(OWNERSHIP_SLOT_STATE_PLAT_INTEG_AUTH);
        CASE_RANGE(OWNERSHIP_SLOT_STATE_PLAT_OWNER_AUTH);
        CASE_RANGE(ROT_CREATOR_AUTH_NON_RAW_MFW_CODESIGN_KEY);
        CASE_RANGE(ROT_CREATOR_AUTH_ROM2_PATCH_SIGVERIFY_KEY);
        CASE_RANGE(ROT_CREATOR_AUTH_KEYMANIFEST_KEY);
        CASE_RANGE(ROT_CREATOR_AUTH_IDENTITY_CERT);
        CASE_RANGE(ROT_CREATOR_AUTH_IDENTITY_CERT_CMAC);
        CASE_DIGEST(ROT_CREATOR_AUTH_DIGEST);
        CASE_RANGE(ROT_OWNER_AUTH_SLOT0_KEYMANIFEST_KEY);
        CASE_RANGE(ROT_OWNER_AUTH_SLOT0_UNLOCK4XFER_KEY);
        CASE_DIGEST(ROT_OWNER_AUTH_SLOT0_DIGEST);
        CASE_RANGE(ROT_OWNER_AUTH_SLOT1_KEYMANIFEST_KEY);
        CASE_RANGE(ROT_OWNER_AUTH_SLOT1_UNLOCK4XFER_KEY);
        CASE_DIGEST(ROT_OWNER_AUTH_SLOT1_DIGEST);
        CASE_RANGE(PLAT_INTEG_AUTH_SLOT0_KEYMANIFEST_KEY);
        CASE_RANGE(PLAT_INTEG_AUTH_SLOT0_UNLOCK4XFER_KEY);
        CASE_DIGEST(PLAT_INTEG_AUTH_SLOT0_DIGEST);
        CASE_RANGE(PLAT_INTEG_AUTH_SLOT1_KEYMANIFEST_KEY);
        CASE_RANGE(PLAT_INTEG_AUTH_SLOT1_UNLOCK4XFER_KEY);
        CASE_DIGEST(PLAT_INTEG_AUTH_SLOT1_DIGEST);
        CASE_RANGE(PLAT_OWNER_AUTH_SLOT0_KEYMANIFEST_KEY);
        CASE_RANGE(PLAT_OWNER_AUTH_SLOT0_UNLOCK4XFER_KEY);
        CASE_DIGEST(PLAT_OWNER_AUTH_SLOT0_DIGEST);
        CASE_RANGE(PLAT_OWNER_AUTH_SLOT1_KEYMANIFEST_KEY);
        CASE_RANGE(PLAT_OWNER_AUTH_SLOT1_UNLOCK4XFER_KEY);
        CASE_DIGEST(PLAT_OWNER_AUTH_SLOT1_DIGEST);
        CASE_RANGE(PLAT_OWNER_AUTH_SLOT2_KEYMANIFEST_KEY);
        CASE_RANGE(PLAT_OWNER_AUTH_SLOT2_UNLOCK4XFER_KEY);
        CASE_DIGEST(PLAT_OWNER_AUTH_SLOT2_DIGEST);
        CASE_RANGE(PLAT_OWNER_AUTH_SLOT3_KEYMANIFEST_KEY);
        CASE_RANGE(PLAT_OWNER_AUTH_SLOT3_UNLOCK4XFER_KEY);
        CASE_DIGEST(PLAT_OWNER_AUTH_SLOT3_DIGEST);
        CASE_RANGE(EXT_NVM_ANTIREPLAY_FRESHNESS_CNT);
        CASE_RANGE(ROM_PATCH_DATA);
        CASE_DIGEST(ROM_PATCH_DIGEST);
        CASE_RANGE(HW_CFG0_DEVICE_ID);
        CASE_RANGE(HW_CFG0_MANUF_STATE);
        CASE_DIGEST(HW_CFG0_DIGEST);
        CASE_SCALAR(HW_CFG1_SOC_DBG_STATE);
        CASE_SCALAR(HW_CFG1_EN_SRAM_IFETCH);
        CASE_DIGEST(HW_CFG1_DIGEST);
        CASE_RANGE(SECRET0_TEST_UNLOCK_TOKEN);
        CASE_RANGE(SECRET0_TEST_EXIT_TOKEN);
        CASE_DIGEST(SECRET0_DIGEST);
        CASE_RANGE(SECRET1_FLASH_ADDR_KEY_SEED);
        CASE_RANGE(SECRET1_FLASH_DATA_KEY_SEED);
        CASE_RANGE(SECRET1_SRAM_DATA_KEY_SEED);
        CASE_DIGEST(SECRET1_DIGEST);
        CASE_RANGE(SECRET2_RMA_TOKEN);
        CASE_RANGE(SECRET2_CREATOR_ROOT_KEY_SHARE0);
        CASE_RANGE(SECRET2_CREATOR_ROOT_KEY_SHARE1);
        CASE_RANGE(SECRET2_CREATOR_SEED);
        CASE_DIGEST(SECRET2_DIGEST);
        CASE_RANGE(SECRET3_OWNER_SEED);
        CASE_DIGEST(SECRET3_DIGEST);
        CASE_RANGE(LC_TRANSITION_CNT);
        CASE_RANGE(LC_STATE);
    default:
        return "<?>";
    }

#undef CASE_SCALAR
#undef CASE_RANGE
}

static MemTxResult ot_otp_dj_swcfg_read_with_attrs(
    void *opaque, hwaddr addr, uint64_t *data, unsigned size, MemTxAttrs attrs)
{
    OtOTPDjState *s = OT_OTP_DJ(opaque);
    (void)attrs;
    uint32_t val32;

    g_assert(addr + size <= SW_CFG_WINDOW_SIZE);

    hwaddr reg = R32_OFF(addr);
    int partition = ot_otp_dj_get_part_from_address(s, addr);

    if (partition < 0) {
        trace_ot_otp_access_error_on(s->ot_id, partition, addr, "invalid");
        val32 = 0;
    }

    if (ot_otp_dj_is_buffered(partition)) {
        trace_ot_otp_access_error_on(s->ot_id, partition, addr, "buffered");
        ot_otp_dj_set_error(s, (unsigned)partition, OTP_ACCESS_ERROR);

        /* real HW seems to stall the Tile Link bus in this case */
        return MEMTX_ACCESS_ERROR;
    }

    bool is_digest = ot_otp_dj_is_part_digest_offset(partition, addr);
    bool is_readable = ot_otp_dj_is_readable(s, partition);

    if (!is_digest && !is_readable) {
        trace_ot_otp_access_error_on(s->ot_id, partition, addr, "not readable");
        ot_otp_dj_set_error(s, (unsigned)partition, OTP_ACCESS_ERROR);

        return MEMTX_DECODE_ERROR;
    }

    if (!(partition < 0)) {
        val32 = s->otp->data[reg];
        ot_otp_dj_set_error(s, (unsigned)partition, OTP_NO_ERROR);
    }

    uint64_t pc;

    pc = ibex_get_current_pc();
    trace_ot_otp_io_swcfg_read_out(s->ot_id, (uint32_t)addr,
                                   ot_otp_dj_swcfg_reg_name(reg), val32, pc);

    *data = (uint64_t)val32;

    return MEMTX_OK;
}

static void ot_otp_dj_decode_lc_partition(OtOTPDjState *s)
{
    OtOTPStorage *otp = s->otp;
    OtOTPLCIController *lci = s->lci;
    const OtOTPPartDesc *lcdesc = &OtOTPPartDescs[OTP_PART_LIFE_CYCLE];

    g_assert(lcdesc->offset == A_LC_TRANSITION_CNT);

    lci->lc.state = LC_ENCODE_STATE(LC_STATE_INVALID);
    lci->lc.tcount = LC_TRANSITION_COUNT_MAX + 1u;

    for (unsigned ix = 0; ix < ARRAY_SIZE(lc_transition_cnts); ix++) {
        if (!memcmp(&otp->data[R_LC_TRANSITION_CNT], lc_transition_cnts[ix],
                    LC_TRANSITION_CNT_SIZE)) {
            lci->lc.tcount = ix;
            break;
        }
    }

    for (unsigned ix = 0; ix < ARRAY_SIZE(lc_states); ix++) {
        if (!memcmp(&otp->data[R_LC_STATE], lc_states[ix], LC_STATE_SIZE)) {
            lci->lc.state = LC_ENCODE_STATE(ix);
            break;
        }
    }

    trace_ot_otp_initial_lifecycle(s->ot_id, lci->lc.state, lci->lc.tcount);
}

static void ot_otp_dj_load_hw_cfg(OtOTPDjState *s)
{
    OtOTPStorage *otp = s->otp;
    OtOTPHWCfg *hw_cfg = s->hw_cfg;

    memcpy(hw_cfg->device_id, &otp->data[R_HW_CFG0_DEVICE_ID],
           sizeof(*hw_cfg->device_id));
    memcpy(hw_cfg->manuf_state, &otp->data[R_HW_CFG0_MANUF_STATE],
           sizeof(*hw_cfg->manuf_state));

    hw_cfg->soc_dbg_state = otp->data[R_HW_CFG1_SOC_DBG_STATE];
    hw_cfg->en_sram_ifetch = (uint8_t)otp->data[R_HW_CFG1_EN_SRAM_IFETCH];
}

static void ot_otp_dj_load_tokens(OtOTPDjState *s)
{
    memset(s->tokens, 0, sizeof(*s->tokens));

    const uint32_t *data = s->otp->data;
    OtOTPTokens *tokens = s->tokens;

    static_assert(sizeof(OtOTPTokenValue) == 16u, "Invalid token size");

    for (unsigned tkx = 0; tkx < OTP_TOKEN_COUNT; tkx++) {
        unsigned partition;
        uint32_t reg;

        switch (tkx) {
        case OTP_TOKEN_TEST_UNLOCK:
            partition = OTP_PART_SECRET0;
            reg = R_SECRET0_TEST_UNLOCK_TOKEN;
            break;
        case OTP_TOKEN_TEST_EXIT:
            partition = OTP_PART_SECRET0;
            reg = R_SECRET0_TEST_EXIT_TOKEN;
            break;
        case OTP_TOKEN_RMA:
            partition = OTP_PART_SECRET2;
            reg = R_SECRET2_RMA_TOKEN;
            break;
        default:
            g_assert_not_reached();
            break;
        }

        OtOTPTokenValue value;
        memcpy(&value, &data[reg], sizeof(OtOTPTokenValue));
        if (s->partctrls[partition].locked) {
            tokens->values[tkx] = value;
            tokens->valid_bm |= 1u << tkx;
        }
        trace_ot_otp_load_token(s->ot_id, OTP_TOKEN_NAME(tkx), tkx, value.hi,
                                value.lo,
                                (s->tokens->valid_bm & (1u << tkx)) ? "" :
                                                                      "in");
    }
}

static void ot_otp_dj_get_lc_info(
    const OtOTPState *s, uint32_t *lc_state, unsigned *tcount,
    uint8_t *lc_valid, uint8_t *secret_valid, const OtOTPTokens **tokens)
{
    const OtOTPDjState *ds = OT_OTP_DJ(s);
    const OtOTPLCIController *lci = ds->lci;

    if (lc_state) {
        *lc_state = lci->lc.state;
    }
    if (tcount) {
        *tcount = lci->lc.tcount;
    }
    if (lc_valid) {
        *lc_valid = !(ds->partctrls[OTP_PART_SECRET0].failed ||
                      ds->partctrls[OTP_PART_SECRET2].failed ||
                      ds->partctrls[OTP_PART_LIFE_CYCLE].failed) ?
                        OT_MULTIBITBOOL_LC4_TRUE :
                        OT_MULTIBITBOOL_LC4_FALSE;
    }
    if (secret_valid) {
        *secret_valid = (!ds->partctrls[OTP_PART_SECRET2].failed &&
                         ds->partctrls[OTP_PART_SECRET2].locked) ?
                            OT_MULTIBITBOOL_LC4_TRUE :
                            OT_MULTIBITBOOL_LC4_FALSE;
    }
    if (tokens) {
        *tokens = ds->tokens;
    }
}

static const OtOTPHWCfg *ot_otp_dj_get_hw_cfg(const OtOTPState *s)
{
    const OtOTPDjState *ds = OT_OTP_DJ(s);

    return ds->hw_cfg;
}

static const OtOTPEntropyCfg *ot_otp_dj_get_entropy_cfg(const OtOTPState *s)
{
    (void)s;
    /* not present Darjeeling OTP */
    return NULL;
}

static void ot_otp_dj_request_entropy_bh(void *opaque)
{
    OtOTPDjState *s = opaque;

    /*
     * Use a BH as entropy should be filled in as soon as possible after reset.
     * However, as the EDN / OTP reset order is unknown, this initial request
     * can only be performed once the reset sequence is over.
     */
    if (!s->keygen->edn_sched) {
        int rc = ot_edn_request_entropy(s->edn, s->edn_ep);
        g_assert(rc == 0);
        s->keygen->edn_sched = true;
    }
}

static void
ot_otp_dj_keygen_push_entropy(void *opaque, uint32_t bits, bool fips)
{
    OtOTPDjState *s = opaque;
    (void)fips;

    s->keygen->edn_sched = false;

    if (!ot_fifo32_is_full(&s->keygen->entropy_buf)) {
        ot_fifo32_push(&s->keygen->entropy_buf, bits);
    }

    bool resched = !ot_fifo32_is_full(&s->keygen->entropy_buf);

    trace_ot_otp_keygen_entropy(s->ot_id,
                                ot_fifo32_num_used(&s->keygen->entropy_buf),
                                resched);

    if (resched && !s->keygen->edn_sched) {
        qemu_bh_schedule(s->keygen->entropy_bh);
    }
}

static void ot_otp_dj_generate_otp_sram_key(OtOTPDjState *s, OtOTPKey *key)
{
    static_assert(SRAM_NONCE_WORDS * sizeof(uint32_t) < OT_OTP_NONCE_MAX_SIZE,
                  "Invalid nonce size");

    /* fill in Nonce */
    OtFifo32 *entropy = &s->keygen->entropy_buf;
    g_assert(ot_fifo32_num_used(entropy) >= SRAM_NONCE_WORDS);
    for (unsigned ix = 0; ix < SRAM_NONCE_WORDS; ix++) {
        stl_le_p(&key->nonce[ix * sizeof(uint32_t)], ot_fifo32_pop(entropy));
    }
    key->nonce_size = SRAM_NONCE_WORDS * sizeof(uint32_t);

    /* handle key from key seed */

    OtPresentState *ps = s->keygen->present;

    OtOTPPartController *pctrl = &s->partctrls[OTP_PART_SECRET1];
    bool valid = pctrl->locked && !pctrl->failed;
    g_assert(ot_otp_dj_is_buffered(OTP_PART_SECRET1));
    const uint32_t *sram_data_key_seed =
        &pctrl->buffer
             .data[R_SECRET1_SRAM_DATA_KEY_SEED -
                   OtOTPPartDescs[OTP_PART_SECRET1].offset / sizeof(uint32_t)];
    uint32_t tmpkey[SRAM_KEY_WORDS];
    for (unsigned rix = 0; rix < SRAM_KEY_WIDTH / 64u; rix++) {
        uint64_t data = RND_CNST_DIGEST_IV;

        ot_present_init(ps, (const uint8_t *)sram_data_key_seed);
        ot_present_encrypt(ps, data, &data);

        g_assert(ot_fifo32_num_used(entropy) >= SRAM_KEY_WORDS);
        for (unsigned ix = 0; ix < SRAM_NONCE_WORDS; ix++) {
            tmpkey[ix] = ot_fifo32_pop(entropy);
        }
        ot_present_init(ps, (uint8_t *)&tmpkey[0]);
        ot_present_encrypt(ps, data, &data);

        ot_present_init(ps, RND_CNST_DIGEST_CONST);
        ot_present_encrypt(ps, data, &data);

        key->seed[rix] = data;
    }

    key->seed_valid = valid;
    key->seed_size = SRAM_KEY_WIDTH / 8u;

    /* some entropy bits have been used, refill the buffer */
    qemu_bh_schedule(s->keygen->entropy_bh);
}

static void ot_otp_dj_fake_entropy(OtOTPDjState *s, unsigned count)
{
    /*
     * This part departs from real HW: OTP needs to have bufferized enough
     * entropy for any SRAM OTP key request to be successfully completed.
     * On real HW, entropy is requested on demand, but in QEMU this very API
     * (#get_otp_key) needs to be synchronous, as it should be able to complete
     * on SRAM controller I/O request, which is itself fully synchronous.
     * When not enough entropy has been initiatially collected, this function
     * adds some fake entropy to entropy buffer. The main use case is to enable
     * SRAM initialization with random values and does not need to be trully
     * secure, while limiting emulation code size and complexity.
     */

    OtOTPKeyGen *kgen = s->keygen;
    while (count-- && !ot_fifo32_is_full(&kgen->entropy_buf)) {
        ot_fifo32_push(&kgen->entropy_buf, ot_prng_random_u32(kgen->prng));
    }
}

static void ot_otp_dj_get_otp_key(OtOTPState *s, OtOTPKeyType type,
                                  OtOTPKey *key)
{
    OtOTPDjState *ds = OT_OTP_DJ(s);

    unsigned avail_entropy = ot_fifo32_num_used(&ds->keygen->entropy_buf);
    unsigned need_entropy;

    switch (type) {
    case OTP_KEY_FLASH_DATA:
    case OTP_KEY_FLASH_ADDR:
        /* there is no flash key on Darjeeling */
        qemu_log_mask(LOG_UNIMP, "%s: flash key is not supported\n", __func__);
        break;
    case OTP_KEY_OTBN:
        memset(key, 0, sizeof(*key));
        break;
    case OTP_KEY_SRAM:
        memcpy(key->seed, RND_CNST_SCRMBL_KEY_INIT.key,
               sizeof(RND_CNST_SCRMBL_KEY_INIT.key));
        memcpy(key->nonce, RND_CNST_SCRMBL_KEY_INIT.nonce,
               sizeof(RND_CNST_SCRMBL_KEY_INIT.nonce));
        key->seed_size = sizeof(RND_CNST_SCRMBL_KEY_INIT.key);
        key->nonce_size = sizeof(RND_CNST_SCRMBL_KEY_INIT.nonce);
        key->seed_valid = false;
        need_entropy = (SRAM_KEY_WIDTH * 2u + SRAM_NONCE_WIDTH) / 32u;
        if (avail_entropy < need_entropy) {
            unsigned count = need_entropy - avail_entropy;
            error_report("%s: not enough entropy for key %d, fake %u words",
                         __func__, type, count);
            ot_otp_dj_fake_entropy(ds, count);
        }
        ot_otp_dj_generate_otp_sram_key(ds, key);
        break;
    default:
        error_report("%s: invalid OTP key type: %d", __func__, type);
        break;
    }
}

static void ot_otp_dj_create_lc_data(OtOTPLCIController *lci, uint32_t lc_state,
                                     unsigned tcount)
{
    if (tcount >= ARRAY_SIZE(lc_transition_cnts)) {
        error_setg(&error_fatal, "%s: Invalid tcount: 0x%08x", __func__,
                   lc_state);
        /* linter does not know error_setg never returns */
        return;
    }

    unsigned lcix = LC_STATE_BITS(lc_state);
    if (LC_ENCODE_STATE(lcix) != lc_state || lcix >= ARRAY_SIZE(lc_states)) {
        error_setg(&error_fatal, "%s: Invalid lc_state: 0x%08x", __func__,
                   lc_state);
        /* linter does not know error_setg never returns */
        return;
    }

    unsigned hpos = 0;
    memcpy(&lci->data[hpos], lc_transition_cnts[tcount],
           sizeof(lc_transition_cnts[tcount]));
    hpos += sizeof(lc_transition_cnts[lcix]) / sizeof(uint16_t);
    memcpy(&lci->data[hpos], lc_states[lcix], sizeof(lc_states[lcix]));
    hpos += sizeof(lc_states[lcix]) / sizeof(uint16_t);
    g_assert(hpos ==
             OtOTPPartDescs[OTP_PART_LIFE_CYCLE].size / sizeof(uint16_t));

    /* current position in LC buffer to write to backend */
    lci->hpos = 0;
}

static bool ot_otp_dj_program_req(OtOTPState *s, uint32_t lc_state,
                                  unsigned tcount, ot_otp_program_ack_fn ack,
                                  void *opaque)
{
    OtOTPDjState *ds = OT_OTP_DJ(s);
    OtOTPLCIController *lci = ds->lci;

    switch (lci->state) {
    case OTP_LCI_IDLE:
    case OTP_LCI_ERROR:
        /* error case is handled asynchronously */
        g_assert(!(lci->ack_fn || lci->ack_data));
        break;
    case OTP_LCI_WRITE:
    case OTP_LCI_WRITE_WAIT:
        /* another LC programming request is on-going */
        return false;
    case OTP_LCI_RESET:
        /* cannot reach this point if PwrMgr init has been executed */
    default:
        g_assert_not_reached();
        break;
    }

    lci->ack_fn = ack;
    lci->ack_data = opaque;

    if (lci->state == OTP_LCI_IDLE) {
        ot_otp_dj_create_lc_data(lci, lc_state, tcount);
    }

    /*
     * schedule even if LCI FSM is already in error to report the issue
     * asynchronously
     */
    qemu_bh_schedule(lci->prog_bh);

    return true;
}

static void ot_otp_dj_lci_write_complete(OtOTPDjState *s, bool success)
{
    OtOTPLCIController *lci = s->lci;

    if (lci->hpos) {
        /*
         * if the LC partition has been modified somehow, even if the request
         * has failed, update the backend file
         */
        const OtOTPPartDesc *lcdesc = &OtOTPPartDescs[OTP_PART_LIFE_CYCLE];
        unsigned lc_off = lcdesc->offset / sizeof(uint32_t);
        uintptr_t offset = (uintptr_t)s->otp->data - (uintptr_t)s->otp->storage;
        if (ot_otp_dj_write_backend(s, &s->otp->data[lc_off],
                                    (unsigned)(offset + lcdesc->offset),
                                    lcdesc->size)) {
            error_report("%s: cannot update OTP backend", __func__);
            if (lci->error == OTP_NO_ERROR) {
                lci->error = OTP_MACRO_ERROR;
                LCI_CHANGE_STATE(s, OTP_LCI_ERROR);
            }
        }
        if (ot_otp_dj_is_ecc_enabled(s)) {
            offset = (uintptr_t)s->otp->ecc - (uintptr_t)s->otp->storage;
            if (ot_otp_dj_write_backend(s, &((uint16_t *)&s->otp->ecc)[lc_off],
                                        (unsigned)(offset +
                                                   (lcdesc->offset >> 1u)),
                                        lcdesc->size >> 1u)) {
                error_report("%s: cannot update OTP backend", __func__);
                if (lci->error == OTP_NO_ERROR) {
                    lci->error = OTP_MACRO_ERROR;
                    LCI_CHANGE_STATE(s, OTP_LCI_ERROR);
                }
            }
        }
    }

    g_assert(lci->ack_fn);
    ot_otp_program_ack_fn ack_fn = lci->ack_fn;
    void *ack_data = lci->ack_data;
    lci->ack_fn = NULL;
    lci->ack_data = NULL;
    lci->hpos = 0;

    if (!success && lci->error != OTP_NO_ERROR) {
        ot_otp_dj_set_error(s, OTP_PART_LIFE_CYCLE, lci->error);
    }

    (*ack_fn)(ack_data, success);
}

static void ot_otp_dj_lci_write_word(void *opaque)
{
    OtOTPDjState *s = OT_OTP_DJ(opaque);
    OtOTPLCIController *lci = s->lci;
    const OtOTPPartDesc *lcdesc = &OtOTPPartDescs[OTP_PART_LIFE_CYCLE];

    /* should not be called if already in error */
    if (lci->state == OTP_LCI_ERROR) {
        lci->error = OTP_FSM_STATE_ERROR;
        ot_otp_dj_lci_write_complete(s, false);
        return;
    }

    if (!ot_otp_dj_is_backend_writable(s)) {
        /* OTP backend missing or read-only; reject any write request */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: OTP backend file is missing or R/O\n", __func__);
        lci->error = OTP_MACRO_ERROR;
        LCI_CHANGE_STATE(s, OTP_LCI_ERROR);
        ot_otp_dj_lci_write_complete(s, false);
        /* abort immediately */
        return;
    }

    if (lci->hpos >= lcdesc->size / sizeof(uint16_t)) {
        /* the whole LC partition has been updated */
        if (lci->error == OTP_NO_ERROR) {
            LCI_CHANGE_STATE(s, OTP_LCI_IDLE);
            ot_otp_dj_lci_write_complete(s, true);
        } else {
            LCI_CHANGE_STATE(s, OTP_LCI_ERROR);
            ot_otp_dj_lci_write_complete(s, false);
        }
        return;
    }

    LCI_CHANGE_STATE(s, OTP_LCI_WRITE);

    uint16_t *lc_dst =
        (uint16_t *)&s->otp->data[lcdesc->offset / sizeof(uint32_t)];

    uint16_t cur_val = lc_dst[lci->hpos];
    uint16_t new_val = lci->data[lci->hpos];

    trace_ot_otp_lci_write(s->ot_id, lci->hpos, cur_val, new_val);

    if (cur_val & ~new_val) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Cannot clear OTP bits @ %u: 0x%04x / 0x%04x\n",
                      __func__, lci->hpos, cur_val, new_val);
        if (lci->error == OTP_NO_ERROR) {
            lci->error = OTP_MACRO_WRITE_BLANK_ERROR;
        }
        /*
         * "Note that if errors occur, we aggregate the error code but still
         *  attempt to program all remaining words. This is done to ensure that
         *  a life cycle state with ECC correctable errors in some words can
         * still be scrapped."
         */
        new_val |= cur_val;
    }

    lc_dst[lci->hpos] = new_val;

    if (ot_otp_dj_is_ecc_enabled(s)) {
        uint8_t *lc_edst =
            (uint8_t *)&s->otp->ecc[lcdesc->offset / (2u * sizeof(uint32_t))];
        uint8_t cur_ecc = lc_edst[lci->hpos];
        uint8_t new_ecc = ot_otp_dj_compute_ecc_u16(new_val);

        trace_ot_otp_lci_write_ecc(s->ot_id, lci->hpos, cur_ecc, new_ecc);

        if (cur_ecc & ~new_ecc) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: Cannot clear OTP ECC @ %u: 0x%02x / 0x%02x\n",
                          __func__, lci->hpos, cur_ecc, new_ecc);
            if (lci->error == OTP_NO_ERROR) {
                lci->error = OTP_MACRO_WRITE_BLANK_ERROR;
            }
            new_ecc |= cur_ecc;
        }

        lc_edst[lci->hpos] = new_ecc;
    }

    lci->hpos += 1;

    timer_mod(lci->prog_delay,
              qemu_clock_get_ns(OT_VIRTUAL_CLOCK) + LCI_PROG_DELAY_NS);

    LCI_CHANGE_STATE(s, OTP_LCI_WRITE_WAIT);
}

static void ot_otp_dj_pwr_otp_req(void *opaque, int n, int level)
{
    OtOTPDjState *s = opaque;

    g_assert(n == 0);

    if (level) {
        trace_ot_otp_pwr_otp_req(s->ot_id, "signaled");
        qemu_bh_schedule(s->pwr_otp_bh);
    }
}

static void ot_otp_dj_pwr_otp_bh(void *opaque)
{
    OtOTPDjState *s = opaque;

    trace_ot_otp_pwr_otp_req(s->ot_id, "initialize");

    ot_otp_dj_initialize_partitions(s);
    ot_otp_dj_decode_lc_partition(s);
    ot_otp_dj_load_hw_cfg(s);
    ot_otp_dj_load_tokens(s);

    ot_otp_dj_dai_init(s);
    ot_otp_dj_lci_init(s);

    trace_ot_otp_pwr_otp_req(s->ot_id, "done");

    ibex_irq_set(&s->pwc_otp_rsp, 1);
    ibex_irq_set(&s->pwc_otp_rsp, 0);
}

static void ot_otp_dj_load(OtOTPDjState *s, Error **errp)
{
    /*
     * HEADER_FORMAT
     *
     *  | magic    |     4 char | "vOFTP"                                |
     *  | hlength  |   uint32_t | count of header bytes after this point |
     *  | version  |   uint32_t | version of the header (v2)             |
     *  | eccbits  |   uint16_t | ECC size in bits                       |
     *  | eccgran  |   uint16_t | ECC granule                            |
     *  | dlength  |   uint32_t | count of data bytes (% uint64_t)       |
     *  | elength  |   uint32_t | count of ecc bytes (% uint64_t)        |
     *  | -------- | ---------- | only in V2                             |
     *  | dig_iv   |  8 uint8_t | Present digest initialization vector   |
     *  | dig_iv   | 16 uint8_t | Present digest initialization vector   |
     */

    struct otp_header {
        char magic[4];
        uint32_t hlength;
        uint32_t version;
        uint16_t eccbits;
        uint16_t eccgran;
        uint32_t data_len;
        uint32_t ecc_len;
        /* added in V2 */
        uint8_t digest_iv[8u];
        uint8_t digest_constant[16u];
    };

    /* data following header should always be 64-bit aligned */
    static_assert((sizeof(struct otp_header) % sizeof(uint64_t)) == 0,
                  "invalid header definition");

    size_t header_size = sizeof(struct otp_header);
    size_t data_size = 0u;
    size_t ecc_size = 0u;

    for (unsigned ix = 0u; ix < OTP_PART_COUNT; ix++) {
        size_t psize = (size_t)OtOTPPartDescs[ix].size;
        size_t dsize = ROUND_UP(psize, sizeof(uint64_t));
        data_size += dsize;
        /* up to 1 ECC byte for 2 data bytes */
        ecc_size += DIV_ROUND_UP(dsize, 2u);
    }
    size_t otp_size = header_size + data_size + ecc_size;

    otp_size = ROUND_UP(otp_size, 4096u);

    OtOTPStorage *otp = s->otp;

    /* always allocates the requested size even if blk is NULL */
    otp->storage = blk_blockalign(s->blk, otp_size);
    uintptr_t base = (uintptr_t)otp->storage;
    g_assert(!(base & (sizeof(uint64_t) - 1u)));

    memset(otp->storage, 0, otp_size);

    otp->data = (uint32_t *)(base + sizeof(struct otp_header));
    otp->ecc = (uint32_t *)(base + sizeof(struct otp_header) + data_size);
    otp->ecc_bit_count = 0u;
    otp->ecc_granule = 0u;
    memset(&otp->digest_iv, 0, sizeof(otp->digest_iv));
    memset(otp->digest_constant, 0, sizeof(otp->digest_constant));

    if (s->blk) {
        bool write = blk_supports_write_perm(s->blk);
        uint64_t perm = BLK_PERM_CONSISTENT_READ | (write ? BLK_PERM_WRITE : 0);
        if (blk_set_perm(s->blk, perm, perm, errp)) {
            warn_report("%s: OTP backend is R/O", __func__);
            write = false;
        }

        int rc = blk_pread(s->blk, 0, (int64_t)otp_size, otp->storage, 0);
        if (rc < 0) {
            error_setg(errp, "failed to read the initial OTP content: %d", rc);
            return;
        }

        const struct otp_header *otp_hdr = (const struct otp_header *)base;

        if (memcmp(otp_hdr->magic, "vOTP", sizeof(otp_hdr->magic)) != 0) {
            error_setg(errp, "OTP file is not a valid OTP backend");
            return;
        }
        if (otp_hdr->version != 1u && otp_hdr->version != 2u) {
            error_setg(errp, "OTP file version %u is not supported",
                       otp_hdr->version);
            return;
        }

        uintptr_t data_offset = otp_hdr->hlength + 8u; /* magic & length */
        uintptr_t ecc_offset = data_offset + otp_hdr->data_len;

        otp->data = (uint32_t *)(base + data_offset);
        otp->ecc = (uint32_t *)(base + ecc_offset);
        otp->ecc_bit_count = otp_hdr->eccbits;
        otp->ecc_granule = otp_hdr->eccgran;

        if (otp->ecc_bit_count != 6u || !ot_otp_dj_is_ecc_enabled(s)) {
            qemu_log_mask(LOG_UNIMP,
                          "%s: support for ECC %u/%u not implemented\n",
                          __func__, otp->ecc_granule, otp->ecc_bit_count);
        }

        trace_ot_otp_load_backend(s->ot_id, otp_hdr->version,
                                  write ? "R/W" : "R/O", otp->ecc_bit_count,
                                  otp->ecc_granule);

        if (otp_hdr->version > 1u) {
            otp->digest_iv = ldq_le_p(otp_hdr->digest_iv);
            memcpy(otp->digest_constant, otp_hdr->digest_constant,
                   sizeof(otp->digest_constant));
        }
    }

    otp->data_size = data_size;
    otp->ecc_size = ecc_size;
}

static Property ot_otp_dj_properties[] = {
    DEFINE_PROP_STRING("ot_id", OtOTPDjState, ot_id),
    DEFINE_PROP_DRIVE("drive", OtOTPDjState, blk),
    DEFINE_PROP_LINK("backend", OtOTPDjState, otp_backend, TYPE_OT_OTP_BE_IF,
                     OtOtpBeIf *),
    DEFINE_PROP_LINK("edn", OtOTPDjState, edn, TYPE_OT_EDN, OtEDNState *),
    DEFINE_PROP_UINT8("edn-ep", OtOTPDjState, edn_ep, UINT8_MAX),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_otp_dj_reg_ops = {
    .read = &ot_otp_dj_reg_read,
    .write = &ot_otp_dj_reg_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static const MemoryRegionOps ot_otp_dj_swcfg_ops = {
    .read_with_attrs = &ot_otp_dj_swcfg_read_with_attrs,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void ot_otp_dj_reset(DeviceState *dev)
{
    OtOTPDjState *s = OT_OTP_DJ(dev);

    trace_ot_otp_reset(s->ot_id);

    timer_del(s->dai->delay);
    timer_del(s->lci->prog_delay);
    qemu_bh_cancel(s->keygen->entropy_bh);
    s->keygen->edn_sched = false;

    ot_edn_connect_endpoint(s->edn, s->edn_ep, &ot_otp_dj_keygen_push_entropy,
                            s);

    memset(s->regs, 0, REGS_COUNT * sizeof(uint32_t));

    s->regs[R_DIRECT_ACCESS_REGWEN] = 0x1u;
    s->regs[R_CHECK_TRIGGER_REGWEN] = 0x1u;
    s->regs[R_CHECK_REGWEN] = 0x1u;
    s->regs[R_VENDOR_TEST_READ_LOCK] = 0x1u;
    s->regs[R_CREATOR_SW_CFG_READ_LOCK] = 0x1u;
    s->regs[R_OWNER_SW_CFG_READ_LOCK] = 0x1u;
    s->regs[R_OWNERSHIP_SLOT_STATE_READ_LOCK] = 0x1u;
    s->regs[R_ROT_CREATOR_AUTH_READ_LOCK] = 0x1u;
    s->regs[R_ROT_OWNER_AUTH_SLOT0_READ_LOCK] = 0x1u;
    s->regs[R_ROT_OWNER_AUTH_SLOT1_READ_LOCK] = 0x1u;
    s->regs[R_PLAT_INTEG_AUTH_SLOT0_READ_LOCK] = 0x1u;
    s->regs[R_PLAT_INTEG_AUTH_SLOT1_READ_LOCK] = 0x1u;
    s->regs[R_PLAT_OWNER_AUTH_SLOT0_READ_LOCK] = 0x1u;
    s->regs[R_PLAT_OWNER_AUTH_SLOT1_READ_LOCK] = 0x1u;
    s->regs[R_PLAT_OWNER_AUTH_SLOT2_READ_LOCK] = 0x1u;
    s->regs[R_PLAT_OWNER_AUTH_SLOT3_READ_LOCK] = 0x1u;
    s->regs[R_EXT_NVM_READ_LOCK] = 0x1u;
    s->regs[R_ROM_PATCH_READ_LOCK] = 0x1u;

    s->alert_bm = 0u;

    s->lc_broadcast.level = 0u;
    s->lc_broadcast.signal = 0u;

    ot_otp_dj_update_irqs(s);
    ot_otp_dj_update_alerts(s);
    ibex_irq_set(&s->pwc_otp_rsp, 0);

    for (unsigned ix = 0; ix < OTP_PART_COUNT; ix++) {
        /* TODO: initialize with actual default partition data once known */
        if (OtOTPPartDescs[ix].buffered) {
            s->partctrls[ix].state.b = OTP_BUF_IDLE;
        } else {
            s->partctrls[ix].state.u = OTP_UNBUF_IDLE;
            continue;
        }
        unsigned part_size = OT_OTP_PART_DATA_BYTE_SIZE(ix);
        memset(s->partctrls[ix].buffer.data, 0, part_size);
        s->partctrls[ix].buffer.digest = 0;
        if (OtOTPPartDescs[ix].iskeymgr_creator ||
            OtOTPPartDescs[ix].iskeymgr_owner) {
            s->partctrls[ix].read_lock = true;
            s->partctrls[ix].write_lock = true;
        }
    }
    DAI_CHANGE_STATE(s, OTP_DAI_RESET);
    LCI_CHANGE_STATE(s, OTP_LCI_RESET);

    qemu_bh_schedule(s->keygen->entropy_bh);
}

static void ot_otp_dj_realize(DeviceState *dev, Error **errp)
{
    (void)errp;
    OtOTPDjState *s = OT_OTP_DJ(dev);

    if (!s->ot_id) {
        s->ot_id = g_strdup("otp");
    }

    ot_otp_dj_load(s, &error_fatal);
}

static void ot_otp_dj_init(Object *obj)
{
    OtOTPDjState *s = OT_OTP_DJ(obj);

    /*
     * "ctrl" region covers two sub-regions:
     *   - "regs", registers:
     *     offset 0, size REGS_SIZE
     *   - "swcfg", software config window
     *     offset SW_CFG_WINDOW, size SW_CFG_WINDOW_SIZE
     */
    memory_region_init(&s->mmio.ctrl, obj, TYPE_OT_OTP "-ctrl",
                       SW_CFG_WINDOW + SW_CFG_WINDOW_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio.ctrl);

    memory_region_init_io(&s->mmio.sub.regs, obj, &ot_otp_dj_reg_ops, s,
                          TYPE_OT_OTP "-regs", REGS_SIZE);
    memory_region_add_subregion(&s->mmio.ctrl, 0u, &s->mmio.sub.regs);

    /* TODO: it might be worthwhile to use a ROM-kind here */
    memory_region_init_io(&s->mmio.sub.swcfg, obj, &ot_otp_dj_swcfg_ops, s,
                          TYPE_OT_OTP "-swcfg", SW_CFG_WINDOW_SIZE);
    memory_region_add_subregion(&s->mmio.ctrl, SW_CFG_WINDOW,
                                &s->mmio.sub.swcfg);

    ibex_qdev_init_irq(obj, &s->pwc_otp_rsp, OT_PWRMGR_OTP_RSP);

    qdev_init_gpio_in_named(DEVICE(obj), &ot_otp_dj_pwr_otp_req,
                            OT_PWRMGR_OTP_REQ, 1);

    for (unsigned ix = 0; ix < ARRAY_SIZE(s->irqs); ix++) {
        ibex_sysbus_init_irq(obj, &s->irqs[ix]);
    }
    for (unsigned ix = 0; ix < ARRAY_SIZE(s->alerts); ix++) {
        ibex_qdev_init_irq(obj, &s->alerts[ix], OT_DEVICE_ALERT);
    }

    qdev_init_gpio_in_named(DEVICE(obj), &ot_otp_dj_lc_broadcast_recv,
                            OT_LC_BROADCAST, OT_OTP_LC_BROADCAST_COUNT);

    s->hw_cfg = g_new0(OtOTPHWCfg, 1u);
    s->tokens = g_new0(OtOTPTokens, 1u);
    s->regs = g_new0(uint32_t, REGS_COUNT);
    s->dai = g_new0(OtOTPDAIController, 1u);
    s->lci = g_new0(OtOTPLCIController, 1u);
    s->partctrls = g_new0(OtOTPPartController, OTP_PART_COUNT);
    s->keygen = g_new0(OtOTPKeyGen, 1u);
    s->otp = g_new0(OtOTPStorage, 1u);

    for (unsigned ix = 0; ix < OTP_PART_COUNT; ix++) {
        if (!OtOTPPartDescs[ix].buffered) {
            continue;
        }
        size_t part_words = OT_OTP_PART_DATA_BYTE_SIZE(ix) / sizeof(uint32_t);
        s->partctrls[ix].buffer.data = g_new0(uint32_t, part_words);
    }

    ot_fifo32_create(&s->keygen->entropy_buf, OTP_ENTROPY_BUF_COUNT);
    s->keygen->present = ot_present_new();
    s->keygen->prng = ot_prng_allocate();

    s->dai->delay = timer_new_ns(OT_VIRTUAL_CLOCK, &ot_otp_dj_dai_complete, s);
    s->dai->digest_bh = qemu_bh_new(&ot_otp_dj_dai_write_digest, s);
    s->lci->prog_delay =
        timer_new_ns(OT_VIRTUAL_CLOCK, &ot_otp_dj_lci_write_word, s);
    s->lci->prog_bh = qemu_bh_new(&ot_otp_dj_lci_write_word, s);
    s->pwr_otp_bh = qemu_bh_new(&ot_otp_dj_pwr_otp_bh, s);
    s->lc_broadcast.bh = qemu_bh_new(&ot_otp_dj_lc_broadcast_bh, s);
    s->keygen->entropy_bh = qemu_bh_new(&ot_otp_dj_request_entropy_bh, s);

    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    ot_prng_reseed(s->keygen->prng, (uint32_t)now);
}

static void ot_otp_dj_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    g_assert(OTP_PART_LIFE_CYCLE_SIZE ==
             OtOTPPartDescs[OTP_PART_LIFE_CYCLE].size);

    dc->reset = &ot_otp_dj_reset;
    dc->realize = &ot_otp_dj_realize;
    device_class_set_props(dc, ot_otp_dj_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    OtOTPStateClass *odc = OT_OTP_CLASS(klass);

    odc->get_lc_info = &ot_otp_dj_get_lc_info;
    odc->get_hw_cfg = &ot_otp_dj_get_hw_cfg;
    odc->get_entropy_cfg = &ot_otp_dj_get_entropy_cfg;
    odc->get_otp_key = &ot_otp_dj_get_otp_key;
    odc->program_req = &ot_otp_dj_program_req;
}

static const TypeInfo ot_otp_dj_info = {
    .name = TYPE_OT_OTP_DJ,
    .parent = TYPE_OT_OTP,
    .instance_size = sizeof(OtOTPDjState),
    .instance_init = &ot_otp_dj_init,
    .class_size = sizeof(OtOTPStateClass),
    .class_init = &ot_otp_dj_class_init,
};

static void ot_otp_dj_register_types(void)
{
    type_register_static(&ot_otp_dj_info);
}

type_init(ot_otp_dj_register_types);
