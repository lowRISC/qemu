/*
 * QEMU OpenTitan Key Manager device
 *
 * Copyright (c) 2025 lowRISC contributors.
 * Copyright (c) 2025 Rivos, Inc.
 *
 * Author(s):
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

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "hw/opentitan/ot_aes.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_edn.h"
#include "hw/opentitan/ot_flash.h"
#include "hw/opentitan/ot_key_sink.h"
#include "hw/opentitan/ot_keymgr.h"
#include "hw/opentitan/ot_kmac.h"
#include "hw/opentitan/ot_lc_ctrl.h"
#include "hw/opentitan/ot_otbn.h"
#include "hw/opentitan/ot_otp.h"
#include "hw/opentitan/ot_prng.h"
#include "hw/opentitan/ot_rom_ctrl.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "trace.h"

/* properties from keymgr.hjson */
#define NUM_SALT_REG       8u
#define NUM_SW_BINDING_REG 8u
#define NUM_OUTPUT_REG     8u

/* properties from keymgr_pkg.sv */
#define NUM_CDIS                  2u
#define NUM_KEY_SHARES            2u
#define KEYMGR_KEY_WIDTH          256u
#define KEYMGR_KEY_BYTES          ((KEYMGR_KEY_WIDTH) / 8u)
#define KEYMGR_SW_BINDING_WIDTH   ((NUM_SW_BINDING_REG) * 32u)
#define KEYMGR_SALT_WIDTH         ((NUM_SALT_REG) * 32u)
#define KEYMGR_KEY_VERSION_WIDTH  32u
#define KEYMGR_HEALTH_STATE_WIDTH 128u
#define KEYMGR_DEV_ID_WIDTH       256u
#define KEYMGR_LFSR_WIDTH         64u

#define KEYMGR_SALT_BYTES       ((NUM_SALT_REG) * sizeof(uint32_t))
#define KEYMGR_SW_BINDING_BYTES ((NUM_SW_BINDING_REG) * sizeof(uint32_t))

/* the largest Advance input data used across all keymgr stages */
#define KEYMGR_ADV_DATA_BYTES \
    ((KEYMGR_SW_BINDING_WIDTH + (2 * (KEYMGR_KEY_WIDTH)) + \
      KEYMGR_DEV_ID_WIDTH + KEYMGR_HEALTH_STATE_WIDTH) / \
     8u)

#define KEYMGR_ID_DATA_BYTES (KEYMGR_KEY_BYTES)

#define KEYMGR_GEN_DATA_BYTES \
    ((KEYMGR_KEY_VERSION_WIDTH + KEYMGR_SALT_WIDTH + KEYMGR_KEY_WIDTH * 2u) / \
     8u)

#define KEYMGR_KDF_BUFFER_WIDTH 1600u
#define KEYMGR_KDF_BUFFER_BYTES ((KEYMGR_KDF_BUFFER_WIDTH) / 8u)
#define KEYMGR_SEED_BYTES       (KEYMGR_KEY_BYTES)
#define _MAX(_a_, _b_)          ((_a_) > (_b_) ? (_a_) : (_b_))
#define KEYMGR_KEY_SIZE_MAX     _MAX(KEYMGR_KEY_BYTES, OT_OTBN_KEY_SIZE)

/*
 * The EDN provides words of entropy at a time, so the keymgr needs
 * to send multiple entropy requests to reseeds its internal LFSR.
 */
#define KEYMGR_RESEED_COUNT (KEYMGR_LFSR_WIDTH / (8u * sizeof(uint32_t)))

static_assert(KEYMGR_ADV_DATA_BYTES <= KEYMGR_KDF_BUFFER_BYTES,
              "KeyMgr ADV data does not fit in KDF buffer");
static_assert(KEYMGR_ID_DATA_BYTES <= KEYMGR_KDF_BUFFER_BYTES,
              "KeyMgr ID data does not fit in KDF buffer");
static_assert(KEYMGR_GEN_DATA_BYTES <= KEYMGR_KDF_BUFFER_BYTES,
              "KeyMgr GEN data does not fit in KDF buffer");
static_assert((KEYMGR_KDF_BUFFER_BYTES % OT_KMAC_APP_MSG_BYTES) == 0u,
              "KeyMgr KDF buffer not a multiple of KMAC message size");
/* NOLINTBEGIN(misc-redundant-expression) */
static_assert(KEYMGR_KEY_BYTES == OT_OTP_KEYMGR_SECRET_SIZE,
              "KeyMgr key size does not match OTP KeyMgr secret size");
static_assert(KEYMGR_KEY_BYTES == OT_KMAC_KEY_SIZE,
              "KeyMgr key size does not match KMAC key size");
/* NOLINTEND(misc-redundant-expression) */
static_assert(KEYMGR_KEY_SIZE_MAX <= OT_KMAC_APP_DIGEST_BYTES,
              "KeyMgr key size does not match KMAC digest size");

#define KEYMGR_ENTROPY_WIDTH  (KEYMGR_LFSR_WIDTH / 2u)
#define KEYMGR_ENTROPY_ROUNDS (KEYMGR_KEY_WIDTH / KEYMGR_ENTROPY_WIDTH)

#define KEYMGR_LFSR_SEED_BYTES ((KEYMGR_LFSR_WIDTH) / 8u)

static_assert(KEYMGR_LFSR_SEED_BYTES <= KEYMGR_SEED_BYTES,
              "Keymgr LFSR seed is larger than generic KeyMgr seed size");

/* Use a timer with a small delay instead of a BH for reliability */
#define FSM_TICK_DELAY 200u /* 200 ns */

/* clang-format off */
REG32(INTR_STATE, 0x0u)
    SHARED_FIELD(INTR_OP_DONE, 0u, 1u)
REG32(INTR_ENABLE, 0x4u)
REG32(INTR_TEST, 0x8u)
REG32(ALERT_TEST, 0xcu)
    FIELD(ALERT_TEST, RECOV_OPERATION_ERR, 0u, 1u)
    FIELD(ALERT_TEST, FATAL_FAULT_ERR, 1u, 1u)
REG32(CFG_REGWEN, 0x10u)
    FIELD(CFG_REGWEN, EN, 0u, 1u)
REG32(START, 0x14u)
    FIELD(START, EN, 0u, 1u)
REG32(CONTROL_SHADOWED, 0x18u)
    FIELD(CONTROL_SHADOWED, OPERATION, 4u, 3u)
    FIELD(CONTROL_SHADOWED, CDI_SEL, 7u, 1u)
    FIELD(CONTROL_SHADOWED, DEST_SEL, 12u, 2u)
REG32(SIDELOAD_CLEAR, 0x1cu)
    FIELD(SIDELOAD_CLEAR, VAL, 0u, 3u)
REG32(RESEED_INTERVAL_REGWEN, 0x20u)
    FIELD(RESEED_INTERVAL_REGWEN, EN, 0u, 1u)
REG32(RESEED_INTERVAL_SHADOWED, 0x24u)
    FIELD(RESEED_INTERVAL_SHADOWED, VAL, 0u, 16u)
REG32(SW_BINDING_REGWEN, 0x28u)
    FIELD(SW_BINDING_REGWEN, EN, 0u, 1u)
REG32(SEALING_SW_BINDING_0, 0x2cu)
REG32(SEALING_SW_BINDING_1, 0x30u)
REG32(SEALING_SW_BINDING_2, 0x34u)
REG32(SEALING_SW_BINDING_3, 0x38u)
REG32(SEALING_SW_BINDING_4, 0x3cu)
REG32(SEALING_SW_BINDING_5, 0x40u)
REG32(SEALING_SW_BINDING_6, 0x44u)
REG32(SEALING_SW_BINDING_7, 0x48u)
REG32(ATTEST_SW_BINDING_0, 0x4cu)
REG32(ATTEST_SW_BINDING_1, 0x50u)
REG32(ATTEST_SW_BINDING_2, 0x54u)
REG32(ATTEST_SW_BINDING_3, 0x58u)
REG32(ATTEST_SW_BINDING_4, 0x5cu)
REG32(ATTEST_SW_BINDING_5, 0x60u)
REG32(ATTEST_SW_BINDING_6, 0x64u)
REG32(ATTEST_SW_BINDING_7, 0x68u)
REG32(SALT_0, 0x6cu)
REG32(SALT_1, 0x70u)
REG32(SALT_2, 0x74u)
REG32(SALT_3, 0x78u)
REG32(SALT_4, 0x7cu)
REG32(SALT_5, 0x80u)
REG32(SALT_6, 0x84u)
REG32(SALT_7, 0x88u)
REG32(KEY_VERSION, 0x8cu)
REG32(MAX_CREATOR_KEY_VER_REGWEN, 0x90u)
    FIELD(MAX_CREATOR_KEY_VER_REGWEN, EN, 0u, 1u)
REG32(MAX_CREATOR_KEY_VER_SHADOWED, 0x94u)
REG32(MAX_OWNER_INT_KEY_VER_REGWEN, 0x98u)
    FIELD(MAX_OWNER_INT_KEY_VER_REGWEN, EN, 0u, 1u)
REG32(MAX_OWNER_INT_KEY_VER_SHADOWED, 0x9cu)
REG32(MAX_OWNER_KEY_VER_REGWEN, 0xa0u)
    FIELD(MAX_OWNER_KEY_VER_REGWEN, EN, 0u, 1u)
REG32(MAX_OWNER_KEY_VER_SHADOWED, 0xa4u)
REG32(SW_SHARE0_OUTPUT_0, 0xa8u)
REG32(SW_SHARE0_OUTPUT_1, 0xacu)
REG32(SW_SHARE0_OUTPUT_2, 0xb0u)
REG32(SW_SHARE0_OUTPUT_3, 0xb4u)
REG32(SW_SHARE0_OUTPUT_4, 0xb8u)
REG32(SW_SHARE0_OUTPUT_5, 0xbcu)
REG32(SW_SHARE0_OUTPUT_6, 0xc0u)
REG32(SW_SHARE0_OUTPUT_7, 0xc4u)
REG32(SW_SHARE1_OUTPUT_0, 0xc8u)
REG32(SW_SHARE1_OUTPUT_1, 0xccu)
REG32(SW_SHARE1_OUTPUT_2, 0xd0u)
REG32(SW_SHARE1_OUTPUT_3, 0xd4u)
REG32(SW_SHARE1_OUTPUT_4, 0xd8u)
REG32(SW_SHARE1_OUTPUT_5, 0xdcu)
REG32(SW_SHARE1_OUTPUT_6, 0xe0u)
REG32(SW_SHARE1_OUTPUT_7, 0xe4u)
REG32(WORKING_STATE, 0xe8u)
    FIELD(WORKING_STATE, STATE, 0u, 3u)
REG32(OP_STATUS, 0xecu)
    FIELD(OP_STATUS, STATUS, 0u, 2u)
REG32(ERR_CODE, 0xf0u)
    FIELD(ERR_CODE, INVALID_OP, 0u, 1u)
    FIELD(ERR_CODE, INVALID_KMAC_INPUT, 1u, 1u)
    FIELD(ERR_CODE, INVALID_SHADOW_UPDATE, 2u, 1u)
REG32(FAULT_STATUS, 0xf4u)
    FIELD(FAULT_STATUS, CMD, 0u, 1u)
    FIELD(FAULT_STATUS, KMAC_FSM, 1u, 1u)
    FIELD(FAULT_STATUS, KMAC_DONE, 2u, 1u)
    FIELD(FAULT_STATUS, KMAC_OP, 3u, 1u)
    FIELD(FAULT_STATUS, KMAC_OUT, 4u, 1u)
    FIELD(FAULT_STATUS, REGFILE_INTG, 5u, 1u)
    FIELD(FAULT_STATUS, SHADOW, 6u, 1u)
    FIELD(FAULT_STATUS, CTRL_FSM_INTG, 7u, 1u)
    FIELD(FAULT_STATUS, CTRL_FSM_CHK, 8u, 1u)
    FIELD(FAULT_STATUS, CTRL_FSM_CNT, 9u, 1u)
    FIELD(FAULT_STATUS, RESEED_CNT, 10u, 1u)
    FIELD(FAULT_STATUS, SIDE_CTRL_FSM, 11u, 1u)
    FIELD(FAULT_STATUS, SIDE_CTRL_SEL, 12u, 1u)
    FIELD(FAULT_STATUS, KEY_ECC, 13u, 1u)
REG32(DEBUG, 0xf8u)
    FIELD(DEBUG, INVALID_CREATOR_SEED, 0u, 1u)
    FIELD(DEBUG, INVALID_OWNER_SEED, 1u, 1u)
    FIELD(DEBUG, INVALID_DEV_ID, 2u, 1u)
    FIELD(DEBUG, INVALID_HEALTH_STATE, 3u, 1u)
    FIELD(DEBUG, INVALID_KEY_VERSION, 4u, 1u)
    FIELD(DEBUG, INVALID_KEY, 5u, 1u)
    FIELD(DEBUG, INVALID_DIGEST, 6u, 1u)
/* clang-format on */

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_DEBUG)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))

#define INTR_MASK (INTR_OP_DONE_MASK)
#define ALERT_MASK \
    (R_ALERT_TEST_RECOV_OPERATION_ERR_MASK | R_ALERT_TEST_FATAL_FAULT_ERR_MASK)

#define R_CONTROL_SHADOWED_MASK \
    (R_CONTROL_SHADOWED_OPERATION_MASK | R_CONTROL_SHADOWED_CDI_SEL_MASK | \
     R_CONTROL_SHADOWED_DEST_SEL_MASK)

#define ERR_CODE_MASK \
    (R_ERR_CODE_INVALID_OP_MASK | R_ERR_CODE_INVALID_KMAC_INPUT_MASK | \
     R_ERR_CODE_INVALID_SHADOW_UPDATE_MASK)

#define FAULT_STATUS_MASK \
    (R_FAULT_STATUS_CMD_MASK | R_FAULT_STATUS_KMAC_FSM_MASK | \
     R_FAULT_STATUS_KMAC_DONE_MASK | R_FAULT_STATUS_KMAC_OP_MASK | \
     R_FAULT_STATUS_KMAC_OUT_MASK | R_FAULT_STATUS_REGFILE_INTG_MASK | \
     R_FAULT_STATUS_SHADOW_MASK | R_FAULT_STATUS_CTRL_FSM_INTG_MASK | \
     R_FAULT_STATUS_CTRL_FSM_CHK_MASK | R_FAULT_STATUS_CTRL_FSM_CNT_MASK | \
     R_FAULT_STATUS_RESEED_CNT_MASK | R_FAULT_STATUS_SIDE_CTRL_FSM_MASK | \
     R_FAULT_STATUS_SIDE_CTRL_SEL_MASK | R_FAULT_STATUS_KEY_ECC_MASK)

#define DEBUG_MASK \
    (R_DEBUG_INVALID_CREATOR_SEED_MASK | R_DEBUG_INVALID_OWNER_SEED_MASK | \
     R_DEBUG_INVALID_DEV_ID_MASK | R_DEBUG_INVALID_HEALTH_STATE_MASK | \
     R_DEBUG_INVALID_KEY_VERSION_MASK | R_DEBUG_INVALID_KEY_MASK | \
     R_DEBUG_INVALID_DIGEST_MASK)

typedef enum {
    KEYMGR_KEY_SINK_AES,
    KEYMGR_KEY_SINK_KMAC,
    KEYMGR_KEY_SINK_OTBN,
    KEYMGR_KEY_SINK_COUNT
} OtKeyMgrKeySink;

#define KEY_SINK_OFFSET 1

typedef enum {
    KEYMGR_STAGE_CREATOR,
    KEYMGR_STAGE_OWNER_INT,
    KEYMGR_STAGE_OWNER,
    KEYMGR_STAGE_DISABLE,
} OtKeyMgrStage;

/* values for CONTROL_SHADOWED.OPERATION */
typedef enum {
    KEYMGR_OP_ADVANCE = 0,
    KEYMGR_OP_GENERATE_ID = 1,
    KEYMGR_OP_GENERATE_SW_OUTPUT = 2,
    KEYMGR_OP_GENERATE_HW_OUTPUT = 3,
    KEYMGR_OP_DISABLE = 4,
} OtKeyMgrOperation;

/* values for CONTROL_SHADOWED.DEST_SEL */
typedef enum {
    KEYMGR_DEST_SEL_VALUE_NONE = 0,
    KEYMGR_DEST_SEL_VALUE_AES = KEYMGR_KEY_SINK_AES + KEY_SINK_OFFSET,
    KEYMGR_DEST_SEL_VALUE_KMAC = KEYMGR_KEY_SINK_KMAC + KEY_SINK_OFFSET,
    KEYMGR_DEST_SEL_VALUE_OTBN = KEYMGR_KEY_SINK_OTBN + KEY_SINK_OFFSET,
} OtKeyMgrDestSel;

/* values for CONTROL_SHADOWED.CDI_SEL */
typedef enum {
    KEYMGR_CDI_SEALING = 0,
    KEYMGR_CDI_ATTESTATION = 1,
} OtKeyMgrCdi;

/* values for SIDELOAD_CLEAR.VAL */
typedef enum {
    KEYMGR_SIDELOAD_CLEAR_NONE = 0,
    KEYMGR_SIDELOAD_CLEAR_AES = KEYMGR_KEY_SINK_AES + KEY_SINK_OFFSET,
    KEYMGR_SIDELOAD_CLEAR_KMAC = KEYMGR_KEY_SINK_KMAC + KEY_SINK_OFFSET,
    KEYMGR_SIDELOAD_CLEAR_OTBN = KEYMGR_KEY_SINK_OTBN + KEY_SINK_OFFSET,
} OtKeyMgrSideloadClear;

/* values for WORKING_STATE.STATE */
typedef enum {
    KEYMGR_WORKING_STATE_RESET = 0,
    KEYMGR_WORKING_STATE_INIT = 1,
    KEYMGR_WORKING_STATE_CREATOR_ROOT_KEY = 2,
    KEYMGR_WORKING_STATE_OWNER_INTERMEDIATE_KEY = 3,
    KEYMGR_WORKING_STATE_OWNER_KEY = 4,
    KEYMGR_WORKING_STATE_DISABLED = 5,
    KEYMGR_WORKING_STATE_INVALID = 6,
} OtKeyMgrWorkingState;

/* value for OP_STATUS.STATUS */
typedef enum {
    KEYMGR_OP_STATUS_IDLE = 0,
    KEYMGR_OP_STATUS_WIP = 1,
    KEYMGR_OP_STATUS_DONE_SUCCESS = 2,
    KEYMGR_OP_STATUS_DONE_ERROR = 3,
} OtKeyMgrOpStatus;

enum {
    /* clang-format off */
    ALERT_RECOVERABLE,
    ALERT_FATAL,
    ALERT_COUNT
    /* clang-format on */
};

enum {
    KEYMGR_SEED_LFSR,
    KEYMGR_SEED_REV,
    KEYMGR_SEED_CREATOR_IDENTITY,
    KEYMGR_SEED_OWNER_INT_IDENTITY,
    KEYMGR_SEED_OWNER_IDENTITY,
    KEYMGR_SEED_SW_OUT,
    KEYMGR_SEED_HW_OUT,
    KEYMGR_SEED_AES,
    KEYMGR_SEED_KMAC,
    KEYMGR_SEED_OTBN,
    KEYMGR_SEED_CDI,
    KEYMGR_SEED_NONE,
    KEYMGR_SEED_COUNT,
};

typedef enum {
    KEYMGR_ST_RESET,
    KEYMGR_ST_ENTROPY_RESEED,
    KEYMGR_ST_RANDOM,
    KEYMGR_ST_ROOT_KEY,
    KEYMGR_ST_INIT,
    KEYMGR_ST_CREATOR_ROOT_KEY,
    KEYMGR_ST_OWNER_INT_KEY,
    KEYMGR_ST_OWNER_KEY,
    KEYMGR_ST_DISABLED,
    KEYMGR_ST_WIPE,
    KEYMGR_ST_INVALID,
} OtKeyMgrFSMState;

typedef struct {
    OtEDNState *device;
    uint8_t ep;
    bool connected;
    bool scheduled;
} OtKeyMgrEDN;

typedef struct {
    OtPrngState *state;
    bool reseed_req;
    bool reseed_ack;
    uint8_t reseed_cnt;
} OtKeyMgrPrng;

typedef struct {
    uint8_t share0[KEYMGR_KEY_BYTES];
    uint8_t share1[KEYMGR_KEY_BYTES];
    bool valid;
} OtKeyMgrKey;

typedef struct {
    bool op_req;
    bool op_ack;
    bool valid_inputs;
    OtKeyMgrStage stage;
    OtKeyMgrCdi adv_cdi_cnt;
} OtKeyMgrOpState;

typedef struct {
    uint8_t *data;
    unsigned offset; /* current read offset (in bytes ) */
    unsigned length; /* current length (in bytes) */
} OtKeyMgrKdfBuffer;

typedef struct OtKeyMgrState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    IbexIRQ irq;
    IbexIRQ alerts[ALERT_COUNT];
    QEMUTimer *fsm_tick_timer;
    OtKeyMgrKdfBuffer kdf_buf;

    uint32_t regs[REGS_COUNT];
    OtShadowReg control;
    OtShadowReg reseed_interval;
    OtShadowReg max_creator_key_ver;
    OtShadowReg max_owner_int_key_ver;
    OtShadowReg max_owner_key_ver;
    uint8_t *salt;
    uint8_t *sealing_sw_binding;
    uint8_t *attest_sw_binding;

    bool enabled;
    OtKeyMgrFSMState state;
    OtKeyMgrPrng prng;
    OtKeyMgrOpState op_state;
    uint8_t *seeds[KEYMGR_SEED_COUNT];

    /* key states */
    OtKeyMgrKey *key_states;
    OtKeyMgrKey *saved_kmac_key; /* store KMAC key & restore when completing */

    /* SW output keys */
    OtKeyMgrKey *sw_out_key;

    char *hexstr;

    /* properties */
    char *ot_id;
    OtKeyMgrEDN edn;
    OtKMACState *kmac;
    uint8_t kmac_app;
    OtFlashState *flash_ctrl;
    OtLcCtrlState *lc_ctrl;
    OtOTPState *otp_ctrl;
    OtRomCtrlState *rom_ctrl;
    DeviceState *key_sinks[KEYMGR_KEY_SINK_COUNT];
    char *seed_xstrs[KEYMGR_SEED_COUNT];
} OtKeyMgrState;

struct OtKeyMgrClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

static const size_t OT_KEY_MGR_KEY_SINK_SIZES[KEYMGR_KEY_SINK_COUNT] = {
    [KEYMGR_KEY_SINK_AES] = OT_AES_KEY_SIZE,
    [KEYMGR_KEY_SINK_KMAC] = OT_KMAC_KEY_SIZE,
    [KEYMGR_KEY_SINK_OTBN] = OT_OTBN_KEY_SIZE,
};

/* see kmac_pkg::AppCfg in `kmac_pkg.sv` */
static const OtKMACAppCfg KMAC_APP_CFG = OT_KMAC_CONFIG(KMAC, 256u, "KMAC", "");

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    REG_NAME_ENTRY(INTR_STATE),
    REG_NAME_ENTRY(INTR_ENABLE),
    REG_NAME_ENTRY(INTR_TEST),
    REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(CFG_REGWEN),
    REG_NAME_ENTRY(START),
    REG_NAME_ENTRY(CONTROL_SHADOWED),
    REG_NAME_ENTRY(SIDELOAD_CLEAR),
    REG_NAME_ENTRY(RESEED_INTERVAL_REGWEN),
    REG_NAME_ENTRY(RESEED_INTERVAL_SHADOWED),
    REG_NAME_ENTRY(SW_BINDING_REGWEN),
    REG_NAME_ENTRY(SEALING_SW_BINDING_0),
    REG_NAME_ENTRY(SEALING_SW_BINDING_1),
    REG_NAME_ENTRY(SEALING_SW_BINDING_2),
    REG_NAME_ENTRY(SEALING_SW_BINDING_3),
    REG_NAME_ENTRY(SEALING_SW_BINDING_4),
    REG_NAME_ENTRY(SEALING_SW_BINDING_5),
    REG_NAME_ENTRY(SEALING_SW_BINDING_6),
    REG_NAME_ENTRY(SEALING_SW_BINDING_7),
    REG_NAME_ENTRY(ATTEST_SW_BINDING_0),
    REG_NAME_ENTRY(ATTEST_SW_BINDING_1),
    REG_NAME_ENTRY(ATTEST_SW_BINDING_2),
    REG_NAME_ENTRY(ATTEST_SW_BINDING_3),
    REG_NAME_ENTRY(ATTEST_SW_BINDING_4),
    REG_NAME_ENTRY(ATTEST_SW_BINDING_5),
    REG_NAME_ENTRY(ATTEST_SW_BINDING_6),
    REG_NAME_ENTRY(ATTEST_SW_BINDING_7),
    REG_NAME_ENTRY(SALT_0),
    REG_NAME_ENTRY(SALT_1),
    REG_NAME_ENTRY(SALT_2),
    REG_NAME_ENTRY(SALT_3),
    REG_NAME_ENTRY(SALT_4),
    REG_NAME_ENTRY(SALT_5),
    REG_NAME_ENTRY(SALT_6),
    REG_NAME_ENTRY(SALT_7),
    REG_NAME_ENTRY(KEY_VERSION),
    REG_NAME_ENTRY(MAX_CREATOR_KEY_VER_REGWEN),
    REG_NAME_ENTRY(MAX_CREATOR_KEY_VER_SHADOWED),
    REG_NAME_ENTRY(MAX_OWNER_INT_KEY_VER_REGWEN),
    REG_NAME_ENTRY(MAX_OWNER_INT_KEY_VER_SHADOWED),
    REG_NAME_ENTRY(MAX_OWNER_KEY_VER_REGWEN),
    REG_NAME_ENTRY(MAX_OWNER_KEY_VER_SHADOWED),
    REG_NAME_ENTRY(SW_SHARE0_OUTPUT_0),
    REG_NAME_ENTRY(SW_SHARE0_OUTPUT_1),
    REG_NAME_ENTRY(SW_SHARE0_OUTPUT_2),
    REG_NAME_ENTRY(SW_SHARE0_OUTPUT_3),
    REG_NAME_ENTRY(SW_SHARE0_OUTPUT_4),
    REG_NAME_ENTRY(SW_SHARE0_OUTPUT_5),
    REG_NAME_ENTRY(SW_SHARE0_OUTPUT_6),
    REG_NAME_ENTRY(SW_SHARE0_OUTPUT_7),
    REG_NAME_ENTRY(SW_SHARE1_OUTPUT_0),
    REG_NAME_ENTRY(SW_SHARE1_OUTPUT_1),
    REG_NAME_ENTRY(SW_SHARE1_OUTPUT_2),
    REG_NAME_ENTRY(SW_SHARE1_OUTPUT_3),
    REG_NAME_ENTRY(SW_SHARE1_OUTPUT_4),
    REG_NAME_ENTRY(SW_SHARE1_OUTPUT_5),
    REG_NAME_ENTRY(SW_SHARE1_OUTPUT_6),
    REG_NAME_ENTRY(SW_SHARE1_OUTPUT_7),
    REG_NAME_ENTRY(WORKING_STATE),
    REG_NAME_ENTRY(OP_STATUS),
    REG_NAME_ENTRY(ERR_CODE),
    REG_NAME_ENTRY(FAULT_STATUS),
    REG_NAME_ENTRY(DEBUG),
};
#undef REG_NAME_ENTRY
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define STAGE_ENTRY(_st_) [KEYMGR_STAGE_##_st_] = stringify(_st_)
static const char *STAGE_NAMES[] = {
    STAGE_ENTRY(CREATOR),
    STAGE_ENTRY(OWNER_INT),
    STAGE_ENTRY(OWNER),
    STAGE_ENTRY(DISABLE),
};
#undef STAGE_ENTRY
#define STAGE_NAME(_st_) \
    (((unsigned)(_st_)) < ARRAY_SIZE(STAGE_NAMES) ? STAGE_NAMES[(_st_)] : "?")

#define CDI_ENTRY(_cd_) [KEYMGR_CDI_##_cd_] = stringify(_cd_)
static const char *CDI_NAMES[] = {
    CDI_ENTRY(SEALING),
    CDI_ENTRY(ATTESTATION),
};
#undef CDI_ENTRY
#define CDI_NAME(_cd_) \
    (((unsigned)(_cd_)) < ARRAY_SIZE(CDI_NAMES) ? CDI_NAMES[(_cd_)] : "?")

#define OP_ENTRY(_op_) [KEYMGR_OP_##_op_] = stringify(_op_)
static const char *OP_NAMES[] = {
    OP_ENTRY(ADVANCE),
    OP_ENTRY(GENERATE_ID),
    OP_ENTRY(GENERATE_SW_OUTPUT),
    OP_ENTRY(GENERATE_HW_OUTPUT),
    OP_ENTRY(DISABLE),
};
#undef OP_ENTRY
#define OP_NAME(_op_) \
    (((unsigned)(_op_)) < ARRAY_SIZE(OP_NAMES) ? OP_NAMES[(_op_)] : "?")

#define KEY_SINK_ENTRY(_st_) [KEYMGR_KEY_SINK_##_st_] = stringify(_st_)
static const char *KEY_SINK_NAMES[] = {
    KEY_SINK_ENTRY(AES),
    KEY_SINK_ENTRY(KMAC),
    KEY_SINK_ENTRY(OTBN),
};
#undef KEY_SINK_ENTRY
#define KEY_SINK_NAME(_st_) \
    (((unsigned)(_st_)) < ARRAY_SIZE(KEY_SINK_NAMES) ? \
         KEY_SINK_NAMES[(_st_)] : \
         "?")

#define WORKING_STATE_ENTRY(_st_) \
    [KEYMGR_WORKING_STATE_##_st_] = stringify(_st_)
static const char *WORKING_STATE_NAMES[] = {
    WORKING_STATE_ENTRY(RESET),
    WORKING_STATE_ENTRY(INIT),
    WORKING_STATE_ENTRY(CREATOR_ROOT_KEY),
    WORKING_STATE_ENTRY(OWNER_INTERMEDIATE_KEY),
    WORKING_STATE_ENTRY(OWNER_KEY),
    WORKING_STATE_ENTRY(DISABLED),
    WORKING_STATE_ENTRY(INVALID),
};
#undef WORKING_STATE_ENTRY
#define WORKING_STATE_NAME(_st_) \
    (((unsigned)(_st_)) < ARRAY_SIZE(WORKING_STATE_NAMES) ? \
         WORKING_STATE_NAMES[(_st_)] : \
         "?")

#define OP_STATUS_ENTRY(_st_) [KEYMGR_OP_STATUS_##_st_] = stringify(_st_)
static const char *OP_STATUS_NAMES[] = {
    OP_STATUS_ENTRY(IDLE),
    OP_STATUS_ENTRY(WIP),
    OP_STATUS_ENTRY(DONE_SUCCESS),
    OP_STATUS_ENTRY(DONE_ERROR),
};
#undef OP_STATUS_ENTRY
#define OP_STATUS_NAME(_st_) \
    (((unsigned)(_st_)) < ARRAY_SIZE(OP_STATUS_NAMES) ? \
         OP_STATUS_NAMES[(_st_)] : \
         "?")

#define FST_ENTRY(_st_) [KEYMGR_ST_##_st_] = stringify(_st_)
static const char *FST_NAMES[] = {
    /* clang-format off */
    FST_ENTRY(RESET),
    FST_ENTRY(ENTROPY_RESEED),
    FST_ENTRY(RANDOM),
    FST_ENTRY(ROOT_KEY),
    FST_ENTRY(INIT),
    FST_ENTRY(CREATOR_ROOT_KEY),
    FST_ENTRY(OWNER_INT_KEY),
    FST_ENTRY(OWNER_KEY),
    FST_ENTRY(DISABLED),
    FST_ENTRY(WIPE),
    FST_ENTRY(INVALID),
    /* clang-format on */
};
#undef FST_ENTRY
#define FST_NAME(_st_) \
    (((unsigned)(_st_)) < ARRAY_SIZE(FST_NAMES) ? FST_NAMES[(_st_)] : "?")

#define OT_KEYMGR_HEXSTR_SIZE 256u

#define ot_keymgr_dump_bigint(_s_, _b_, _l_) \
    ot_common_lhexdump(_b_, _l_, true, (_s_)->hexstr, OT_KEYMGR_HEXSTR_SIZE)

static void ot_keymgr_dump_kdf_buf(const OtKeyMgrState *s, const char *op)
{
    if (trace_event_get_state(TRACE_OT_KEYMGR_DUMP_KDF_BUF)) {
        size_t msgs = (s->kdf_buf.length + OT_KMAC_APP_MSG_BYTES - 1u) /
                      OT_KMAC_APP_MSG_BYTES;
        for (size_t ix = 0u; ix < msgs; ix++) {
            trace_ot_keymgr_dump_kdf_buf(
                s->ot_id, op, ix,
                ot_keymgr_dump_bigint(s,
                                      &s->kdf_buf
                                           .data[ix * OT_KMAC_APP_MSG_BYTES],
                                      OT_KMAC_APP_MSG_BYTES));
        }
    }
}

static void ot_keymgr_update_irq(OtKeyMgrState *s)
{
    bool level = (bool)(s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE]);
    trace_ot_keymgr_irq(s->ot_id, s->regs[R_INTR_STATE], s->regs[R_INTR_ENABLE],
                        level);
    ibex_irq_set(&s->irq, (int)level);
}

static void ot_keymgr_update_alerts(OtKeyMgrState *s)
{
    uint32_t levels = s->regs[R_ALERT_TEST];

    bool recov_operation = s->regs[R_ERR_CODE] & ERR_CODE_MASK;
    if (recov_operation) {
        levels |= 1u << ALERT_RECOVERABLE;
    }

    bool fatal_fault = s->regs[R_FAULT_STATUS] & FAULT_STATUS_MASK;
    if (fatal_fault) {
        levels |= 1u << ALERT_FATAL;
    }

    for (unsigned ix = 0u; ix < ALERT_COUNT; ix++) {
        int level = (int)((levels >> ix) & 0x1u);
        if (level != ibex_irq_get_level(&s->alerts[ix])) {
            trace_ot_keymgr_update_alert(s->ot_id,
                                         ibex_irq_get_level(&s->alerts[ix]),
                                         level);
        }
        ibex_irq_set(&s->alerts[ix], level);
    }

    if (!s->regs[R_ALERT_TEST] && !recov_operation) {
        return;
    }
    /* ALERT_TEST and recoverable error alerts are transient */
    s->regs[R_ALERT_TEST] = 0u;
    s->regs[R_FAULT_STATUS] &= ~FAULT_STATUS_MASK;
    levels = fatal_fault ? (1u << ALERT_FATAL) : 0u;

    for (unsigned ix = 0u; ix < ALERT_COUNT; ix++) {
        int level = (int)((levels >> ix) & 0x1u);
        if (level != ibex_irq_get_level(&s->alerts[ix])) {
            trace_ot_keymgr_update_alert(s->ot_id,
                                         ibex_irq_get_level(&s->alerts[ix]),
                                         level);
        }
        ibex_irq_set(&s->alerts[ix], level);
    }
}

#define ot_keymgr_change_working_state(_s_, _working_state_) \
    ot_keymgr_xchange_working_state(_s_, _working_state_, __LINE__)

static OtKeyMgrWorkingState ot_keymgr_get_working_state(const OtKeyMgrState *s)
{
    uint32_t working_state =
        FIELD_EX32(s->regs[R_WORKING_STATE], WORKING_STATE, STATE);

    if (working_state <= KEYMGR_WORKING_STATE_INVALID) {
        return working_state;
    }
    return KEYMGR_WORKING_STATE_INVALID;
}

static void ot_keymgr_xchange_working_state(
    OtKeyMgrState *s, OtKeyMgrWorkingState working_state, int line)
{
    OtKeyMgrWorkingState prev_working_state = ot_keymgr_get_working_state(s);

    if (prev_working_state != working_state) {
        trace_ot_keymgr_change_working_state(s->ot_id, line,
                                             WORKING_STATE_NAME(
                                                 prev_working_state),
                                             prev_working_state,
                                             WORKING_STATE_NAME(working_state),
                                             working_state);
        s->regs[R_WORKING_STATE] = working_state;
    }
}

static OtKeyMgrOpStatus ot_keymgr_get_op_status(const OtKeyMgrState *s)
{
    uint32_t op_status = FIELD_EX32(s->regs[R_OP_STATUS], OP_STATUS, STATUS);

    if (op_status <= KEYMGR_OP_STATUS_DONE_ERROR) {
        return op_status;
    }
    g_assert_not_reached();
}

#define ot_keymgr_change_op_status(_s_, _op_status_) \
    ot_keymgr_xchange_op_status(_s_, _op_status_, __LINE__)

static void ot_keymgr_xchange_op_status(OtKeyMgrState *s,
                                        OtKeyMgrOpStatus op_status, int line)
{
    OtKeyMgrOpStatus prev_status = ot_keymgr_get_op_status(s);
    if (prev_status != op_status) {
        trace_ot_keymgr_change_op_status(s->ot_id, line,
                                         OP_STATUS_NAME(prev_status),
                                         prev_status, OP_STATUS_NAME(op_status),
                                         op_status);
        s->regs[R_OP_STATUS] =
            FIELD_DP32(s->regs[R_OP_STATUS], OP_STATUS, STATUS, op_status);
    }
}

#define ot_keymgr_schedule_fsm(_s_) \
    ot_keymgr_xschedule_fsm(_s_, __func__, __LINE__)

static void ot_keymgr_xschedule_fsm(OtKeyMgrState *s, const char *func,
                                    int line)
{
    trace_ot_keymgr_schedule_fsm(s->ot_id, func, line);
    if (!timer_pending(s->fsm_tick_timer)) {
        uint64_t now = qemu_clock_get_ns(OT_VIRTUAL_CLOCK);
        timer_mod(s->fsm_tick_timer, (int64_t)(now + FSM_TICK_DELAY));
    }
}

static void ot_keymgr_request_entropy(OtKeyMgrState *s);

static void ot_keymgr_push_entropy(void *opaque, uint32_t bits, bool fips)
{
    (void)fips;
    OtKeyMgrState *s = opaque;
    OtKeyMgrEDN *edn = &s->edn;
    OtKeyMgrPrng *prng = &s->prng;

    if (!edn->scheduled) {
        trace_ot_keymgr_error(s->ot_id, "Unexpected entropy");
        return;
    }
    edn->scheduled = false;

    ot_prng_reseed(prng->state, bits);
    prng->reseed_cnt++;

    bool reschedule = prng->reseed_cnt < KEYMGR_RESEED_COUNT;

    trace_ot_keymgr_entropy(s->ot_id, prng->reseed_cnt, reschedule);

    if (reschedule) {
        /* we need more entropy */
        ot_keymgr_request_entropy(s);
    } else if (prng->reseed_req) {
        prng->reseed_ack = true;
        prng->reseed_cnt = 0u;
        ot_keymgr_schedule_fsm(s);
    }
}

static void ot_keymgr_request_entropy(OtKeyMgrState *s)
{
    OtKeyMgrEDN *edn = &s->edn;

    if (!edn->connected) {
        ot_edn_connect_endpoint(edn->device, edn->ep, &ot_keymgr_push_entropy,
                                s);
        edn->connected = true;
    }

    if (!edn->scheduled && s->prng.reseed_req) {
        edn->scheduled = true;
        if (ot_edn_request_entropy(edn->device, edn->ep)) {
            error_setg(&error_fatal, "%s: %s: keymgr failed to request entropy",
                       __func__, s->ot_id);
        }
    }
}

static void ot_keymgr_wipe_key_states(OtKeyMgrState *s)
{
    /* @todo: should wipe with random entropy, but just zero for now */
    for (unsigned cdi = 0u; cdi < NUM_CDIS; cdi++) {
        memset(s->key_states[cdi].share0, 0u, KEYMGR_KEY_BYTES);
        memset(s->key_states[cdi].share1, 0u, KEYMGR_KEY_BYTES);
        s->key_states[cdi].valid = false;
    }
    memset(s->sw_out_key->share0, 0u, KEYMGR_KEY_BYTES);
    memset(s->sw_out_key->share1, 0u, KEYMGR_KEY_BYTES);
}

static void ot_keymgr_push_key(
    OtKeyMgrState *s, OtKeyMgrKeySink key_sink, const uint8_t *key_share0,
    const uint8_t *key_share1, bool valid, bool sideload_key)
{
    g_assert((unsigned)key_sink < KEYMGR_KEY_SINK_COUNT);

    size_t key_size = OT_KEY_MGR_KEY_SINK_SIZES[key_sink];
    DeviceState *sink = s->key_sinks[key_sink];

    g_assert(sink);

    /*
     * Save the latest KMAC sideloading key, as it needs to be restored after
     * any keymgr operations that load the KMAC KDF key to offload KDF.
     */
    if (key_share0 && key_share1 && sideload_key &&
        key_sink == KEYMGR_KEY_SINK_KMAC) {
        memcpy(s->saved_kmac_key->share0, key_share0, key_size);
        memcpy(s->saved_kmac_key->share1, key_share1, key_size);
        s->saved_kmac_key->valid = valid;
    }

    if (trace_event_get_state(TRACE_OT_KEYMGR_PUSH_KEY)) {
        if (!key_share0 || !key_share1) {
            trace_ot_keymgr_push_key(s->ot_id, KEY_SINK_NAME(key_sink),
                                     sideload_key, valid, "");
        } else {
            /* compute the unmasked key for tracing */
            uint8_t key_value[KEYMGR_KEY_SIZE_MAX];
            for (unsigned ix = 0u; ix < key_size; ix++) {
                key_value[ix] = key_share0[ix] ^ key_share1[ix];
            }

            trace_ot_keymgr_push_key(s->ot_id, KEY_SINK_NAME(key_sink),
                                     sideload_key, valid,
                                     ot_keymgr_dump_bigint(s, key_value,
                                                           key_size));
        }
    }

    OtKeySinkIfClass *kc = OT_KEY_SINK_IF_GET_CLASS(sink);
    OtKeySinkIf *ki = OT_KEY_SINK_IF(sink);

    g_assert(kc->push_key);

    kc->push_key(ki, key_share0, key_share1, key_size, valid);
}

static void ot_keymgr_push_kdf_key(OtKeyMgrState *s, const uint8_t *key_share0,
                                   const uint8_t *key_share1, bool valid)
{
    trace_ot_keymgr_push_kdf_key(s->ot_id, valid);

    /* @todo: add additional KMAC KDF key integrity checks */

    ot_keymgr_push_key(s, KEYMGR_KEY_SINK_KMAC, key_share0, key_share1, valid,
                       false);
}

/* check that 'data' is not all zeros or all ones */
static bool ot_keymgr_valid_data_check(const uint8_t *data, size_t len)
{
    size_t popcount = 0u;
    for (unsigned ix = 0u; ix < len; ix++) {
        popcount += __builtin_popcount(data[ix]);
    }
    return (popcount && popcount != (len * BITS_PER_BYTE));
}

static void ot_keymgr_reset_kdf_buffer(OtKeyMgrState *s)
{
    memset(s->kdf_buf.data, 0u, KEYMGR_KDF_BUFFER_BYTES);
    s->kdf_buf.offset = 0u;
    s->kdf_buf.length = 0u;
}

static void ot_keymgr_kdf_push_bytes(OtKeyMgrState *s, const uint8_t *data,
                                     size_t len)
{
    g_assert(s->kdf_buf.length + len <= KEYMGR_KDF_BUFFER_BYTES);

    memcpy(&s->kdf_buf.data[s->kdf_buf.length], data, len);
    s->kdf_buf.length += len;
}

static void ot_keymgr_dump_kdf_material(
    const OtKeyMgrState *s, const char *what, const uint8_t *buf, size_t len)
{
    if (trace_event_get_state(TRACE_OT_KEYMGR_DUMP_KDF_MATERIAL)) {
        const char *hexstr = ot_keymgr_dump_bigint(s, buf, len);
        trace_ot_keymgr_dump_kdf_material(s->ot_id, what, hexstr);
    }
}

static size_t ot_keymgr_kdf_append_rev_seed(OtKeyMgrState *s)
{
    ot_keymgr_kdf_push_bytes(s, s->seeds[KEYMGR_SEED_REV], KEYMGR_SEED_BYTES);
    ot_keymgr_dump_kdf_material(s, "REV_SEED", s->seeds[KEYMGR_SEED_REV],
                                KEYMGR_SEED_BYTES);
    return KEYMGR_SEED_BYTES;
}

static size_t ot_keymgr_kdf_append_rom_digest(OtKeyMgrState *s, bool *dvalid)
{
    uint8_t rom_digest[OT_ROM_DIGEST_BYTES] = { 0u };

    OtRomCtrlClass *rcc = OT_ROM_CTRL_GET_CLASS(s->rom_ctrl);
    rcc->get_rom_digest(s->rom_ctrl, rom_digest);

    ot_keymgr_kdf_push_bytes(s, rom_digest, OT_ROM_DIGEST_BYTES);
    *dvalid &= ot_keymgr_valid_data_check(rom_digest, OT_ROM_DIGEST_BYTES);

    ot_keymgr_dump_kdf_material(s, "ROM_DIGEST", rom_digest,
                                OT_ROM_DIGEST_BYTES);
    return OT_ROM_DIGEST_BYTES;
}

static size_t ot_keymgr_kdf_append_km_div(OtKeyMgrState *s, bool *dvalid)
{
    OtLcCtrlKeyMgrDiv km_div = { 0u };

    OtLcCtrlClass *lc = OT_LC_CTRL_GET_CLASS(s->lc_ctrl);
    lc->get_keymgr_div(s->lc_ctrl, &km_div);

    ot_keymgr_kdf_push_bytes(s, km_div.data, OT_LC_KEYMGR_DIV_BYTES);
    *dvalid &= ot_keymgr_valid_data_check(km_div.data, OT_LC_KEYMGR_DIV_BYTES);

    ot_keymgr_dump_kdf_material(s, "KM_DIV", km_div.data,
                                OT_LC_KEYMGR_DIV_BYTES);
    return OT_LC_KEYMGR_DIV_BYTES;
}

static size_t ot_keymgr_kdf_append_dev_id(OtKeyMgrState *s, bool *dvalid)
{
    OtOTPClass *otp_oc = OBJECT_GET_CLASS(OtOTPClass, s->otp_ctrl, TYPE_OT_OTP);
    const OtOTPHWCfg *hw_cfg = otp_oc->get_hw_cfg(s->otp_ctrl);

    ot_keymgr_kdf_push_bytes(s, hw_cfg->device_id,
                             OT_OTP_HWCFG_DEVICE_ID_BYTES);
    *dvalid &= ot_keymgr_valid_data_check(hw_cfg->device_id,
                                          OT_OTP_HWCFG_DEVICE_ID_BYTES);

    ot_keymgr_dump_kdf_material(s, "DEVICE_ID", hw_cfg->device_id,
                                OT_OTP_HWCFG_DEVICE_ID_BYTES);
    return OT_OTP_HWCFG_DEVICE_ID_BYTES;
}

static size_t
ot_keymgr_kdf_append_flash_seed(OtKeyMgrState *s, OtFlashKeyMgrSecretType type,
                                const char *seed_name, bool *dvalid)
{
    OtFlashKeyMgrSecret seed = { 0u };

    OtFlashClass *fc = OT_FLASH_GET_CLASS(s->flash_ctrl);
    fc->get_keymgr_secret(s->flash_ctrl, type, &seed);

    ot_keymgr_kdf_push_bytes(s, seed.secret, OT_FLASH_KEYMGR_SECRET_BYTES);
    *dvalid &=
        ot_keymgr_valid_data_check(seed.secret, OT_FLASH_KEYMGR_SECRET_BYTES);
    *dvalid &= seed.valid;

    ot_keymgr_dump_kdf_material(s, seed_name, seed.secret,
                                OT_FLASH_KEYMGR_SECRET_BYTES);
    return OT_FLASH_KEYMGR_SECRET_BYTES;
}

static size_t ot_keymgr_kdf_append_sw_binding(OtKeyMgrState *s, OtKeyMgrCdi cdi)
{
    uint8_t *sw_binding = (cdi == KEYMGR_CDI_SEALING) ? s->sealing_sw_binding :
                                                        s->attest_sw_binding;

    ot_keymgr_kdf_push_bytes(s, sw_binding, KEYMGR_SW_BINDING_BYTES);

    char buf[32u];
    const char *binding_suffix = "_SW_BINDING";
    const char *cdi_name = CDI_NAME(cdi);
    g_assert(strlen(binding_suffix) + strlen(cdi_name) + 1 < sizeof(buf));
    snprintf(buf, sizeof(buf), "%s%s", cdi_name, binding_suffix);
    ot_keymgr_dump_kdf_material(s, buf, sw_binding, KEYMGR_SW_BINDING_BYTES);
    return KEYMGR_SW_BINDING_BYTES;
}

static size_t ot_keymgr_kdf_append_key_seed(
    OtKeyMgrState *s, const uint8_t *seed, const char *name)
{
    ot_keymgr_kdf_push_bytes(s, seed, KEYMGR_SEED_BYTES);
    ot_keymgr_dump_kdf_material(s, name, seed, KEYMGR_SEED_BYTES);

    return KEYMGR_SEED_BYTES;
}

static size_t ot_keymgr_kdf_append_output_seed(OtKeyMgrState *s, bool sw)
{
    int seed_idx = sw ? KEYMGR_SEED_SW_OUT : KEYMGR_SEED_HW_OUT;
    const char *seed_name = sw ? "SW_OUT_KEY_SEED" : "HW_OUT_KEY_SEED";
    return ot_keymgr_kdf_append_key_seed(s, s->seeds[seed_idx], seed_name);
}

static size_t
ot_keymgr_kdf_append_destination_seed(OtKeyMgrState *s, OtKeyMgrDestSel dest)
{
    uint8_t *dest_seed;
    switch (dest) {
    case KEYMGR_DEST_SEL_VALUE_AES:
        dest_seed = s->seeds[KEYMGR_SEED_AES];
        break;
    case KEYMGR_DEST_SEL_VALUE_KMAC:
        dest_seed = s->seeds[KEYMGR_SEED_KMAC];
        break;
    case KEYMGR_DEST_SEL_VALUE_OTBN:
        dest_seed = s->seeds[KEYMGR_SEED_OTBN];
        break;
    case KEYMGR_DEST_SEL_VALUE_NONE:
    default:
        dest_seed = s->seeds[KEYMGR_SEED_NONE];
        break;
    }
    return ot_keymgr_kdf_append_key_seed(s, dest_seed, "DEST_SEED");
}

static size_t ot_keymgr_kdf_append_salt(OtKeyMgrState *s)
{
    ot_keymgr_kdf_push_bytes(s, s->salt, KEYMGR_SALT_BYTES);
    ot_keymgr_dump_kdf_material(s, "SALT", s->salt, KEYMGR_SALT_BYTES);

    return KEYMGR_SALT_BYTES;
}

static size_t ot_keymgr_kdf_append_key_version(OtKeyMgrState *s)
{
    uint8_t buf[sizeof(uint32_t)];
    stl_le_p(buf, s->regs[R_KEY_VERSION]);
    ot_keymgr_kdf_push_bytes(s, buf, sizeof(uint32_t));
    ot_keymgr_dump_kdf_material(s, "KEY_VERSION", buf, sizeof(uint32_t));

    return sizeof(uint32_t);
}

static void ot_keymgr_get_root_key(OtKeyMgrState *s, OtOTPKeyMgrSecret *share0,
                                   OtOTPKeyMgrSecret *share1)
{
    OtOTPClass *oc = OBJECT_GET_CLASS(OtOTPClass, s->otp_ctrl, TYPE_OT_OTP);
    g_assert(oc);
    oc->get_keymgr_secret(s->otp_ctrl,
                          OTP_KEYMGR_SECRET_CREATOR_ROOT_KEY_SHARE0, share0);
    oc->get_keymgr_secret(s->otp_ctrl,
                          OTP_KEYMGR_SECRET_CREATOR_ROOT_KEY_SHARE1, share1);

    if (trace_event_get_state(TRACE_OT_KEYMGR_DUMP_CREATOR_ROOT_KEY)) {
        trace_ot_keymgr_dump_creator_root_key(
            s->ot_id, 0, share0->valid,
            ot_keymgr_dump_bigint(s, share0->secret,
                                  OT_OTP_KEYMGR_SECRET_SIZE));
        trace_ot_keymgr_dump_creator_root_key(
            s->ot_id, 1, share1->valid,
            ot_keymgr_dump_bigint(s, share1->secret,
                                  OT_OTP_KEYMGR_SECRET_SIZE));
    }
}

#define ot_keymgr_change_main_fsm_state(_s_, _op_status_) \
    ot_keymgr_xchange_main_fsm_state(_s_, _op_status_, __LINE__)

static void ot_keymgr_xchange_main_fsm_state(OtKeyMgrState *s,
                                             OtKeyMgrFSMState state, int line)
{
    if (s->state != state) {
        trace_ot_keymgr_change_main_fsm_state(s->ot_id, line,
                                              FST_NAME(s->state), s->state,
                                              FST_NAME(state), state);
        s->state = state;
    }
}

static void ot_keymgr_send_kmac_req(OtKeyMgrState *s)
{
    g_assert(s->kdf_buf.length);
    g_assert(s->kdf_buf.offset < s->kdf_buf.length);

    unsigned msg_len = s->kdf_buf.length - s->kdf_buf.offset;
    if (msg_len > OT_KMAC_APP_MSG_BYTES) {
        msg_len = OT_KMAC_APP_MSG_BYTES;
    }

    unsigned next_offset = s->kdf_buf.offset + msg_len;

    OtKMACAppReq req = {
        .last = next_offset == s->kdf_buf.length,
        .msg_len = msg_len,
    };
    memcpy(req.msg_data, &s->kdf_buf.data[s->kdf_buf.offset], msg_len);

    trace_ot_keymgr_kmac_req(s->ot_id, s->kdf_buf.length, s->kdf_buf.offset,
                             req.msg_len, req.last);

    s->kdf_buf.offset = next_offset;

    OtKMACClass *kc = OT_KMAC_GET_CLASS(s->kmac);
    kc->app_request(s->kmac, s->kmac_app, &req);
}

static void ot_keymgr_operation_disable(OtKeyMgrState *s)
{
    ot_keymgr_wipe_key_states(s);
    ot_keymgr_change_main_fsm_state(s, KEYMGR_ST_DISABLED);
    s->op_state.op_ack = true;
    ot_keymgr_schedule_fsm(s);
}

static void ot_keymgr_operation_advance(OtKeyMgrState *s, OtKeyMgrStage stage,
                                        OtKeyMgrCdi cdi)
{
    trace_ot_keymgr_advance(s->ot_id, STAGE_NAME(stage), (int)stage,
                            CDI_NAME(cdi), (int)cdi);

    bool dvalid = true;

    /* @todo: do we need to check for any error states here? */

    ot_keymgr_reset_kdf_buffer(s);

    size_t expected_kdf_len = 0u;

    switch (stage) {
    case KEYMGR_STAGE_CREATOR:
        /* Revision Seed (which is a netlist constant) */
        expected_kdf_len += ot_keymgr_kdf_append_rev_seed(s);

        /* Rom Digest (from rom_ctrl) */
        expected_kdf_len += ot_keymgr_kdf_append_rom_digest(s, &dvalid);

        /* KeyManager Diversification (from lc_ctrl) */
        expected_kdf_len += ot_keymgr_kdf_append_km_div(s, &dvalid);

        /* Device ID (from OTP) */
        expected_kdf_len += ot_keymgr_kdf_append_dev_id(s, &dvalid);
        break;
    case KEYMGR_STAGE_OWNER_INT:
        /* Creator Seed (from flash) */
        expected_kdf_len +=
            ot_keymgr_kdf_append_flash_seed(s, FLASH_KEYMGR_SECRET_CREATOR_SEED,
                                            "CREATOR_SEED", &dvalid);
        break;
    case KEYMGR_STAGE_OWNER:
        /* Owner Seed (from flash) */
        expected_kdf_len +=
            ot_keymgr_kdf_append_flash_seed(s, FLASH_KEYMGR_SECRET_OWNER_SEED,
                                            "OWNER_SEED", &dvalid);
        break;
    case KEYMGR_STAGE_DISABLE:
        /* you can "advance" from the OwnerRootKey to the `Disabled` state */
        ot_keymgr_operation_disable(s);
        return;
    default:
        g_assert_not_reached();
    }

    /* Software Binding (software-provided via {x_SW_BINDING_y registers) */
    expected_kdf_len += ot_keymgr_kdf_append_sw_binding(s, cdi);

    /* check that we have pushed all expected KDF data */
    g_assert(s->kdf_buf.length == expected_kdf_len);

    /*
     * @todo: store `dvalid` somewhere and, if the data is invalid, replace the
     * KMAC response with decoy data (a random permutation of entropy share 1).
     */
    if (!dvalid) {
        s->regs[R_ERR_CODE] |= R_ERR_CODE_INVALID_KMAC_INPUT_MASK;
    }

    g_assert(s->kdf_buf.length <= KEYMGR_ADV_DATA_BYTES);
    s->kdf_buf.length = KEYMGR_ADV_DATA_BYTES;

    /* send the current key state to KMAC as the KDF key */
    g_assert(cdi < NUM_CDIS);
    OtKeyMgrKey *key_state = &s->key_states[cdi];
    ot_keymgr_push_kdf_key(s, key_state->share0, key_state->share1,
                           key_state->valid);

    /* transmit the contents of the KDF buffer to KMAC for computation */
    ot_keymgr_dump_kdf_buf(s, "adv");
    ot_keymgr_send_kmac_req(s);
}

static void
ot_keymgr_operation_gen_output(OtKeyMgrState *s, OtKeyMgrStage stage, bool sw)
{
    uint32_t ctrl = ot_shadow_reg_peek(&s->control);
    OtKeyMgrCdi cdi = (OtKeyMgrCdi)FIELD_EX32(ctrl, CONTROL_SHADOWED, CDI_SEL);
    OtKeyMgrDestSel dest =
        (OtKeyMgrDestSel)FIELD_EX32(ctrl, CONTROL_SHADOWED, DEST_SEL);

    trace_ot_keymgr_gen_output(s->ot_id, STAGE_NAME(stage), (int)stage,
                               CDI_NAME(cdi), (int)cdi, (sw ? "sw" : "hw"));

    ot_keymgr_reset_kdf_buffer(s);
    size_t expected_kdf_len = 0u;

    /* Output Key Seed (SW/HW key) */
    expected_kdf_len += ot_keymgr_kdf_append_output_seed(s, sw);

    /* Destination Seed (netlist constant i.e. seed)*/
    expected_kdf_len += ot_keymgr_kdf_append_destination_seed(s, dest);

    /* Salt (from SW, input via the `SALT_x` registers) */
    expected_kdf_len += ot_keymgr_kdf_append_salt(s);

    /* Key Version (from SW, input via the `KEY_VERSION` register) */
    expected_kdf_len += ot_keymgr_kdf_append_key_version(s);

    g_assert(s->kdf_buf.length == expected_kdf_len);
    g_assert(s->kdf_buf.length == KEYMGR_GEN_DATA_BYTES);

    uint32_t max_key_version;
    switch (stage) {
    case KEYMGR_STAGE_CREATOR:
        max_key_version = ot_shadow_reg_peek(&s->max_creator_key_ver);
        break;
    case KEYMGR_STAGE_OWNER_INT:
        max_key_version = ot_shadow_reg_peek(&s->max_owner_int_key_ver);
        break;
    case KEYMGR_STAGE_OWNER:
        max_key_version = ot_shadow_reg_peek(&s->max_owner_key_ver);
        break;
    case KEYMGR_STAGE_DISABLE:
    default:
        max_key_version = 0;
    }
    bool valid_key_version = s->regs[R_KEY_VERSION] <= max_key_version;

    if (!valid_key_version) {
        /*
         * Report the error in DEBUG now, but only in ERR_CODE when the KMAC
         * response has been received.
         */
        s->regs[R_DEBUG] |= R_DEBUG_INVALID_KEY_VERSION_MASK;
        s->op_state.valid_inputs = false;
    }

    /* send the current key state to KMAC as the KDF key */
    g_assert(cdi < NUM_CDIS);
    OtKeyMgrKey *key_state = &s->key_states[cdi];
    ot_keymgr_push_kdf_key(s, key_state->share0, key_state->share1,
                           key_state->valid);

    /* transmit the contents of the KDF buffer to KMAC for computation */
    ot_keymgr_dump_kdf_buf(s, "gen");
    ot_keymgr_send_kmac_req(s);
}

static void ot_keymgr_start_operation(OtKeyMgrState *s)
{
    uint32_t ctrl = ot_shadow_reg_peek(&s->control);
    int op = (int)FIELD_EX32(ctrl, CONTROL_SHADOWED, OPERATION);

    trace_ot_keymgr_operation(s->ot_id, STAGE_NAME(s->op_state.stage),
                              OP_NAME(op), op);

    switch (op) {
    case KEYMGR_OP_ADVANCE:
        s->op_state.adv_cdi_cnt = (OtKeyMgrCdi)0;
        ot_keymgr_operation_advance(s, s->op_state.stage,
                                    s->op_state.adv_cdi_cnt);
        break;
    case KEYMGR_OP_GENERATE_ID:
        /* @todo: implement generate_id operation */
        qemu_log_mask(LOG_UNIMP, "%s: %s, Operation %s is not implemented.\n",
                      __func__, s->ot_id, OP_NAME(op));
        break;
    case KEYMGR_OP_GENERATE_SW_OUTPUT:
        ot_keymgr_operation_gen_output(s, s->op_state.stage, true);
        break;
    case KEYMGR_OP_GENERATE_HW_OUTPUT:
        ot_keymgr_operation_gen_output(s, s->op_state.stage, false);
        break;
    case KEYMGR_OP_DISABLE:
        ot_keymgr_operation_disable(s);
        break;
    default:
        /* should only be called with a valid operation */
        g_assert_not_reached();
    }
}

static void ot_keymgr_restore_kmac_key(OtKeyMgrState *s)
{
    trace_ot_keymgr_restore_kmac_key(s->ot_id);

    /* restore saved KMAC sideload key now that offloaded KDF KMAC is done */
    ot_keymgr_push_key(s, KEYMGR_KEY_SINK_KMAC, s->saved_kmac_key->share0,
                       s->saved_kmac_key->share1, s->saved_kmac_key->valid,
                       true);
}

static bool
ot_keymgr_handle_kmac_resp_advance(OtKeyMgrState *s, const OtKMACAppRsp *rsp)
{
    unsigned cdi = s->op_state.adv_cdi_cnt;

    g_assert(cdi < NUM_CDIS);

    OtKeyMgrKey *key_state = &s->key_states[cdi];
    key_state->valid = true;
    memcpy(key_state->share0, rsp->digest_share0, OT_KMAC_KEY_SIZE);
    memcpy(key_state->share1, rsp->digest_share1, OT_KMAC_KEY_SIZE);

    /* SW can lock the `SW_BINDING` regs, and HW unlocks after an advance */
    s->regs[R_SW_BINDING_REGWEN] |= R_SW_BINDING_REGWEN_EN_MASK;

    unsigned next_cdi = cdi + 1;
    if (next_cdi < NUM_CDIS) {
        /* when advancing, we must advance all CDIs before completing */
        s->op_state.adv_cdi_cnt = next_cdi;
        ot_keymgr_operation_advance(s, s->op_state.stage, next_cdi);
        return false;
    }

    /* all CDIs have been advanced, so complete the advance operation */
    s->op_state.adv_cdi_cnt = 0u;

    /* SW can lock the `SW_BINDING` regs, and HW unlocks after an advance */
    s->regs[R_SW_BINDING_REGWEN] |= R_SW_BINDING_REGWEN_EN_MASK;

    switch (s->op_state.stage) {
    case KEYMGR_STAGE_CREATOR:
        ot_keymgr_change_main_fsm_state(s, KEYMGR_ST_CREATOR_ROOT_KEY);
        break;
    case KEYMGR_STAGE_OWNER_INT:
        ot_keymgr_change_main_fsm_state(s, KEYMGR_ST_OWNER_INT_KEY);
        break;
    case KEYMGR_STAGE_OWNER:
        ot_keymgr_change_main_fsm_state(s, KEYMGR_ST_OWNER_KEY);
        break;
    case KEYMGR_STAGE_DISABLE:
    default:
        /* should not be advancing to the `disable` state */
        g_assert_not_reached();
    }

    return true;
}

static bool ot_keymgr_handle_kmac_resp_gen_output_hw(OtKeyMgrState *s,
                                                     const OtKMACAppRsp *rsp)
{
    uint32_t ctrl = ot_shadow_reg_peek(&s->control);
    OtKeyMgrDestSel dest =
        (OtKeyMgrDestSel)FIELD_EX32(ctrl, CONTROL_SHADOWED, DEST_SEL);

    switch (dest) {
    case KEYMGR_DEST_SEL_VALUE_AES:
    case KEYMGR_DEST_SEL_VALUE_KMAC:
    case KEYMGR_DEST_SEL_VALUE_OTBN: {
        OtKeyMgrKeySink key_sink = (OtKeyMgrKeySink)(dest - KEY_SINK_OFFSET);
        bool key_valid = s->op_state.valid_inputs;
        ot_keymgr_push_key(s, key_sink, rsp->digest_share0, rsp->digest_share1,
                           key_valid, true);
        break;
    }
    case KEYMGR_DEST_SEL_VALUE_NONE:
        break; /* no-op, just ack */
    default:
        g_assert_not_reached();
    }

    return true;
}

static bool ot_keymgr_handle_kmac_resp_gen_output_sw(OtKeyMgrState *s,
                                                     const OtKMACAppRsp *rsp)
{
    memcpy(s->sw_out_key->share0, rsp->digest_share0, KEYMGR_KEY_BYTES);
    memcpy(s->sw_out_key->share1, rsp->digest_share1, KEYMGR_KEY_BYTES);
    s->sw_out_key->valid = s->op_state.valid_inputs;

    return true;
}

static void
ot_keymgr_handle_kmac_response(void *opaque, const OtKMACAppRsp *rsp)
{
    OtKeyMgrState *s = OT_KEYMGR(opaque);

    if (trace_event_get_state(TRACE_OT_KEYMGR_KMAC_RSP)) {
        /* if tracing, trace the unmasked KMAC response key */
        if (rsp->done) {
            uint8_t key[OT_KMAC_APP_DIGEST_BYTES];
            for (unsigned ix = 0u; ix < OT_KMAC_APP_DIGEST_BYTES; ix++) {
                key[ix] = rsp->digest_share0[ix] ^ rsp->digest_share1[ix];
            }
            trace_ot_keymgr_kmac_rsp(
                s->ot_id, true,
                ot_keymgr_dump_bigint(s, key, OT_KMAC_APP_DIGEST_BYTES));
        } else {
            trace_ot_keymgr_kmac_rsp(s->ot_id, false, "");
        }
    }

    if (!rsp->done) {
        /* not the last response from KMAC, send more data */
        ot_keymgr_send_kmac_req(s);
        return;
    }

    g_assert(s->kdf_buf.offset == s->kdf_buf.length);

    uint32_t ctrl = ot_shadow_reg_peek(&s->control);
    bool op_complete;

    int op = (int)FIELD_EX32(ctrl, CONTROL_SHADOWED, OPERATION);
    switch (op) {
    case KEYMGR_OP_ADVANCE:
        op_complete = ot_keymgr_handle_kmac_resp_advance(s, rsp);
        break;
    case KEYMGR_OP_GENERATE_ID:
        /* @todo: handle KMAC app responses to the `generate_id` operation */
        op_complete = true;
        qemu_log_mask(LOG_UNIMP,
                      "%s: %s: KMAC response in op %s is not implemented.\n",
                      __func__, s->ot_id, OP_NAME(s->state));
        break;
    case KEYMGR_OP_GENERATE_SW_OUTPUT:
        op_complete = ot_keymgr_handle_kmac_resp_gen_output_sw(s, rsp);
        break;
    case KEYMGR_OP_GENERATE_HW_OUTPUT:
        op_complete = ot_keymgr_handle_kmac_resp_gen_output_hw(s, rsp);
        break;
    case KEYMGR_OP_DISABLE:
        /* disabling should not require the KMAC */
    default:
        g_assert_not_reached();
    }

    if (op_complete) {
        /* reload the sideloaded key into the KMAC key sink */
        ot_keymgr_restore_kmac_key(s);

        /* complete the operation, and reschedule the FSM */
        s->op_state.op_ack = true;
        if (!s->op_state.valid_inputs) {
            /* now that the operation completed, report any errors */
            s->regs[R_ERR_CODE] |= R_ERR_CODE_INVALID_KMAC_INPUT_MASK;
        }
        ot_keymgr_schedule_fsm(s);
    }
}

static void ot_keymgr_fsm_key_stage(OtKeyMgrState *s, bool supports_generation,
                                    OtKeyMgrStage advance,
                                    OtKeyMgrStage generate)
{
    bool op_start = s->regs[R_START] & R_START_EN_MASK;
    bool invalid_state = s->regs[R_FAULT_STATUS] & FAULT_STATUS_MASK;
    uint32_t ctrl = ot_shadow_reg_peek(&s->control);

    /* check for op errors before enablement & state validity */
    uint32_t op = FIELD_EX32(ctrl, CONTROL_SHADOWED, OPERATION);
    bool invalid_op = false;
    switch (op) {
    case KEYMGR_OP_GENERATE_ID:
    case KEYMGR_OP_GENERATE_HW_OUTPUT:
    case KEYMGR_OP_GENERATE_SW_OUTPUT:
        if (op_start && !supports_generation) {
            s->regs[R_ERR_CODE] |= R_ERR_CODE_INVALID_OP_MASK;
            invalid_op = true;
        }
        break;
    default:
        break;
    }

    /* wipe if disabled or in an invalid state */
    if (!s->enabled || invalid_state) {
        ot_keymgr_change_main_fsm_state(s, KEYMGR_ST_WIPE);
        return;
    }

    if (!op_start || s->op_state.op_req || invalid_op) {
        return; /* no state change if op_start is not set */
    }

    switch (op) {
    case KEYMGR_OP_DISABLE:
        s->op_state.stage = KEYMGR_STAGE_DISABLE;
        break;
    case KEYMGR_OP_ADVANCE:
        s->op_state.stage = advance;
        break;
    default:
        s->op_state.stage = generate;
    }
    s->op_state.op_req = true;
    s->op_state.valid_inputs = true;
    ot_keymgr_start_operation(s);
}

/* Tick the main FSM. Returns whether there was any state change this tick. */
static bool ot_keymgr_main_fsm_tick(OtKeyMgrState *s)
{
    /* store current state so we can determine (& return) any state change */
    OtKeyMgrFSMState state = s->state;
    bool op_start = s->regs[R_START] & R_START_EN_MASK;
    bool invalid_state = s->regs[R_FAULT_STATUS] & FAULT_STATUS_MASK;
    bool init = false;
    uint32_t ctrl = ot_shadow_reg_peek(&s->control);

    trace_ot_keymgr_main_fsm_tick(s->ot_id, FST_NAME(s->state), s->state);

    switch (s->state) {
    case KEYMGR_ST_RESET:
        ot_keymgr_change_working_state(s, KEYMGR_WORKING_STATE_RESET);
        bool op_advance =
            FIELD_EX32(ctrl, CONTROL_SHADOWED, OPERATION) == KEYMGR_OP_ADVANCE;
        bool advance_sel = op_start && op_advance && s->enabled;
        if (op_start && !advance_sel) {
            s->regs[R_ERR_CODE] |= R_ERR_CODE_INVALID_OP_MASK;
        }

        if (invalid_state) {
            ot_keymgr_change_main_fsm_state(s, KEYMGR_ST_WIPE);
        } else if (advance_sel) {
            ot_keymgr_change_main_fsm_state(s, KEYMGR_ST_ENTROPY_RESEED);
        }
        break;
    case KEYMGR_ST_ENTROPY_RESEED:
        ot_keymgr_change_working_state(s, KEYMGR_WORKING_STATE_RESET);
        /*
         * The Earlgrey 1.0.0 keymgr does not immediately transition to
         * ST_INVALID if disabled by the lc_ctrl whilst reseeding entropy.
         */
        if (!s->prng.reseed_req) {
            s->prng.reseed_ack = false;
            s->prng.reseed_req = true;
            ot_keymgr_request_entropy(s);
        } else if (s->prng.reseed_ack) {
            ot_keymgr_change_main_fsm_state(s, KEYMGR_ST_RANDOM);
        }
        break;
    case KEYMGR_ST_RANDOM:
        ot_keymgr_change_working_state(s, KEYMGR_WORKING_STATE_RESET);
        /*
         * The Earlgrey 1.0.0 keymgr does not immediately transition to
         * ST_INVALID if disabled by the lc_ctrl whilst reseeding entropy.
         *
         * @todo: normally, this state would initialise the key state with
         * some random entropy for masking of key shares, but we leave these
         * uninitialised while there is no KMAC masking.
         */
        ot_keymgr_change_main_fsm_state(s, KEYMGR_ST_ROOT_KEY);
        break;
    case KEYMGR_ST_ROOT_KEY:
        ot_keymgr_change_working_state(s, KEYMGR_WORKING_STATE_INIT);
        /* If the keymgr is disabled or the root key is invalid, we must wipe */
        if (!s->enabled) {
            ot_keymgr_change_main_fsm_state(s, KEYMGR_ST_WIPE);
            break;
        }
        init = true;

        /* Retrieve Creator Root Key from OTP */
        OtOTPKeyMgrSecret secret_share0 = { 0u };
        OtOTPKeyMgrSecret secret_share1 = { 0u };
        ot_keymgr_get_root_key(s, &secret_share0, &secret_share1);

        if (secret_share0.valid && secret_share1.valid) {
            memset(s->key_states, 0u, NUM_CDIS * sizeof(OtKeyMgrKey));
            for (unsigned cdi = 0u; cdi < NUM_CDIS; cdi++) {
                memcpy(s->key_states[cdi].share0, secret_share0.secret,
                       OT_OTP_KEYMGR_SECRET_SIZE);
                memcpy(s->key_states[cdi].share1, secret_share1.secret,
                       OT_OTP_KEYMGR_SECRET_SIZE);
                s->key_states[cdi].valid = true;
            }
            ot_keymgr_change_main_fsm_state(s, KEYMGR_ST_INIT);
        } else {
            ot_keymgr_change_main_fsm_state(s, KEYMGR_ST_WIPE);
        }
        break;
    case KEYMGR_ST_INIT:
        ot_keymgr_change_working_state(s, KEYMGR_WORKING_STATE_INIT);
        ot_keymgr_fsm_key_stage(s, false, KEYMGR_STAGE_CREATOR,
                                KEYMGR_STAGE_DISABLE);
        break;
    case KEYMGR_ST_CREATOR_ROOT_KEY:
        ot_keymgr_change_working_state(s,
                                       KEYMGR_WORKING_STATE_CREATOR_ROOT_KEY);
        ot_keymgr_fsm_key_stage(s, true, KEYMGR_STAGE_OWNER_INT,
                                KEYMGR_STAGE_CREATOR);
        break;
    case KEYMGR_ST_OWNER_INT_KEY:
        ot_keymgr_change_working_state(
            s, KEYMGR_WORKING_STATE_OWNER_INTERMEDIATE_KEY);
        ot_keymgr_fsm_key_stage(s, true, KEYMGR_STAGE_OWNER,
                                KEYMGR_STAGE_OWNER_INT);
        break;
    case KEYMGR_ST_OWNER_KEY:
        ot_keymgr_change_working_state(s, KEYMGR_WORKING_STATE_OWNER_KEY);
        ot_keymgr_fsm_key_stage(s, true, KEYMGR_STAGE_DISABLE,
                                KEYMGR_STAGE_OWNER);
        break;
    case KEYMGR_ST_DISABLED:
        ot_keymgr_change_working_state(s, KEYMGR_WORKING_STATE_DISABLED);
        if (!s->enabled || invalid_state) {
            ot_keymgr_change_main_fsm_state(s, KEYMGR_ST_WIPE);
        }
        break;
    case KEYMGR_ST_WIPE:
        /* whilst wiping, keymgr retains the previous working state */
        ot_keymgr_wipe_key_states(s);
        /* see OT keymgr_ctrl.sv RTL: maintain & complete ongoing ops */
        if (op_start) {
            s->regs[R_ERR_CODE] |= R_ERR_CODE_INVALID_OP_MASK;
        } else {
            ot_keymgr_change_main_fsm_state(s, KEYMGR_ST_INVALID);
        }
        break;
    case KEYMGR_ST_INVALID:
        ot_keymgr_change_working_state(s, KEYMGR_WORKING_STATE_INVALID);
        if (op_start) {
            s->regs[R_ERR_CODE] |= R_ERR_CODE_INVALID_OP_MASK;
        }
        break;
    default:
        break;
    }

    /* update the last requested operation status */
    bool invalid_op = (bool)(s->regs[R_ERR_CODE] & R_ERR_CODE_INVALID_OP_MASK);
    bool op_done =
        s->op_state.op_req ? s->op_state.op_ack : (init || invalid_op);
    if (op_done) {
        s->op_state.op_req = false;
        s->op_state.op_ack = false;
        s->regs[R_START] &= ~R_START_EN_MASK;
        if (s->regs[R_ERR_CODE] || s->regs[R_FAULT_STATUS]) {
            ot_keymgr_update_alerts(s);
            ot_keymgr_change_op_status(s, KEYMGR_OP_STATUS_DONE_ERROR);
        } else {
            ot_keymgr_change_op_status(s, KEYMGR_OP_STATUS_DONE_SUCCESS);
        }
        s->regs[R_INTR_STATE] |= INTR_OP_DONE_MASK;
        ot_keymgr_update_irq(s);
    } else if (op_start) {
        ot_keymgr_change_op_status(s, KEYMGR_OP_STATUS_WIP);
    } else {
        ot_keymgr_change_op_status(s, KEYMGR_OP_STATUS_IDLE);
    }

    /* lock CFG_REGWEN when an operation is ongoing */
    if (s->enabled && op_start) {
        if (op_done) {
            s->regs[R_CFG_REGWEN] |= R_CFG_REGWEN_EN_MASK;
        } else {
            s->regs[R_CFG_REGWEN] &= ~R_CFG_REGWEN_EN_MASK;
        }
    }

    return state != s->state;
}

static void ot_keymgr_fsm_tick(void *opaque)
{
    OtKeyMgrState *s = opaque;

    bool fsm_state_changed = ot_keymgr_main_fsm_tick(s);
    if (fsm_state_changed) {
        /* FSM state changed, so schedule an FSM update once more */
        ot_keymgr_schedule_fsm(s);
    } else {
        /* no FSM state change, so go idle and wait for some external event */
        trace_ot_keymgr_go_idle(s->ot_id);
    }
}

static void ot_keymgr_lc_signal(void *opaque, int irq, int level)
{
    OtKeyMgrState *s = opaque;
    bool enable_keymgr = (bool)level;

    g_assert(irq == 0);

    trace_ot_keymgr_lc_signal(s->ot_id, level);

    bool enablement_changed = enable_keymgr ^ s->enabled;
    if (!enablement_changed) {
        return;
    }

    s->enabled = enable_keymgr;

    if (s->enabled) {
        s->regs[R_CFG_REGWEN] |= R_CFG_REGWEN_EN_MASK;
    } else {
        s->regs[R_CFG_REGWEN] &= ~R_CFG_REGWEN_EN_MASK;
    }

    ot_keymgr_schedule_fsm(s);
}

#define ot_keymgr_check_reg_write(_s_, _reg_, _regwen_) \
    ot_keymgr_check_reg_write_func(__func__, _s_, _reg_, _regwen_)

static inline bool ot_keymgr_check_reg_write_func(
    const char *func, OtKeyMgrState *s, hwaddr reg, hwaddr regwen)
{
    if (s->regs[regwen]) {
        return true;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Write to %s protected with %s\n",
                  func, s->ot_id, REG_NAME(reg), REG_NAME(regwen));
    return false;
}

static uint64_t ot_keymgr_read(void *opaque, hwaddr addr, unsigned size)
{
    OtKeyMgrState *s = opaque;
    (void)size;

    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_INTR_STATE:
    case R_INTR_ENABLE:
    case R_CFG_REGWEN:
    case R_START:
    case R_SIDELOAD_CLEAR:
    case R_RESEED_INTERVAL_REGWEN:
    case R_SW_BINDING_REGWEN:
    case R_KEY_VERSION:
    case R_MAX_CREATOR_KEY_VER_REGWEN:
    case R_MAX_OWNER_INT_KEY_VER_REGWEN:
    case R_MAX_OWNER_KEY_VER_REGWEN:
    case R_WORKING_STATE:
    case R_OP_STATUS:
    case R_ERR_CODE:
    case R_FAULT_STATUS:
    case R_DEBUG:
        val32 = s->regs[reg];
        break;
    case R_CONTROL_SHADOWED:
        val32 = ot_shadow_reg_read(&s->control);
        break;
    case R_RESEED_INTERVAL_SHADOWED:
        val32 = ot_shadow_reg_read(&s->reseed_interval);
        break;
    case R_MAX_CREATOR_KEY_VER_SHADOWED:
        val32 = ot_shadow_reg_read(&s->max_creator_key_ver);
        break;
    case R_MAX_OWNER_INT_KEY_VER_SHADOWED:
        val32 = ot_shadow_reg_read(&s->max_owner_int_key_ver);
        break;
    case R_MAX_OWNER_KEY_VER_SHADOWED:
        val32 = ot_shadow_reg_read(&s->max_owner_key_ver);
        break;
    case R_SEALING_SW_BINDING_0:
    case R_SEALING_SW_BINDING_1:
    case R_SEALING_SW_BINDING_2:
    case R_SEALING_SW_BINDING_3:
    case R_SEALING_SW_BINDING_4:
    case R_SEALING_SW_BINDING_5:
    case R_SEALING_SW_BINDING_6:
    case R_SEALING_SW_BINDING_7: {
        unsigned offset = (reg - R_SEALING_SW_BINDING_0) * sizeof(uint32_t);
        val32 = ldl_le_p(&s->sealing_sw_binding[offset]);
        break;
    }
    case R_ATTEST_SW_BINDING_0:
    case R_ATTEST_SW_BINDING_1:
    case R_ATTEST_SW_BINDING_2:
    case R_ATTEST_SW_BINDING_3:
    case R_ATTEST_SW_BINDING_4:
    case R_ATTEST_SW_BINDING_5:
    case R_ATTEST_SW_BINDING_6:
    case R_ATTEST_SW_BINDING_7: {
        unsigned offset = (reg - R_ATTEST_SW_BINDING_0) * sizeof(uint32_t);
        val32 = ldl_le_p(&s->attest_sw_binding[offset]);
        break;
    }
    case R_SALT_0:
    case R_SALT_1:
    case R_SALT_2:
    case R_SALT_3:
    case R_SALT_4:
    case R_SALT_5:
    case R_SALT_6:
    case R_SALT_7: {
        unsigned offset = (reg - R_SALT_0) * sizeof(uint32_t);
        val32 = ldl_le_p(&s->salt[offset]);
        break;
    }
    case R_SW_SHARE0_OUTPUT_0:
    case R_SW_SHARE0_OUTPUT_1:
    case R_SW_SHARE0_OUTPUT_2:
    case R_SW_SHARE0_OUTPUT_3:
    case R_SW_SHARE0_OUTPUT_4:
    case R_SW_SHARE0_OUTPUT_5:
    case R_SW_SHARE0_OUTPUT_6:
    case R_SW_SHARE0_OUTPUT_7: {
        unsigned offset = (reg - R_SW_SHARE0_OUTPUT_0) * sizeof(uint32_t);
        void *ptr = &s->sw_out_key->share0[offset];
        val32 = ldl_le_p(ptr);
        stl_le_p(ptr, 0u); /* RC */
        break;
    }
    case R_SW_SHARE1_OUTPUT_0:
    case R_SW_SHARE1_OUTPUT_1:
    case R_SW_SHARE1_OUTPUT_2:
    case R_SW_SHARE1_OUTPUT_3:
    case R_SW_SHARE1_OUTPUT_4:
    case R_SW_SHARE1_OUTPUT_5:
    case R_SW_SHARE1_OUTPUT_6:
    case R_SW_SHARE1_OUTPUT_7: {
        unsigned offset = (reg - R_SW_SHARE1_OUTPUT_0) * sizeof(uint32_t);
        void *ptr = &s->sw_out_key->share1[offset];
        val32 = ldl_le_p(ptr);
        stl_le_p(ptr, 0u); /* RC */
        break;
    }
    case R_INTR_TEST:
    case R_ALERT_TEST:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: W/O register 0x%02" HWADDR_PRIx " (%s)\n",
                      __func__, s->ot_id, addr, REG_NAME(reg));
        val32 = 0u;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Bad offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, addr);
        val32 = 0u;
        break;
    }

    uint64_t pc = ibex_get_current_pc();
    trace_ot_keymgr_io_read_out(s->ot_id, (unsigned)addr, REG_NAME(reg),
                                (uint64_t)val32, pc);

    return (uint64_t)val32;
};

static void ot_keymgr_write(void *opaque, hwaddr addr, uint64_t val64,
                            unsigned size)
{
    OtKeyMgrState *s = opaque;
    (void)size;

    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint64_t pc = ibex_get_current_pc();
    trace_ot_keymgr_io_write(s->ot_id, (unsigned)addr, REG_NAME(reg), val64,
                             pc);

    switch (reg) {
    case R_INTR_STATE:
        val32 &= INTR_MASK;
        s->regs[reg] &= ~val32; /* RW1C */
        ot_keymgr_update_irq(s);
        break;
    case R_INTR_ENABLE:
        val32 &= INTR_MASK;
        s->regs[reg] = val32;
        ot_keymgr_update_irq(s);
        break;
    case R_INTR_TEST:
        val32 &= INTR_MASK;
        s->regs[R_INTR_STATE] |= val32;
        ot_keymgr_update_irq(s);
        break;
    case R_ALERT_TEST:
        val32 &= ALERT_MASK;
        s->regs[reg] |= val32;
        ot_keymgr_update_alerts(s);
        break;
    case R_START:
        if (!ot_keymgr_check_reg_write(s, reg, R_CFG_REGWEN)) {
            break;
        }
        val32 &= R_START_EN_MASK;
        s->regs[reg] = val32;
        ot_keymgr_fsm_tick(s);
        break;
    case R_CONTROL_SHADOWED:
        if (!ot_keymgr_check_reg_write(s, reg, R_CFG_REGWEN)) {
            break;
        }
        val32 &= R_CONTROL_SHADOWED_MASK;
        switch (ot_shadow_reg_write(&s->control, val32)) {
        case OT_SHADOW_REG_STAGED:
        case OT_SHADOW_REG_COMMITTED:
            break;
        case OT_SHADOW_REG_ERROR:
        default:
            s->regs[R_ERR_CODE] |= R_ERR_CODE_INVALID_SHADOW_UPDATE_MASK;
            ot_keymgr_update_alerts(s);
        }
        break;
    case R_SIDELOAD_CLEAR:
        if (!ot_keymgr_check_reg_write(s, reg, R_CFG_REGWEN)) {
            break;
        }
        val32 &= R_SIDELOAD_CLEAR_VAL_MASK;
        s->regs[reg] = val32;
        /* @todo: implement R_SIDELOAD_CLEAR */
        break;
    case R_RESEED_INTERVAL_REGWEN:
        val32 &= R_RESEED_INTERVAL_REGWEN_EN_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_RESEED_INTERVAL_SHADOWED:
        if (!ot_keymgr_check_reg_write(s, reg, R_RESEED_INTERVAL_REGWEN)) {
            break;
        }
        val32 &= R_RESEED_INTERVAL_SHADOWED_VAL_MASK;
        switch (ot_shadow_reg_write(&s->reseed_interval, val32)) {
        case OT_SHADOW_REG_STAGED:
        case OT_SHADOW_REG_COMMITTED:
            break;
        case OT_SHADOW_REG_ERROR:
        default:
            s->regs[R_ERR_CODE] |= R_ERR_CODE_INVALID_SHADOW_UPDATE_MASK;
            ot_keymgr_update_alerts(s);
        }
        break;
    case R_SW_BINDING_REGWEN:
        val32 &= R_SW_BINDING_REGWEN_EN_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_SEALING_SW_BINDING_0:
    case R_SEALING_SW_BINDING_1:
    case R_SEALING_SW_BINDING_2:
    case R_SEALING_SW_BINDING_3:
    case R_SEALING_SW_BINDING_4:
    case R_SEALING_SW_BINDING_5:
    case R_SEALING_SW_BINDING_6:
    case R_SEALING_SW_BINDING_7: {
        if (!ot_keymgr_check_reg_write(s, reg, R_SW_BINDING_REGWEN)) {
            break;
        }
        unsigned offset = (reg - R_SEALING_SW_BINDING_0) * sizeof(uint32_t);
        stl_le_p(&s->sealing_sw_binding[offset], val32);
        break;
    }
    case R_ATTEST_SW_BINDING_0:
    case R_ATTEST_SW_BINDING_1:
    case R_ATTEST_SW_BINDING_2:
    case R_ATTEST_SW_BINDING_3:
    case R_ATTEST_SW_BINDING_4:
    case R_ATTEST_SW_BINDING_5:
    case R_ATTEST_SW_BINDING_6:
    case R_ATTEST_SW_BINDING_7: {
        if (!ot_keymgr_check_reg_write(s, reg, R_SW_BINDING_REGWEN)) {
            break;
        }
        unsigned offset = (reg - R_ATTEST_SW_BINDING_0) * sizeof(uint32_t);
        stl_le_p(&s->attest_sw_binding[offset], val32);
        break;
    }
    case R_SALT_0:
    case R_SALT_1:
    case R_SALT_2:
    case R_SALT_3:
    case R_SALT_4:
    case R_SALT_5:
    case R_SALT_6:
    case R_SALT_7: {
        if (!ot_keymgr_check_reg_write(s, reg, R_CFG_REGWEN)) {
            break;
        }
        unsigned offset = (reg - R_SALT_0) * sizeof(uint32_t);
        stl_le_p(&s->salt[offset], val32);
        break;
    }
    case R_KEY_VERSION:
        if (!ot_keymgr_check_reg_write(s, reg, R_CFG_REGWEN)) {
            break;
        }
        s->regs[reg] = val32;
        break;
    case R_MAX_CREATOR_KEY_VER_REGWEN:
        val32 &= R_MAX_CREATOR_KEY_VER_REGWEN_EN_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_MAX_CREATOR_KEY_VER_SHADOWED:
        if (!ot_keymgr_check_reg_write(s, reg, R_MAX_CREATOR_KEY_VER_REGWEN)) {
            break;
        }
        switch (ot_shadow_reg_write(&s->max_creator_key_ver, val32)) {
        case OT_SHADOW_REG_STAGED:
        case OT_SHADOW_REG_COMMITTED:
            break;
        case OT_SHADOW_REG_ERROR:
        default:
            s->regs[R_ERR_CODE] |= R_ERR_CODE_INVALID_SHADOW_UPDATE_MASK;
            ot_keymgr_update_alerts(s);
        }
        break;
    case R_MAX_OWNER_INT_KEY_VER_REGWEN:
        val32 &= R_MAX_OWNER_INT_KEY_VER_REGWEN_EN_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_MAX_OWNER_INT_KEY_VER_SHADOWED:
        if (!ot_keymgr_check_reg_write(s, reg,
                                       R_MAX_OWNER_INT_KEY_VER_REGWEN)) {
            break;
        }
        switch (ot_shadow_reg_write(&s->max_owner_int_key_ver, val32)) {
        case OT_SHADOW_REG_STAGED:
        case OT_SHADOW_REG_COMMITTED:
            break;
        case OT_SHADOW_REG_ERROR:
        default:
            s->regs[R_ERR_CODE] |= R_ERR_CODE_INVALID_SHADOW_UPDATE_MASK;
            ot_keymgr_update_alerts(s);
        }
        break;
    case R_MAX_OWNER_KEY_VER_REGWEN:
        val32 &= R_MAX_OWNER_KEY_VER_REGWEN_EN_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_MAX_OWNER_KEY_VER_SHADOWED:
        if (!ot_keymgr_check_reg_write(s, reg, R_MAX_OWNER_KEY_VER_REGWEN)) {
            break;
        }
        switch (ot_shadow_reg_write(&s->max_owner_key_ver, val32)) {
        case OT_SHADOW_REG_STAGED:
        case OT_SHADOW_REG_COMMITTED:
            break;
        case OT_SHADOW_REG_ERROR:
        default:
            s->regs[R_ERR_CODE] |= R_ERR_CODE_INVALID_SHADOW_UPDATE_MASK;
            ot_keymgr_update_alerts(s);
        }
        break;
    case R_OP_STATUS: {
        val32 &= R_OP_STATUS_STATUS_MASK;
        ot_keymgr_change_op_status(s, s->regs[reg] & ~val32); /* RW1C */
        /* SW write may clear `WIP` here which may be immediately set again */
        ot_keymgr_fsm_tick(s);
        break;
    }
    case R_ERR_CODE:
        val32 &= ERR_CODE_MASK;
        s->regs[reg] &= ~val32; /* RW1C */
        ot_keymgr_update_alerts(s);
        break;
    case R_DEBUG:
        val32 &= DEBUG_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_CFG_REGWEN:
    case R_WORKING_STATE:
    case R_SW_SHARE0_OUTPUT_0:
    case R_SW_SHARE0_OUTPUT_1:
    case R_SW_SHARE0_OUTPUT_2:
    case R_SW_SHARE0_OUTPUT_3:
    case R_SW_SHARE0_OUTPUT_4:
    case R_SW_SHARE0_OUTPUT_5:
    case R_SW_SHARE0_OUTPUT_6:
    case R_SW_SHARE0_OUTPUT_7:
    case R_SW_SHARE1_OUTPUT_0:
    case R_SW_SHARE1_OUTPUT_1:
    case R_SW_SHARE1_OUTPUT_2:
    case R_SW_SHARE1_OUTPUT_3:
    case R_SW_SHARE1_OUTPUT_4:
    case R_SW_SHARE1_OUTPUT_5:
    case R_SW_SHARE1_OUTPUT_6:
    case R_SW_SHARE1_OUTPUT_7:
    case R_FAULT_STATUS:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: R/O register 0x02%" HWADDR_PRIx " (%s)\n",
                      __func__, s->ot_id, addr, REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Bad offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, addr);
        break;
    }
};

static void ot_keymgr_configure_constants(OtKeyMgrState *s)
{
    for (unsigned ix = 0u; ix < KEYMGR_SEED_COUNT; ix++) {
        if (!s->seed_xstrs[ix]) {
            trace_ot_keymgr_seed_missing(s->ot_id, ix);
            continue;
        }

        size_t len = strlen(s->seed_xstrs[ix]);
        size_t seed_len_bytes = KEYMGR_SEED_BYTES;
        /* the LFSR seed constant is smaller than other seed constants */
        if (ix == KEYMGR_SEED_LFSR) {
            seed_len_bytes = KEYMGR_LFSR_SEED_BYTES;
        }
        if (len != (seed_len_bytes * 2u)) {
            error_setg(&error_fatal, "%s: %s invalid seed #%u length", __func__,
                       s->ot_id, ix);
            continue;
        }

        if (ot_common_parse_hexa_str(s->seeds[ix], s->seed_xstrs[ix],
                                     seed_len_bytes, true, true)) {
            error_setg(&error_fatal, "%s: %s unable to parse seed #%u",
                       __func__, s->ot_id, ix);
            continue;
        }
    }
}

static Property ot_keymgr_properties[] = {
    DEFINE_PROP_STRING(OT_COMMON_DEV_ID, OtKeyMgrState, ot_id),
    DEFINE_PROP_LINK("edn", OtKeyMgrState, edn.device, TYPE_OT_EDN,
                     OtEDNState *),
    DEFINE_PROP_UINT8("edn-ep", OtKeyMgrState, edn.ep, UINT8_MAX),
    DEFINE_PROP_LINK("flash_ctrl", OtKeyMgrState, flash_ctrl, TYPE_OT_FLASH,
                     OtFlashState *),
    DEFINE_PROP_LINK("kmac", OtKeyMgrState, kmac, TYPE_OT_KMAC, OtKMACState *),
    DEFINE_PROP_UINT8("kmac-app", OtKeyMgrState, kmac_app, UINT8_MAX),
    DEFINE_PROP_LINK("lc_ctrl", OtKeyMgrState, lc_ctrl, TYPE_OT_LC_CTRL,
                     OtLcCtrlState *),
    DEFINE_PROP_LINK("otp_ctrl", OtKeyMgrState, otp_ctrl, TYPE_OT_OTP,
                     OtOTPState *),
    DEFINE_PROP_LINK("rom_ctrl", OtKeyMgrState, rom_ctrl, TYPE_OT_ROM_CTRL,
                     OtRomCtrlState *),
    DEFINE_PROP_LINK("aes", OtKeyMgrState, key_sinks[KEYMGR_KEY_SINK_AES],
                     TYPE_OT_KEY_SINK_IF, DeviceState *),
    DEFINE_PROP_LINK("otbn", OtKeyMgrState, key_sinks[KEYMGR_KEY_SINK_OTBN],
                     TYPE_OT_KEY_SINK_IF, DeviceState *),
    DEFINE_PROP_STRING("lfsr_seed", OtKeyMgrState,
                       seed_xstrs[KEYMGR_SEED_LFSR]),
    DEFINE_PROP_STRING("revision_seed", OtKeyMgrState,
                       seed_xstrs[KEYMGR_SEED_REV]),
    DEFINE_PROP_STRING("creator_identity_seed", OtKeyMgrState,
                       seed_xstrs[KEYMGR_SEED_CREATOR_IDENTITY]),
    DEFINE_PROP_STRING("owner_int_identity_seed", OtKeyMgrState,
                       seed_xstrs[KEYMGR_SEED_OWNER_INT_IDENTITY]),
    DEFINE_PROP_STRING("owner_identity_seed", OtKeyMgrState,
                       seed_xstrs[KEYMGR_SEED_OWNER_IDENTITY]),
    DEFINE_PROP_STRING("soft_output_seed", OtKeyMgrState,
                       seed_xstrs[KEYMGR_SEED_SW_OUT]),
    DEFINE_PROP_STRING("hard_output_seed", OtKeyMgrState,
                       seed_xstrs[KEYMGR_SEED_HW_OUT]),
    DEFINE_PROP_STRING("aes_seed", OtKeyMgrState, seed_xstrs[KEYMGR_SEED_AES]),
    DEFINE_PROP_STRING("kmac_seed", OtKeyMgrState,
                       seed_xstrs[KEYMGR_SEED_KMAC]),
    DEFINE_PROP_STRING("otbn_seed", OtKeyMgrState,
                       seed_xstrs[KEYMGR_SEED_OTBN]),
    DEFINE_PROP_STRING("cdi_seed", OtKeyMgrState, seed_xstrs[KEYMGR_SEED_CDI]),
    DEFINE_PROP_STRING("none_seed", OtKeyMgrState,
                       seed_xstrs[KEYMGR_SEED_NONE]),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_keymgr_regs_ops = {
    .read = &ot_keymgr_read,
    .write = &ot_keymgr_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_keymgr_reset_enter(Object *obj, ResetType type)
{
    OtKeyMgrClass *c = OT_KEYMGR_GET_CLASS(obj);
    OtKeyMgrState *s = OT_KEYMGR(obj);

    trace_ot_keymgr_reset(s->ot_id, "enter");

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    timer_del(s->fsm_tick_timer);

    g_assert(s->edn.device);
    g_assert(s->edn.ep != UINT8_MAX);
    g_assert(s->flash_ctrl);
    g_assert(s->kmac);
    g_assert(s->kmac_app != UINT8_MAX);
    g_assert(s->lc_ctrl);
    g_assert(s->otp_ctrl);
    g_assert(s->rom_ctrl);

    /* reset registers */
    memset(s->regs, 0u, sizeof(s->regs));
    s->regs[R_CFG_REGWEN] = 0x1u;
    ot_shadow_reg_init(&s->control, 0x10u);
    s->regs[R_RESEED_INTERVAL_REGWEN] = 0x1u;
    ot_shadow_reg_init(&s->reseed_interval, 0x100u);
    s->regs[R_SW_BINDING_REGWEN] = 0x1u;
    s->regs[R_MAX_CREATOR_KEY_VER_REGWEN] = 0x1u;
    ot_shadow_reg_init(&s->max_creator_key_ver, 0x0u);
    s->regs[R_MAX_OWNER_INT_KEY_VER_REGWEN] = 0x1u;
    ot_shadow_reg_init(&s->max_owner_int_key_ver, 0x1u);
    s->regs[R_MAX_OWNER_KEY_VER_REGWEN] = 0x1u;
    ot_shadow_reg_init(&s->max_owner_key_ver, 0x0u);
    memset(s->salt, 0u, KEYMGR_SALT_BYTES);
    memset(s->sealing_sw_binding, 0u, KEYMGR_SW_BINDING_BYTES);
    memset(s->attest_sw_binding, 0u, KEYMGR_SW_BINDING_BYTES);

    /* reset internal state */
    s->enabled = false;
    s->state = KEYMGR_ST_RESET;
    s->prng.reseed_req = false;
    s->prng.reseed_ack = false;
    s->prng.reseed_cnt = 0u;
    s->op_state.op_req = false;
    s->op_state.op_ack = false;
    s->op_state.valid_inputs = true;
    s->op_state.stage = KEYMGR_STAGE_DISABLE;
    s->op_state.adv_cdi_cnt = 0u;
    ot_keymgr_reset_kdf_buffer(s);

    /* reset key state */
    memset(s->key_states, 0u, NUM_CDIS * sizeof(OtKeyMgrKey));
    memset(s->saved_kmac_key, 0u, sizeof(OtKeyMgrKey));

    /* reset output keys */
    memset(s->sw_out_key, 0u, sizeof(OtKeyMgrKey));

    /* update IRQ and alert states */
    ot_keymgr_update_irq(s);
    ot_keymgr_update_alerts(s);

    /* connect to KMAC */
    OtKMACClass *kc = OT_KMAC_GET_CLASS(s->kmac);
    kc->connect_app(s->kmac, s->kmac_app, &KMAC_APP_CFG,
                    ot_keymgr_handle_kmac_response, s);
}

static void ot_keymgr_reset_exit(Object *obj, ResetType type)
{
    OtKeyMgrClass *c = OT_KEYMGR_GET_CLASS(obj);
    OtKeyMgrState *s = OT_KEYMGR(obj);

    trace_ot_keymgr_reset(s->ot_id, "exit");

    if (c->parent_phases.exit) {
        c->parent_phases.exit(obj, type);
    }

    /* invalidate all sideloaded keys when exiting reset */
    for (unsigned ix = 0u; ix < KEYMGR_KEY_SINK_COUNT; ix++) {
        OtKeyMgrKeySink key_sink = (OtKeyMgrKeySink)ix;
        ot_keymgr_push_key(s, key_sink, NULL, NULL, false, true);
    }
}

static void ot_keymgr_realize(DeviceState *dev, Error **errp)
{
    OtKeyMgrState *s = OT_KEYMGR(dev);

    (void)errp; /* unused */

    if (!s->ot_id) {
        s->ot_id =
            g_strdup(object_get_canonical_path_component(OBJECT(s)->parent));
    }

    for (unsigned ix = 0u; ix < KEYMGR_KEY_SINK_COUNT; ix++) {
        if (s->key_sinks[ix]) {
            OBJECT_CHECK(OtKeySinkIf, s->key_sinks[ix], TYPE_OT_KEY_SINK_IF);
        }
    }

    ot_keymgr_configure_constants(s);

    /*
     * Define KMAC separately in `s->kmac` instead of directly as a key sink,
     * as we also need to offload KDF operations to it.
     */
    s->key_sinks[KEYMGR_KEY_SINK_KMAC] = DEVICE(s->kmac);
}

static void ot_keymgr_init(Object *obj)
{
    OtKeyMgrState *s = OT_KEYMGR(obj);

    memory_region_init_io(&s->mmio, obj, &ot_keymgr_regs_ops, s, TYPE_OT_KEYMGR,
                          REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    ibex_sysbus_init_irq(obj, &s->irq);
    for (unsigned ix = 0u; ix < ALERT_COUNT; ix++) {
        ibex_qdev_init_irq(obj, &s->alerts[ix], OT_DEVICE_ALERT);
    }
    qdev_init_gpio_in_named(DEVICE(s), ot_keymgr_lc_signal, OT_KEYMGR_ENABLE,
                            1);

    s->prng.state = ot_prng_allocate();

    s->kdf_buf.data = g_new0(uint8_t, KEYMGR_KDF_BUFFER_BYTES);
    s->salt = g_new0(uint8_t, KEYMGR_SALT_BYTES);
    s->sealing_sw_binding = g_new0(uint8_t, KEYMGR_SW_BINDING_BYTES);
    s->attest_sw_binding = g_new0(uint8_t, KEYMGR_SW_BINDING_BYTES);
    for (unsigned ix = 0u; ix < ARRAY_SIZE(s->seeds); ix++) {
        s->seeds[ix] = g_new0(uint8_t, KEYMGR_SEED_BYTES);
    }
    s->key_states = g_new0(OtKeyMgrKey, NUM_CDIS);
    s->saved_kmac_key = g_new0(OtKeyMgrKey, 1u);
    s->sw_out_key = g_new0(OtKeyMgrKey, 1u);

    s->fsm_tick_timer = timer_new_ns(OT_VIRTUAL_CLOCK, &ot_keymgr_fsm_tick, s);

    s->hexstr = g_new0(char, OT_KEYMGR_HEXSTR_SIZE);
}

static void ot_keymgr_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    (void)data; /* unused */

    dc->realize = &ot_keymgr_realize;
    device_class_set_props(dc, ot_keymgr_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    ResettableClass *rc = RESETTABLE_CLASS(klass);
    OtKeyMgrClass *kmc = OT_KEYMGR_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_keymgr_reset_enter, NULL,
                                       &ot_keymgr_reset_exit,
                                       &kmc->parent_phases);
}

static const TypeInfo ot_keymgr_info = {
    .name = TYPE_OT_KEYMGR,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtKeyMgrState),
    .instance_init = &ot_keymgr_init,
    .class_size = sizeof(OtKeyMgrClass),
    .class_init = &ot_keymgr_class_init,
};

static void ot_keymgr_register_types(void)
{
    type_register_static(&ot_keymgr_info);
}

type_init(ot_keymgr_register_types)
