/*
 * QEMU OpenTitan Key Manager DPE device
 *
 * Copyright (c) 2025 Rivos, Inc.
 * Copyright (c) 2025 lowRISC contributors.
 *
 * Author(s):
 *  Loïc Lefort <loic@rivosinc.com>
 *  Samuel Ortiz <sameo@rivosinc.com>
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
#include <assert.h>
#include <stdint.h>
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "hw/opentitan/ot_aes.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_edn.h"
#include "hw/opentitan/ot_key_sink.h"
#include "hw/opentitan/ot_keymgr_dpe.h"
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

#define NUM_SALT_REG          8u
#define NUM_SW_BINDING_REG    8u
#define NUM_ROM_DIGEST_INPUTS 2u
#define NUM_BOOT_STAGES       4u
#define NUM_SLOTS             4u

#define KEYMGR_DPE_KEY_WIDTH          256u
#define KEYMGR_DPE_SW_BINDING_WIDTH   ((NUM_SW_BINDING_REG) * 32u)
#define KEYMGR_DPE_SALT_WITDH         ((NUM_SALT_REG) * 32u)
#define KEYMGR_DPE_HEALTH_STATE_WIDTH 128u
#define KEYMGR_DPE_DEV_ID_WIDTH       256u

#define KEYMGR_DPE_ADV_DATA_BYTES \
    ((KEYMGR_DPE_SW_BINDING_WIDTH + KEYMGR_DPE_KEY_WIDTH + \
      (1u + (NUM_ROM_DIGEST_INPUTS)) * KEYMGR_DPE_KEY_WIDTH + \
      KEYMGR_DPE_DEV_ID_WIDTH + KEYMGR_DPE_HEALTH_STATE_WIDTH) / \
     8u)

/* key version + salt + key ID + constant */
#define KEYMGR_DPE_GEN_DATA_BYTES \
    ((32u + KEYMGR_DPE_SALT_WITDH + KEYMGR_DPE_KEY_WIDTH * 2u) / 8u)

#define KEYMGR_DPE_SEED_BYTES       (KEYMGR_DPE_KEY_WIDTH / 8u)
#define KEYMGR_DPE_KDF_BUFFER_BYTES (1984u / 8u)
#define KEYMGR_DPE_KEY_SIZE         (256u / 8u)
#define _MAX(_a_, _b_)              ((_a_) > (_b_) ? (_a_) : (_b_))
#define KEYMGR_DPE_KEY_SIZE_MAX     _MAX(KEYMGR_DPE_KEY_SIZE, OT_OTBN_KEY_SIZE)

static_assert(KEYMGR_DPE_ADV_DATA_BYTES <= KEYMGR_DPE_KDF_BUFFER_BYTES,
              "KeyMgr ADV data does not fit in KDF buffer");
static_assert(KEYMGR_DPE_GEN_DATA_BYTES <= KEYMGR_DPE_KDF_BUFFER_BYTES,
              "KeyMgr GEN data does not fit in KDF buffer");
static_assert((KEYMGR_DPE_KDF_BUFFER_BYTES % OT_KMAC_APP_MSG_BYTES) == 0u,
              "KeyMgr KDF buffer not a multiple of KMAC message size");
static_assert(KEYMGR_DPE_KEY_SIZE_MAX <= OT_KMAC_APP_DIGEST_BYTES,
              "KeyMgr key size does not match KMAC digest size");
/* NOLINTNEXTLINE(misc-redundant-expression) */
static_assert(OT_OTBN_KEY_SIZE <= OT_KMAC_APP_DIGEST_BYTES,
              "KeyMgr OTBN key size does not match KMAC digest size");
static_assert(KEYMGR_DPE_KEY_SIZE == OT_OTP_KEYMGR_SECRET_SIZE,
              "KeyMgr key size does not match OTP KeyMgr secret size");
/* NOLINTBEGIN(misc-redundant-expression) */
static_assert(KEYMGR_DPE_KEY_SIZE == OT_AES_KEY_SIZE, "invalid key size");
static_assert(KEYMGR_DPE_KEY_SIZE == OT_KMAC_KEY_SIZE, "invalid key size");
/* NOLINTEND(misc-redundant-expression) */

/* clang-format off */
REG32(INTR_STATE, 0x0u)
    SHARED_FIELD(INTR_OP_DONE, 0u, 1u)
REG32(INTR_ENABLE, 0x4u)
REG32(INTR_TEST, 0x8u)
REG32(ALERT_TEST, 0xcu)
    FIELD(ALERT_TEST, RECOV_OPERATION, 0u, 1u)
    FIELD(ALERT_TEST, FATAL_FAULT, 1u, 1u)
REG32(CFG_REGWEN, 0x10u)
    FIELD(CFG_REGWEN, EN, 0u, 1u)
REG32(START, 0x14u)
    FIELD(START, EN, 0u, 1u)
REG32(CONTROL_SHADOWED, 0x18u)
    FIELD(CONTROL_SHADOWED, OPERATION, 4u, 3u)
    FIELD(CONTROL_SHADOWED, DEST_SEL, 12u, 2u)
    FIELD(CONTROL_SHADOWED, SLOT_SRC_SEL, 14u, 2u)
    FIELD(CONTROL_SHADOWED, SLOT_DST_SEL, 18u, 2u)
REG32(SIDELOAD_CLEAR, 0x1cu)
    FIELD(SIDELOAD_CLEAR, VAL, 0u, 3u)
REG32(RESEED_INTERVAL_REGWEN, 0x20u)
    FIELD(RESEED_INTERVAL_REGWEN, EN, 0u, 1u)
REG32(RESEED_INTERVAL_SHADOWED, 0x24u)
    FIELD(RESEED_INTERVAL_SHADOWED, VAL, 0u, 16u)
REG32(SLOT_POLICY_REGWEN, 0x28u)
    FIELD(SLOT_POLICY_REGWEN, EN, 0u, 1u)
REG32(SLOT_POLICY, 0x2cu)
    FIELD(SLOT_POLICY, ALLOW_CHILD, 0u, 1u)
    FIELD(SLOT_POLICY, EXPORTABLE, 1u, 1u)
    FIELD(SLOT_POLICY, RETAIN_PARENT, 2u, 1u)
REG32(SW_BINDING_REGWEN, 0x30u)
    FIELD(SW_BINDING_REGWEN, EN, 0u, 1u)
REG32(SW_BINDING_0, 0x34u)
REG32(SW_BINDING_1, 0x38u)
REG32(SW_BINDING_2, 0x3cu)
REG32(SW_BINDING_3, 0x40u)
REG32(SW_BINDING_4, 0x44u)
REG32(SW_BINDING_5, 0x48u)
REG32(SW_BINDING_6, 0x4cu)
REG32(SW_BINDING_7, 0x50u)
REG32(SALT_0, 0x54u)
REG32(SALT_1, 0x58u)
REG32(SALT_2, 0x5cu)
REG32(SALT_3, 0x60u)
REG32(SALT_4, 0x64u)
REG32(SALT_5, 0x68u)
REG32(SALT_6, 0x6cu)
REG32(SALT_7, 0x70u)
REG32(KEY_VERSION, 0x74u)
REG32(MAX_KEY_VER_REGWEN, 0x78u)
    FIELD(MAX_KEY_VER_REGWEN, EN, 0u, 1u)
REG32(MAX_KEY_VER_SHADOWED, 0x7cu)
REG32(SW_SHARE0_OUTPUT_0, 0x80u)
REG32(SW_SHARE0_OUTPUT_1, 0x84u)
REG32(SW_SHARE0_OUTPUT_2, 0x88u)
REG32(SW_SHARE0_OUTPUT_3, 0x8cu)
REG32(SW_SHARE0_OUTPUT_4, 0x90u)
REG32(SW_SHARE0_OUTPUT_5, 0x94u)
REG32(SW_SHARE0_OUTPUT_6, 0x98u)
REG32(SW_SHARE0_OUTPUT_7, 0x9cu)
REG32(SW_SHARE1_OUTPUT_0, 0xa0u)
REG32(SW_SHARE1_OUTPUT_1, 0xa4u)
REG32(SW_SHARE1_OUTPUT_2, 0xa8u)
REG32(SW_SHARE1_OUTPUT_3, 0xacu)
REG32(SW_SHARE1_OUTPUT_4, 0xb0u)
REG32(SW_SHARE1_OUTPUT_5, 0xb4u)
REG32(SW_SHARE1_OUTPUT_6, 0xb8u)
REG32(SW_SHARE1_OUTPUT_7, 0xbcu)
REG32(WORKING_STATE, 0xc0u)
    FIELD(WORKING_STATE, VAL, 0u, 2u)
REG32(OP_STATUS, 0xc4u)
    FIELD(OP_STATUS, VAL, 0u, 2u)
REG32(ERR_CODE, 0xc8u)
    FIELD(ERR_CODE, INVALID_OP, 0u, 1u)
    FIELD(ERR_CODE, INVALID_KMAC_INPUT, 1u, 1u)
    FIELD(ERR_CODE, INVALID_SHADOW_UPDATE, 2u, 1u)
REG32(FAULT_STATUS, 0xccu)
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
REG32(DEBUG, 0xd0u)
    FIELD(DEBUG, INVALID_CREATOR_SEED, 0u, 1u)
    FIELD(DEBUG, INVALID_OWNER_SEED, 1u, 1u)
    FIELD(DEBUG, INVALID_DEV_ID, 2u, 1u)
    FIELD(DEBUG, INVALID_HEALTH_STATE, 3u, 1u)
    FIELD(DEBUG, INVALID_KEY_VERSION, 4u, 1u)
    FIELD(DEBUG, INVALID_KEY, 5u, 1u)
    FIELD(DEBUG, INVALID_DIGEST, 6u, 1u)
    FIELD(DEBUG, INVALID_ROOT_KEY, 7u, 1u)
    FIELD(DEBUG, INACTIVE_LC_EN, 8u, 1u)
/* clang-format on */

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_DEBUG)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))

#define INTR_MASK (INTR_OP_DONE_MASK)
#define ALERT_MASK \
    (R_ALERT_TEST_RECOV_OPERATION_MASK | R_ALERT_TEST_FATAL_FAULT_MASK)

#define R_CONTROL_SHADOWED_MASK \
    (R_CONTROL_SHADOWED_OPERATION_MASK | R_CONTROL_SHADOWED_DEST_SEL_MASK | \
     R_CONTROL_SHADOWED_SLOT_SRC_SEL_MASK | \
     R_CONTROL_SHADOWED_SLOT_DST_SEL_MASK)

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
     R_DEBUG_INVALID_DIGEST_MASK | R_DEBUG_INVALID_ROOT_KEY_MASK | \
     R_DEBUG_INACTIVE_LC_EN_MASK)

#define KEYMGR_DPE_LFSR_WIDTH    64u
#define KEYMGR_DPE_ENTROPY_WIDTH (KEYMGR_DPE_LFSR_WIDTH / 2u)
#define KEYMGR_DPE_ENTROPY_ROUNDS \
    (KEYMGR_DPE_KEY_WIDTH / KEYMGR_DPE_ENTROPY_WIDTH)

#define KEYMGR_DPE_RESEED_COUNT \
    (KEYMGR_DPE_LFSR_WIDTH / (8u * sizeof(uint32_t)))

typedef enum {
    KEYMGR_DPE_KEY_SINK_AES,
    KEYMGR_DPE_KEY_SINK_KMAC,
    KEYMGR_DPE_KEY_SINK_OTBN,
    KEYMGR_DPE_KEY_SINK_COUNT
} OtKeyMgrDpeKeySink;

#define KEY_SINK_OFFSET 1

/* values for CONTROL_SHADOWED.OPERATION */
typedef enum {
    KEYMGR_DPE_OP_ADVANCE = 0,
    KEYMGR_DPE_OP_ERASE_SLOT = 1,
    KEYMGR_DPE_OP_GENERATE_SW_OUTPUT = 2,
    KEYMGR_DPE_OP_GENERATE_HW_OUTPUT = 3,
    KEYMGR_DPE_OP_DISABLE = 4,
} OtKeyMgrDpeOperation;

/* values for CONTROL_SHADOWED.DEST_SEL */
typedef enum {
    KEYMGR_DPE_DEST_SEL_VALUE_NONE = 0,
    KEYMGR_DPE_DEST_SEL_VALUE_AES = KEYMGR_DPE_KEY_SINK_AES + KEY_SINK_OFFSET,
    KEYMGR_DPE_DEST_SEL_VALUE_KMAC = KEYMGR_DPE_KEY_SINK_KMAC + KEY_SINK_OFFSET,
    KEYMGR_DPE_DEST_SEL_VALUE_OTBN = KEYMGR_DPE_KEY_SINK_OTBN + KEY_SINK_OFFSET,
} OtKeyMgrDpeDestSel;

/* values for SIDELOAD_CLEAR.VAL */
typedef enum {
    KEYMGR_DPE_SIDELOAD_CLEAR_NONE = 0,
    KEYMGR_DPE_SIDELOAD_CLEAR_AES = KEYMGR_DPE_KEY_SINK_AES + KEY_SINK_OFFSET,
    KEYMGR_DPE_SIDELOAD_CLEAR_KMAC = KEYMGR_DPE_KEY_SINK_KMAC + KEY_SINK_OFFSET,
    KEYMGR_DPE_SIDELOAD_CLEAR_OTBN = KEYMGR_DPE_KEY_SINK_OTBN + KEY_SINK_OFFSET,
} OtKeyMgrDpeSideloadClear;

/* values for WORKING_STATE.STATE */
typedef enum {
    KEYMGR_DPE_WORKING_STATE_RESET = 0,
    KEYMGR_DPE_WORKING_STATE_AVAILABLE = 1,
    KEYMGR_DPE_WORKING_STATE_DISABLED = 2,
    KEYMGR_DPE_WORKING_STATE_INVALID = 3,
} OtKeyMgrDpeWorkingState;

/* value for OP_STATUS.STATUS */
typedef enum {
    KEYMGR_DPE_OP_STATUS_IDLE = 0,
    KEYMGR_DPE_OP_STATUS_WIP = 1,
    KEYMGR_DPE_OP_STATUS_DONE_SUCCESS = 2,
    KEYMGR_DPE_OP_STATUS_DONE_ERROR = 3,
} OtKeyMgrDpeOpStatus;

enum {
    /* clang-format off */
    ALERT_RECOVERABLE,
    ALERT_FATAL,
    ALERT_COUNT
    /* clang-format on */
};

enum {
    KEYMGR_DPE_SEED_LFSR,
    KEYMGR_DPE_SEED_REV,
    KEYMGR_DPE_SEED_SW_OUT,
    KEYMGR_DPE_SEED_HW_OUT,
    KEYMGR_DPE_SEED_AES,
    KEYMGR_DPE_SEED_KMAC,
    KEYMGR_DPE_SEED_OTBN,
    KEYMGR_DPE_SEED_NONE,
    KEYMGR_DPE_SEED_COUNT,
};

typedef enum {
    KEYMGR_DPE_ST_RESET,
    KEYMGR_DPE_ST_ENTROPY_RESEED,
    KEYMGR_DPE_ST_RANDOM,
    KEYMGR_DPE_ST_ROOTKEY,
    KEYMGR_DPE_ST_AVAILABLE,
    KEYMGR_DPE_ST_WIPE,
    KEYMGR_DPE_ST_DISABLING,
    KEYMGR_DPE_ST_DISABLED,
    KEYMGR_DPE_ST_INVALID,
} OtKeyMgrDpeFSMState;

typedef struct {
    OtEDNState *device;
    uint8_t ep;
    bool connected;
    bool scheduled;
} OtKeyMgrDpeEDN;

typedef struct {
    OtPrngState *state;
    bool reseed_req;
    bool reseed_ack;
    uint8_t reseed_cnt;
} OtKeyMgrDpePrng;

typedef struct {
    bool allow_child;
    bool exportable;
    bool retain_parent;
} OtKeyMgrDpeSlotPolicy;

typedef struct {
    uint8_t share0[KEYMGR_DPE_KEY_SIZE];
    uint8_t share1[KEYMGR_DPE_KEY_SIZE];
    bool valid;
} OtKeyMgrDpeKey;

typedef struct {
    OtKeyMgrDpeKey key; /* always 256 bit keys */
    uint32_t max_key_version;
    uint8_t boot_stage;
    OtKeyMgrDpeSlotPolicy policy;
    bool valid;
} OtKeyMgrDpeSlot;

typedef struct {
    uint8_t share0[OT_OTBN_KEY_SIZE];
    uint8_t share1[OT_OTBN_KEY_SIZE];
    bool valid;
} OtKeyMgrDpeOtbnKey;

typedef struct {
    bool op_req;
    bool op_ack;
} OtKeyMgrDpeOpState;

typedef struct {
    uint8_t *data;
    unsigned offset; /* current read offset (in bytes) */
    unsigned length; /* current length (in bytes) */
} OtKeyMgrDpeKdfBuffer;

typedef struct OtKeyMgrDpeState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    IbexIRQ irq;
    IbexIRQ alerts[ALERT_COUNT];
    QEMUBH *fsm_tick_bh;
    OtKeyMgrDpeKdfBuffer kdf_buf;

    uint32_t regs[REGS_COUNT];
    OtShadowReg control;
    OtShadowReg reseed_interval;
    OtShadowReg max_key_ver;
    uint8_t *salt;
    uint8_t *sw_binding;

    bool enabled;
    OtKeyMgrDpeFSMState state;
    OtKeyMgrDpePrng prng;
    OtKeyMgrDpeOpState op_state;
    uint8_t *seeds[KEYMGR_DPE_SEED_COUNT];

    /* key slots */
    OtKeyMgrDpeSlot *key_slots;

    /* SW output keys */
    OtKeyMgrDpeKey *sw_out_key;

    char *hexstr;

    /* properties */
    char *ot_id;
    OtKeyMgrDpeEDN edn;
    OtKMACState *kmac;
    uint8_t kmac_app;
    OtLcCtrlState *lc_ctrl;
    OtOTPState *otp;
    OtRomCtrlState *rom_ctrl[NUM_ROM_DIGEST_INPUTS];
    DeviceState *key_sinks[KEYMGR_DPE_KEY_SINK_COUNT];
    char *seed_xstrs[KEYMGR_DPE_SEED_COUNT];
} OtKeyMgrDpeState;

struct OtKeyMgrDpeClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

static const OtKeyMgrDpeSlotPolicy DEFAULT_UDS_POLICY = {
    .allow_child = true,
    .exportable = false,
    .retain_parent = false,
};

static const size_t OT_KEY_MGR_DPE_KEY_SINK_SIZES[KEYMGR_DPE_KEY_SINK_COUNT] = {
    [KEYMGR_DPE_KEY_SINK_AES] = OT_AES_KEY_SIZE,
    [KEYMGR_DPE_KEY_SINK_KMAC] = OT_KMAC_KEY_SIZE,
    [KEYMGR_DPE_KEY_SINK_OTBN] = OT_OTBN_KEY_SIZE,
};

static const OtKMACAppCfg KMAC_APP_CFG = OT_KMAC_CONFIG(KMAC, 256u, "KMAC", "");

#define REG_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    REG_ENTRY(INTR_STATE),
    REG_ENTRY(INTR_ENABLE),
    REG_ENTRY(INTR_TEST),
    REG_ENTRY(ALERT_TEST),
    REG_ENTRY(CFG_REGWEN),
    REG_ENTRY(START),
    REG_ENTRY(CONTROL_SHADOWED),
    REG_ENTRY(SIDELOAD_CLEAR),
    REG_ENTRY(RESEED_INTERVAL_REGWEN),
    REG_ENTRY(RESEED_INTERVAL_SHADOWED),
    REG_ENTRY(SLOT_POLICY_REGWEN),
    REG_ENTRY(SLOT_POLICY),
    REG_ENTRY(SW_BINDING_REGWEN),
    REG_ENTRY(SW_BINDING_0),
    REG_ENTRY(SW_BINDING_1),
    REG_ENTRY(SW_BINDING_2),
    REG_ENTRY(SW_BINDING_3),
    REG_ENTRY(SW_BINDING_4),
    REG_ENTRY(SW_BINDING_5),
    REG_ENTRY(SW_BINDING_6),
    REG_ENTRY(SW_BINDING_7),
    REG_ENTRY(SALT_0),
    REG_ENTRY(SALT_1),
    REG_ENTRY(SALT_2),
    REG_ENTRY(SALT_3),
    REG_ENTRY(SALT_4),
    REG_ENTRY(SALT_5),
    REG_ENTRY(SALT_6),
    REG_ENTRY(SALT_7),
    REG_ENTRY(KEY_VERSION),
    REG_ENTRY(MAX_KEY_VER_REGWEN),
    REG_ENTRY(MAX_KEY_VER_SHADOWED),
    REG_ENTRY(SW_SHARE0_OUTPUT_0),
    REG_ENTRY(SW_SHARE0_OUTPUT_1),
    REG_ENTRY(SW_SHARE0_OUTPUT_2),
    REG_ENTRY(SW_SHARE0_OUTPUT_3),
    REG_ENTRY(SW_SHARE0_OUTPUT_4),
    REG_ENTRY(SW_SHARE0_OUTPUT_5),
    REG_ENTRY(SW_SHARE0_OUTPUT_6),
    REG_ENTRY(SW_SHARE0_OUTPUT_7),
    REG_ENTRY(SW_SHARE1_OUTPUT_0),
    REG_ENTRY(SW_SHARE1_OUTPUT_1),
    REG_ENTRY(SW_SHARE1_OUTPUT_2),
    REG_ENTRY(SW_SHARE1_OUTPUT_3),
    REG_ENTRY(SW_SHARE1_OUTPUT_4),
    REG_ENTRY(SW_SHARE1_OUTPUT_5),
    REG_ENTRY(SW_SHARE1_OUTPUT_6),
    REG_ENTRY(SW_SHARE1_OUTPUT_7),
    REG_ENTRY(WORKING_STATE),
    REG_ENTRY(OP_STATUS),
    REG_ENTRY(ERR_CODE),
    REG_ENTRY(FAULT_STATUS),
    REG_ENTRY(DEBUG),
};
#undef REG_ENTRY
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define OP_ENTRY(_op_) [KEYMGR_DPE_OP_##_op_] = stringify(_op_)
static const char *OP_NAMES[] = {
    OP_ENTRY(ADVANCE),
    OP_ENTRY(ERASE_SLOT),
    OP_ENTRY(GENERATE_SW_OUTPUT),
    OP_ENTRY(GENERATE_HW_OUTPUT),
    OP_ENTRY(DISABLE),
};
#undef OP_ENTRY
#define OP_NAME(_op_) \
    (((unsigned)(_op_)) < ARRAY_SIZE(OP_NAMES) ? OP_NAMES[(_op_)] : "?")

#define SIDELOAD_CLEAR_ENTRY(_st_) \
    [KEYMGR_DPE_SIDELOAD_CLEAR_##_st_] = stringify(_st_)
static const char *SIDELOAD_CLEAR_NAMES[] = {
    SIDELOAD_CLEAR_ENTRY(NONE),
    SIDELOAD_CLEAR_ENTRY(AES),
    SIDELOAD_CLEAR_ENTRY(KMAC),
    SIDELOAD_CLEAR_ENTRY(OTBN),
};
#undef SIDELOAD_CLEAR_ENTRY
#define SIDELOAD_CLEAR_NAME(_st_) \
    (((unsigned)(_st_)) < ARRAY_SIZE(SIDELOAD_CLEAR_NAMES) ? \
         SIDELOAD_CLEAR_NAMES[(_st_)] : \
         "?")

#define WORKING_STATE_ENTRY(_st_) \
    [KEYMGR_DPE_WORKING_STATE_##_st_] = stringify(_st_)
static const char *WORKING_STATE_NAMES[] = {
    WORKING_STATE_ENTRY(RESET),
    WORKING_STATE_ENTRY(AVAILABLE),
    WORKING_STATE_ENTRY(DISABLED),
    WORKING_STATE_ENTRY(INVALID),
};
#undef WORKING_STATE_ENTRY
#define WORKING_STATE_NAME(_st_) \
    (((unsigned)(_st_)) < ARRAY_SIZE(WORKING_STATE_NAMES) ? \
         WORKING_STATE_NAMES[(_st_)] : \
         "?")

#define OP_STATUS_ENTRY(_st_) [KEYMGR_DPE_OP_STATUS_##_st_] = stringify(_st_)
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

#define FST_ENTRY(_st_) [KEYMGR_DPE_ST_##_st_] = stringify(_st_)
static const char *FST_NAMES[] = {
    /* clang-format off */
    FST_ENTRY(RESET),
    FST_ENTRY(ENTROPY_RESEED),
    FST_ENTRY(RANDOM),
    FST_ENTRY(ROOTKEY),
    FST_ENTRY(AVAILABLE),
    FST_ENTRY(WIPE),
    FST_ENTRY(DISABLING),
    FST_ENTRY(DISABLED),
    FST_ENTRY(INVALID),
    /* clang-format on */
};
#undef FST_ENTRY
#define FST_NAME(_st_) \
    (((unsigned)(_st_)) < ARRAY_SIZE(FST_NAMES) ? FST_NAMES[(_st_)] : "?")

#define OT_KEYMGR_DPE_HEXSTR_SIZE 256u

#define ot_keymgr_dpe_dump_bigint(_s_, _b_, _l_) \
    ot_common_lhexdump(_b_, _l_, true, (_s_)->hexstr, OT_KEYMGR_DPE_HEXSTR_SIZE)

static void ot_keymgr_dpe_dump_kdf_buf(OtKeyMgrDpeState *s, const char *op)
{
    if (trace_event_get_state(TRACE_OT_KEYMGR_DPE_DUMP_KDF_BUF)) {
        size_t msgs = (s->kdf_buf.length + OT_KMAC_APP_MSG_BYTES - 1u) /
                      OT_KMAC_APP_MSG_BYTES;
        for (size_t ix = 0u; ix < msgs; ix++) {
            trace_ot_keymgr_dpe_dump_kdf_buf(
                s->ot_id, op, ix,
                ot_keymgr_dpe_dump_bigint(
                    s, &s->kdf_buf.data[ix * OT_KMAC_APP_MSG_BYTES],
                    OT_KMAC_APP_MSG_BYTES));
        }
    }
}

#define ot_keymgr_dpe_schedule_fsm(_s_) \
    ot_keymgr_dpe_xschedule_fsm(_s_, __func__, __LINE__)

static void
ot_keymgr_dpe_xschedule_fsm(OtKeyMgrDpeState *s, const char *func, int line)
{
    trace_ot_keymgr_dpe_schedule_fsm(s->ot_id, func, line);
    qemu_bh_schedule(s->fsm_tick_bh);
}

static void ot_keymgr_dpe_update_irq(OtKeyMgrDpeState *s)
{
    bool level = (bool)(s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE]);
    trace_ot_keymgr_dpe_irq(s->ot_id, s->regs[R_INTR_STATE],
                            s->regs[R_INTR_ENABLE], level);
    ibex_irq_set(&s->irq, (int)level);
}

static void ot_keymgr_dpe_update_alert(OtKeyMgrDpeState *s)
{
    uint32_t level = s->regs[R_ALERT_TEST];

    if (s->regs[R_FAULT_STATUS] & FAULT_STATUS_MASK) {
        level |= 1u << ALERT_FATAL;
    }
    if (s->regs[R_ERR_CODE] & ERR_CODE_MASK) {
        level |= 1u << ALERT_RECOVERABLE;
    }

    for (unsigned ix = 0u; ix < ARRAY_SIZE(s->alerts); ix++) {
        ibex_irq_set(&s->alerts[ix], (int)((level >> ix) & 0x1u));
    }
}

static OtKeyMgrDpeWorkingState
ot_keymgr_dpe_get_working_state(const OtKeyMgrDpeState *s)
{
    switch (FIELD_EX32(s->regs[R_WORKING_STATE], WORKING_STATE, VAL)) {
    case KEYMGR_DPE_WORKING_STATE_RESET:
        return KEYMGR_DPE_WORKING_STATE_RESET;
    case KEYMGR_DPE_WORKING_STATE_AVAILABLE:
        return KEYMGR_DPE_WORKING_STATE_AVAILABLE;
    case KEYMGR_DPE_WORKING_STATE_DISABLED:
        return KEYMGR_DPE_WORKING_STATE_DISABLED;
    default:
        return KEYMGR_DPE_WORKING_STATE_INVALID;
    }
}

#define ot_keymgr_dpe_change_working_state(_s_, _working_state_) \
    ot_keymgr_dpe_xchange_working_state(_s_, _working_state_, __LINE__)

static void ot_keymgr_dpe_xchange_working_state(
    OtKeyMgrDpeState *s, OtKeyMgrDpeWorkingState working_state, int line)
{
    OtKeyMgrDpeWorkingState prev_working_state =
        ot_keymgr_dpe_get_working_state(s);

    if (prev_working_state != working_state) {
        trace_ot_keymgr_dpe_change_working_state(s->ot_id, line,
                                                 WORKING_STATE_NAME(
                                                     prev_working_state),
                                                 prev_working_state,
                                                 WORKING_STATE_NAME(
                                                     working_state),
                                                 working_state);
        s->regs[R_WORKING_STATE] = working_state;
    }
}

static OtKeyMgrDpeOpStatus
ot_keymgr_dpe_get_op_status(const OtKeyMgrDpeState *s)
{
    switch (FIELD_EX32(s->regs[R_OP_STATUS], OP_STATUS, VAL)) {
    case KEYMGR_DPE_OP_STATUS_IDLE:
        return KEYMGR_DPE_OP_STATUS_IDLE;
    case KEYMGR_DPE_OP_STATUS_WIP:
        return KEYMGR_DPE_OP_STATUS_WIP;
    case KEYMGR_DPE_OP_STATUS_DONE_SUCCESS:
        return KEYMGR_DPE_OP_STATUS_DONE_SUCCESS;
    case KEYMGR_DPE_OP_STATUS_DONE_ERROR:
        return KEYMGR_DPE_OP_STATUS_DONE_ERROR;
    default:
        g_assert_not_reached();
    }
}

#define ot_keymgr_dpe_change_op_status(_s_, _op_status_) \
    ot_keymgr_dpe_xchange_op_status(_s_, _op_status_, __LINE__)

static void ot_keymgr_dpe_xchange_op_status(
    OtKeyMgrDpeState *s, OtKeyMgrDpeOpStatus op_status, int line)
{
    OtKeyMgrDpeOpStatus prev_op_status = ot_keymgr_dpe_get_op_status(s);
    if (prev_op_status != op_status) {
        trace_ot_keymgr_dpe_change_op_status(s->ot_id, line,
                                             OP_STATUS_NAME(prev_op_status),
                                             prev_op_status,
                                             OP_STATUS_NAME(op_status),
                                             op_status);
        s->regs[R_OP_STATUS] =
            FIELD_DP32(s->regs[R_OP_STATUS], OP_STATUS, VAL, op_status);
    }
}

static void ot_keymgr_dpe_request_entropy(OtKeyMgrDpeState *s);

static void ot_keymgr_dpe_push_entropy(void *opaque, uint32_t bits, bool fips)
{
    (void)fips;
    OtKeyMgrDpeState *s = opaque;
    OtKeyMgrDpeEDN *edn = &s->edn;
    OtKeyMgrDpePrng *prng = &s->prng;

    if (!edn->scheduled) {
        trace_ot_keymgr_dpe_error(s->ot_id, "Unexpected entropy");
        return;
    }
    edn->scheduled = false;

    ot_prng_reseed(prng->state, bits);
    prng->reseed_cnt++;

    bool resched = prng->reseed_cnt < KEYMGR_DPE_RESEED_COUNT;

    trace_ot_keymgr_dpe_entropy(s->ot_id, prng->reseed_cnt, resched);

    if (resched) {
        /* We need more entropy */
        ot_keymgr_dpe_request_entropy(s);
    } else if (prng->reseed_req) {
        prng->reseed_ack = true;
        prng->reseed_cnt = 0u;
        ot_keymgr_dpe_schedule_fsm(s);
    }
}

static void ot_keymgr_dpe_request_entropy(OtKeyMgrDpeState *s)
{
    OtKeyMgrDpeEDN *edn = &s->edn;

    if (!edn->connected) {
        ot_edn_connect_endpoint(edn->device, edn->ep,
                                &ot_keymgr_dpe_push_entropy, s);
        edn->connected = true;
    }

    if (!edn->scheduled) {
        edn->scheduled = s->prng.reseed_req;
        if (!edn->scheduled) {
            return;
        }
        if (ot_edn_request_entropy(edn->device, edn->ep)) {
            error_setg(&error_fatal,
                       "%s: %s: keymgr_dpe failed to request entropy", __func__,
                       s->ot_id);
        }
    }
}

static void ot_keymgr_dpe_push_key(
    OtKeyMgrDpeState *s, OtKeyMgrDpeKeySink key_sink, const uint8_t *key_share0,
    const uint8_t *key_share1, bool valid)
{
    g_assert((unsigned)key_sink < KEYMGR_DPE_KEY_SINK_COUNT);

    size_t key_size = OT_KEY_MGR_DPE_KEY_SINK_SIZES[key_sink];

    DeviceState *sink = s->key_sinks[key_sink];
    if (!sink) {
        return;
    }

    OtKeySinkIfClass *kc = OT_KEY_SINK_IF_GET_CLASS(sink);
    OtKeySinkIf *ki = OT_KEY_SINK_IF(sink);

    g_assert(kc->push_key);

    kc->push_key(ki, key_share0, key_share1, key_size, valid);
}

static void ot_keymgr_dpe_kmac_push_key(OtKeyMgrDpeState *s)
{
    uint32_t ctrl = ot_shadow_reg_peek(&s->control);
    uint8_t slot_src_sel =
        (uint8_t)FIELD_EX32(ctrl, CONTROL_SHADOWED, SLOT_SRC_SEL);
    OtKeyMgrDpeSlot *src_slot = &s->key_slots[slot_src_sel];

    if (trace_event_get_state(TRACE_OT_KEYMGR_DPE_KMAC_PUSH_KEY)) {
        uint8_t key_value[OT_KMAC_KEY_SIZE];
        for (unsigned ix = 0u; ix < OT_KMAC_KEY_SIZE; ix++) {
            key_value[ix] = src_slot->key.share0[ix] ^ src_slot->key.share1[ix];
        }

        trace_ot_keymgr_dpe_kmac_push_key(
            s->ot_id, src_slot->valid,
            ot_keymgr_dpe_dump_bigint(s, key_value, OT_KMAC_KEY_SIZE));
    }

    ot_keymgr_dpe_push_key(s, KEYMGR_DPE_KEY_SINK_KMAC, src_slot->key.share0,
                           src_slot->key.share1, src_slot->valid);
}

static void ot_keymgr_dpe_send_kmac_req(OtKeyMgrDpeState *s)
{
    uint32_t len = s->kdf_buf.offset;

    g_assert(s->kdf_buf.length);
    g_assert(s->kdf_buf.offset < s->kdf_buf.length);

    unsigned msg_len = s->kdf_buf.length - s->kdf_buf.offset;
    if (msg_len > OT_KMAC_APP_MSG_BYTES) {
        msg_len = OT_KMAC_APP_MSG_BYTES;
    }

    unsigned offset = s->kdf_buf.offset;
    s->kdf_buf.offset += msg_len;

    OtKMACAppReq req = {
        .last = s->kdf_buf.offset == s->kdf_buf.length,
        .msg_len = msg_len,
    };
    memcpy(req.msg_data, &s->kdf_buf.data[offset], msg_len);

    trace_ot_keymgr_dpe_kmac_req(s->ot_id, len, req.msg_len, req.last);

    OtKMACClass *kc = OT_KMAC_GET_CLASS(s->kmac);
    kc->app_request(s->kmac, s->kmac_app, &req);
}

static bool ot_keymgr_dpe_handle_kmac_resp_advance(
    OtKeyMgrDpeState *s, uint8_t slot_src_sel, uint8_t slot_dst_sel,
    const OtKMACAppRsp *rsp)
{
    uint32_t max_key_version = ot_shadow_reg_peek(&s->max_key_ver);
    uint32_t slot_policy = s->regs[R_SLOT_POLICY];
    OtKeyMgrDpeSlot *src_slot = &s->key_slots[slot_src_sel];
    OtKeyMgrDpeSlot *dst_slot = &s->key_slots[slot_dst_sel];

    dst_slot->valid = true;
    memcpy(dst_slot->key.share0, rsp->digest_share0, OT_KMAC_KEY_SIZE);
    memcpy(dst_slot->key.share1, rsp->digest_share1, OT_KMAC_KEY_SIZE);
    dst_slot->max_key_version = max_key_version;
    dst_slot->boot_stage = src_slot->boot_stage + 1u;
    dst_slot->policy.allow_child =
        (bool)(slot_policy & R_SLOT_POLICY_ALLOW_CHILD_MASK);
    dst_slot->policy.exportable =
        (bool)(slot_policy & R_SLOT_POLICY_EXPORTABLE_MASK);
    dst_slot->policy.retain_parent =
        (bool)(slot_policy & R_SLOT_POLICY_RETAIN_PARENT_MASK);

    /*
     * Unlock `SW_BINDING`, `SLOT_POLICY` and `MAX_KEY_VERSION` registers
     * after succesful advance
     */
    s->regs[R_SW_BINDING_REGWEN] |= R_SW_BINDING_REGWEN_EN_MASK;
    s->regs[R_SLOT_POLICY_REGWEN] |= R_SLOT_POLICY_REGWEN_EN_MASK;
    s->regs[R_MAX_KEY_VER_REGWEN] |= R_MAX_KEY_VER_REGWEN_EN_MASK;

    return true;
}

static bool ot_keymgr_dpe_handle_kmac_resp_gen_hw_out(
    OtKeyMgrDpeState *s, uint8_t dest_sel, const OtKMACAppRsp *rsp)
{
    switch (dest_sel) {
    case KEYMGR_DPE_DEST_SEL_VALUE_AES:
    case KEYMGR_DPE_DEST_SEL_VALUE_KMAC:
    case KEYMGR_DPE_DEST_SEL_VALUE_OTBN: {
        OtKeyMgrDpeKeySink key_sink = dest_sel - KEY_SINK_OFFSET;
        ot_keymgr_dpe_push_key(s, key_sink, rsp->digest_share0,
                               rsp->digest_share1, true);
        return dest_sel != KEYMGR_DPE_DEST_SEL_VALUE_KMAC;
    }
    case KEYMGR_DPE_DEST_SEL_VALUE_NONE:
        /* no output, just ack the operation */
        return true;
    default:
        g_assert_not_reached();
    }
}

static bool ot_keymgr_dpe_handle_kmac_resp_gen_sw_out(OtKeyMgrDpeState *s,
                                                      const OtKMACAppRsp *rsp)
{
    memcpy(s->sw_out_key->share0, rsp->digest_share0, OT_KMAC_KEY_SIZE);
    memcpy(s->sw_out_key->share1, rsp->digest_share1, OT_KMAC_KEY_SIZE);
    s->sw_out_key->valid = true;
    return true;
}

static void
ot_keymgr_dpe_handle_kmac_response(void *opaque, const OtKMACAppRsp *rsp)
{
    OtKeyMgrDpeState *s = OT_KEYMGR_DPE(opaque);
    uint32_t ctrl = ot_shadow_reg_peek(&s->control);
    bool reset_kmac_key = true;

    if (trace_event_get_state(TRACE_OT_KEYMGR_DPE_KMAC_RSP)) {
        if (rsp->done) {
            uint8_t key[OT_KMAC_APP_DIGEST_BYTES];
            for (unsigned ix = 0u; ix < OT_KMAC_APP_DIGEST_BYTES; ix++) {
                key[ix] = rsp->digest_share0[ix] ^ rsp->digest_share1[ix];
            }
            trace_ot_keymgr_dpe_kmac_rsp(
                s->ot_id, true,
                ot_keymgr_dpe_dump_bigint(s, key, OT_KMAC_APP_DIGEST_BYTES));
        } else {
            trace_ot_keymgr_dpe_kmac_rsp(s->ot_id, false, "");
        }
    }

    if (!rsp->done) {
        /* not last response from KMAC, send more data */
        ot_keymgr_dpe_send_kmac_req(s);
        return;
    }

    g_assert(s->kdf_buf.offset == s->kdf_buf.length);

    switch (FIELD_EX32(ctrl, CONTROL_SHADOWED, OPERATION)) {
    case KEYMGR_DPE_OP_ADVANCE: {
        uint8_t slot_src_sel =
            (uint8_t)FIELD_EX32(ctrl, CONTROL_SHADOWED, SLOT_SRC_SEL);
        uint8_t slot_dst_sel =
            (uint8_t)FIELD_EX32(ctrl, CONTROL_SHADOWED, SLOT_DST_SEL);
        reset_kmac_key =
            ot_keymgr_dpe_handle_kmac_resp_advance(s, slot_src_sel,
                                                   slot_dst_sel, rsp);
        break;
    }
    case KEYMGR_DPE_OP_GENERATE_HW_OUTPUT: {
        uint8_t dest_sel =
            (uint8_t)FIELD_EX32(ctrl, CONTROL_SHADOWED, DEST_SEL);
        reset_kmac_key =
            ot_keymgr_dpe_handle_kmac_resp_gen_hw_out(s, dest_sel, rsp);
        break;
    }
    case KEYMGR_DPE_OP_GENERATE_SW_OUTPUT:
        reset_kmac_key = ot_keymgr_dpe_handle_kmac_resp_gen_sw_out(s, rsp);
        break;
    default:
        g_assert_not_reached();
    }

    /* reset KMAC key if required */
    if (reset_kmac_key) {
        /* TODO: maybe push last sideloaded KMAC key instead? */
        trace_ot_keymgr_dpe_reset_kmac_key(s->ot_id);
        ot_keymgr_dpe_push_key(s, KEYMGR_DPE_KEY_SINK_KMAC, NULL, NULL, false);
    }

    /* operation complete */
    s->op_state.op_ack = true;
    ot_keymgr_dpe_schedule_fsm(s);
}

/* check that 'data' is not all zeros or all ones */
static bool ot_keymgr_dpe_valid_data_check(const uint8_t *data, size_t len)
{
    size_t popcount = 0u;
    for (unsigned ix = 0u; ix < len; ix++) {
        popcount += __builtin_popcount(data[ix]);
    }
    return (popcount && popcount != (len * BITS_PER_BYTE));
}

static void ot_keymgr_dpe_reset_kdf_buffer(OtKeyMgrDpeState *s)
{
    memset(s->kdf_buf.data, 0u, KEYMGR_DPE_KDF_BUFFER_BYTES);
    s->kdf_buf.offset = 0u;
    s->kdf_buf.length = 0u;
}

static void ot_keymgr_dpe_kdf_push_bytes(OtKeyMgrDpeState *s,
                                         const uint8_t *data, size_t len)
{
    g_assert(s->kdf_buf.length + len <= KEYMGR_DPE_KDF_BUFFER_BYTES);

    memcpy(&s->kdf_buf.data[s->kdf_buf.length], data, len);
    s->kdf_buf.length += len;
}

static void ot_keymgr_dpe_dump_kdf_material(
    OtKeyMgrDpeState *s, const char *what, const uint8_t *buf, size_t len)
{
    if (trace_event_get_state(TRACE_OT_KEYMGR_DPE_DUMP_KDF_MATERIAL)) {
        const char *hexstr = ot_keymgr_dpe_dump_bigint(s, buf, len);
        trace_ot_keymgr_dpe_dump_kdf_material(s->ot_id, what, hexstr);
    }
}

static void ot_keymgr_dpe_kdf_push_key_version(OtKeyMgrDpeState *s)
{
    uint8_t buf[sizeof(uint32_t)];
    stl_le_p(buf, s->regs[R_KEY_VERSION]);
    ot_keymgr_dpe_kdf_push_bytes(s, buf, sizeof(uint32_t));

    ot_keymgr_dpe_dump_kdf_material(s, "KEY_VERSION", buf, sizeof(uint32_t));
}

static size_t
ot_keymgr_dpe_kdf_append_creator_seed(OtKeyMgrDpeState *s, bool *dvalid)
{
    OtOTPKeyMgrSecret secret = { 0u };

    OtOTPClass *otp_oc = OBJECT_GET_CLASS(OtOTPClass, s->otp, TYPE_OT_OTP);
    g_assert(otp_oc);
    otp_oc->get_keymgr_secret(s->otp, OTP_KEYMGR_SECRET_CREATOR_SEED, &secret);

    ot_keymgr_dpe_kdf_push_bytes(s, secret.secret, OT_OTP_KEYMGR_SECRET_SIZE);
    *dvalid &= ot_keymgr_dpe_valid_data_check(secret.secret,
                                              OT_OTP_KEYMGR_SECRET_SIZE);

    ot_keymgr_dpe_dump_kdf_material(s, "CREATOR_SEED", secret.secret,
                                    OT_OTP_KEYMGR_SECRET_SIZE);
    return OT_OTP_KEYMGR_SECRET_SIZE;
}

static size_t ot_keymgr_dpe_kdf_append_rom_digest(
    OtKeyMgrDpeState *s, unsigned rom_idx, bool *dvalid)
{
    uint8_t rom_digest[OT_ROM_DIGEST_BYTES] = { 0u };

    OtRomCtrlClass *rcc = OT_ROM_CTRL_GET_CLASS(s->rom_ctrl[rom_idx]);
    rcc->get_rom_digest(s->rom_ctrl[rom_idx], rom_digest);

    ot_keymgr_dpe_kdf_push_bytes(s, rom_digest, OT_ROM_DIGEST_BYTES);
    *dvalid &= ot_keymgr_dpe_valid_data_check(rom_digest, OT_ROM_DIGEST_BYTES);

    char buf[16u];
    snprintf(buf, sizeof(buf), "ROM%u_DIGEST", rom_idx);
    ot_keymgr_dpe_dump_kdf_material(s, buf, rom_digest, OT_ROM_DIGEST_BYTES);

    return OT_ROM_DIGEST_BYTES;
}

static size_t ot_keymgr_dpe_kdf_append_km_div(OtKeyMgrDpeState *s, bool *dvalid)
{
    OtLcCtrlKeyMgrDiv km_div = { 0u };

    OtLcCtrlClass *lc = OT_LC_CTRL_GET_CLASS(s->lc_ctrl);
    lc->get_keymgr_div(s->lc_ctrl, &km_div);

    ot_keymgr_dpe_kdf_push_bytes(s, km_div.data, OT_LC_KEYMGR_DIV_BYTES);
    *dvalid &=
        ot_keymgr_dpe_valid_data_check(km_div.data, OT_LC_KEYMGR_DIV_BYTES);

    ot_keymgr_dpe_dump_kdf_material(s, "KM_DIV", km_div.data,
                                    OT_LC_KEYMGR_DIV_BYTES);
    return OT_LC_KEYMGR_DIV_BYTES;
}

static size_t ot_keymgr_dpe_kdf_append_dev_id(OtKeyMgrDpeState *s, bool *dvalid)
{
    OtOTPClass *otp_oc = OBJECT_GET_CLASS(OtOTPClass, s->otp, TYPE_OT_OTP);
    const OtOTPHWCfg *hw_cfg = otp_oc->get_hw_cfg(s->otp);

    ot_keymgr_dpe_kdf_push_bytes(s, hw_cfg->device_id,
                                 OT_OTP_HWCFG_DEVICE_ID_BYTES);
    *dvalid &= ot_keymgr_dpe_valid_data_check(hw_cfg->device_id,
                                              OT_OTP_HWCFG_DEVICE_ID_BYTES);

    ot_keymgr_dpe_dump_kdf_material(s, "DEVICE_ID", hw_cfg->device_id,
                                    OT_OTP_HWCFG_DEVICE_ID_BYTES);
    return OT_OTP_HWCFG_DEVICE_ID_BYTES;
}

static size_t ot_keymgr_dpe_kdf_append_rev_seed(OtKeyMgrDpeState *s)
{
    ot_keymgr_dpe_kdf_push_bytes(s, s->seeds[KEYMGR_DPE_SEED_REV],
                                 KEYMGR_DPE_SEED_BYTES);

    ot_keymgr_dpe_dump_kdf_material(s, "REV_SEED",
                                    s->seeds[KEYMGR_DPE_SEED_REV],
                                    KEYMGR_DPE_SEED_BYTES);

    return KEYMGR_DPE_SEED_BYTES;
}

static size_t
ot_keymgr_dpe_kdf_append_owner_seed(OtKeyMgrDpeState *s, bool *dvalid)
{
    OtOTPKeyMgrSecret secret = { 0u };

    OtOTPClass *otp_oc = OBJECT_GET_CLASS(OtOTPClass, s->otp, TYPE_OT_OTP);
    otp_oc->get_keymgr_secret(s->otp, OTP_KEYMGR_SECRET_OWNER_SEED, &secret);

    ot_keymgr_dpe_kdf_push_bytes(s, secret.secret, OT_OTP_KEYMGR_SECRET_SIZE);
    *dvalid &= ot_keymgr_dpe_valid_data_check(secret.secret,
                                              OT_OTP_KEYMGR_SECRET_SIZE);

    ot_keymgr_dpe_dump_kdf_material(s, "OWNER_SEED", secret.secret,
                                    OT_OTP_KEYMGR_SECRET_SIZE);
    return OT_OTP_KEYMGR_SECRET_SIZE;
}

static void ot_keymgr_dpe_operation_advance(OtKeyMgrDpeState *s)
{
    bool dvalid = true;

    uint32_t ctrl = ot_shadow_reg_peek(&s->control);
    uint8_t slot_dst_sel =
        (uint8_t)FIELD_EX32(ctrl, CONTROL_SHADOWED, SLOT_DST_SEL);
    uint8_t slot_src_sel =
        (uint8_t)FIELD_EX32(ctrl, CONTROL_SHADOWED, SLOT_SRC_SEL);
    OtKeyMgrDpeSlot *src_slot = &s->key_slots[slot_src_sel];
    OtKeyMgrDpeSlot *dst_slot = &s->key_slots[slot_dst_sel];

    bool invalid_allow_child = !src_slot->policy.allow_child;
    bool invalid_max_boot_stage =
        src_slot->boot_stage >= (NUM_BOOT_STAGES - 1u);
    bool invalid_src_slot = !src_slot->valid;
    bool invalid_retain_parent =
        src_slot->policy.retain_parent ?
            (slot_src_sel == slot_dst_sel || dst_slot->valid) :
            (slot_src_sel != slot_dst_sel);

    /* TODO trigger error */
    (void)(invalid_allow_child || invalid_max_boot_stage || invalid_src_slot ||
           invalid_retain_parent);

    ot_keymgr_dpe_reset_kdf_buffer(s);

    size_t expected_kdf_len = 0u;

    switch (src_slot->boot_stage) {
    /* Creator */
    case 0u: {
        /* Creator Seed (OTP) */
        expected_kdf_len += ot_keymgr_dpe_kdf_append_creator_seed(s, &dvalid);

        /* ROM digests (ROM_CTRL) */
        for (unsigned ix = 0u; ix < NUM_ROM_DIGEST_INPUTS; ix++) {
            expected_kdf_len +=
                ot_keymgr_dpe_kdf_append_rom_digest(s, ix, &dvalid);
        }

        /* KeyManager diversification (LC_CTRL) */
        expected_kdf_len += ot_keymgr_dpe_kdf_append_km_div(s, &dvalid);

        /* Device ID (OTP) */
        expected_kdf_len += ot_keymgr_dpe_kdf_append_dev_id(s, &dvalid);

        /* Revision Seed (netlist constant) */
        expected_kdf_len += ot_keymgr_dpe_kdf_append_rev_seed(s);

        break;
    }
    /* OwnerInt */
    case 1u: {
        /* Owner Seed (OTP) */
        expected_kdf_len += ot_keymgr_dpe_kdf_append_owner_seed(s, &dvalid);

        break;
    }
    default:
        break;
    }

    /* Software Binding (software-provided via SW_BINDING_x registers) */
    ot_keymgr_dpe_kdf_push_bytes(s, s->sw_binding,
                                 NUM_SW_BINDING_REG * sizeof(uint32_t));
    expected_kdf_len += NUM_SW_BINDING_REG * sizeof(uint32_t);
    ot_keymgr_dpe_dump_kdf_material(s, "SW_BINDING", s->sw_binding,
                                    NUM_SW_BINDING_REG * sizeof(uint32_t));

    /* check that we have pushed all expected KDF data*/
    g_assert(s->kdf_buf.length == expected_kdf_len);

    /*
     * @todo store `dvalid` somewhere and, if the data is invalid, replace the
     * kmac response with decoy data (random permutation of entropy share 1).
     */
    if (!dvalid) {
        s->regs[R_ERR_CODE] |= R_ERR_CODE_INVALID_KMAC_INPUT_MASK;
    }

    g_assert(s->kdf_buf.length <= KEYMGR_DPE_ADV_DATA_BYTES);
    s->kdf_buf.length = KEYMGR_DPE_ADV_DATA_BYTES;

    ot_keymgr_dpe_kmac_push_key(s);

    ot_keymgr_dpe_dump_kdf_buf(s, "adv");
    ot_keymgr_dpe_send_kmac_req(s);
}

static void ot_keymgr_dpe_operation_erase_slot(OtKeyMgrDpeState *s)
{
    uint32_t ctrl = ot_shadow_reg_peek(&s->control);
    uint8_t slot_dst_sel =
        (uint8_t)FIELD_EX32(ctrl, CONTROL_SHADOWED, SLOT_DST_SEL);
    OtKeyMgrDpeSlot *dst_slot = &s->key_slots[slot_dst_sel];

    bool invalid_erase = !dst_slot->valid;

    /* TODO trigger error */
    (void)invalid_erase;

    memset(dst_slot, 0u, sizeof(OtKeyMgrDpeSlot));

    s->op_state.op_ack = true;
    ot_keymgr_dpe_schedule_fsm(s);
}

static void ot_keymgr_dpe_operation_gen_output(OtKeyMgrDpeState *s, bool sw)
{
    uint32_t ctrl = ot_shadow_reg_peek(&s->control);
    uint8_t dest_sel = (uint8_t)FIELD_EX32(ctrl, CONTROL_SHADOWED, DEST_SEL);
    uint8_t slot_src_sel =
        (uint8_t)FIELD_EX32(ctrl, CONTROL_SHADOWED, SLOT_SRC_SEL);
    OtKeyMgrDpeSlot *src_slot = &s->key_slots[slot_src_sel];

    ot_keymgr_dpe_reset_kdf_buffer(s);

    if (src_slot->valid) {
        /* Output Key Seed (SW/HW key) */
        uint8_t *output_key = sw ? s->seeds[KEYMGR_DPE_SEED_SW_OUT] :
                                   s->seeds[KEYMGR_DPE_SEED_HW_OUT];
        ot_keymgr_dpe_kdf_push_bytes(s, output_key, KEYMGR_DPE_SEED_BYTES);
        ot_keymgr_dpe_dump_kdf_material(s, "OUT_KEY_SEED", output_key,
                                        KEYMGR_DPE_SEED_BYTES);

        /* Destination Seed (AES/KMAC/OTBN/other) */
        uint8_t *dest_seed;
        switch (dest_sel) {
        case KEYMGR_DPE_DEST_SEL_VALUE_AES:
            dest_seed = s->seeds[KEYMGR_DPE_SEED_AES];
            break;
        case KEYMGR_DPE_DEST_SEL_VALUE_KMAC:
            dest_seed = s->seeds[KEYMGR_DPE_SEED_KMAC];
            break;
        case KEYMGR_DPE_DEST_SEL_VALUE_OTBN:
            dest_seed = s->seeds[KEYMGR_DPE_SEED_OTBN];
            break;
        case KEYMGR_DPE_DEST_SEL_VALUE_NONE:
        default:
            dest_seed = s->seeds[KEYMGR_DPE_SEED_NONE];
            break;
        }
        ot_keymgr_dpe_kdf_push_bytes(s, dest_seed, KEYMGR_DPE_SEED_BYTES);
        ot_keymgr_dpe_dump_kdf_material(s, "DEST_SEED", dest_seed,
                                        KEYMGR_DPE_SEED_BYTES);

        /* Salt (software-provided via SALT_x registers) */
        ot_keymgr_dpe_kdf_push_bytes(s, s->salt,
                                     NUM_SALT_REG * sizeof(uint32_t));
        ot_keymgr_dpe_dump_kdf_material(s, "SALT", s->salt,
                                        NUM_SALT_REG * sizeof(uint32_t));

        /* Key Version (software-provided via KEY_VERSION register) */
        ot_keymgr_dpe_kdf_push_key_version(s);
    } else {
        /* active key slot is not valid, push random data */
        unsigned count = KEYMGR_DPE_GEN_DATA_BYTES / sizeof(uint32_t);
        while (count--) {
            uint32_t data = ot_prng_random_u32(s->prng.state);
            ot_keymgr_dpe_kdf_push_bytes(s, (uint8_t *)&data, sizeof(data));
        }
    }

    bool key_version_valid =
        s->regs[R_KEY_VERSION] <= src_slot->max_key_version;

    /* TODO trigger error */
    (void)key_version_valid;

    g_assert(s->kdf_buf.length == KEYMGR_DPE_GEN_DATA_BYTES);

    ot_keymgr_dpe_kmac_push_key(s);

    ot_keymgr_dpe_dump_kdf_buf(s, "gen");
    ot_keymgr_dpe_send_kmac_req(s);
}

static void ot_keymgr_dpe_operation_sw_output(OtKeyMgrDpeState *s)
{
    ot_keymgr_dpe_operation_gen_output(s, true);
}

static void ot_keymgr_dpe_operation_hw_output(OtKeyMgrDpeState *s)
{
    ot_keymgr_dpe_operation_gen_output(s, false);
}

static void ot_keymgr_dpe_operation_disable(OtKeyMgrDpeState *s)
{
    /* TODO implement */
    s->regs[R_ERR_CODE] |= R_ERR_CODE_INVALID_OP_MASK;
    s->op_state.op_ack = true;
    ot_keymgr_dpe_schedule_fsm(s);
}

static void ot_keymgr_dpe_start_operation(OtKeyMgrDpeState *s)
{
    uint32_t ctrl = ot_shadow_reg_peek(&s->control);
    int op = (int)FIELD_EX32(ctrl, CONTROL_SHADOWED, OPERATION);

    trace_ot_keymgr_dpe_operation(s->ot_id, OP_NAME(op), op);

    switch (op) {
    case KEYMGR_DPE_OP_ADVANCE:
        ot_keymgr_dpe_operation_advance(s);
        break;
    case KEYMGR_DPE_OP_ERASE_SLOT:
        ot_keymgr_dpe_operation_erase_slot(s);
        break;
    case KEYMGR_DPE_OP_GENERATE_SW_OUTPUT:
        ot_keymgr_dpe_operation_sw_output(s);
        break;
    case KEYMGR_DPE_OP_GENERATE_HW_OUTPUT:
        ot_keymgr_dpe_operation_hw_output(s);
        break;
    case KEYMGR_DPE_OP_DISABLE:
        ot_keymgr_dpe_operation_disable(s);
        break;
    default:
        s->regs[R_ERR_CODE] |= R_ERR_CODE_INVALID_OP_MASK;
        s->op_state.op_ack = true;
        ot_keymgr_dpe_schedule_fsm(s);
        break;
    }
}

static void ot_keymgr_dpe_sideload_clear(OtKeyMgrDpeState *s)
{
    uint32_t sl_clear =
        FIELD_EX32(s->regs[R_SIDELOAD_CLEAR], SIDELOAD_CLEAR, VAL);

    trace_ot_keymgr_dpe_sideload_clear(s->ot_id, SIDELOAD_CLEAR_NAME(sl_clear),
                                       sl_clear);

    uint32_t bm;
    switch ((int)sl_clear) {
    case KEYMGR_DPE_SIDELOAD_CLEAR_NONE:
        /* nothing to clear, exit */
        return;
    case KEYMGR_DPE_SIDELOAD_CLEAR_AES:
    case KEYMGR_DPE_SIDELOAD_CLEAR_OTBN:
    case KEYMGR_DPE_SIDELOAD_CLEAR_KMAC:
        bm = 1u << (sl_clear - KEY_SINK_OFFSET);
        break;
    default:
        /*
         * "If the value programmed is not one of the enumerated values below,
         * ALL sideload key slots are continuously cleared."
         */
        bm = (1u << KEYMGR_DPE_KEY_SINK_COUNT) - 1u;
        break;
    }

    uint8_t share0[KEYMGR_DPE_KEY_SIZE_MAX];
    uint8_t share1[KEYMGR_DPE_KEY_SIZE_MAX];

    for (unsigned kix = 0; bm && kix < KEYMGR_DPE_KEY_SINK_COUNT; kix++) {
        if (bm & (1u << kix)) {
            bm &= ~(1u << kix);

            DeviceState *sink = s->key_sinks[kix];
            if (!sink) {
                continue;
            }

            OtKeySinkIfClass *kc = OT_KEY_SINK_IF_GET_CLASS(sink);
            OtKeySinkIf *ki = OT_KEY_SINK_IF(sink);

            g_assert(kc->push_key);

            size_t key_size = OT_KEY_MGR_DPE_KEY_SINK_SIZES[kix];

            /* TODO this needs to use random data */
            memset(share0, 0, key_size);
            memset(share1, 0, key_size);

            kc->push_key(ki, share0, share1, key_size, false);
        }
    }
}

static void ot_keymgr_dpe_lc_signal(void *opaque, int irq, int level)
{
    OtKeyMgrDpeState *s = opaque;
    bool enable_keymgr = (bool)level;

    g_assert(irq == 0);

    trace_ot_keymgr_dpe_lc_signal(s->ot_id, level);

    bool changed = enable_keymgr ^ s->enabled;
    if (!changed) {
        /* no change, exit */
        return;
    }

    s->enabled = enable_keymgr;

    if (s->enabled) {
        s->regs[R_DEBUG] &= ~R_DEBUG_INACTIVE_LC_EN_MASK;
        s->regs[R_CFG_REGWEN] |= R_CFG_REGWEN_EN_MASK;
    } else {
        s->regs[R_DEBUG] |= R_DEBUG_INACTIVE_LC_EN_MASK;
        s->regs[R_CFG_REGWEN] &= ~R_CFG_REGWEN_EN_MASK;
    }

    ot_keymgr_dpe_schedule_fsm(s);
}

#define ot_keymgr_dpe_change_main_fsm_state(_s_, _op_status_) \
    ot_keymgr_dpe_xchange_main_fsm_state(_s_, _op_status_, __LINE__)

static void ot_keymgr_dpe_xchange_main_fsm_state(
    OtKeyMgrDpeState *s, OtKeyMgrDpeFSMState state, int line)
{
    if (s->state != state) {
        trace_ot_keymgr_dpe_change_main_fsm_state(s->ot_id, line,
                                                  FST_NAME(s->state), s->state,
                                                  FST_NAME(state), state);
        s->state = state;
    }
}

static void ot_keymgr_dpe_get_root_key(
    OtKeyMgrDpeState *s, OtOTPKeyMgrSecret *share0, OtOTPKeyMgrSecret *share1)
{
    OtOTPClass *oc = OBJECT_GET_CLASS(OtOTPClass, s->otp, TYPE_OT_OTP);
    g_assert(oc);
    oc->get_keymgr_secret(s->otp, OTP_KEYMGR_SECRET_CREATOR_ROOT_KEY_SHARE0,
                          share0);
    oc->get_keymgr_secret(s->otp, OTP_KEYMGR_SECRET_CREATOR_ROOT_KEY_SHARE1,
                          share1);

    if (trace_event_get_state(TRACE_OT_KEYMGR_DPE_DUMP_CREATOR_ROOT_KEY)) {
        trace_ot_keymgr_dpe_dump_creator_root_key(
            s->ot_id, 0, share0->valid,
            ot_keymgr_dpe_dump_bigint(s, share0->secret,
                                      OT_OTP_KEYMGR_SECRET_SIZE));
        trace_ot_keymgr_dpe_dump_creator_root_key(
            s->ot_id, 1, share1->valid,
            ot_keymgr_dpe_dump_bigint(s, share1->secret,
                                      OT_OTP_KEYMGR_SECRET_SIZE));
    }
}

static bool ot_keymgr_dpe_main_fsm_tick(OtKeyMgrDpeState *s)
{
    OtKeyMgrDpeFSMState state = s->state;
    bool op_start = s->regs[R_START] & R_START_EN_MASK;
    bool init = false;
    uint32_t ctrl = ot_shadow_reg_peek(&s->control);
    uint8_t slot_dst_sel =
        (uint8_t)FIELD_EX32(ctrl, CONTROL_SHADOWED, SLOT_DST_SEL);
    OtKeyMgrDpeSlot *dst_slot = &s->key_slots[slot_dst_sel];

    trace_ot_keymgr_dpe_main_fsm_tick(s->ot_id, FST_NAME(s->state), s->state);

    switch (s->state) {
    case KEYMGR_DPE_ST_RESET:
        ot_keymgr_dpe_change_working_state(s, KEYMGR_DPE_WORKING_STATE_RESET);
        if (!op_start) {
            break;
        }
        bool op_advance = FIELD_EX32(ctrl, CONTROL_SHADOWED, OPERATION) ==
                          KEYMGR_DPE_OP_ADVANCE;
        if (!s->enabled || !op_advance) {
            s->regs[R_ERR_CODE] |= R_ERR_CODE_INVALID_OP_MASK;
        } else {
            ot_keymgr_dpe_change_main_fsm_state(s,
                                                KEYMGR_DPE_ST_ENTROPY_RESEED);
        }
        break;
    case KEYMGR_DPE_ST_ENTROPY_RESEED:
        ot_keymgr_dpe_change_working_state(s, KEYMGR_DPE_WORKING_STATE_RESET);
        if (!s->enabled) {
            s->regs[R_ERR_CODE] |= R_ERR_CODE_INVALID_OP_MASK;
            ot_keymgr_dpe_change_main_fsm_state(s, KEYMGR_DPE_ST_INVALID);
        } else if (!s->prng.reseed_req) {
            s->prng.reseed_ack = false;
            s->prng.reseed_req = true;
            ot_keymgr_dpe_request_entropy(s);
        } else if (s->prng.reseed_ack) {
            s->prng.reseed_req = true;
            ot_keymgr_dpe_change_main_fsm_state(s, KEYMGR_DPE_ST_RANDOM);
        }
        break;
    case KEYMGR_DPE_ST_RANDOM:
        ot_keymgr_dpe_change_working_state(s, KEYMGR_DPE_WORKING_STATE_RESET);
        if (!s->enabled) {
            s->regs[R_ERR_CODE] |= R_ERR_CODE_INVALID_OP_MASK;
            ot_keymgr_dpe_change_main_fsm_state(s, KEYMGR_DPE_ST_INVALID);
        } else {
            /* current RTL only initializes slot 'slot_dst_sel' with 0s */
            memset(dst_slot, 0u, sizeof(OtKeyMgrDpeSlot));
            ot_keymgr_dpe_change_main_fsm_state(s, KEYMGR_DPE_ST_ROOTKEY);
        }
        break;
    case KEYMGR_DPE_ST_ROOTKEY:
        ot_keymgr_dpe_change_working_state(s, KEYMGR_DPE_WORKING_STATE_RESET);
        if (!s->enabled) {
            s->regs[R_ERR_CODE] |= R_ERR_CODE_INVALID_OP_MASK;
            ot_keymgr_dpe_change_main_fsm_state(s, KEYMGR_DPE_ST_INVALID);
        } else {
            init = true;

            /* retrieve Creator Root Key from OTP */
            OtOTPKeyMgrSecret secret_share0 = { 0u };
            OtOTPKeyMgrSecret secret_share1 = { 0u };
            ot_keymgr_dpe_get_root_key(s, &secret_share0, &secret_share1);

            if (secret_share0.valid && secret_share1.valid) {
                memset(dst_slot, 0u, sizeof(OtKeyMgrDpeSlot));
                dst_slot->valid = true;
                dst_slot->boot_stage = 0u;
                memcpy(dst_slot->key.share0, secret_share0.secret,
                       OT_OTP_KEYMGR_SECRET_SIZE);
                memcpy(dst_slot->key.share1, secret_share1.secret,
                       OT_OTP_KEYMGR_SECRET_SIZE);
                dst_slot->max_key_version = ot_shadow_reg_peek(&s->max_key_ver);
                dst_slot->policy = DEFAULT_UDS_POLICY;

                ot_keymgr_dpe_change_main_fsm_state(s, KEYMGR_DPE_ST_AVAILABLE);
            } else {
                s->regs[R_DEBUG] |= R_DEBUG_INVALID_ROOT_KEY_MASK;

                ot_keymgr_dpe_change_main_fsm_state(s, KEYMGR_DPE_ST_INVALID);
            }
        }
        break;
    case KEYMGR_DPE_ST_AVAILABLE:
        ot_keymgr_dpe_change_working_state(s,
                                           KEYMGR_DPE_WORKING_STATE_AVAILABLE);
        if (!op_start) {
            /* no state change if op_start is not set */
            break;
        }
        if (!s->enabled) {
            s->regs[R_ERR_CODE] |= R_ERR_CODE_INVALID_OP_MASK;
            /*
             * Given that the root key was latched by an earlier FSM state, we
             * need to take care of clearing the sensitive root key.
             */
            ot_keymgr_dpe_change_main_fsm_state(s, KEYMGR_DPE_ST_WIPE);
        } else if (!s->op_state.op_req) {
            s->op_state.op_req = true;
            ot_keymgr_dpe_start_operation(s);
        }
        break;
    case KEYMGR_DPE_ST_WIPE:
        ot_keymgr_dpe_change_main_fsm_state(s, KEYMGR_DPE_ST_DISABLING);
        break;
    case KEYMGR_DPE_ST_DISABLING:
        /* TODO wipe before going to disabled state */
        ot_keymgr_dpe_change_main_fsm_state(s, KEYMGR_DPE_ST_DISABLED);
        break;
    case KEYMGR_DPE_ST_DISABLED:
        ot_keymgr_dpe_change_working_state(s,
                                           KEYMGR_DPE_WORKING_STATE_DISABLED);
        break;
    case KEYMGR_DPE_ST_INVALID:
    default:
        ot_keymgr_dpe_change_working_state(s, KEYMGR_DPE_WORKING_STATE_INVALID);
        break;
    }

    /* last requested operation status */
    bool invalid_op = (bool)(s->regs[R_ERR_CODE] & R_ERR_CODE_INVALID_OP_MASK);
    bool op_done =
        s->op_state.op_req ? s->op_state.op_ack : (init || invalid_op);
    if (op_done) {
        s->op_state.op_req = false;
        s->op_state.op_ack = false;
        s->regs[R_START] &= ~R_START_EN_MASK;
        if (s->regs[R_ERR_CODE] | s->regs[R_FAULT_STATUS]) {
            ot_keymgr_dpe_update_alert(s);
            ot_keymgr_dpe_change_op_status(s, KEYMGR_DPE_OP_STATUS_DONE_ERROR);
        } else {
            ot_keymgr_dpe_change_op_status(s,
                                           KEYMGR_DPE_OP_STATUS_DONE_SUCCESS);
        }
    } else if (op_start) {
        ot_keymgr_dpe_change_op_status(s, KEYMGR_DPE_OP_STATUS_WIP);
    } else {
        ot_keymgr_dpe_change_op_status(s, KEYMGR_DPE_OP_STATUS_IDLE);
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

static void ot_keymgr_dpe_fsm_tick(void *opaque)
{
    OtKeyMgrDpeState *s = opaque;

    if (ot_keymgr_dpe_main_fsm_tick(s)) {
        /* schedule FSM update once more if its state has changed */
        ot_keymgr_dpe_schedule_fsm(s);
    } else {
        /* otherwise, go idle and wait for an external event */
        trace_ot_keymgr_dpe_go_idle(s->ot_id);
    }
}

#define ot_keymgr_dpe_check_reg_write(_s_, _reg_, _regwen_) \
    ot_keymgr_dpe_check_reg_write_func(__func__, _s_, _reg_, _regwen_)

static inline bool ot_keymgr_dpe_check_reg_write_func(
    const char *func, OtKeyMgrDpeState *s, hwaddr reg, hwaddr regwen)
{
    if (!s->regs[regwen]) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Write to %s protected with %s\n",
                      func, REG_NAME(reg), REG_NAME(regwen));
        return false;
    }

    return true;
}

static uint64_t ot_keymgr_dpe_read(void *opaque, hwaddr addr, unsigned size)
{
    OtKeyMgrDpeState *s = opaque;
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
    case R_SLOT_POLICY_REGWEN:
    case R_SLOT_POLICY:
    case R_SW_BINDING_REGWEN:
    case R_KEY_VERSION:
    case R_MAX_KEY_VER_REGWEN:
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
    case R_MAX_KEY_VER_SHADOWED:
        val32 = ot_shadow_reg_read(&s->max_key_ver);
        break;
    case R_SW_BINDING_0:
    case R_SW_BINDING_1:
    case R_SW_BINDING_2:
    case R_SW_BINDING_3:
    case R_SW_BINDING_4:
    case R_SW_BINDING_5:
    case R_SW_BINDING_6:
    case R_SW_BINDING_7: {
        unsigned offset = (reg - R_SW_BINDING_0) * sizeof(uint32_t);
        val32 = ldl_le_p(&s->sw_binding[offset]);
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
        /* TODO should this depend on the current state? */
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
        /* TODO should this depend on the current state? */
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
    trace_ot_keymgr_dpe_io_read_out(s->ot_id, (unsigned)addr, REG_NAME(reg),
                                    (uint64_t)val32, pc);

    return (uint64_t)val32;
};

static void ot_keymgr_dpe_write(void *opaque, hwaddr addr, uint64_t val64,
                                unsigned size)
{
    OtKeyMgrDpeState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint64_t pc = ibex_get_current_pc();
    trace_ot_keymgr_dpe_io_write(s->ot_id, (unsigned)addr, REG_NAME(reg), val64,
                                 pc);

    switch (reg) {
    case R_INTR_STATE:
        val32 &= INTR_MASK;
        s->regs[reg] &= ~val32; /* RW1C */
        ot_keymgr_dpe_update_irq(s);
        break;
    case R_INTR_ENABLE:
        val32 &= INTR_MASK;
        s->regs[reg] = val32;
        ot_keymgr_dpe_update_irq(s);
        break;
    case R_INTR_TEST:
        val32 &= INTR_MASK;
        s->regs[R_INTR_STATE] |= val32;
        ot_keymgr_dpe_update_irq(s);
        break;
    case R_ALERT_TEST:
        val32 &= ALERT_MASK;
        s->regs[R_ALERT_TEST] |= val32;
        ot_keymgr_dpe_update_alert(s);
        break;
    case R_START:
        if (!ot_keymgr_dpe_check_reg_write(s, reg, R_CFG_REGWEN)) {
            break;
        }
        val32 &= R_START_EN_MASK;
        s->regs[reg] = val32;
        ot_keymgr_dpe_fsm_tick(s);
        break;
    case R_CONTROL_SHADOWED:
        if (!ot_keymgr_dpe_check_reg_write(s, reg, R_CFG_REGWEN)) {
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
            ot_keymgr_dpe_update_alert(s);
        }
        break;
    case R_SIDELOAD_CLEAR:
        if (!ot_keymgr_dpe_check_reg_write(s, reg, R_CFG_REGWEN)) {
            break;
        }
        val32 &= R_SIDELOAD_CLEAR_VAL_MASK;
        s->regs[reg] = val32;
        ot_keymgr_dpe_sideload_clear(s);
        break;
    case R_RESEED_INTERVAL_REGWEN:
        val32 &= R_RESEED_INTERVAL_REGWEN_EN_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_RESEED_INTERVAL_SHADOWED:
        if (!ot_keymgr_dpe_check_reg_write(s, reg, R_RESEED_INTERVAL_REGWEN)) {
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
            ot_keymgr_dpe_update_alert(s);
        }
        break;
    case R_SLOT_POLICY_REGWEN:
        val32 &= R_SLOT_POLICY_REGWEN_EN_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_SLOT_POLICY:
        if (!ot_keymgr_dpe_check_reg_write(s, reg, R_SLOT_POLICY_REGWEN)) {
            break;
        }
        s->regs[reg] = val32;
        break;
    case R_SW_BINDING_REGWEN:
        val32 &= R_SW_BINDING_REGWEN_EN_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_SW_BINDING_0:
    case R_SW_BINDING_1:
    case R_SW_BINDING_2:
    case R_SW_BINDING_3:
    case R_SW_BINDING_4:
    case R_SW_BINDING_5:
    case R_SW_BINDING_6:
    case R_SW_BINDING_7: {
        if (!ot_keymgr_dpe_check_reg_write(s, reg, R_SW_BINDING_REGWEN)) {
            break;
        }
        unsigned offset = (reg - R_SW_BINDING_0) * sizeof(uint32_t);
        stl_le_p(&s->sw_binding[offset], val32);
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
        if (!ot_keymgr_dpe_check_reg_write(s, reg, R_CFG_REGWEN)) {
            break;
        }
        unsigned offset = (reg - R_SALT_0) * sizeof(uint32_t);
        stl_le_p(&s->salt[offset], val32);
        break;
    }
    case R_KEY_VERSION:
        if (!ot_keymgr_dpe_check_reg_write(s, reg, R_CFG_REGWEN)) {
            break;
        }
        /* TODO */
        s->regs[reg] = val32;
        break;
    case R_MAX_KEY_VER_REGWEN:
        val32 &= R_MAX_KEY_VER_REGWEN_EN_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_MAX_KEY_VER_SHADOWED:
        if (!ot_keymgr_dpe_check_reg_write(s, reg, R_MAX_KEY_VER_REGWEN)) {
            break;
        }
        switch (ot_shadow_reg_write(&s->max_key_ver, val32)) {
        case OT_SHADOW_REG_STAGED:
        case OT_SHADOW_REG_COMMITTED:
            break;
        case OT_SHADOW_REG_ERROR:
        default:
            s->regs[R_ERR_CODE] |= R_ERR_CODE_INVALID_SHADOW_UPDATE_MASK;
            ot_keymgr_dpe_update_alert(s);
        }
        break;
    case R_OP_STATUS:
        val32 &= R_OP_STATUS_VAL_MASK;
        ot_keymgr_dpe_change_op_status(s, s->regs[reg] & ~val32); /* RW1C */
        /* SW write may clear `WIP` here which may be immediately set again */
        ot_keymgr_dpe_fsm_tick(s);
        break;
    case R_ERR_CODE:
        val32 &= ERR_CODE_MASK;
        s->regs[reg] &= ~val32; /* RW1C */
        ot_keymgr_dpe_update_alert(s);
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

static void ot_keymgr_dpe_configure_constants(OtKeyMgrDpeState *s)
{
    for (unsigned ix = 0u; ix < KEYMGR_DPE_SEED_COUNT; ix++) {
        if (!s->seed_xstrs[ix]) {
            trace_ot_keymgr_dpe_seed_missing(s->ot_id, ix);
            continue;
        }

        size_t len = strlen(s->seed_xstrs[ix]);
        size_t seed_len_bytes = KEYMGR_DPE_SEED_BYTES;
        if (ix == KEYMGR_DPE_SEED_LFSR) {
            seed_len_bytes = 8u;
        }
        if (len != (seed_len_bytes * 2u)) {
            error_setg(&error_fatal, "%s: %s invalid seed #%u length\n",
                       __func__, s->ot_id, ix);
            continue;
        }

        if (ot_common_parse_hexa_str(s->seeds[ix], s->seed_xstrs[ix],
                                     seed_len_bytes, true, true)) {
            error_setg(&error_fatal, "%s: %s unable to parse seed #%u\n",
                       __func__, s->ot_id, ix);
            continue;
        }
    }
}

static Property ot_keymgr_dpe_properties[] = {
    DEFINE_PROP_STRING(OT_COMMON_DEV_ID, OtKeyMgrDpeState, ot_id),
    DEFINE_PROP_LINK("edn", OtKeyMgrDpeState, edn.device, TYPE_OT_EDN,
                     OtEDNState *),
    DEFINE_PROP_UINT8("edn-ep", OtKeyMgrDpeState, edn.ep, UINT8_MAX),
    DEFINE_PROP_LINK("kmac", OtKeyMgrDpeState, kmac, TYPE_OT_KMAC,
                     OtKMACState *),
    DEFINE_PROP_UINT8("kmac-app", OtKeyMgrDpeState, kmac_app, UINT8_MAX),
    DEFINE_PROP_LINK("lc_ctrl", OtKeyMgrDpeState, lc_ctrl, TYPE_OT_LC_CTRL,
                     OtLcCtrlState *),
    DEFINE_PROP_LINK("otp_ctrl", OtKeyMgrDpeState, otp, TYPE_OT_OTP,
                     OtOTPState *),
    DEFINE_PROP_LINK("rom0", OtKeyMgrDpeState, rom_ctrl[0], TYPE_OT_ROM_CTRL,
                     OtRomCtrlState *),
    DEFINE_PROP_LINK("rom1", OtKeyMgrDpeState, rom_ctrl[1], TYPE_OT_ROM_CTRL,
                     OtRomCtrlState *),
    DEFINE_PROP_LINK("aes", OtKeyMgrDpeState,
                     key_sinks[KEYMGR_DPE_KEY_SINK_AES], TYPE_OT_KEY_SINK_IF,
                     DeviceState *),
    DEFINE_PROP_LINK("otbn", OtKeyMgrDpeState,
                     key_sinks[KEYMGR_DPE_KEY_SINK_OTBN], TYPE_OT_KEY_SINK_IF,
                     DeviceState *),
    DEFINE_PROP_STRING("aes_seed", OtKeyMgrDpeState,
                       seed_xstrs[KEYMGR_DPE_SEED_AES]),
    DEFINE_PROP_STRING("hard_output_seed", OtKeyMgrDpeState,
                       seed_xstrs[KEYMGR_DPE_SEED_HW_OUT]),
    DEFINE_PROP_STRING("kmac_seed", OtKeyMgrDpeState,
                       seed_xstrs[KEYMGR_DPE_SEED_KMAC]),
    DEFINE_PROP_STRING("lfsr_seed", OtKeyMgrDpeState,
                       seed_xstrs[KEYMGR_DPE_SEED_LFSR]),
    DEFINE_PROP_STRING("none_seed", OtKeyMgrDpeState,
                       seed_xstrs[KEYMGR_DPE_SEED_NONE]),
    DEFINE_PROP_STRING("otbn_seed", OtKeyMgrDpeState,
                       seed_xstrs[KEYMGR_DPE_SEED_OTBN]),
    DEFINE_PROP_STRING("revision_seed", OtKeyMgrDpeState,
                       seed_xstrs[KEYMGR_DPE_SEED_REV]),
    DEFINE_PROP_STRING("soft_output_seed", OtKeyMgrDpeState,
                       seed_xstrs[KEYMGR_DPE_SEED_SW_OUT]),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_keymgr_dpe_regs_ops = {
    .read = &ot_keymgr_dpe_read,
    .write = &ot_keymgr_dpe_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_keymgr_dpe_reset_enter(Object *obj, ResetType type)
{
    OtKeyMgrDpeClass *c = OT_KEYMGR_DPE_GET_CLASS(obj);
    OtKeyMgrDpeState *s = OT_KEYMGR_DPE(obj);

    trace_ot_keymgr_dpe_reset(s->ot_id, "enter");

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    qemu_bh_cancel(s->fsm_tick_bh);

    g_assert(s->edn.device);
    g_assert(s->edn.ep != UINT8_MAX);
    g_assert(s->kmac);
    g_assert(s->kmac_app != UINT8_MAX);
    g_assert(s->lc_ctrl);
    g_assert(s->otp);
    g_assert(s->rom_ctrl[0]);
    g_assert(s->rom_ctrl[1]);

    s->key_sinks[KEYMGR_DPE_KEY_SINK_KMAC] = DEVICE(s->kmac);

    /* reset registers */
    memset(s->regs, 0u, sizeof(s->regs));
    s->regs[R_CFG_REGWEN] = 0x1u;
    ot_shadow_reg_init(&s->control, 10u);
    s->regs[R_RESEED_INTERVAL_REGWEN] = 0x1u;
    ot_shadow_reg_init(&s->reseed_interval, 0x100u);
    s->regs[R_SLOT_POLICY_REGWEN] = 0x1u;
    s->regs[R_SW_BINDING_REGWEN] = 0x1u;
    memset(s->sw_binding, 0u, NUM_SW_BINDING_REG * sizeof(uint32_t));
    memset(s->salt, 0u, NUM_SALT_REG * sizeof(uint32_t));
    s->regs[R_MAX_KEY_VER_REGWEN] = 0x1u;
    ot_shadow_reg_init(&s->max_key_ver, 0u);

    /* reset internal state */
    s->enabled = false;
    s->state = KEYMGR_DPE_ST_RESET;
    s->prng.reseed_req = false;
    s->prng.reseed_ack = false;
    s->prng.reseed_cnt = 0u;
    s->op_state.op_req = false;
    s->op_state.op_ack = false;
    ot_keymgr_dpe_reset_kdf_buffer(s);

    /* reset slots */
    memset(s->key_slots, 0u, NUM_SLOTS * sizeof(OtKeyMgrDpeSlot));

    /* reset output keys */
    memset(s->sw_out_key, 0u, sizeof(OtKeyMgrDpeKey));

    /* update IRQ and alert states */
    ot_keymgr_dpe_update_irq(s);
    ot_keymgr_dpe_update_alert(s);

    /* connect to KMAC */
    OtKMACClass *kc = OT_KMAC_GET_CLASS(s->kmac);
    kc->connect_app(s->kmac, s->kmac_app, &KMAC_APP_CFG,
                    ot_keymgr_dpe_handle_kmac_response, s);
}

static void ot_keymgr_dpe_reset_exit(Object *obj, ResetType type)
{
    OtKeyMgrDpeClass *c = OT_KEYMGR_DPE_GET_CLASS(obj);
    OtKeyMgrDpeState *s = OT_KEYMGR_DPE(obj);

    trace_ot_keymgr_dpe_reset(s->ot_id, "exit");

    if (c->parent_phases.exit) {
        c->parent_phases.exit(obj, type);
    }

    for (unsigned ix = 0u; ix < KEYMGR_DPE_KEY_SINK_COUNT; ix++) {
        OtKeyMgrDpeKeySink key_sink = (OtKeyMgrDpeKeySink)ix;
        ot_keymgr_dpe_push_key(s, key_sink, NULL, NULL, false);
    }
}

static void ot_keymgr_dpe_realize(DeviceState *dev, Error **errp)
{
    OtKeyMgrDpeState *s = OT_KEYMGR_DPE(dev);

    (void)errp; /* unused */

    if (!s->ot_id) {
        s->ot_id =
            g_strdup(object_get_canonical_path_component(OBJECT(s)->parent));
    }

    for (unsigned ix = 0u; ix < KEYMGR_DPE_KEY_SINK_COUNT; ix++) {
        if (s->key_sinks[ix]) {
            OBJECT_CHECK(OtKeySinkIf, s->key_sinks[ix], TYPE_OT_KEY_SINK_IF);
        }
    }

    ot_keymgr_dpe_configure_constants(s);
}

static void ot_keymgr_dpe_init(Object *obj)
{
    OtKeyMgrDpeState *s = OT_KEYMGR_DPE(obj);

    memory_region_init_io(&s->mmio, obj, &ot_keymgr_dpe_regs_ops, s,
                          TYPE_OT_KEYMGR_DPE, REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    ibex_sysbus_init_irq(obj, &s->irq);
    for (unsigned ix = 0u; ix < ALERT_COUNT; ix++) {
        ibex_qdev_init_irq(obj, &s->alerts[ix], OT_DEVICE_ALERT);
    }
    qdev_init_gpio_in_named(DEVICE(s), ot_keymgr_dpe_lc_signal,
                            OT_KEYMGR_DPE_ENABLE, 1);

    s->prng.state = ot_prng_allocate();

    s->kdf_buf.data = g_new0(uint8_t, KEYMGR_DPE_KDF_BUFFER_BYTES);
    s->salt = g_new0(uint8_t, NUM_SALT_REG * sizeof(uint32_t));
    s->sw_binding = g_new0(uint8_t, NUM_SW_BINDING_REG * sizeof(uint32_t));
    for (unsigned ix = 0; ix < ARRAY_SIZE(s->seeds); ix++) {
        s->seeds[ix] = g_new0(uint8_t, KEYMGR_DPE_SEED_BYTES);
    }
    s->key_slots = g_new0(OtKeyMgrDpeSlot, NUM_SLOTS);
    s->sw_out_key = g_new0(OtKeyMgrDpeKey, 1u);

    s->fsm_tick_bh = qemu_bh_new(&ot_keymgr_dpe_fsm_tick, s);

    s->hexstr = g_new0(char, OT_KEYMGR_DPE_HEXSTR_SIZE);
}

static void ot_keymgr_dpe_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    (void)data; /* unused */

    dc->realize = &ot_keymgr_dpe_realize;
    device_class_set_props(dc, ot_keymgr_dpe_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    ResettableClass *rc = RESETTABLE_CLASS(klass);
    OtKeyMgrDpeClass *kmc = OT_KEYMGR_DPE_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_keymgr_dpe_reset_enter, NULL,
                                       &ot_keymgr_dpe_reset_exit,
                                       &kmc->parent_phases);
}

static const TypeInfo ot_keymgr_dpe_info = {
    .name = TYPE_OT_KEYMGR_DPE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtKeyMgrDpeState),
    .instance_init = &ot_keymgr_dpe_init,
    .class_size = sizeof(OtKeyMgrDpeClass),
    .class_init = &ot_keymgr_dpe_class_init,
};

static void ot_keymgr_dpe_register_types(void)
{
    type_register_static(&ot_keymgr_dpe_info);
}

type_init(ot_keymgr_dpe_register_types)
