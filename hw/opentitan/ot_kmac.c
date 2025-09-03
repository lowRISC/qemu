/*
 * QEMU OpenTitan KMAC device
 *
 * Copyright (c) 2023-2025 Rivos, Inc.
 * Copyright (c) 2025 lowRISC contributors.
 *
 * Author(s):
 *  Loïc Lefort <loic@rivosinc.com>
 *
 * For details check the documentation here:
 *    https://opentitan.org/book/hw/ip/kmac
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
 * Note: This implementation is missing some features:
 *   - Masking (current implementation does not consume entropy)
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/fifo8.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_clkmgr.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_edn.h"
#include "hw/opentitan/ot_key_sink.h"
#include "hw/opentitan/ot_kmac.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_clock_src.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "tomcrypt.h"
#include "trace.h"

#define KMAC_PARAM_NUM_ALERTS 2u

/* clang-format off */
REG32(INTR_STATE, 0x00u)
    SHARED_FIELD(INTR_KMAC_DONE, 0u, 1u)
    SHARED_FIELD(INTR_FIFO_EMPTY, 1u, 1u)
    SHARED_FIELD(INTR_KMAC_ERR, 2u, 1u)
REG32(INTR_ENABLE, 0x04u)
REG32(INTR_TEST, 0x08u)
REG32(ALERT_TEST, 0x0cu)
    FIELD(ALERT_TEST, RECOV_OPERATION, 0u, 1u)
    FIELD(ALERT_TEST, FATAL_FAULT, 1u, 1u)
REG32(CFG_REGWEN, 0x10u)
    FIELD(CFG_REGWEN, EN, 0u, 1u)
REG32(CFG_SHADOWED, 0x14u)
    FIELD(CFG_SHADOWED, KMAC_EN, 0u, 1u)
    FIELD(CFG_SHADOWED, KSTRENGTH, 1u, 3u)
    FIELD(CFG_SHADOWED, MODE, 4u, 2u)
    FIELD(CFG_SHADOWED, MSG_ENDIANNESS, 8u, 1u)
    FIELD(CFG_SHADOWED, STATE_ENDIANNESS, 9u, 1u)
    FIELD(CFG_SHADOWED, SIDELOAD, 12u, 1u)
    FIELD(CFG_SHADOWED, ENTROPY_MODE, 16u, 2u)
    FIELD(CFG_SHADOWED, ENTROPY_FAST_PROCESS, 19u, 1u)
    FIELD(CFG_SHADOWED, MSG_MASK, 20u, 1u)
    FIELD(CFG_SHADOWED, ENTROPY_READY, 24u, 1u)
    FIELD(CFG_SHADOWED, EN_UNSUPPORTED_MODESTRENGTH, 26u, 1u)
REG32(CMD, 0x18u)
    FIELD(CMD, CMD, 0u, 6u)
    FIELD(CMD, ENTROPY_REQ, 8u, 1u)
    FIELD(CMD, HASH_CNT_CLR, 9u, 1u)
    FIELD(CMD, ERR_PROCESSED, 10u, 1u)
REG32(STATUS, 0x1cu)
    FIELD(STATUS, SHA3_IDLE, 0u, 1u)
    FIELD(STATUS, SHA3_ABSORB, 1u, 1u)
    FIELD(STATUS, SHA3_SQUEEZE, 2u, 1u)
    FIELD(STATUS, FIFO_DEPTH, 8u, 5u)
    FIELD(STATUS, FIFO_EMPTY, 14u, 1u)
    FIELD(STATUS, FIFO_FULL, 15u, 1u)
    FIELD(STATUS, ALERT_FATAL_FAULT, 16u, 1u)
    FIELD(STATUS, ALERT_RECOV_CTRL_UPDATE_ERR, 17u, 1u)
REG32(ENTROPY_PERIOD, 0x20u)
    FIELD(ENTROPY_PERIOD, PRESCALER, 0u, 10u)
    FIELD(ENTROPY_PERIOD, WAIT_TIMER, 16u, 16u)
REG32(ENTROPY_REFRESH_HASH_CNT, 0x24u)
    FIELD(ENTROPY_REFRESH_HASH_CNT, HASH_CNT, 0u, 10u)
REG32(ENTROPY_REFRESH_THRESHOLD_SHADOWED, 0x28u)
    FIELD(ENTROPY_REFRESH_THRESHOLD_SHADOWED, THRESHOLD, 0u, 10u)
REG32(ENTROPY_SEED, 0x2cu)
REG32(KEY_SHARE0_0, 0x30u)
REG32(KEY_SHARE0_1, 0x34u)
REG32(KEY_SHARE0_2, 0x38u)
REG32(KEY_SHARE0_3, 0x3cu)
REG32(KEY_SHARE0_4, 0x40u)
REG32(KEY_SHARE0_5, 0x44u)
REG32(KEY_SHARE0_6, 0x48u)
REG32(KEY_SHARE0_7, 0x4cu)
REG32(KEY_SHARE0_8, 0x50u)
REG32(KEY_SHARE0_9, 0x54u)
REG32(KEY_SHARE0_10, 0x58u)
REG32(KEY_SHARE0_11, 0x5cu)
REG32(KEY_SHARE0_12, 0x60u)
REG32(KEY_SHARE0_13, 0x64u)
REG32(KEY_SHARE0_14, 0x68u)
REG32(KEY_SHARE0_15, 0x6cu)
REG32(KEY_SHARE1_0, 0x70u)
REG32(KEY_SHARE1_1, 0x74u)
REG32(KEY_SHARE1_2, 0x78u)
REG32(KEY_SHARE1_3, 0x7cu)
REG32(KEY_SHARE1_4, 0x80u)
REG32(KEY_SHARE1_5, 0x84u)
REG32(KEY_SHARE1_6, 0x88u)
REG32(KEY_SHARE1_7, 0x8cu)
REG32(KEY_SHARE1_8, 0x90u)
REG32(KEY_SHARE1_9, 0x94u)
REG32(KEY_SHARE1_10, 0x98u)
REG32(KEY_SHARE1_11, 0x9cu)
REG32(KEY_SHARE1_12, 0xa0u)
REG32(KEY_SHARE1_13, 0xa4u)
REG32(KEY_SHARE1_14, 0xa8u)
REG32(KEY_SHARE1_15, 0xacu)
REG32(KEY_LEN, 0xb0u)
    FIELD(KEY_LEN, LEN, 0u, 3u)
REG32(PREFIX_0, 0xb4u)
REG32(PREFIX_1, 0xb8u)
REG32(PREFIX_2, 0xbcu)
REG32(PREFIX_3, 0xc0u)
REG32(PREFIX_4, 0xc4u)
REG32(PREFIX_5, 0xc8u)
REG32(PREFIX_6, 0xccu)
REG32(PREFIX_7, 0xd0u)
REG32(PREFIX_8, 0xd4u)
REG32(PREFIX_9, 0xd8u)
REG32(PREFIX_10, 0xdcu)
REG32(ERR_CODE, 0xe0u)
    FIELD(ERR_CODE, INFO, 0u, 24u)
    FIELD(ERR_CODE, CODE, 24u, 8u)
/* clang-format on */

#define INTR_MASK \
    (INTR_KMAC_ERR_MASK | INTR_FIFO_EMPTY_MASK | INTR_KMAC_DONE_MASK)
#define ALERT_MASK \
    (R_ALERT_TEST_FATAL_FAULT_MASK | R_ALERT_TEST_RECOV_OPERATION_MASK)
#define CFG_MASK \
    (R_CFG_SHADOWED_KMAC_EN_MASK | R_CFG_SHADOWED_KSTRENGTH_MASK | \
     R_CFG_SHADOWED_MODE_MASK | R_CFG_SHADOWED_MSG_ENDIANNESS_MASK | \
     R_CFG_SHADOWED_STATE_ENDIANNESS_MASK | R_CFG_SHADOWED_SIDELOAD_MASK | \
     R_CFG_SHADOWED_ENTROPY_MODE_MASK | \
     R_CFG_SHADOWED_ENTROPY_FAST_PROCESS_MASK | R_CFG_SHADOWED_MSG_MASK_MASK | \
     R_CFG_SHADOWED_ENTROPY_READY_MASK | \
     R_CFG_SHADOWED_EN_UNSUPPORTED_MODESTRENGTH_MASK)

enum {
    OT_KMAC_CMD_NONE = 0,
    OT_KMAC_CMD_START = 0x1d,
    OT_KMAC_CMD_PROCESS = 0x2e,
    OT_KMAC_CMD_MANUAL_RUN = 0x31,
    OT_KMAC_CMD_DONE = 0x16,
};

#define CMD_NAME_ENTRY(_st_) [OT_KMAC_CMD_##_st_] = stringify(_st_)
static const char *CMD_NAMES[] = {
    CMD_NAME_ENTRY(NONE),       CMD_NAME_ENTRY(START), CMD_NAME_ENTRY(PROCESS),
    CMD_NAME_ENTRY(MANUAL_RUN), CMD_NAME_ENTRY(DONE),
};
#undef CMD_NAME_ENTRY
#define CMD_NAME(_st_) \
    ((_st_) >= 0 && (_st_) < ARRAY_SIZE(CMD_NAMES) ? CMD_NAMES[(_st_)] : "?")

enum {
    OT_KMAC_ERR_NONE = 0,
    OT_KMAC_ERR_KEY_NOT_VALID = 0x01,
    OT_KMAC_ERR_SW_PUSHED_MSG_FIFO = 0x02,
    OT_KMAC_ERR_SW_ISSUED_CMD_IN_APP_ACTIVE = 0x03,
    OT_KMAC_ERR_WAIT_TIMER_EXPIRED = 0x04,
    OT_KMAC_ERR_INCORRECT_ENTROPY_MODE = 0x05,
    OT_KMAC_ERR_UNEXPECTED_MODE_STRENGTH = 0x06,
    OT_KMAC_ERR_INCORRECT_FUNCTION_NAME = 0x07,
    OT_KMAC_ERR_SW_CMD_SEQUENCE = 0x08,
    OT_KMAC_ERR_SW_HASHING_WITHOUT_ENTROPY_READY = 0x09,
    OT_KMAC_ERR_SHADOW_REG_UPDATE = 0xc0,
    OT_KMAC_ERR_FATAL_ERROR = 0xc1,
    OT_KMAC_ERR_PACKER_INTEGRITY = 0xc2,
    OT_KMAC_ERR_MSG_FIFO_INTEGRITY = 0xc3,
};

#define ERR_NAME_ENTRY(_st_) [OT_KMAC_ERR_##_st_] = stringify(_st_)
static const char *ERR_NAMES[] = {
    ERR_NAME_ENTRY(NONE),
    ERR_NAME_ENTRY(KEY_NOT_VALID),
    ERR_NAME_ENTRY(SW_PUSHED_MSG_FIFO),
    ERR_NAME_ENTRY(SW_ISSUED_CMD_IN_APP_ACTIVE),
    ERR_NAME_ENTRY(WAIT_TIMER_EXPIRED),
    ERR_NAME_ENTRY(INCORRECT_ENTROPY_MODE),
    ERR_NAME_ENTRY(UNEXPECTED_MODE_STRENGTH),
    ERR_NAME_ENTRY(INCORRECT_FUNCTION_NAME),
    ERR_NAME_ENTRY(SW_CMD_SEQUENCE),
    ERR_NAME_ENTRY(SW_HASHING_WITHOUT_ENTROPY_READY),
    ERR_NAME_ENTRY(SHADOW_REG_UPDATE),
    ERR_NAME_ENTRY(FATAL_ERROR),
    ERR_NAME_ENTRY(PACKER_INTEGRITY),
    ERR_NAME_ENTRY(MSG_FIFO_INTEGRITY),
};
#undef ERR_NAME_ENTRY
#define ERR_NAME(_st_) \
    ((_st_) >= 0 && (_st_) < ARRAY_SIZE(ERR_NAMES) ? ERR_NAMES[(_st_)] : "?")

/* base offset for MMIO registers */
#define OT_KMAC_REGS_BASE 0x00000000u
/* base offset for MMIO STATE */
#define OT_KMAC_STATE_BASE 0x00000400u
/* length of MMIO STATE */
#define OT_KMAC_STATE_SIZE 0x00000200u
/* base offset for MMIO MSG_FIFO */
#define OT_KMAC_MSG_FIFO_BASE 0x00000800u
/* length of MMIO FIFO */
#define OT_KMAC_MSG_FIFO_SIZE 0x00000800u
/* length of the whole device MMIO region */
#define OT_KMAC_WHOLE_SIZE (OT_KMAC_MSG_FIFO_BASE + OT_KMAC_MSG_FIFO_SIZE)

#define OT_KMAC_CLOCK_ACTIVE "clock-active"
#define OT_KMAC_CLOCK_INPUT  "clock-in"

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_ERR_CODE)
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
    REG_NAME_ENTRY(CFG_REGWEN),
    REG_NAME_ENTRY(CFG_SHADOWED),
    REG_NAME_ENTRY(CMD),
    REG_NAME_ENTRY(STATUS),
    REG_NAME_ENTRY(ENTROPY_PERIOD),
    REG_NAME_ENTRY(ENTROPY_REFRESH_HASH_CNT),
    REG_NAME_ENTRY(ENTROPY_REFRESH_THRESHOLD_SHADOWED),
    REG_NAME_ENTRY(ENTROPY_SEED),
    REG_NAME_ENTRY(KEY_SHARE0_0),
    REG_NAME_ENTRY(KEY_SHARE0_1),
    REG_NAME_ENTRY(KEY_SHARE0_2),
    REG_NAME_ENTRY(KEY_SHARE0_3),
    REG_NAME_ENTRY(KEY_SHARE0_4),
    REG_NAME_ENTRY(KEY_SHARE0_5),
    REG_NAME_ENTRY(KEY_SHARE0_6),
    REG_NAME_ENTRY(KEY_SHARE0_7),
    REG_NAME_ENTRY(KEY_SHARE0_8),
    REG_NAME_ENTRY(KEY_SHARE0_9),
    REG_NAME_ENTRY(KEY_SHARE0_10),
    REG_NAME_ENTRY(KEY_SHARE0_11),
    REG_NAME_ENTRY(KEY_SHARE0_12),
    REG_NAME_ENTRY(KEY_SHARE0_13),
    REG_NAME_ENTRY(KEY_SHARE0_14),
    REG_NAME_ENTRY(KEY_SHARE0_15),
    REG_NAME_ENTRY(KEY_SHARE1_0),
    REG_NAME_ENTRY(KEY_SHARE1_1),
    REG_NAME_ENTRY(KEY_SHARE1_2),
    REG_NAME_ENTRY(KEY_SHARE1_3),
    REG_NAME_ENTRY(KEY_SHARE1_4),
    REG_NAME_ENTRY(KEY_SHARE1_5),
    REG_NAME_ENTRY(KEY_SHARE1_6),
    REG_NAME_ENTRY(KEY_SHARE1_7),
    REG_NAME_ENTRY(KEY_SHARE1_8),
    REG_NAME_ENTRY(KEY_SHARE1_9),
    REG_NAME_ENTRY(KEY_SHARE1_10),
    REG_NAME_ENTRY(KEY_SHARE1_11),
    REG_NAME_ENTRY(KEY_SHARE1_12),
    REG_NAME_ENTRY(KEY_SHARE1_13),
    REG_NAME_ENTRY(KEY_SHARE1_14),
    REG_NAME_ENTRY(KEY_SHARE1_15),
    REG_NAME_ENTRY(KEY_LEN),
    REG_NAME_ENTRY(PREFIX_0),
    REG_NAME_ENTRY(PREFIX_1),
    REG_NAME_ENTRY(PREFIX_2),
    REG_NAME_ENTRY(PREFIX_3),
    REG_NAME_ENTRY(PREFIX_4),
    REG_NAME_ENTRY(PREFIX_5),
    REG_NAME_ENTRY(PREFIX_6),
    REG_NAME_ENTRY(PREFIX_7),
    REG_NAME_ENTRY(PREFIX_8),
    REG_NAME_ENTRY(PREFIX_9),
    REG_NAME_ENTRY(PREFIX_10),
    REG_NAME_ENTRY(ERR_CODE),
};
#undef REG_NAME_ENTRY

#define OT_KMAC_KEY_HEXSTR_SIZE (OT_KMAC_KEY_SIZE * 2u + 2u)

/* Input FIFO length is 80 bytes (10 x 64 bits) */
#define FIFO_LENGTH 80u

/* Delay FIFO ingestion and compute by 100ns */
#define BH_TRIGGER_DELAY_NS 100u

/* Max size of the KECCAK state */
#define KECCAK_STATE_BITS  1600u
#define KECCAK_STATE_BYTES (KECCAK_STATE_BITS / 8u)

/*
 * Size of the state window for each share. Each window contains
 * KECCAK_STATE_BYTES of state followed by zeros.
 */
#define KECCAK_STATE_SHARE_BYTES 256u

/* Number of KEY_* registers */
#define NUM_KEY_REGS 16u

/* Number of PREFIX_* registers */
#define NUM_PREFIX_REGS 11u

/* function prefix for KMAC operations (first 6 bytes of PREFIX_*) */
#define KMAC_PREFIX_0      0x4d4b2001u
#define KMAC_PREFIX_0_MASK 0xffffffffu
#define KMAC_PREFIX_1      0x00004341u
#define KMAC_PREFIX_1_MASK 0x0000ffffu

enum {
    ALERT_RECOVERABLE = 0,
    ALERT_FATAL = 1,
};

/*
 * FSM states, values hard-coded to st_logical_e values from RTL for direct use
 * in error reporting
 */
typedef enum {
    /* idle */
    KMAC_ST_IDLE = 0,
    /* MSG_FEED: receive the message bitstream */
    KMAC_ST_MSG_FEED = 1,
    /* PROCESSING: computes the keccak rounds */
    KMAC_ST_PROCESSING = 2,
    /* ABSORBED: digest is available */
    KMAC_ST_ABSORBED = 3,
    /* SQUEEZING: compute more keccak rounds */
    KMAC_ST_SQUEEZING = 4,
    /* illegal state reached and hang */
    KMAC_ST_TERMINAL_ERROR = 5,
} OtKMACFsmState;

#define STATE_NAME_ENTRY(_st_) [KMAC_ST_##_st_] = stringify(_st_)
static const char *STATE_NAMES[] = {
    STATE_NAME_ENTRY(IDLE),       STATE_NAME_ENTRY(MSG_FEED),
    STATE_NAME_ENTRY(PROCESSING), STATE_NAME_ENTRY(ABSORBED),
    STATE_NAME_ENTRY(SQUEEZING),  STATE_NAME_ENTRY(TERMINAL_ERROR),
};
#undef STATE_NAME_ENTRY
#define STATE_NAME(_st_) \
    ((_st_) >= 0 && (_st_) < ARRAY_SIZE(STATE_NAMES) ? STATE_NAMES[(_st_)] : \
                                                       "?")
typedef struct {
    unsigned index; /* app index */
    OtKMACAppCfg cfg; /* configuration */
    OtKMACAppReq req; /* pending request */
    OtKmacResponse fn; /* response callback */
    void *opaque; /* opaque parameter to response callback */
    bool connected; /* app is connected to KMAC */
    bool req_pending; /* true if pending request */
} OtKMACApp;

typedef struct {
    uint8_t share0[OT_KMAC_KEY_SIZE];
    uint8_t share1[OT_KMAC_KEY_SIZE];
    bool valid;
} OtKMACKey;

struct OtKMACState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    MemoryRegion regs_mmio;
    MemoryRegion state_mmio;
    MemoryRegion msgfifo_mmio;
    IbexIRQ irqs[3u];
    IbexIRQ alerts[KMAC_PARAM_NUM_ALERTS];
    IbexIRQ clock_active;

    uint32_t *regs;
    OtShadowReg cfg;
    OtShadowReg entropy_refresh_threshold;

    OtKMACFsmState state; /* Main FSM state */
    bool invalid_state_read;
    bool error_awaiting_sw; /* error awaiting SW acknowledgement */
    hash_state ltc_state; /* TomCrypt hash state */
    uint8_t keccak_state[KECCAK_STATE_BYTES];

    OtKMACAppCfg sw_cfg;
    OtKMACAppCfg *current_cfg;
    unsigned pclk; /* Current input clock */
    const char *clock_src_name; /* IRQ name once connected */

    OtKMACApp *apps;
    OtKMACApp *current_app;
    OtKMACKey *sl_key;
    char *hexstr;
    uint32_t pending_apps;

    Fifo8 input_fifo;
    QEMUTimer *bh_timer; /* timer to delay bh when triggered from vCPU */
    QEMUBH *bh;

    char *ot_id;
    char *clock_name;
    DeviceState *clock_src;
    OtEDNState *edn;
    uint8_t edn_ep;
    uint8_t num_app;
};

static void
ot_kmac_change_fsm_state_line(OtKMACState *s, OtKMACFsmState state, int line)
{
    if (s->state == state) {
        return;
    }

    if (s->current_app) {
        trace_ot_kmac_change_state_app(s->ot_id, s->current_app->index, line,
                                       STATE_NAME(s->state), s->state,
                                       STATE_NAME(state), state);
    } else {
        trace_ot_kmac_change_state_sw(s->ot_id, line, STATE_NAME(s->state),
                                      s->state, STATE_NAME(state), state);
    }

    s->state = state;
}

#define ot_kmac_change_fsm_state(_s_, _st_) \
    ot_kmac_change_fsm_state_line(_s_, _st_, __LINE__)

static void ot_kmac_trigger_deferred_bh(OtKMACState *s)
{
    timer_del(s->bh_timer);
    timer_mod(s->bh_timer,
              qemu_clock_get_ns(OT_VIRTUAL_CLOCK) + BH_TRIGGER_DELAY_NS);
}

static void ot_kmac_bh_timer_handler(void *opaque)
{
    OtKMACState *s = opaque;
    qemu_bh_schedule(s->bh);
}

static void ot_kmac_cancel_bh(OtKMACState *s)
{
    timer_del(s->bh_timer);
    qemu_bh_cancel(s->bh);
}

static void ot_kmac_update_irq(OtKMACState *s)
{
    uint32_t level = s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE];
    for (unsigned ix = 0; ix < ARRAY_SIZE(s->irqs); ix++) {
        ibex_irq_set(&s->irqs[ix], (int)((level >> ix) & 0x1u));
    }
}

static void ot_kmac_update_alert(OtKMACState *s)
{
    uint32_t level = s->regs[R_ALERT_TEST];

    if (s->regs[R_STATUS] & R_STATUS_ALERT_FATAL_FAULT_MASK) {
        level |= 1u << ALERT_FATAL;
    }
    if (s->regs[R_STATUS] & R_STATUS_ALERT_RECOV_CTRL_UPDATE_ERR_MASK) {
        level |= 1u << ALERT_RECOVERABLE;
    }

    for (unsigned ix = 0; ix < ARRAY_SIZE(s->alerts); ix++) {
        ibex_irq_set(&s->alerts[ix], (int)((level >> ix) & 0x1u));
    }
}

static void ot_kmac_report_error(OtKMACState *s, int code, uint32_t info)
{
    trace_ot_kmac_report_error(s->ot_id, code, ERR_NAME(code), info);

    uint32_t error = 0;
    error = FIELD_DP32(error, ERR_CODE, CODE, code);
    error = FIELD_DP32(error, ERR_CODE, INFO, info);

    s->error_awaiting_sw = true;
    s->regs[R_ERR_CODE] = error;
    s->regs[R_INTR_STATE] |= INTR_KMAC_ERR_MASK;
    ot_kmac_update_irq(s);
}

static void ot_kmac_get_sw_config(OtKMACState *s)
{
    uint32_t cfg = ot_shadow_reg_peek(&s->cfg);

    switch (FIELD_EX32(cfg, CFG_SHADOWED, MODE)) {
    case 0:
        s->sw_cfg.mode = OT_KMAC_MODE_SHA3;
        break;
    case 2u:
        s->sw_cfg.mode = OT_KMAC_MODE_SHAKE;
        break;
    case 3u:
        if (FIELD_EX32(cfg, CFG_SHADOWED, KMAC_EN) != 0) {
            s->sw_cfg.mode = OT_KMAC_MODE_KMAC;
        } else {
            s->sw_cfg.mode = OT_KMAC_MODE_CSHAKE;
        }
        break;
    default:
        /* invalid modes are checked when processing START command */
        s->sw_cfg.mode = OT_KMAC_MODE_NONE;
        break;
    }

    switch (FIELD_EX32(cfg, CFG_SHADOWED, KSTRENGTH)) {
    case 0:
        s->sw_cfg.strength = 128u;
        break;
    case 1u:
        s->sw_cfg.strength = 224u;
        break;
    case 2u:
        s->sw_cfg.strength = 256u;
        break;
    case 3u:
        s->sw_cfg.strength = 384u;
        break;
    case 4u:
        s->sw_cfg.strength = 512u;
        break;
    default:
        /* invalid key strength are checked when processing START command */
        s->sw_cfg.strength = 0;
        break;
    }
}

static inline size_t ot_kmac_get_key_length(const OtKMACState *s)
{
    uint32_t key_len = FIELD_EX32(s->regs[R_KEY_LEN], KEY_LEN, LEN);
    switch (key_len) {
    case 0:
        return 16u;
    case 1u:
        return 24u;
    case 2u:
        return 32u;
    case 3u:
        return 48u;
    case 4u:
        return 64u;
    default:
        /* invalid key length values are traced at register write */
        return 0;
    }
}

static void ot_kmac_push_key(OtKeySinkIf *ifd, const uint8_t *share0,
                             const uint8_t *share1, size_t key_len, bool valid)
{
    g_assert(!key_len || key_len == OT_KMAC_KEY_SIZE);

    OtKMACState *s = OT_KMAC(ifd);
    OtKMACKey *key = s->sl_key;

    if (key_len && share0) {
        memcpy(key->share0, share0, key_len);
    } else {
        memset(key->share0, 0, OT_KMAC_KEY_SIZE);
    }
    if (key_len && share1) {
        memcpy(key->share1, share1, key_len);
    } else {
        memset(key->share1, 0, OT_KMAC_KEY_SIZE);
    }
    key->valid = valid;

    if (trace_event_get_state(TRACE_OT_KMAC_PUSH_KEY)) {
        uint8_t key_value[OT_KMAC_KEY_SIZE];
        for (unsigned ix = 0u; ix < OT_KMAC_KEY_SIZE; ix++) {
            key_value[ix] = key->share0[ix] ^ key->share1[ix];
        }

        trace_ot_kmac_push_key(s->ot_id, valid,
                               ot_common_lhexdump(key_value, OT_KMAC_KEY_SIZE,
                                                  true, s->hexstr,
                                                  OT_KMAC_KEY_HEXSTR_SIZE));
    }
}

static void ot_kmac_get_key(OtKMACState *s, uint8_t *key, size_t *keylen)
{
    uint32_t cfg = ot_shadow_reg_peek(&s->cfg);
    bool sideload = FIELD_EX32(cfg, CFG_SHADOWED, SIDELOAD) != 0;

    /* force sideload for app interface */
    if (s->current_app) {
        sideload = true;
    }

    if (sideload) {
        *keylen = OT_KMAC_KEY_SIZE;
        const OtKMACKey *sl_key = s->sl_key;
        for (size_t ix = 0; ix < OT_KMAC_KEY_SIZE; ix++) {
            key[ix] = sl_key->share0[ix] ^ sl_key->share1[ix];
        }

        if (!sl_key->valid) {
            /* HW defaults to info = app_id = 0 when in a SW operation. */
            uint32_t err_info = s->current_app ? s->current_app->index : 0;
            ot_kmac_report_error(s, OT_KMAC_ERR_KEY_NOT_VALID, err_info);
        }
        return;
    }

    *keylen = ot_kmac_get_key_length(s);
    for (size_t ix = 0; ix < *keylen; ix++) {
        uint8_t reg = ix / sizeof(uint32_t);
        uint8_t byteoffset = ix & (sizeof(uint32_t) - 1u);

        uint8_t share0 = (uint8_t)(s->regs[R_KEY_SHARE0_0 + reg] >>
                                   (byteoffset * BITS_PER_BYTE));
        uint8_t share1 = (uint8_t)(s->regs[R_KEY_SHARE1_0 + reg] >>
                                   (byteoffset * BITS_PER_BYTE));
        key[ix] = share0 ^ share1;
    }
}

static inline size_t ot_kmac_get_keccak_rate_bytes(size_t kstrength)
{
    /*
     * Rate is calculated with:
     * rate = (1600 - 2*x) where x is the security strength (i.e. half the
     * capacity).
     */
    return (KECCAK_STATE_BITS - 2u * kstrength) / 8u;
}

static void ot_kmac_reset_state(OtKMACState *s)
{
    memset(s->keccak_state, 0, sizeof(s->keccak_state));
    memset(&s->ltc_state, 0, sizeof(s->ltc_state));
    s->current_cfg = NULL;
}

static void ot_kmac_start_pending_app(OtKMACState *s);

static void ot_kmac_return_to_idle(OtKMACState *s)
{
    /* flush state */
    ot_kmac_change_fsm_state(s, KMAC_ST_IDLE);
    ot_kmac_reset_state(s);
    ot_kmac_cancel_bh(s);
    /* now is a good time to check for pending app requests */
    ot_kmac_start_pending_app(s);
}

static void ot_kmac_complete_app_req(OtKMACState *s)
{
    trace_ot_kmac_app_finished(s->ot_id, s->current_app->index);
    s->current_app = NULL;
    ot_kmac_return_to_idle(s);
}

/* BH handler for processing FIFO and compute */
static void ot_kmac_process(void *opaque)
{
    OtKMACState *s = opaque;
    OtKMACAppCfg *cfg = s->current_cfg;
    OtKMACAppRsp rsp;

    g_assert(cfg);

    if (s->current_app) {
        /* App mode, FIFO should be empty */
        g_assert(fifo8_is_empty(&s->input_fifo));

        if (s->current_app->req_pending) {
            sha3_process(&s->ltc_state, s->current_app->req.msg_data,
                         s->current_app->req.msg_len);
            s->current_app->req_pending = false;
            if (s->current_app->req.last) {
                /* append right-encoded output width if KMAC */
                if (cfg->mode == OT_KMAC_MODE_KMAC) {
                    uint8_t enc_out_len[3];
                    uint32_t output_length = OT_KMAC_APP_DIGEST_BYTES * 8u;
                    enc_out_len[0] = (output_length >> 8u) & 0xffu;
                    enc_out_len[1] = output_length & 0xffu;
                    enc_out_len[2] = 2u;
                    sha3_process(&s->ltc_state, enc_out_len,
                                 sizeof(enc_out_len));
                }
                /* go to PROCESSING state, response will be sent there */
                ot_kmac_change_fsm_state(s, KMAC_ST_PROCESSING);
            } else {
                /* send empty response as acknowledge */
                if (s->current_app->fn) {
                    memset(&rsp, 0, sizeof(rsp));
                    s->current_app->fn(s->current_app->opaque, &rsp);
                }
            }
        }
    } else {
        /* SW mode, process FIFO data */
        if (!fifo8_is_empty(&s->input_fifo)) {
            while (!fifo8_is_empty(&s->input_fifo)) {
                uint8_t value = fifo8_pop(&s->input_fifo);
                sha3_process(&s->ltc_state, &value, 1);
            }

            /* assert FIFO Empty interrupt */
            s->regs[R_INTR_STATE] |= INTR_FIFO_EMPTY_MASK;
        }
    }

    switch (s->state) {
    case KMAC_ST_PROCESSING:
    case KMAC_ST_SQUEEZING:
        switch (cfg->mode) {
        case OT_KMAC_MODE_SHA3:
            sha3_done(&s->ltc_state, &s->keccak_state[0]);
            break;
        case OT_KMAC_MODE_SHAKE:
            sha3_shake_done(&s->ltc_state, &s->keccak_state[0],
                            ot_kmac_get_keccak_rate_bytes(cfg->strength));
            break;
        /* NOLINTNEXTLINE(bugprone-branch-clone) */
        case OT_KMAC_MODE_CSHAKE:
        case OT_KMAC_MODE_KMAC:
            sha3_cshake_done(&s->ltc_state, &s->keccak_state[0],
                             ot_kmac_get_keccak_rate_bytes(cfg->strength));
            break;
        default:
            /*
             * should never happen: mode was validated when going from state
             * IDLE to START
             */
            g_assert_not_reached();
        }

        /*
         * "If key is sideloaded and KMAC is SW initiated, hide the capacity
         * from SW by zeroing", i.e. if not doing a SW-initiated sideloaded
         * operation, the entire Keccak state (including the meaningless
         * capacity bytes) should be loaded.
         */
        uint32_t reg_cfg = ot_shadow_reg_peek(&s->cfg);
        bool sideload = FIELD_EX32(reg_cfg, CFG_SHADOWED, SIDELOAD) != 0;
        if (!sideload || s->current_app) {
            static_assert(sizeof(s->ltc_state.sha3.sb) == KECCAK_STATE_BYTES,
                          "LibTomCrypt's Keccak state is an unexpected size");
            /* manually extract entire Keccak state, including capacity */
            memcpy(&s->keccak_state[0], s->ltc_state.sha3.sb,
                   KECCAK_STATE_BYTES);
        }

        if (s->current_app) {
            /* App mode, send response and go back to IDLE state */
            if (s->current_app->fn) {
                rsp.done = true;
                memcpy(&rsp.digest_share0[0], &s->keccak_state[0],
                       sizeof(rsp.digest_share0));
                memset(&rsp.digest_share1[0], 0, sizeof(rsp.digest_share1));
                s->current_app->fn(s->current_app->opaque, &rsp);
            }
            if (!s->error_awaiting_sw) {
                ot_kmac_complete_app_req(s);
            }
        } else {
            /* SW mode, go to ABSORBED state */
            ot_kmac_change_fsm_state(s, KMAC_ST_ABSORBED);

            /* assert KMAC Done interrupt */
            s->regs[R_INTR_STATE] |= INTR_KMAC_DONE_MASK;
        }

        break;
    default:
        /* nothing to do for other states */
        break;
    }

    ot_kmac_update_irq(s);
}

static inline bool ot_kmac_config_enabled(const OtKMACState *s)
{
    /* configuration is enabled only in idle mode */
    return s->state == KMAC_ST_IDLE;
}

static inline bool ot_kmac_check_reg_write(const OtKMACState *s, hwaddr reg)
{
    if (!ot_kmac_config_enabled(s)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: write to %s ignored while busy\n", __func__,
                      s->ot_id, REG_NAME(reg));
        return false;
    }

    return true;
}

static bool ot_kmac_check_mode_and_strength(const OtKMACAppCfg *cfg)
{
    switch (cfg->mode) {
    case OT_KMAC_MODE_SHA3:
        switch (cfg->strength) {
        case 224u:
        case 256u:
        case 384u:
        case 512u:
            return true;
        default:
            /* unsupported strength for SHA3 */
            return false;
        }
        break;
    case OT_KMAC_MODE_SHAKE:
    case OT_KMAC_MODE_CSHAKE:
    case OT_KMAC_MODE_KMAC:
        switch (cfg->strength) {
        case 128u:
        case 256u:
            return true;
        default:
            /* unsupported strength for SHAKE/cSHAKE/KMAC */
            return false;
        }
        break;
    default:
        /* unsupported mode */
        return false;
    }
}

static inline uint8_t
ot_kmac_get_prefix_byte(const OtKMACState *s, size_t offset)
{
    size_t reg = offset / sizeof(uint32_t);
    size_t byteoffset = offset - reg * sizeof(uint32_t);

    if (reg >= NUM_PREFIX_REGS) {
        /*
         * Just return 0, bounds checking should have been done by the caller
         * anyway.
         */
        return 0;
    }

    return (uint8_t)(s->regs[R_PREFIX_0 + reg] >> (byteoffset * 8u));
}

static size_t ot_kmac_left_decode(const OtKMACState *s, size_t offset,
                                  size_t *value)
{
    size_t len;
    size_t val = 0;

    /* first byte is the length in bytes of encoded value */
    len = (size_t)ot_kmac_get_prefix_byte(s, offset);
    if (len < 1u || len > 4u) {
        return 0;
    }

    /* compute value */
    for (size_t ix = 1u; ix < len + 1u; ix++) {
        val = (val << 8u) | ot_kmac_get_prefix_byte(s, offset + ix);
    }

    *value = val;
    return 1u + len;
}

static bool ot_kmac_decode_sw_prefix(OtKMACState *s)
{
    size_t offset = 0;
    size_t used;
    OtKMACPrefix *prefix = &s->sw_cfg.prefix;

    used = ot_kmac_left_decode(s, offset, &prefix->funcname_len);
    prefix->funcname_len /= 8u;
    offset += used;

    if (prefix->funcname_len > OT_KMAC_PREFIX_FUNCNAME_LEN) {
        goto error;
    }
    for (size_t ix = 0; ix < prefix->funcname_len; ix++) {
        prefix->funcname[ix] = ot_kmac_get_prefix_byte(s, offset + ix);
    }
    offset += prefix->funcname_len;

    used = ot_kmac_left_decode(s, offset, &prefix->customstr_len);
    prefix->customstr_len /= 8u;
    offset += used;

    if (prefix->customstr_len > OT_KMAC_PREFIX_CUSTOMSTR_LEN) {
        goto error;
    }
    for (size_t ix = 0; ix < prefix->customstr_len; ix++) {
        prefix->customstr[ix] = ot_kmac_get_prefix_byte(s, offset + ix);
    }
    offset += prefix->funcname_len;

    if (offset <= NUM_PREFIX_REGS * sizeof(uint32_t)) {
        return true;
    }

error:
    memset(prefix, 0, sizeof(OtKMACPrefix));
    return false;
}

static bool ot_kmac_check_kmac_sw_prefix(const OtKMACState *s)
{
    /*
     * check that the encoded prefix in PREFIX_x registers starts with a "KMAC"
     * function name.
     */
    return ((s->regs[R_PREFIX_0] & KMAC_PREFIX_0_MASK) == KMAC_PREFIX_0) &&
           ((s->regs[R_PREFIX_1] & KMAC_PREFIX_1_MASK) == KMAC_PREFIX_1);
}

static void ot_kmac_process_start(OtKMACState *s)
{
    OtKMACAppCfg *cfg = s->current_cfg;

    g_assert(cfg);

    switch (cfg->mode) {
    case OT_KMAC_MODE_SHA3:
        switch (cfg->strength) {
        case 224u:
            sha3_224_init(&s->ltc_state);
            break;
        case 256u:
            sha3_256_init(&s->ltc_state);
            break;
        case 384u:
            sha3_384_init(&s->ltc_state);
            break;
        case 512u:
            sha3_512_init(&s->ltc_state);
            break;
        default:
            /* should never happen: strength was already validated earlier */
            g_assert_not_reached();
        }
        break;
    case OT_KMAC_MODE_SHAKE:
        switch (cfg->strength) {
        case 128u:
        case 256u:
            sha3_shake_init(&s->ltc_state, (int)cfg->strength);
            break;
        default:
            /* should never happen: strength was already validated earlier */
            g_assert_not_reached();
        }
        break;
    case OT_KMAC_MODE_CSHAKE:
    case OT_KMAC_MODE_KMAC:
        switch (cfg->strength) {
        case 128u:
        case 256u: {
            sha3_cshake_init(&s->ltc_state, (int)cfg->strength,
                             cfg->prefix.funcname, cfg->prefix.funcname_len,
                             cfg->prefix.customstr, cfg->prefix.customstr_len);
            /* if KMAC mode is enabled, process key */
            if (cfg->mode == OT_KMAC_MODE_KMAC) {
                uint8_t key[NUM_KEY_REGS * sizeof(uint32_t)];
                size_t keylen = 0;
                static_assert(OT_KMAC_KEY_SIZE <= ARRAY_SIZE(key),
                              "key buffer too small to hold sideloaded key");
                ot_kmac_get_key(s, key, &keylen);
                sha3_process_kmac_key(&s->ltc_state, key, keylen);
            }
            break;
        }
        default:
            /* should never happen: strength was already validated earlier */
            g_assert_not_reached();
        }
        break;
    default:
        /* should never happen: mode was already validated earlier */
        g_assert_not_reached();
    }
}

static void ot_kmac_sw_err_processed(OtKMACState *s, int cmd)
{
    s->error_awaiting_sw = false;

    if (s->current_app) {
        /*
         * If we have already received the entire app request data and sent a
         * response, now we just need SW acknowledgement to complete.
         *
         * If we haven't got all the data, we should wait for it all before
         * sending a response (but we store SW acknowledgement).
         */
        if (s->state == KMAC_ST_PROCESSING || s->state == KMAC_ST_SQUEEZING) {
            ot_kmac_complete_app_req(s);
        }
    } else {
        /* for SW: ignore further msg feed, absorb, report done & go to idle */
        switch (s->state) {
        case KMAC_ST_IDLE:
        case KMAC_ST_MSG_FEED:
            ot_kmac_change_fsm_state(s, KMAC_ST_PROCESSING);
            /* fallthrough */
        case KMAC_ST_PROCESSING:
        case KMAC_ST_SQUEEZING:
            ot_kmac_process((void *)s);
            /* fallthrough */
        case KMAC_ST_ABSORBED:
            ot_kmac_return_to_idle(s);
            break;
        case KMAC_ST_TERMINAL_ERROR:
            break;
        default:
            g_assert_not_reached();
        }
    }

    /* Clear the status / alert */
    s->regs[R_STATUS] &= ~R_STATUS_ALERT_RECOV_CTRL_UPDATE_ERR_MASK;
    ot_kmac_update_alert(s);

    /* Clear the error */
    s->regs[R_ERR_CODE] = 0u;

    /* SW shoud not send a command along with the err_processed bit */
    if (cmd != OT_KMAC_CMD_NONE) {
        if (s->current_app) {
            ot_kmac_report_error(s, OT_KMAC_ERR_SW_ISSUED_CMD_IN_APP_ACTIVE,
                                 cmd);
        } else {
            /* see error encoding in (hw/ip/kmac/rtl/kmac_pkg.sv) */
            uint32_t info = (1 << 11u);
            info |= (uint32_t)s->state << 8u;
            info |= cmd;
            ot_kmac_report_error(s, OT_KMAC_ERR_SW_CMD_SEQUENCE, info);
        }
    }
}


static void ot_kmac_process_sw_command(OtKMACState *s, uint32_t cmd_reg)
{
    bool err_processed = (cmd_reg & R_CMD_ERR_PROCESSED_MASK) != 0;
    int cmd = (int)FIELD_EX32(cmd_reg, CMD, CMD);
    if (s->error_awaiting_sw) {
        if (err_processed) {
            ot_kmac_sw_err_processed(s, cmd);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: Control write without err_processed whilst "
                          "KMAC is handling an error\n",
                          __func__, s->ot_id);
        }
        return;
    }

    uint32_t cfg = ot_shadow_reg_peek(&s->cfg);
    bool err_swsequence = false;
    bool err_modestrength = false;
    bool err_prefix = false;
    bool err_entropy_ready = false;

    /* check if an app is active */
    if (s->current_app) {
        ot_kmac_report_error(s, OT_KMAC_ERR_SW_ISSUED_CMD_IN_APP_ACTIVE, cmd);
        return;
    }

    trace_ot_kmac_process_sw_command(s->ot_id, cmd, CMD_NAME(cmd));

    switch (s->state) {
    case KMAC_ST_IDLE:
        if (cmd == OT_KMAC_CMD_NONE) {
            /* nothing to do */
        } else if (cmd == OT_KMAC_CMD_START) {
            /* retrieve configuration from CFG_SHADOWED register */
            ot_kmac_get_sw_config(s);

            if (!ot_kmac_check_mode_and_strength(&s->sw_cfg)) {
                err_modestrength = true;
                break;
            }
            /* if KMAC mode, check prefix & entropy ready */
            if (s->sw_cfg.mode == OT_KMAC_MODE_KMAC) {
                if (!ot_kmac_check_kmac_sw_prefix(s)) {
                    err_prefix = true;
                    break;
                }
                if (false /* TODO: check entropy ready */) {
                    err_entropy_ready = true;
                    break;
                }
            }
            /* if cSHAKE or KMAC modes, decode prefix from PREFIX_x registers */
            if (s->sw_cfg.mode == OT_KMAC_MODE_CSHAKE ||
                s->sw_cfg.mode == OT_KMAC_MODE_KMAC) {
                if (!ot_kmac_decode_sw_prefix(s)) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "%s: %s: could not decode cSHAKE prefix, "
                                  "digest result will be wrong!\n",
                                  __func__, s->ot_id);
                    memset(&s->sw_cfg.prefix, 0, sizeof(s->sw_cfg.prefix));
                }
            }

            s->current_cfg = &s->sw_cfg;
            ot_kmac_process_start(s);
            ot_kmac_change_fsm_state(s, KMAC_ST_MSG_FEED);
        } else {
            err_swsequence = true;
        }
        break;
    case KMAC_ST_MSG_FEED:
        if (cmd == OT_KMAC_CMD_NONE) {
            /* nothing to do */
        } else if (cmd == OT_KMAC_CMD_PROCESS) {
            ot_kmac_change_fsm_state(s, KMAC_ST_PROCESSING);
            ot_kmac_trigger_deferred_bh(s);
        } else {
            err_swsequence = true;
        }
        break;
    case KMAC_ST_PROCESSING:
    case KMAC_ST_SQUEEZING:
        /* computing stages during which no command can be issued */
        if (cmd != OT_KMAC_CMD_NONE) {
            err_swsequence = true;
        }
        break;
    case KMAC_ST_ABSORBED:
        if (cmd == OT_KMAC_CMD_NONE) {
            /* nothing to do */
        } else if (cmd == OT_KMAC_CMD_MANUAL_RUN) {
            ot_kmac_change_fsm_state(s, KMAC_ST_SQUEEZING);
            ot_kmac_trigger_deferred_bh(s);
        } else if (cmd == OT_KMAC_CMD_DONE) {
            ot_kmac_return_to_idle(s);
        } else {
            err_swsequence = true;
        }
        break;
    case KMAC_ST_TERMINAL_ERROR:
    default:
        ot_kmac_change_fsm_state(s, KMAC_ST_TERMINAL_ERROR);
        ot_kmac_reset_state(s);
        ot_kmac_cancel_bh(s);
        s->regs[R_STATUS] |= R_STATUS_ALERT_FATAL_FAULT_MASK;
        ot_kmac_update_alert(s);
        break;
    }

    /* report errors */
    if (err_swsequence | err_modestrength | err_prefix | err_entropy_ready) {
        uint8_t code;
        uint32_t info = 0;
        /*
         * error encoding is not documented, reference is OpenTitan RTL
         * (hw/ip/kmac/rtl/kmac_pkg.sv)
         */
        info |= err_swsequence ? 1 << 11u : 0;
        info |= err_modestrength ? 1 << 10u : 0;
        info |= err_prefix ? 1 << 9u : 0;
        if (err_swsequence) {
            info |= (uint32_t)s->state << 8u;
            info |= cmd;
            code = OT_KMAC_ERR_SW_CMD_SEQUENCE;
        } else if (err_modestrength) {
            info |= FIELD_EX32(cfg, CFG_SHADOWED, MODE) << 4u;
            info |= FIELD_EX32(cfg, CFG_SHADOWED, KSTRENGTH);
            code = OT_KMAC_ERR_UNEXPECTED_MODE_STRENGTH;
        } else if (err_prefix) {
            code = OT_KMAC_ERR_INCORRECT_FUNCTION_NAME;
        } else if (err_entropy_ready) {
            info |= err_entropy_ready ? 1 << 12u : 0;
            info |= FIELD_EX32(cfg, CFG_SHADOWED, KMAC_EN) ? 1 << 1u : 0;
            code = OT_KMAC_ERR_SW_HASHING_WITHOUT_ENTROPY_READY;
        } else {
            g_assert_not_reached();
        }
        ot_kmac_report_error(s, code, info);
    } else if (!s->error_awaiting_sw) {
        s->regs[R_ERR_CODE] = 0;
    }
}

static void ot_kmac_clock_input(void *opaque, int irq, int level)
{
    OtKMACState *s = opaque;

    g_assert(irq == 0);

    s->pclk = (unsigned)level;

    /* TODO: disable KMAC execution when PCLK is 0 */
}

static uint64_t ot_kmac_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtKMACState *s = OT_KMAC(opaque);
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);
    switch (reg) {
    case R_CFG_REGWEN:
        val32 = ot_kmac_config_enabled(s) ? R_CFG_REGWEN_EN_MASK : 0;
        break;
    case R_CFG_SHADOWED:
        val32 = ot_shadow_reg_read(&s->cfg);
        break;
    case R_STATUS:
        val32 = 0u;
        switch (s->state) {
        case KMAC_ST_IDLE:
            val32 |= R_STATUS_SHA3_IDLE_MASK;
            break;
        case KMAC_ST_MSG_FEED:
            val32 |= R_STATUS_SHA3_ABSORB_MASK;
            break;
        case KMAC_ST_ABSORBED:
            val32 |= R_STATUS_SHA3_SQUEEZE_MASK;
            break;
        default:
            break;
        }
        uint32_t num_used = fifo8_num_used(&s->input_fifo);
        if (num_used == 0) {
            val32 |= R_STATUS_FIFO_EMPTY_MASK;
        } else {
            val32 |= ((num_used / 4u) << R_STATUS_FIFO_DEPTH_SHIFT) &
                     R_STATUS_FIFO_DEPTH_MASK;
            if (num_used == FIFO_LENGTH) {
                val32 |= R_STATUS_FIFO_FULL_MASK;
            }
        }
        break;
    case R_ENTROPY_REFRESH_THRESHOLD_SHADOWED:
        val32 = ot_shadow_reg_read(&s->entropy_refresh_threshold);
        break;
    case R_INTR_STATE:
    case R_INTR_ENABLE:
    case R_ENTROPY_PERIOD:
    case R_ENTROPY_REFRESH_HASH_CNT:
    case R_PREFIX_0:
    case R_PREFIX_1:
    case R_PREFIX_2:
    case R_PREFIX_3:
    case R_PREFIX_4:
    case R_PREFIX_5:
    case R_PREFIX_6:
    case R_PREFIX_7:
    case R_PREFIX_8:
    case R_PREFIX_9:
    case R_PREFIX_10:
    case R_ERR_CODE:
        val32 = s->regs[reg];
        break;
    case R_CMD:
        /* always read 0: CMD is r0w1c */
        val32 = 0;
        break;
    case R_INTR_TEST:
    case R_ALERT_TEST:
    case R_ENTROPY_SEED:
    case R_KEY_SHARE0_0:
    case R_KEY_SHARE0_1:
    case R_KEY_SHARE0_2:
    case R_KEY_SHARE0_3:
    case R_KEY_SHARE0_4:
    case R_KEY_SHARE0_5:
    case R_KEY_SHARE0_6:
    case R_KEY_SHARE0_7:
    case R_KEY_SHARE0_8:
    case R_KEY_SHARE0_9:
    case R_KEY_SHARE0_10:
    case R_KEY_SHARE0_11:
    case R_KEY_SHARE0_12:
    case R_KEY_SHARE0_13:
    case R_KEY_SHARE0_14:
    case R_KEY_SHARE0_15:
    case R_KEY_SHARE1_0:
    case R_KEY_SHARE1_1:
    case R_KEY_SHARE1_2:
    case R_KEY_SHARE1_3:
    case R_KEY_SHARE1_4:
    case R_KEY_SHARE1_5:
    case R_KEY_SHARE1_6:
    case R_KEY_SHARE1_7:
    case R_KEY_SHARE1_8:
    case R_KEY_SHARE1_9:
    case R_KEY_SHARE1_10:
    case R_KEY_SHARE1_11:
    case R_KEY_SHARE1_12:
    case R_KEY_SHARE1_13:
    case R_KEY_SHARE1_14:
    case R_KEY_SHARE1_15:
    case R_KEY_LEN:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: W/O register 0x%02" HWADDR_PRIx " (%s)\n",
                      __func__, s->ot_id, addr, REG_NAME(reg));
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: bad offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, addr);
        val32 = 0;
        break;
    }

    uint32_t pc = ibex_get_current_pc();
    trace_ot_kmac_io_read_out(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32,
                              pc);

    return (uint64_t)val32;
}

static void ot_kmac_regs_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    OtKMACState *s = OT_KMAC(opaque);
    (void)size;
    uint32_t val32 = (uint32_t)value;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_kmac_io_write(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32, pc);

    switch (reg) {
    case R_INTR_STATE:
        s->regs[R_INTR_STATE] &= ~(val32 & INTR_MASK);
        ot_kmac_update_irq(s);
        break;
    case R_INTR_ENABLE:
        s->regs[R_INTR_ENABLE] = val32 & INTR_MASK;
        ot_kmac_update_irq(s);
        break;
    case R_INTR_TEST:
        s->regs[R_INTR_STATE] |= val32 & INTR_MASK;
        ot_kmac_update_irq(s);
        break;
    case R_ALERT_TEST:
        s->regs[R_ALERT_TEST] |= val32 & ALERT_MASK;
        ot_kmac_update_alert(s);
        break;
    case R_CFG_SHADOWED:
        if (!ot_kmac_check_reg_write(s, reg)) {
            break;
        }

        /* check for unimplemented config bits */
        if (val32 & R_CFG_SHADOWED_ENTROPY_MODE_MASK) {
            qemu_log_mask(
                LOG_UNIMP,
                "%s: %s: CFG_SHADOWED.ENTROPY_MODE is not supported\n",
                __func__, s->ot_id);
        }
        if (val32 & R_CFG_SHADOWED_ENTROPY_FAST_PROCESS_MASK) {
            qemu_log_mask(LOG_UNIMP,
                          "%s: %s: CFG_SHADOWED.ENTROPY_FAST_PROCESS is not "
                          "supported\n",
                          __func__, s->ot_id);
        }
        if (val32 & R_CFG_SHADOWED_MSG_MASK_MASK) {
            qemu_log_mask(LOG_UNIMP,
                          "%s: %s: CFG_SHADOWED.MSG_MASK is not supported\n",
                          __func__, s->ot_id);
        }
        if (val32 & R_CFG_SHADOWED_ENTROPY_READY_MASK) {
            qemu_log_mask(LOG_UNIMP,
                          "%s: %s: CFG_SHADOWED.ENTROPY_READY is not "
                          "supported\n",
                          __func__, s->ot_id);
        }
        if (val32 & R_CFG_SHADOWED_EN_UNSUPPORTED_MODESTRENGTH_MASK) {
            qemu_log_mask(LOG_UNIMP,
                          "%s: %s: CFG_SHADOWED.EN_UNSUPPORTED_MODESTRENGTH is "
                          "not supported\n",
                          __func__, s->ot_id);
        }

        val32 &= CFG_MASK;
        switch (ot_shadow_reg_write(&s->cfg, val32)) {
        case OT_SHADOW_REG_STAGED:
        case OT_SHADOW_REG_COMMITTED:
            break;
        case OT_SHADOW_REG_ERROR:
        default:
            s->regs[R_STATUS] |= R_STATUS_ALERT_RECOV_CTRL_UPDATE_ERR_MASK;
            ot_kmac_update_alert(s);
            break;
        }
        break;
    case R_CMD: {
        ot_kmac_process_sw_command(s, val32);

        if (val32 & R_CMD_ENTROPY_REQ_MASK) {
            /* TODO: implement entropy */
            qemu_log_mask(LOG_UNIMP,
                          "%s: %s: CMD.ENTROPY_REQ is not supported\n",
                          __func__, s->ot_id);
        }

        if (val32 & R_CMD_HASH_CNT_CLR_MASK) {
            /* TODO: implement entropy */
            qemu_log_mask(LOG_UNIMP,
                          "%s: %s: CMD.HASH_CNT_CLR is not supported\n",
                          __func__, s->ot_id);
        }
        break;
    }
    case R_ENTROPY_PERIOD:
        if (!ot_kmac_check_reg_write(s, reg)) {
            break;
        }

        val32 &= (R_ENTROPY_PERIOD_PRESCALER_MASK |
                  R_ENTROPY_PERIOD_WAIT_TIMER_MASK);
        s->regs[reg] = val32;
        break;
    case R_ENTROPY_REFRESH_THRESHOLD_SHADOWED:
        if (!ot_kmac_check_reg_write(s, reg)) {
            break;
        }

        val32 &= R_ENTROPY_REFRESH_THRESHOLD_SHADOWED_THRESHOLD_MASK;
        switch (ot_shadow_reg_write(&s->entropy_refresh_threshold, val32)) {
        case OT_SHADOW_REG_STAGED:
        case OT_SHADOW_REG_COMMITTED:
            break;
        case OT_SHADOW_REG_ERROR:
        default:
            s->regs[R_STATUS] |= R_STATUS_ALERT_RECOV_CTRL_UPDATE_ERR_MASK;
            ot_kmac_update_alert(s);
            break;
        }
        break;
    case R_ENTROPY_SEED:
        /* TODO: implement entropy */
        qemu_log_mask(LOG_UNIMP, "%s: %s: R_ENTROPY_SEED_* is not supported\n",
                      __func__, s->ot_id);
        break;
    case R_KEY_LEN:
        if (!ot_kmac_check_reg_write(s, reg)) {
            break;
        }
        val32 &= R_KEY_LEN_LEN_MASK;
        s->regs[reg] = val32;
        if (!ot_kmac_get_key_length(s)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: :%s invalid KEY_LEN=%d, using key length 0\n",
                          __func__, s->ot_id, val32);
        }
        break;
    case R_KEY_SHARE0_0:
    case R_KEY_SHARE0_1:
    case R_KEY_SHARE0_2:
    case R_KEY_SHARE0_3:
    case R_KEY_SHARE0_4:
    case R_KEY_SHARE0_5:
    case R_KEY_SHARE0_6:
    case R_KEY_SHARE0_7:
    case R_KEY_SHARE0_8:
    case R_KEY_SHARE0_9:
    case R_KEY_SHARE0_10:
    case R_KEY_SHARE0_11:
    case R_KEY_SHARE0_12:
    case R_KEY_SHARE0_13:
    case R_KEY_SHARE0_14:
    case R_KEY_SHARE0_15:
    case R_KEY_SHARE1_0:
    case R_KEY_SHARE1_1:
    case R_KEY_SHARE1_2:
    case R_KEY_SHARE1_3:
    case R_KEY_SHARE1_4:
    case R_KEY_SHARE1_5:
    case R_KEY_SHARE1_6:
    case R_KEY_SHARE1_7:
    case R_KEY_SHARE1_8:
    case R_KEY_SHARE1_9:
    case R_KEY_SHARE1_10:
    case R_KEY_SHARE1_11:
    case R_KEY_SHARE1_12:
    case R_KEY_SHARE1_13:
    case R_KEY_SHARE1_14:
    case R_KEY_SHARE1_15:
    case R_PREFIX_0:
    case R_PREFIX_1:
    case R_PREFIX_2:
    case R_PREFIX_3:
    case R_PREFIX_4:
    case R_PREFIX_5:
    case R_PREFIX_6:
    case R_PREFIX_7:
    case R_PREFIX_8:
    case R_PREFIX_9:
    case R_PREFIX_10:
        if (!ot_kmac_check_reg_write(s, reg)) {
            break;
        }
        s->regs[reg] = val32;
        break;
    case R_CFG_REGWEN:
    case R_STATUS:
    case R_ENTROPY_REFRESH_HASH_CNT:
    case R_ERR_CODE:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: R/O register 0x%02" HWADDR_PRIx " (%s)\n",
                      __func__, s->ot_id, addr, REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: bad offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, addr);
        break;
    }
}

static uint64_t ot_kmac_state_read(void *opaque, hwaddr addr, unsigned size)
{
    OtKMACState *s = OT_KMAC(opaque);
    uint32_t val32;

    if (s->state != KMAC_ST_ABSORBED) {
        /*
         * State is valid only after all absorbing process is completed.
         * Otherwise it will be zero to prevent information leakage.
         */
        if (!s->invalid_state_read) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: STATE read while in invalid FSM state\n",
                          __func__, s->ot_id);
            s->invalid_state_read = true;
        }
        val32 = 0;
    } else {
        uint32_t cfg = ot_shadow_reg_peek(&s->cfg);
        bool byteswap = FIELD_EX32(cfg, CFG_SHADOWED, STATE_ENDIANNESS) != 0;
        hwaddr offset = addr;
        int share = 0;

        /* reset invalid state marker */
        s->invalid_state_read = false;

        /* compute share index */
        while (offset > KECCAK_STATE_SHARE_BYTES) {
            offset -= KECCAK_STATE_SHARE_BYTES;
            share++;
        }

        switch (share) {
        case 0:
            if (addr + size <= KECCAK_STATE_BYTES) {
                val32 = 0;
                for (unsigned ix = 0; ix < size; ix++) {
                    size_t byte_offset = byteswap ? ix : size - 1 - ix;
                    val32 =
                        (val32 << 8u) + s->keccak_state[offset + byte_offset];
                }
            } else {
                val32 = 0;
            }
            break;
        case 1:
            /*
             * TODO: implement masking. Current version returns unmasked state
             * in first share and zeros in second one.
             */
            val32 = 0;
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: bad offset 0x%" HWADDR_PRIx "\n", __func__,
                          s->ot_id, addr);
            val32 = 0;
            break;
        }
    }

    uint32_t pc = ibex_get_current_pc();
    trace_ot_kmac_state_read_out(s->ot_id, (uint32_t)addr, val32, pc);

    return (uint64_t)val32;
}

static void ot_kmac_state_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    OtKMACState *s = OT_KMAC(opaque);
    (void)addr;
    (void)value;
    (void)size;
    /* on real hardware, writes to STATE are ignored */
    qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: STATE is read only\n", __func__,
                  s->ot_id);
}

static uint64_t ot_kmac_msgfifo_read(void *opaque, hwaddr addr, unsigned size)
{
    OtKMACState *s = OT_KMAC(opaque);
    (void)addr;
    (void)size;
    /* on real hardware, writes to FIFO will block. Let's just return 0. */
    qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: MSG_FIFO is write only\n", __func__,
                  s->ot_id);
    return 0;
}

static void ot_kmac_msgfifo_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    OtKMACState *s = OT_KMAC(opaque);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_kmac_msgfifo_write(s->ot_id, (uint32_t)addr, (uint32_t)value, size,
                                pc);

    /* trigger error if an app is running or not in MSG_FEED state */
    if (s->current_app || s->state != KMAC_ST_MSG_FEED) {
        /* info field mux_sel=1 (SW) or 2 (App) */
        ot_kmac_report_error(s, OT_KMAC_ERR_SW_PUSHED_MSG_FIFO,
                             s->current_app ? 2 : 1);
        return;
    }

    uint32_t cfg = ot_shadow_reg_peek(&s->cfg);
    bool byteswap = FIELD_EX32(cfg, CFG_SHADOWED, MSG_ENDIANNESS) != 0;

    if (fifo8_num_free(&s->input_fifo) < size) {
        /*
         * Not enough room in FIFO. Real hardware would fill the FIFO and stall
         * but it cannot be done in QEMU so instead we artificially process data
         * now to empty the FIFO.
         */
        ot_kmac_process(s);
    }

    for (unsigned ix = 0; ix < size; ix++) {
        size_t byteoffset = byteswap ? (size - 1u - ix) : ix;
        uint8_t b = (uint8_t)(value >> (byteoffset * 8u));
        fifo8_push(&s->input_fifo, b);
    }

    /* trigger delayed processing of FIFO */
    ot_kmac_trigger_deferred_bh(s);
}

static bool ot_kmac_compare_app_cfg(const OtKMACAppCfg *cfg1,
                                    const OtKMACAppCfg *cfg2)
{
    return cfg1->mode == cfg2->mode && cfg1->strength == cfg2->strength &&
           cfg1->prefix.funcname_len == cfg2->prefix.funcname_len &&
           cfg1->prefix.customstr_len == cfg2->prefix.customstr_len &&
           memcmp(cfg1->prefix.funcname, cfg2->prefix.funcname,
                  cfg1->prefix.funcname_len) == 0 &&
           memcmp(cfg1->prefix.customstr, cfg2->prefix.customstr,
                  cfg1->prefix.customstr_len) == 0;
}

static void ot_kmac_connect_app(OtKMACState *s, unsigned app_idx,
                                const OtKMACAppCfg *cfg, OtKmacResponse fn,
                                void *opaque)
{
    g_assert(app_idx < s->num_app);

    OtKMACApp *app = &s->apps[app_idx];

    if (app->connected) {
        if (ot_kmac_compare_app_cfg(&app->cfg, cfg) && fn == app->fn &&
            opaque == app->opaque) {
            /*
             * silently ignore duplicate connection from the same component with
             * the same parameters.
             */
            return;
        }
        qemu_log_mask(
            LOG_GUEST_ERROR,
            "%s: %s: ignoring connection to already used app index %u\n",
            __func__, s->ot_id, app_idx);
        return;
    }

    app->index = app_idx;
    app->cfg = *cfg;
    if (!ot_kmac_check_mode_and_strength(&app->cfg)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Invalid mode/strength for app index %u\n",
                      __func__, s->ot_id, app_idx);
        /* force dummy values, digest will be wrong */
        app->cfg.mode = OT_KMAC_MODE_CSHAKE;
        app->cfg.strength = 128u;
    }
    if (app->cfg.mode == OT_KMAC_MODE_KMAC) {
        if (memcmp(app->cfg.prefix.funcname, "KMAC", 4u) != 0 ||
            app->cfg.prefix.funcname_len != 4u) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: invalid config for app index %u: "
                          "invalid prefix for KMAC\n",
                          __func__, s->ot_id, app_idx);
        }
    }
    app->fn = fn;
    app->opaque = opaque;
    app->connected = true;
}

static void ot_kmac_start_pending_app(OtKMACState *s)
{
    if (s->state == KMAC_ST_IDLE && s->pending_apps) {
        /* select pending app */
        uint8_t app_idx = ctz32(s->pending_apps);
        g_assert(app_idx < s->num_app);
        s->current_app = &s->apps[app_idx];
        s->pending_apps &= ~(1u << app_idx);

        /* process start */
        trace_ot_kmac_app_start(s->ot_id, app_idx);
        s->current_cfg = &s->current_app->cfg;
        ot_kmac_process_start(s);
        ot_kmac_change_fsm_state(s, KMAC_ST_MSG_FEED);

        /* trigger deferred compute */
        qemu_bh_schedule(s->bh);
    }
}

static void ot_kmac_app_request(OtKMACState *s, unsigned app_idx,
                                const OtKMACAppReq *req)
{
    g_assert(app_idx < s->num_app);
    g_assert(req);
    g_assert(req->msg_len <= OT_KMAC_APP_MSG_BYTES);

    if (trace_event_get_state(TRACE_OT_KMAC_APP_REQUEST)) {
        trace_ot_kmac_app_request(s->ot_id, app_idx, req->last, req->msg_len,
                                  ot_common_lhexdump(req->msg_data,
                                                     req->msg_len, false,
                                                     s->hexstr,
                                                     OT_KMAC_KEY_HEXSTR_SIZE));
    }

    OtKMACApp *app = &s->apps[app_idx];

    if (app->req_pending) {
        error_setg(&error_fatal,
                   "%s: %s: dropping request to already busy app index %u",
                   __func__, s->ot_id, app_idx);
    }

    /* save request */
    app->req = *req;
    app->req_pending = true;

    /* check if app already started */
    if (s->current_app == app &&
        (s->state == KMAC_ST_IDLE || s->state == KMAC_ST_MSG_FEED)) {
        /* yes, receiving more data, trigger deferred compute */
        qemu_bh_schedule(s->bh);
    } else {
        /* no, mark as pending and try to start */
        s->pending_apps |= (1u << app_idx);
        ot_kmac_start_pending_app(s);
    }
}

static Property ot_kmac_properties[] = {
    DEFINE_PROP_STRING(OT_COMMON_DEV_ID, OtKMACState, ot_id),
    DEFINE_PROP_STRING("clock-name", OtKMACState, clock_name),
    DEFINE_PROP_LINK("clock-src", OtKMACState, clock_src, TYPE_DEVICE,
                     DeviceState *),
    DEFINE_PROP_LINK("edn", OtKMACState, edn, TYPE_OT_EDN, OtEDNState *),
    DEFINE_PROP_UINT8("edn-ep", OtKMACState, edn_ep, UINT8_MAX),
    DEFINE_PROP_UINT8("num-app", OtKMACState, num_app, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_kmac_regs_ops = {
    .read = &ot_kmac_regs_read,
    .write = &ot_kmac_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4u,
        .max_access_size = 4u,
    },
};

static const MemoryRegionOps ot_kmac_state_ops = {
    .read = &ot_kmac_state_read,
    .write = &ot_kmac_state_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1u,
        .max_access_size = 4u,
    },
};

static const MemoryRegionOps ot_kmac_msgfifo_ops = {
    .read = &ot_kmac_msgfifo_read,
    .write = &ot_kmac_msgfifo_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1u,
        .max_access_size = 4u,
    },
};

static void ot_kmac_reset_enter(Object *obj, ResetType type)
{
    OtKMACClass *c = OT_KMAC_GET_CLASS(obj);
    OtKMACState *s = OT_KMAC(obj);

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    ot_kmac_cancel_bh(s);

    ot_kmac_change_fsm_state(s, KMAC_ST_IDLE);
    ot_kmac_reset_state(s);
    memset(&s->sw_cfg, 0, sizeof(OtKMACAppCfg));
    s->current_app = NULL;
    s->pending_apps = 0;
    s->invalid_state_read = false;
    s->error_awaiting_sw = false;
    memset(s->regs, 0, sizeof(*(s->regs)));
    s->regs[R_STATUS] = 0x4001u;
    ot_shadow_reg_init(&s->cfg, 0u);
    ot_shadow_reg_init(&s->entropy_refresh_threshold, 0u);

    ot_kmac_update_irq(s);
    ot_kmac_update_alert(s);

    fifo8_reset(&s->input_fifo);

    memset(s->sl_key, 0, sizeof(OtKMACKey));

    if (!s->clock_src_name) {
        IbexClockSrcIfClass *ic = IBEX_CLOCK_SRC_IF_GET_CLASS(s->clock_src);
        IbexClockSrcIf *ii = IBEX_CLOCK_SRC_IF(s->clock_src);

        s->clock_src_name =
            ic->get_clock_source(ii, s->clock_name, DEVICE(s), &error_fatal);
        qemu_irq in_irq =
            qdev_get_gpio_in_named(DEVICE(s), OT_KMAC_CLOCK_INPUT, 0);
        qdev_connect_gpio_out_named(s->clock_src, s->clock_src_name, 0, in_irq);

        if (object_dynamic_cast(OBJECT(s->clock_src), TYPE_OT_CLKMGR)) {
            char *hint_name =
                g_strdup_printf(OT_CLOCK_HINT_PREFIX "%s", s->clock_name);
            qemu_irq hint_irq =
                qdev_get_gpio_in_named(s->clock_src, hint_name, 0);
            g_assert(hint_irq);
            qdev_connect_gpio_out_named(DEVICE(s), OT_KMAC_CLOCK_ACTIVE, 0,
                                        hint_irq);
            g_free(hint_name);
        }
    }
}

static void ot_kmac_realize(DeviceState *dev, Error **errp)
{
    OtKMACState *s = OT_KMAC(dev);
    (void)errp;

    g_assert(s->ot_id);
    g_assert(s->clock_name);
    g_assert(s->clock_src);

    /* make sure num-app property is set */
    g_assert(s->num_app > 0);

    /* make sure we don't overflow pending_apps bitmask */
    g_assert(s->num_app <= 32);

    s->apps = g_new0(OtKMACApp, s->num_app);

    qdev_init_gpio_in_named(DEVICE(s), &ot_kmac_clock_input,
                            OT_KMAC_CLOCK_INPUT, 1);
}

static void ot_kmac_init(Object *obj)
{
    OtKMACState *s = OT_KMAC(obj);

    s->regs = g_new0(uint32_t, REGS_COUNT);

    for (unsigned ix = 0; ix < ARRAY_SIZE(s->irqs); ix++) {
        ibex_sysbus_init_irq(obj, &s->irqs[ix]);
    }
    for (unsigned ix = 0; ix < ARRAY_SIZE(s->alerts); ix++) {
        ibex_qdev_init_irq(obj, &s->alerts[ix], OT_DEVICE_ALERT);
    }

    ibex_qdev_init_irq(obj, &s->clock_active, OT_KMAC_CLOCK_ACTIVE);

    memory_region_init(&s->mmio, OBJECT(s), TYPE_OT_KMAC, OT_KMAC_WHOLE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    memory_region_init_io(&s->regs_mmio, obj, &ot_kmac_regs_ops, s,
                          TYPE_OT_KMAC ".regs", REGS_SIZE);
    memory_region_add_subregion(&s->mmio, OT_KMAC_REGS_BASE, &s->regs_mmio);

    memory_region_init_io(&s->state_mmio, obj, &ot_kmac_state_ops, s,
                          TYPE_OT_KMAC ".state", OT_KMAC_STATE_SIZE);
    memory_region_add_subregion(&s->mmio, OT_KMAC_STATE_BASE, &s->state_mmio);

    memory_region_init_io(&s->msgfifo_mmio, obj, &ot_kmac_msgfifo_ops, s,
                          TYPE_OT_KMAC ".msgfifo", OT_KMAC_MSG_FIFO_SIZE);
    memory_region_add_subregion(&s->mmio, OT_KMAC_MSG_FIFO_BASE,
                                &s->msgfifo_mmio);

    /* setup deferred processing */
    s->bh_timer = timer_new_ns(OT_VIRTUAL_CLOCK, &ot_kmac_bh_timer_handler, s);
    s->bh = qemu_bh_new(&ot_kmac_process, s);

    /* FIFO sizes as per OT Spec */
    fifo8_create(&s->input_fifo, FIFO_LENGTH);

    s->sl_key = g_new0(OtKMACKey, 1u);

    s->hexstr = g_new0(char, OT_KMAC_KEY_HEXSTR_SIZE);
}

static void ot_kmac_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->realize = &ot_kmac_realize;
    device_class_set_props(dc, ot_kmac_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    ResettableClass *rc = RESETTABLE_CLASS(klass);
    OtKMACClass *kc = OT_KMAC_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_kmac_reset_enter, NULL, NULL,
                                       &kc->parent_phases);

    kc->connect_app = &ot_kmac_connect_app;
    kc->app_request = &ot_kmac_app_request;

    OtKeySinkIfClass *sc = OT_KEY_SINK_IF_CLASS(klass);
    sc->push_key = &ot_kmac_push_key;
}

static const TypeInfo ot_kmac_info = {
    .name = TYPE_OT_KMAC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtKMACState),
    .instance_init = &ot_kmac_init,
    .class_size = sizeof(OtKMACClass),
    .class_init = &ot_kmac_class_init,
    .interfaces =
        (InterfaceInfo[]){
            { TYPE_OT_KEY_SINK_IF },
            {},
        },
};

static void ot_kmac_register_types(void)
{
    type_register_static(&ot_kmac_info);
}

type_init(ot_kmac_register_types);
