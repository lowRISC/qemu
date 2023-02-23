/*
 * QEMU OpenTitan KMAC device
 *
 * Copyright (c) 2023 Rivos, Inc.
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
 * Note: This implementation is missing some features (Side-loading, Application
 * Interface and Masking)
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/fifo8.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_edn.h"
#include "hw/opentitan/ot_kmac.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "tomcrypt.h"
#include "trace.h"


/* Input FIFO length is 80 bytes (10 x 64 bits) */
#define FIFO_LENGTH 80u

/* Delay FIFO ingestion and compute by 100ns */
#define DEFERRED_TRIGGER_DELAY_NS 100u

/* Number of output IRQ lines */
#define PARAM_NUM_IRQS 3u

/* Number of output Alert lines */
#define PARAM_NUM_ALERTS 2u

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

/* Max size of cSHAKE function name */
#define MAX_FUNCNAME_LEN 32u

/* Max size of cSHAKE customization string */
#define MAX_CUSTOM_LEN 32u

/* function prefix for KMAC operations */
#define KMAC_PREFIX     "KMAC"
#define KMAC_PREFIX_LEN 4u

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
    FIELD(CFG_SHADOWED, ERR_PROCESSED, 25u, 1u)
    FIELD(CFG_SHADOWED, EN_UNSUPPORTED_MODESTRENGTH, 26u, 1u)
REG32(CMD, 0x18u)
    FIELD(CMD, CMD, 0u, 6u)
    FIELD(CMD, ENTROPY_REQ, 8u, 1u)
    FIELD(CMD, HASH_CNT_CLR, 9u, 1u)
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
REG32(ENTROPY_SEED_0, 0x2cu)
REG32(ENTROPY_SEED_1, 0x30u)
REG32(ENTROPY_SEED_2, 0x34u)
REG32(ENTROPY_SEED_3, 0x38u)
REG32(ENTROPY_SEED_4, 0x3cu)
REG32(KEY_SHARE0_0, 0x40u)
REG32(KEY_SHARE0_1, 0x44u)
REG32(KEY_SHARE0_2, 0x48u)
REG32(KEY_SHARE0_3, 0x4cu)
REG32(KEY_SHARE0_4, 0x50u)
REG32(KEY_SHARE0_5, 0x54u)
REG32(KEY_SHARE0_6, 0x58u)
REG32(KEY_SHARE0_7, 0x5cu)
REG32(KEY_SHARE0_8, 0x60u)
REG32(KEY_SHARE0_9, 0x64u)
REG32(KEY_SHARE0_10, 0x68u)
REG32(KEY_SHARE0_11, 0x6cu)
REG32(KEY_SHARE0_12, 0x70u)
REG32(KEY_SHARE0_13, 0x74u)
REG32(KEY_SHARE0_14, 0x78u)
REG32(KEY_SHARE0_15, 0x7cu)
REG32(KEY_SHARE1_0, 0x80u)
REG32(KEY_SHARE1_1, 0x84u)
REG32(KEY_SHARE1_2, 0x88u)
REG32(KEY_SHARE1_3, 0x8cu)
REG32(KEY_SHARE1_4, 0x90u)
REG32(KEY_SHARE1_5, 0x94u)
REG32(KEY_SHARE1_6, 0x98u)
REG32(KEY_SHARE1_7, 0x9cu)
REG32(KEY_SHARE1_8, 0xa0u)
REG32(KEY_SHARE1_9, 0xa4u)
REG32(KEY_SHARE1_10, 0xa8u)
REG32(KEY_SHARE1_11, 0xacu)
REG32(KEY_SHARE1_12, 0xb0u)
REG32(KEY_SHARE1_13, 0xb4u)
REG32(KEY_SHARE1_14, 0xb8u)
REG32(KEY_SHARE1_15, 0xbcu)
REG32(KEY_LEN, 0xc0u)
    FIELD(KEY_LEN, LEN, 0u, 3u)
REG32(PREFIX_0, 0xc4u)
REG32(PREFIX_1, 0xc8u)
REG32(PREFIX_2, 0xccu)
REG32(PREFIX_3, 0xd0u)
REG32(PREFIX_4, 0xd4u)
REG32(PREFIX_5, 0xd8u)
REG32(PREFIX_6, 0xdcu)
REG32(PREFIX_7, 0xe0u)
REG32(PREFIX_8, 0xe4u)
REG32(PREFIX_9, 0xe8u)
REG32(PREFIX_10, 0xecu)
REG32(ERR_CODE, 0xf0u)
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
     R_CFG_SHADOWED_ENTROPY_READY_MASK | R_CFG_SHADOWED_ERR_PROCESSED_MASK | \
     R_CFG_SHADOWED_EN_UNSUPPORTED_MODESTRENGTH_MASK)

#define OT_KMAC_CMD_NONE       0
#define OT_KMAC_CMD_START      0x1du
#define OT_KMAC_CMD_PROCESS    0x2eu
#define OT_KMAC_CMD_MANUAL_RUN 0x31u
#define OT_KMAC_CMD_DONE       0x16u

#define OT_KMAC_ERR_NONE                             0
#define OT_KMAC_ERR_KEY_NOT_VALID                    0x01u
#define OT_KMAC_ERR_SW_PUSHED_MSG_FIFO               0x02u
#define OT_KMAC_ERR_SW_ISSUED_CMD_IN_APP_ACTIVE      0x03u
#define OT_KMAC_ERR_WAIT_TIMER_EXPIRED               0x04u
#define OT_KMAC_ERR_INCORRECT_ENTROPY_MODE           0x05u
#define OT_KMAC_ERR_UNEXPECTED_MODE_STRENGTH         0x06u
#define OT_KMAC_ERR_INCORRECT_FUNCTION_NAME          0x07u
#define OT_KMAC_ERR_SW_CMD_SEQUENCE                  0x08u
#define OT_KMAC_ERR_SW_HASHING_WITHOUT_ENTROPY_READY 0x09u
#define OT_KMAC_ERR_SHADOW_REG_UPDATE                0xc0u
#define OT_KMAC_ERR_FATAL_ERROR                      0xc1u
#define OT_KMAC_ERR_PACKER_INTEGRITY                 0xc2u
#define OT_KMAC_ERR_MSG_FIFO_INTEGRITY               0xc3u

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
    REG_NAME_ENTRY(ENTROPY_SEED_0),
    REG_NAME_ENTRY(ENTROPY_SEED_1),
    REG_NAME_ENTRY(ENTROPY_SEED_2),
    REG_NAME_ENTRY(ENTROPY_SEED_3),
    REG_NAME_ENTRY(ENTROPY_SEED_4),
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
    /* ABSORBED: ? */
    KMAC_ST_ABSORBED = 3,
    /* SQUEEZING: ? */
    KMAC_ST_SQUEEZING = 4,
    /* illegal state reached and hang */
    KMAC_ST_TERMINAL_ERROR = 5,
} OtKMACFsmState;

enum OtKMACMode {
    KMAC_NONE,
    KMAC_SHA3,
    KMAC_SHAKE,
    KMAC_CSHAKE,
};

struct OtKMACPrefix {
    uint8_t funcname[MAX_FUNCNAME_LEN];
    size_t funcname_len;
    uint8_t custom[MAX_CUSTOM_LEN];
    size_t custom_len;
};

struct OtKMACState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    MemoryRegion regs_mmio;
    MemoryRegion state_mmio;
    MemoryRegion msgfifo_mmio;
    IbexIRQ irqs[PARAM_NUM_IRQS];
    IbexIRQ alerts[PARAM_NUM_ALERTS];

    uint32_t *regs;
    OtShadowReg cfg;
    OtShadowReg entropy_refresh_threshold;

    OtKMACFsmState state; /* Main FSM state */
    bool invalid_state_read;
    hash_state ltc_state; /* TomCrypt hash state */
    uint8_t keccak_state[KECCAK_STATE_BYTES];

    Fifo8 input_fifo;
    QEMUTimer *deferred_trigger;

    OtEDNState *edn;
    uint8_t edn_ep;
};

static void ot_kmac_trigger_deferred_processing(OtKMACState *s)
{
    timer_del(s->deferred_trigger);
    timer_mod(s->deferred_trigger, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                       DEFERRED_TRIGGER_DELAY_NS);
}

static void ot_kmac_update_irq(OtKMACState *s)
{
    uint32_t level = s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE];
    for (unsigned ix = 0; ix < PARAM_NUM_IRQS; ix++) {
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

    for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
        ibex_irq_set(&s->alerts[ix], (int)((level >> ix) & 0x1u));
    }
}

static void ot_kmac_report_error(OtKMACState *s, uint8_t code, uint32_t info)
{
    trace_ot_kmac_report_error(code, info);

    uint32_t error = 0;
    error = FIELD_DP32(error, ERR_CODE, CODE, code);
    error = FIELD_DP32(error, ERR_CODE, INFO, info);

    s->regs[R_ERR_CODE] = error;
    s->regs[R_INTR_STATE] |= INTR_KMAC_ERR_MASK;
    ot_kmac_update_irq(s);
}

static inline enum OtKMACMode ot_kmac_get_mode(uint32_t cfg)
{
    switch (FIELD_EX32(cfg, CFG_SHADOWED, MODE)) {
    case 0x00:
        return KMAC_SHA3;
    case 0x02:
        return KMAC_SHAKE;
    case 0x03:
        return KMAC_CSHAKE;
    default:
        /* invalid modes are checked when processing START command */
        return KMAC_NONE;
    }
};

static inline size_t ot_kmac_get_key_strength(uint32_t cfg)
{
    switch (FIELD_EX32(cfg, CFG_SHADOWED, KSTRENGTH)) {
    case 0x00:
        return 128u;
    case 0x01:
        return 224u;
    case 0x02:
        return 256u;
    case 0x03:
        return 384u;
    case 0x04:
        return 512u;
    default:
        /* invalid key strength are checked when processing START command */
        return 0;
    }
};

static inline size_t ot_kmac_get_key_length(OtKMACState *s)
{
    uint32_t key_len = FIELD_EX32(s->regs[R_KEY_LEN], KEY_LEN, LEN);
    switch (key_len) {
    case 0x00:
        return 128u;
    case 0x01:
        return 192u;
    case 0x02:
        return 256u;
    case 0x03:
        return 384u;
    case 0x04:
        return 512u;
    default:
        /* invalid key length values are traced at register write */
        return 0;
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

static void ot_kmac_process(void *opaque)
{
    OtKMACState *s = opaque;

    /* process FIFO data */
    if (!fifo8_is_empty(&s->input_fifo)) {
        while (!fifo8_is_empty(&s->input_fifo)) {
            uint8_t value = fifo8_pop(&s->input_fifo);
            sha3_process(&s->ltc_state, &value, 1);
        }

        /* assert FIFO Empty interrupt */
        s->regs[R_INTR_STATE] |= INTR_FIFO_EMPTY_MASK;
    }

    switch (s->state) {
    case KMAC_ST_PROCESSING:
    case KMAC_ST_SQUEEZING: {
        uint32_t cfg = ot_shadow_reg_peek(&s->cfg);
        enum OtKMACMode mode = ot_kmac_get_mode(cfg);
        size_t kstrength = ot_kmac_get_key_strength(cfg);

        switch (mode) {
        case KMAC_SHA3:
            sha3_done(&s->ltc_state, &s->keccak_state[0]);
            break;
        case KMAC_SHAKE:
            sha3_shake_done(&s->ltc_state, &s->keccak_state[0],
                            ot_kmac_get_keccak_rate_bytes(kstrength));
            break;
        case KMAC_CSHAKE:
            sha3_cshake_done(&s->ltc_state, &s->keccak_state[0],
                             ot_kmac_get_keccak_rate_bytes(kstrength));
            break;
        default:
            /* should never happen: mode was validated when going from state IDLE
             * to START */
            g_assert_not_reached();
        }

        s->state = KMAC_ST_ABSORBED;

        /* assert KMAC Done interrupt */
        s->regs[R_INTR_STATE] |= INTR_KMAC_DONE_MASK;

        break;
    }
    default:
        /* nothing to do for other states */
        break;
    }

    ot_kmac_update_irq(s);
}

static inline bool ot_kmac_config_enabled(OtKMACState *s)
{
    /* configuration is enabled only in idle mode */
    return s->state == KMAC_ST_IDLE;
}

static inline bool ot_kmac_check_reg_write(OtKMACState *s, hwaddr reg)
{
    if (!ot_kmac_config_enabled(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Write to %s ignored while busy\n",
                      __func__, REG_NAME(reg));
        return false;
    }

    return true;
}

static bool ot_kmac_check_mode_and_strength(uint32_t cfg)
{
    enum OtKMACMode mode = ot_kmac_get_mode(cfg);
    size_t kstrength = ot_kmac_get_key_strength(cfg);

    switch (mode) {
    case KMAC_SHA3:
        switch (kstrength) {
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
    case KMAC_SHAKE:
    case KMAC_CSHAKE:
        switch (kstrength) {
        case 128u:
        case 256u:
            return true;
        default:
            /* unsupported strength for SHAKE/cSHAKE */
            return false;
        }
        break;
    default:
        /* unsupported mode */
        return false;
    }
}

static inline uint8_t ot_kmac_get_prefix_byte(OtKMACState *s, size_t offset)
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

static size_t ot_kmac_left_decode(OtKMACState *s, size_t offset, size_t *value)
{
    size_t len, idx;
    size_t val = 0;

    /* first byte is the length in bytes of encoded value */
    len = (size_t)ot_kmac_get_prefix_byte(s, offset);
    if (len < 1u || len > 4u) {
        return 0;
    }

    /* compute value */
    for (idx = 1u; idx < len + 1u; idx++) {
        val = (val << 8u) | ot_kmac_get_prefix_byte(s, offset + idx);
    }

    *value = val;
    return 1u + len;
}

static bool ot_kmac_decode_prefix(OtKMACState *s, struct OtKMACPrefix *prefix)
{
    size_t offset = 0;
    size_t used, idx;

    used = ot_kmac_left_decode(s, offset, &prefix->funcname_len);
    prefix->funcname_len /= 8u;
    offset += used;

    if (prefix->funcname_len > MAX_FUNCNAME_LEN) {
        goto error;
    }
    for (idx = 0; idx < prefix->funcname_len; idx++) {
        prefix->funcname[idx] = ot_kmac_get_prefix_byte(s, offset + idx);
    }
    offset += prefix->funcname_len;

    used = ot_kmac_left_decode(s, offset, &prefix->custom_len);
    prefix->custom_len /= 8u;
    offset += used;

    if (prefix->custom_len > MAX_CUSTOM_LEN) {
        goto error;
    }
    for (idx = 0; idx < prefix->custom_len; idx++) {
        prefix->custom[idx] = ot_kmac_get_prefix_byte(s, offset + idx);
    }
    offset += prefix->funcname_len;

    if (offset <= NUM_PREFIX_REGS * sizeof(uint32_t)) {
        return true;
    }

error:
    memset(prefix, 0, sizeof(struct OtKMACPrefix));
    return false;
}

static void ot_kmac_get_key(OtKMACState *s, uint8_t *key, size_t keylen)
{
    for (size_t idx = 0; idx < keylen && idx < NUM_KEY_REGS * 4u; idx++) {
        uint8_t reg = idx >> 2u;
        uint8_t byteoffset = idx & 3u;

        uint8_t share0 =
            (uint8_t)(s->regs[R_KEY_SHARE0_0 + reg] >> (byteoffset * 8u));
        uint8_t share1 =
            (uint8_t)(s->regs[R_KEY_SHARE1_0 + reg] >> (byteoffset * 8u));
        key[idx] = share0 ^ share1;
    }
}

static bool ot_kmac_check_kmac_prefix(struct OtKMACPrefix *prefix)
{
    return prefix->funcname_len == KMAC_PREFIX_LEN &&
           !memcmp(prefix->funcname, KMAC_PREFIX, KMAC_PREFIX_LEN);
}

static void ot_kmac_command_start(OtKMACState *s, struct OtKMACPrefix *prefix)
{
    uint32_t cfg = ot_shadow_reg_peek(&s->cfg);
    enum OtKMACMode mode = ot_kmac_get_mode(cfg);
    size_t kstrength = ot_kmac_get_key_strength(cfg);

    switch (mode) {
    case KMAC_SHA3:
        switch (kstrength) {
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
            /* should never happen: strength was already validated at this point */
            g_assert_not_reached();
        }
        break;
    case KMAC_SHAKE:
        switch (kstrength) {
        case 128u:
        case 256u:
            sha3_shake_init(&s->ltc_state, kstrength);
            break;
        default:
            /* should never happen: strength was already validated at this point */
            g_assert_not_reached();
        }
        break;
    case KMAC_CSHAKE:
        switch (kstrength) {
        case 128u:
        case 256u: {
            bool kmac_en = FIELD_EX32(cfg, CFG_SHADOWED, KMAC_EN) != 0;
            sha3_cshake_init(&s->ltc_state, kstrength, prefix->funcname,
                             prefix->funcname_len, prefix->custom,
                             prefix->custom_len);
            if (kmac_en) {
                uint8_t key[NUM_KEY_REGS * sizeof(uint32_t)];
                size_t keylen = ot_kmac_get_key_length(s) / 8u;
                ot_kmac_get_key(s, key, keylen);
                sha3_process_kmac_key(&s->ltc_state, key, keylen);
            }
            break;
        }
        default:
            /* should never happen: strength was already validated at this point */
            g_assert_not_reached();
        }
        break;
    default:
        /* should never happen: mode was already validated at this point */
        g_assert_not_reached();
    }
}

static void ot_kmac_process_sw_command(OtKMACState *s, uint32_t cmd)
{
    uint32_t cfg = ot_shadow_reg_peek(&s->cfg);
    bool err_swsequence = false;
    bool err_modestrength = false;
    bool err_prefix = false;
    bool err_entropy_ready = false;

    switch (s->state) {
    case KMAC_ST_IDLE:
        if (cmd == OT_KMAC_CMD_NONE) {
            /* nothing to do */
        } else if (cmd == OT_KMAC_CMD_START) {
            struct OtKMACPrefix prefix;
            if (!ot_kmac_check_mode_and_strength(cfg)) {
                err_modestrength = true;
                break;
            }
            /* decode prefix from registers */
            if (!ot_kmac_decode_prefix(s, &prefix)) {
                err_prefix = true;
                break;
            }
            /* if KMAC mode, check prefix & entropy ready */
            if (FIELD_EX32(cfg, CFG_SHADOWED, KMAC_EN)) {
                if (!ot_kmac_check_kmac_prefix(&prefix)) {
                    err_prefix = true;
                    break;
                }
                if (false /* TODO: check entropy ready */) {
                    err_entropy_ready = true;
                    break;
                }
            }
            ot_kmac_command_start(s, &prefix);
            s->state = KMAC_ST_MSG_FEED;
        } else {
            err_swsequence = true;
        }
        break;
    case KMAC_ST_MSG_FEED:
        if (cmd == OT_KMAC_CMD_NONE) {
            /* nothing to do */
        } else if (cmd == OT_KMAC_CMD_PROCESS) {
            s->state = KMAC_ST_PROCESSING;
            ot_kmac_trigger_deferred_processing(s);
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
            s->state = KMAC_ST_SQUEEZING;
            ot_kmac_trigger_deferred_processing(s);
        } else if (cmd == OT_KMAC_CMD_DONE) {
            /* flush state */
            s->state = KMAC_ST_IDLE;
            memset(s->keccak_state, 0, sizeof(s->keccak_state));
        } else {
            err_swsequence = true;
        }
        break;
    case KMAC_ST_TERMINAL_ERROR:
    default:
        s->state = KMAC_ST_TERMINAL_ERROR;
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
    } else {
        s->regs[R_ERR_CODE] = 0;
    }
}

static uint64_t ot_kmac_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtKMACState *s = OT_KMAC(opaque);
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
    case R_ENTROPY_SEED_0:
    case R_ENTROPY_SEED_1:
    case R_ENTROPY_SEED_2:
    case R_ENTROPY_SEED_3:
    case R_ENTROPY_SEED_4:
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

    uint64_t pc = ibex_get_current_pc();
    trace_ot_kmac_io_read_out((unsigned)addr, REG_NAME(reg), val32, pc);

    return (uint64_t)val32;
}

static void ot_kmac_regs_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    OtKMACState *s = OT_KMAC(opaque);
    uint32_t val32 = (uint32_t)value;

    hwaddr reg = R32_OFF(addr);

    uint64_t pc = ibex_get_current_pc();
    trace_ot_kmac_io_write((unsigned)addr, REG_NAME(reg), val32, pc);

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
        uint32_t cmd = FIELD_EX32(val32, CMD, CMD);

        ot_kmac_process_sw_command(s, cmd);

        if (val32 & R_CMD_ENTROPY_REQ_MASK) {
            /* TODO: implement entropy */
            qemu_log_mask(LOG_UNIMP, "%s: CMD.ENTROPY_REQ is not supported\n",
                          __func__);
        }

        if (val32 & R_CMD_HASH_CNT_CLR_MASK) {
            /* TODO: implement entropy */
            qemu_log_mask(LOG_UNIMP, "%s: CMD.HASH_CNT_CLR is not supported\n",
                          __func__);
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
    case R_ENTROPY_SEED_0:
    case R_ENTROPY_SEED_1:
    case R_ENTROPY_SEED_2:
    case R_ENTROPY_SEED_3:
    case R_ENTROPY_SEED_4:
        /* TODO: implement entropy */
        qemu_log_mask(LOG_UNIMP, "%s: R_ENTROPY_SEED_* is not supported\n",
                      __func__);
        break;
    case R_KEY_LEN:
        if (!ot_kmac_check_reg_write(s, reg)) {
            break;
        }
        val32 &= R_KEY_LEN_LEN_MASK;
        s->regs[reg] = val32;
        if (!ot_kmac_get_key_length(s)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: Invalid KEY_LEN=%d, using key length 0\n",
                          __func__, val32);
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
                      "%s: R/O register 0x02%" HWADDR_PRIx " (%s)\n", __func__,
                      addr, REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
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
                          "%s: STATE read while in invalid FSM state\n",
                          __func__);
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
                for (unsigned idx = 0; idx < size; idx++) {
                    size_t byte_offset = byteswap ? idx : size - 1 - idx;
                    val32 =
                        (val32 << 8u) + s->keccak_state[offset + byte_offset];
                }
            } else {
                val32 = 0;
            }
            break;
        case 1:
            /*
             * TODO: implement masking. Current version returns unmasked state in
             * first share and zeros in second one.
             */
            val32 = 0;
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__,
                          addr);
            val32 = 0;
            break;
        }
    }

    uint64_t pc = ibex_get_current_pc();
    trace_ot_kmac_state_read_out((unsigned)addr, val32, pc);

    return (uint64_t)val32;
}

static void ot_kmac_state_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    /* on real hardware, writes to STATE are ignored */
    qemu_log_mask(LOG_GUEST_ERROR, "%s: STATE is read only\n", __func__);
}

static uint64_t ot_kmac_msgfifo_read(void *opaque, hwaddr addr, unsigned size)
{
    /* on real hardware, writes to FIFO will block. Let's just return 0. */
    qemu_log_mask(LOG_GUEST_ERROR, "%s: MSG_FIFO is write only\n", __func__);
    return 0;
}

static void ot_kmac_msgfifo_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    OtKMACState *s = OT_KMAC(opaque);

    uint64_t pc = ibex_get_current_pc();
    trace_ot_kmac_msgfifo_write((unsigned)addr, (uint32_t)value, size, pc);

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

    for (unsigned idx = 0; idx < size; idx++) {
        size_t byteoffset = byteswap ? (size - 1u - idx) : idx;
        uint8_t b = (uint8_t)(value >> (byteoffset * 8u));
        fifo8_push(&s->input_fifo, b);
    }

    /* trigger delayed processing of FIFO */
    ot_kmac_trigger_deferred_processing(s);
}

static Property ot_kmac_properties[] = {
    DEFINE_PROP_LINK("edn", OtKMACState, edn, TYPE_OT_EDN, OtEDNState *),
    DEFINE_PROP_UINT8("edn-ep", OtKMACState, edn_ep, UINT8_MAX),
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

static void ot_kmac_reset(DeviceState *dev)
{
    OtKMACState *s = OT_KMAC(dev);

    timer_del(s->deferred_trigger);

    s->state = KMAC_ST_IDLE;
    s->invalid_state_read = false;
    memset(s->keccak_state, 0, sizeof(s->keccak_state));
    memset(&s->ltc_state, 0, sizeof(s->ltc_state));
    memset(s->regs, 0, sizeof(*(s->regs)));
    s->regs[R_STATUS] = 0x4001u;
    ot_shadow_reg_init(&s->cfg, 0u);
    ot_shadow_reg_init(&s->entropy_refresh_threshold, 0u);

    ot_kmac_update_irq(s);
    ot_kmac_update_alert(s);

    fifo8_reset(&s->input_fifo);
}

static void ot_kmac_init(Object *obj)
{
    OtKMACState *s = OT_KMAC(obj);

    s->regs = g_new0(uint32_t, REGS_COUNT);

    for (unsigned ix = 0; ix < PARAM_NUM_IRQS; ix++) {
        ibex_sysbus_init_irq(obj, &s->irqs[ix]);
    }
    for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
        ibex_qdev_init_irq(obj, &s->alerts[ix], OPENTITAN_DEVICE_ALERT);
    }

    memory_region_init(&s->mmio, OBJECT(s), TYPE_OT_KMAC, OT_KMAC_WHOLE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    memory_region_init_io(&s->regs_mmio, obj, &ot_kmac_regs_ops, s,
                          TYPE_OT_KMAC "-regs", REGS_SIZE);
    memory_region_add_subregion(&s->mmio, OT_KMAC_REGS_BASE, &s->regs_mmio);

    memory_region_init_io(&s->state_mmio, obj, &ot_kmac_state_ops, s,
                          TYPE_OT_KMAC "-state", OT_KMAC_STATE_SIZE);
    memory_region_add_subregion(&s->mmio, OT_KMAC_STATE_BASE, &s->state_mmio);

    memory_region_init_io(&s->msgfifo_mmio, obj, &ot_kmac_msgfifo_ops, s,
                          TYPE_OT_KMAC "-msgfifo", OT_KMAC_MSG_FIFO_SIZE);
    memory_region_add_subregion(&s->mmio, OT_KMAC_MSG_FIFO_BASE,
                                &s->msgfifo_mmio);

    /* setup deferred processing trigger */
    s->deferred_trigger =
        timer_new_ns(QEMU_CLOCK_VIRTUAL, &ot_kmac_process, s);

    /* FIFO sizes as per OT Spec */
    fifo8_create(&s->input_fifo, FIFO_LENGTH);
}

static void ot_kmac_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = &ot_kmac_reset;
    device_class_set_props(dc, ot_kmac_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_kmac_info = {
    .name = TYPE_OT_KMAC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtKMACState),
    .instance_init = &ot_kmac_init,
    .class_init = &ot_kmac_class_init,
};

static void ot_kmac_register_types(void)
{
    type_register_static(&ot_kmac_info);
}

type_init(ot_kmac_register_types)
