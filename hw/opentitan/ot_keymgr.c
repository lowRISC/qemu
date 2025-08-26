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
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_keymgr.h"
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

static_assert(KEYMGR_ADV_DATA_BYTES <= KEYMGR_KDF_BUFFER_BYTES,
              "KeyMgr ADV data does not fit in KDF buffer");
static_assert(KEYMGR_ID_DATA_BYTES <= KEYMGR_KDF_BUFFER_BYTES,
              "KeyMgr ID data does not fit in KDF buffer");
static_assert(KEYMGR_GEN_DATA_BYTES <= KEYMGR_KDF_BUFFER_BYTES,
              "KeyMgr GEN data does not fit in KDF buffer");

#define KEYMGR_ENTROPY_WIDTH  (KEYMGR_LFSR_WIDTH / 2u)
#define KEYMGR_ENTROPY_ROUNDS (KEYMGR_KEY_WIDTH / KEYMGR_ENTROPY_WIDTH)

#define KEYMGR_LFSR_SEED_BYTES ((KEYMGR_LFSR_WIDTH) / 8u)

static_assert(KEYMGR_LFSR_SEED_BYTES <= KEYMGR_SEED_BYTES,
              "Keymgr LFSR seed is larger than generic KeyMgr seed size");

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

typedef struct OtKeyMgrState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    IbexIRQ irq;
    IbexIRQ alerts[ALERT_COUNT];

    uint32_t regs[REGS_COUNT];
    OtShadowReg control;
    OtShadowReg reseed_interval;
    OtShadowReg max_creator_key_ver;
    OtShadowReg max_owner_int_key_ver;
    OtShadowReg max_owner_key_ver;
    uint8_t *salt;
    uint8_t *sealing_sw_binding;
    uint8_t *attest_sw_binding;

    uint8_t *seeds[KEYMGR_SEED_COUNT];

    /* properties */
    char *ot_id;
    char *seed_xstrs[KEYMGR_SEED_COUNT];
} OtKeyMgrState;

struct OtKeyMgrClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

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
    case R_SW_SHARE0_OUTPUT_7:
    case R_SW_SHARE1_OUTPUT_0:
    case R_SW_SHARE1_OUTPUT_1:
    case R_SW_SHARE1_OUTPUT_2:
    case R_SW_SHARE1_OUTPUT_3:
    case R_SW_SHARE1_OUTPUT_4:
    case R_SW_SHARE1_OUTPUT_5:
    case R_SW_SHARE1_OUTPUT_6:
    case R_SW_SHARE1_OUTPUT_7:
        /* @todo: implement RC software share register reads */
        qemu_log_mask(LOG_UNIMP,
                      "%s: %s: Read from register %s is not implemented.\n",
                      __func__, s->ot_id, REG_NAME(reg));
        val32 = 0u;
        break;
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
        /* @todo: implement R_START */
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
    case R_OP_STATUS:
        val32 &= R_OP_STATUS_STATUS_MASK;
        s->regs[reg] &= ~val32; /* RW1C */
        /* @todo: implement trace on R_OP_STATUS change? */
        break;
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

    /* update IRQ and alert states */
    ot_keymgr_update_irq(s);
    ot_keymgr_update_alerts(s);
}

static void ot_keymgr_reset_exit(Object *obj, ResetType type)
{
    OtKeyMgrClass *c = OT_KEYMGR_GET_CLASS(obj);
    OtKeyMgrState *s = OT_KEYMGR(obj);

    trace_ot_keymgr_reset(s->ot_id, "exit");

    if (c->parent_phases.exit) {
        c->parent_phases.exit(obj, type);
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

    ot_keymgr_configure_constants(s);
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

    s->salt = g_new0(uint8_t, KEYMGR_SALT_BYTES);
    s->sealing_sw_binding = g_new0(uint8_t, KEYMGR_SW_BINDING_BYTES);
    s->attest_sw_binding = g_new0(uint8_t, KEYMGR_SW_BINDING_BYTES);
    for (unsigned ix = 0u; ix < ARRAY_SIZE(s->seeds); ix++) {
        s->seeds[ix] = g_new0(uint8_t, KEYMGR_SEED_BYTES);
    }
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
