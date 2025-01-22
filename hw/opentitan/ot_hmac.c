/*
 * QEMU OpenTitan HMAC device
 *
 * Copyright (c) 2022-2024 Rivos, Inc.
 * Copyright (c) 2024 lowRISC contributors.
 *
 * Author(s):
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

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/fifo8.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_clkmgr.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_hmac.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "tomcrypt.h"
#include "trace.h"

/* Input FIFO length is 64 bytes (16 x 32 bits) */
#define OT_HMAC_FIFO_LENGTH 64u

/* Maximum digest length is 64 bytes (512 bits) */
#define OT_HMAC_MAX_DIGEST_LENGTH 64u

/* Maximum key length is 128 bytes (1024 bits) */
#define OT_HMAC_MAX_KEY_LENGTH 128u

#define PARAM_NUM_IRQS 3u

/* clang-format off */
REG32(INTR_STATE, 0x00u)
    SHARED_FIELD(INTR_HMAC_DONE, 0u, 1u)
    SHARED_FIELD(INTR_FIFO_EMPTY, 1u, 1u)
    SHARED_FIELD(INTR_HMAC_ERR, 2u, 1u)
REG32(INTR_ENABLE, 0x04u)
REG32(INTR_TEST, 0x08u)
REG32(ALERT_TEST, 0x0cu)
    FIELD(ALERT_TEST, FATAL_FAULT, 0u, 1u)
REG32(CFG, 0x10u)
    FIELD(CFG, HMAC_EN, 0u, 1u)
    FIELD(CFG, SHA_EN, 1u, 1u)
    FIELD(CFG, ENDIAN_SWAP, 2u, 1u)
    FIELD(CFG, DIGEST_SWAP, 3u, 1u)
    FIELD(CFG, KEY_SWAP, 4u, 1u)
    FIELD(CFG, DIGEST_SIZE, 5u, 4u)
    FIELD(CFG, KEY_LENGTH, 9u, 6u)
REG32(CMD, 0x14u)
    FIELD(CMD, HASH_START, 0u, 1u)
    FIELD(CMD, HASH_PROCESS, 1u, 1u)
    FIELD(CMD, HASH_STOP, 2u, 1u)
    FIELD(CMD, HASH_CONTINUE, 3u, 1u)
REG32(STATUS, 0x18u)
    FIELD(STATUS, HMAC_IDLE, 0u, 1u)
    FIELD(STATUS, FIFO_EMPTY, 1u, 1u)
    FIELD(STATUS, FIFO_FULL, 2u, 1u)
    FIELD(STATUS, FIFO_DEPTH, 4u, 6u)
REG32(ERR_CODE, 0x1cu)
#define R_ERR_CODE_PUSH_MSG_WHEN_SHA_DISABLED   0x00000001u
#define R_ERR_CODE_HASH_START_WHEN_SHA_DISABLED 0x00000002u
#define R_ERR_CODE_UPDATE_SECRET_KEY_INPROCESS  0x00000003u
#define R_ERR_CODE_HASH_START_WHEN_ACTIVE       0x00000004u
#define R_ERR_CODE_PUSH_MSG_WHEN_DISALLOWED     0x00000005u
#define R_ERR_CODE_INVALID_CONFIG               0x00000006u
REG32(WIPE_SECRET, 0x20u)
REG32(KEY_0, 0x24u)
REG32(KEY_1, 0x28u)
REG32(KEY_2, 0x2cu)
REG32(KEY_3, 0x30u)
REG32(KEY_4, 0x34u)
REG32(KEY_5, 0x38u)
REG32(KEY_6, 0x3cu)
REG32(KEY_7, 0x40u)
REG32(KEY_8, 0x44u)
REG32(KEY_9, 0x48u)
REG32(KEY_10, 0x4cu)
REG32(KEY_11, 0x50u)
REG32(KEY_12, 0x54u)
REG32(KEY_13, 0x58u)
REG32(KEY_14, 0x5cu)
REG32(KEY_15, 0x60u)
REG32(KEY_16, 0x64u)
REG32(KEY_17, 0x68u)
REG32(KEY_18, 0x6cu)
REG32(KEY_19, 0x70u)
REG32(KEY_20, 0x74u)
REG32(KEY_21, 0x78u)
REG32(KEY_22, 0x7cu)
REG32(KEY_23, 0x80u)
REG32(KEY_24, 0x84u)
REG32(KEY_25, 0x88u)
REG32(KEY_26, 0x8cu)
REG32(KEY_27, 0x90u)
REG32(KEY_28, 0x94u)
REG32(KEY_29, 0x98u)
REG32(KEY_30, 0x9cu)
REG32(KEY_31, 0xa0u)
REG32(DIGEST_0, 0xa4u)
REG32(DIGEST_1, 0xa8u)
REG32(DIGEST_2, 0xacu)
REG32(DIGEST_3, 0xb0u)
REG32(DIGEST_4, 0xb4u)
REG32(DIGEST_5, 0xb8u)
REG32(DIGEST_6, 0xbcu)
REG32(DIGEST_7, 0xc0u)
REG32(DIGEST_8, 0xc4u)
REG32(DIGEST_9, 0xc8u)
REG32(DIGEST_10, 0xccu)
REG32(DIGEST_11, 0xd0u)
REG32(DIGEST_12, 0xd4u)
REG32(DIGEST_13, 0xd8u)
REG32(DIGEST_14, 0xdcu)
REG32(DIGEST_15, 0xe0u)
REG32(MSG_LENGTH_LOWER, 0xe4u)
REG32(MSG_LENGTH_UPPER, 0xe8u)
/* clang-format on */

#define INTR_MASK \
    (INTR_HMAC_ERR_MASK | INTR_FIFO_EMPTY_MASK | INTR_HMAC_DONE_MASK)

/* base offset for MMIO registers */
#define OT_HMAC_REGS_BASE 0x00000000u
/* base offset for MMIO FIFO */
#define OT_HMAC_FIFO_BASE 0x00001000u
/* length of MMIO FIFO */
#define OT_HMAC_FIFO_SIZE 0x00001000u
/* length of the whole device MMIO region */
#define OT_HMAC_WHOLE_SIZE (OT_HMAC_FIFO_BASE + OT_HMAC_FIFO_SIZE)

/* value representing 'SHA2_NONE' in the config digest size field */
#define OT_HMAC_CFG_DIGEST_SHA2_NONE 0x8u
/* value representing 'KEY_NONE' in the config key length field */
#define OT_HMAC_CFG_KEY_LENGTH_NONE 0x20u

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_MSG_LENGTH_UPPER)
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
    REG_NAME_ENTRY(CFG),
    REG_NAME_ENTRY(CMD),
    REG_NAME_ENTRY(STATUS),
    REG_NAME_ENTRY(ERR_CODE),
    REG_NAME_ENTRY(WIPE_SECRET),
    REG_NAME_ENTRY(KEY_0),
    REG_NAME_ENTRY(KEY_1),
    REG_NAME_ENTRY(KEY_2),
    REG_NAME_ENTRY(KEY_3),
    REG_NAME_ENTRY(KEY_4),
    REG_NAME_ENTRY(KEY_5),
    REG_NAME_ENTRY(KEY_6),
    REG_NAME_ENTRY(KEY_7),
    REG_NAME_ENTRY(KEY_8),
    REG_NAME_ENTRY(KEY_9),
    REG_NAME_ENTRY(KEY_10),
    REG_NAME_ENTRY(KEY_11),
    REG_NAME_ENTRY(KEY_12),
    REG_NAME_ENTRY(KEY_13),
    REG_NAME_ENTRY(KEY_14),
    REG_NAME_ENTRY(KEY_15),
    REG_NAME_ENTRY(KEY_16),
    REG_NAME_ENTRY(KEY_17),
    REG_NAME_ENTRY(KEY_18),
    REG_NAME_ENTRY(KEY_19),
    REG_NAME_ENTRY(KEY_20),
    REG_NAME_ENTRY(KEY_21),
    REG_NAME_ENTRY(KEY_22),
    REG_NAME_ENTRY(KEY_23),
    REG_NAME_ENTRY(KEY_24),
    REG_NAME_ENTRY(KEY_25),
    REG_NAME_ENTRY(KEY_26),
    REG_NAME_ENTRY(KEY_27),
    REG_NAME_ENTRY(KEY_28),
    REG_NAME_ENTRY(KEY_29),
    REG_NAME_ENTRY(KEY_30),
    REG_NAME_ENTRY(KEY_31),
    REG_NAME_ENTRY(DIGEST_0),
    REG_NAME_ENTRY(DIGEST_1),
    REG_NAME_ENTRY(DIGEST_2),
    REG_NAME_ENTRY(DIGEST_3),
    REG_NAME_ENTRY(DIGEST_4),
    REG_NAME_ENTRY(DIGEST_5),
    REG_NAME_ENTRY(DIGEST_6),
    REG_NAME_ENTRY(DIGEST_7),
    REG_NAME_ENTRY(DIGEST_8),
    REG_NAME_ENTRY(DIGEST_9),
    REG_NAME_ENTRY(DIGEST_10),
    REG_NAME_ENTRY(DIGEST_11),
    REG_NAME_ENTRY(DIGEST_12),
    REG_NAME_ENTRY(DIGEST_13),
    REG_NAME_ENTRY(DIGEST_14),
    REG_NAME_ENTRY(DIGEST_15),
    REG_NAME_ENTRY(MSG_LENGTH_LOWER),
    REG_NAME_ENTRY(MSG_LENGTH_UPPER),
};
#undef REG_NAME_ENTRY

typedef enum OtHMACDigestSize {
    HMAC_SHA2_NONE,
    HMAC_SHA2_256,
    HMAC_SHA2_384,
    HMAC_SHA2_512,
} OtHMACDigestSize;

typedef enum OtHMACKeyLength {
    HMAC_KEY_NONE,
    HMAC_KEY_128,
    HMAC_KEY_256,
    HMAC_KEY_384,
    HMAC_KEY_512,
    HMAC_KEY_1024,
} OtHMACKeyLength;

struct OtHMACRegisters {
    uint32_t intr_state;
    uint32_t intr_enable;
    uint32_t alert_test;
    uint32_t cfg;
    uint32_t cmd;
    uint32_t err_code;
    uint32_t wipe_secret;
    uint32_t key[OT_HMAC_MAX_KEY_LENGTH / sizeof(uint32_t)];
    uint32_t digest[OT_HMAC_MAX_DIGEST_LENGTH / sizeof(uint32_t)];
    uint64_t msg_length;
};
typedef struct OtHMACRegisters OtHMACRegisters;

struct OtHMACContext {
    hash_state state;
    OtHMACDigestSize digest_size_started;
};
typedef struct OtHMACContext OtHMACContext;

struct OtHMACState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    MemoryRegion regs_mmio;
    MemoryRegion fifo_mmio;

    IbexIRQ irqs[PARAM_NUM_IRQS];
    qemu_irq alert;
    IbexIRQ clkmgr;

    OtHMACRegisters *regs;
    OtHMACContext *ctx;
    Fifo8 input_fifo;

    char *ot_id;
};

static inline OtHMACDigestSize ot_hmac_get_digest_size(uint32_t cfg_reg)
{
    switch ((cfg_reg & R_CFG_DIGEST_SIZE_MASK) >> R_CFG_DIGEST_SIZE_SHIFT) {
    case 0x1u:
        return HMAC_SHA2_256;
    case 0x2u:
        return HMAC_SHA2_384;
    case 0x4u:
        return HMAC_SHA2_512;
    case 0x8u:
    default:
        return HMAC_SHA2_NONE;
    }
}

static size_t ot_hmac_get_digest_bytes(OtHMACDigestSize digest_size)
{
    switch (digest_size) {
    case HMAC_SHA2_256:
        return 32u;
    case HMAC_SHA2_384:
        return 48u;
    case HMAC_SHA2_512:
        return 64u;
    case HMAC_SHA2_NONE:
    default:
        /*
         * Should never happen: digest size was validated when calling start /
         * continue to begin operation.
         */
        g_assert_not_reached();
        return 0u;
    }
}

static size_t ot_hmac_get_block_size_bytes(OtHMACState *s)
{
    switch (ot_hmac_get_digest_size(s->regs->cfg)) {
    case HMAC_SHA2_256:
        return 64u;
    case HMAC_SHA2_384:
    case HMAC_SHA2_512:
        return 128u;
    case HMAC_SHA2_NONE:
    default:
        /*
         * Should never happen: digest size was validated when calling start /
         * continue to begin operation.
         */
        g_assert_not_reached();
        return 0u;
    }
}

static inline OtHMACKeyLength ot_hmac_get_key_length(uint32_t cfg_reg)
{
    switch ((cfg_reg & R_CFG_KEY_LENGTH_MASK) >> R_CFG_KEY_LENGTH_SHIFT) {
    case 0x01u:
        return HMAC_KEY_128;
    case 0x02u:
        return HMAC_KEY_256;
    case 0x04u:
        return HMAC_KEY_384;
    case 0x08u:
        return HMAC_KEY_512;
    case 0x10u:
        return HMAC_KEY_1024;
    case 0x20u:
    default:
        return HMAC_KEY_NONE;
    }
}

static size_t ot_hmac_get_key_bytes(OtHMACState *s)
{
    switch (ot_hmac_get_key_length(s->regs->cfg)) {
    case HMAC_KEY_128:
        return 16u;
    case HMAC_KEY_256:
        return 32u;
    case HMAC_KEY_384:
        return 48u;
    case HMAC_KEY_512:
        return 64u;
    case HMAC_KEY_1024:
        return 128u;
    case HMAC_KEY_NONE:
    default:
        /*
         * Should never happen: key length was validated when calling start /
         * continue to begin operation if HMAC was enabled, and HMAC cannot be
         * enabled while the SHA engine is in operation.
         */
        g_assert_not_reached();
        return 0u;
    }
}

static inline bool ot_hmac_key_length_supported(OtHMACDigestSize digest_size,
                                                OtHMACKeyLength key_length)
{
    return !(digest_size == HMAC_SHA2_256 && key_length == HMAC_KEY_1024);
}

static void ot_hmac_update_irqs(OtHMACState *s)
{
    uint32_t levels = s->regs->intr_state & s->regs->intr_enable;
    trace_ot_hmac_irqs(s->ot_id, s->regs->intr_state, s->regs->intr_enable,
                       levels);
    for (unsigned ix = 0; ix < PARAM_NUM_IRQS; ix++) {
        ibex_irq_set(&s->irqs[ix], (int)((levels >> ix) & 0x1u));
    }
}

static void ot_hmac_update_alert(OtHMACState *s)
{
    bool level = s->regs->alert_test & R_ALERT_TEST_FATAL_FAULT_MASK;
    qemu_set_irq(s->alert, level);
}

static void ot_hmac_report_error(OtHMACState *s, uint32_t error)
{
    s->regs->err_code = error;
    s->regs->intr_state |= INTR_HMAC_ERR_MASK;
    ot_hmac_update_irqs(s);
}

static void ot_hmac_writeback_digest_state(OtHMACState *s)
{
    /* copy intermediary digest to mock HMAC's stop/continue behaviour. */
    switch (s->ctx->digest_size_started) {
    case HMAC_SHA2_256:
        for (unsigned idx = 0; idx < 8u; idx++) {
            STORE32H(s->ctx->state.sha256.state[idx], s->regs->digest + idx);
        }
        break;
    case HMAC_SHA2_384:
        /*
         * Even though SHA384 only uses the first six uint64_t values of
         * the SHA512 digest, we must store all for intermediary computation.
         */
    case HMAC_SHA2_512:
        for (unsigned idx = 0; idx < 8u; idx++) {
            STORE64H(s->ctx->state.sha512.state[idx],
                     s->regs->digest + 2 * idx);
        }
        break;
    case HMAC_SHA2_NONE:
    default:
        /*
         * Should never happen: digest size was validated when calling start /
         * continue to begin operation.
         */
        g_assert_not_reached();
    }
}

static void ot_hmac_restore_context(OtHMACState *s)
{
    switch (s->ctx->digest_size_started) {
    case HMAC_SHA2_256:
        s->ctx->state.sha256.curlen = 0;
        s->ctx->state.sha256.length = s->regs->msg_length;
        for (unsigned idx = 0; idx < 8u; idx++) {
            LOAD32H(s->ctx->state.sha256.state[idx], s->regs->digest + idx);
        }
        break;
    case HMAC_SHA2_384:
        /*
         * Even though SHA384 only uses the first six uint64_t values of
         * the SHA512 digest, we must restore all for intermediary computation.
         */
    case HMAC_SHA2_512:
        s->ctx->state.sha512.curlen = 0;
        s->ctx->state.sha512.length = s->regs->msg_length;
        for (unsigned idx = 0; idx < 8u; idx++) {
            LOAD64H(s->ctx->state.sha512.state[idx], s->regs->digest + 2 * idx);
        }
        break;
    case HMAC_SHA2_NONE:
    default:
        /*
         * Should never happen: digest size was validated when receiving the
         * continue command to (re-)begin operation.
         */
        g_assert_not_reached();
    }
}

static size_t ot_hmac_get_curlen(OtHMACState *s)
{
    switch (s->ctx->digest_size_started) {
    case HMAC_SHA2_256:
        return s->ctx->state.sha256.curlen;
    case HMAC_SHA2_384:
    case HMAC_SHA2_512:
        return s->ctx->state.sha512.curlen;
    case HMAC_SHA2_NONE:
    default:
        /*
         * Should never happen: digest size was validated when calling start /
         * continue to begin operation.
         */
        g_assert_not_reached();
        return 0u;
    }
}

static void ot_hmac_sha_init(OtHMACState *s, bool write_back)
{
    switch (s->ctx->digest_size_started) {
    case HMAC_SHA2_256:
        sha256_init(&s->ctx->state);
        break;
    case HMAC_SHA2_384:
        sha384_init(&s->ctx->state);
        break;
    case HMAC_SHA2_512:
        sha512_init(&s->ctx->state);
        break;
    case HMAC_SHA2_NONE:
    default:
        /*
         * Should never happen: digest size was validated when calling start /
         * continue to begin operation.
         */
        g_assert_not_reached();
        return;
    }
    if (write_back) {
        ot_hmac_writeback_digest_state(s);
    }
}

static void ot_hmac_sha_process(OtHMACState *s, const uint8_t *in, size_t inlen,
                                bool write_back)
{
    switch (s->ctx->digest_size_started) {
    case HMAC_SHA2_256:
        sha256_process(&s->ctx->state, in, inlen);
        break;
    /* NOLINTNEXTLINE */
    case HMAC_SHA2_384:
        sha384_process(&s->ctx->state, in, inlen);
        break;
    case HMAC_SHA2_512:
        sha512_process(&s->ctx->state, in, inlen);
        break;
    case HMAC_SHA2_NONE:
    default:
        /*
         * Should never happen: digest size was validated when calling start /
         * continue to begin operation.
         */
        g_assert_not_reached();
        return;
    }
    if (write_back) {
        ot_hmac_writeback_digest_state(s);
    }
}

static void ot_hmac_sha_done(OtHMACState *s)
{
    switch (s->ctx->digest_size_started) {
    case HMAC_SHA2_256:
        sha256_done(&s->ctx->state, (uint8_t *)s->regs->digest);
        return;
    case HMAC_SHA2_384:
        sha384_done(&s->ctx->state, (uint8_t *)s->regs->digest);
        return;
    case HMAC_SHA2_512:
        sha512_done(&s->ctx->state, (uint8_t *)s->regs->digest);
        return;
    case HMAC_SHA2_NONE:
    default:
        /*
         * Should never happen: digest size was validated when calling start /
         * continue to begin operation.
         */
        g_assert_not_reached();
        return;
    }
}

static void ot_hmac_compute_digest(OtHMACState *s)
{
    trace_ot_hmac_debug(s->ot_id, __func__);

    /* HMAC mode, perform outer hash */
    if (s->regs->cfg & R_CFG_HMAC_EN_MASK) {
        ot_hmac_sha_done(s);

        size_t key_length_b = ot_hmac_get_key_bytes(s);
        size_t block_size_b = ot_hmac_get_block_size_bytes(s);
        /* pad key to right with 0s when it is smaller than the block size. */
        size_t pad_length_b = MAX(key_length_b, block_size_b);
        size_t pad_length_w = pad_length_b / sizeof(uint64_t);
        uint64_t opad[OT_HMAC_MAX_KEY_LENGTH / sizeof(uint64_t)];
        memset(opad, 0, sizeof(opad));
        memcpy(opad, s->regs->key, key_length_b);
        for (size_t idx = 0; idx < pad_length_w; idx++) {
            opad[idx] ^= 0x5c5c5c5c5c5c5c5cull;
        }
        ot_hmac_sha_init(s, false);
        ot_hmac_sha_process(s, (const uint8_t *)opad, pad_length_b, false);
        ot_hmac_sha_process(s, (const uint8_t *)s->regs->digest,
                            ot_hmac_get_digest_bytes(
                                s->ctx->digest_size_started),
                            true);
    }
    ot_hmac_sha_done(s);
}

static void ot_hmac_process_fifo(OtHMACState *s)
{
    trace_ot_hmac_debug(s->ot_id, __func__);

    bool stop = s->regs->cmd & R_CMD_HASH_STOP_MASK;

    if (!fifo8_is_empty(&s->input_fifo) &&
        (!stop || ot_hmac_get_curlen(s) != 0)) {
        while (!fifo8_is_empty(&s->input_fifo) &&
               (!stop || ot_hmac_get_curlen(s) != 0)) {
            uint8_t value = fifo8_pop(&s->input_fifo);
            ot_hmac_sha_process(s, &value, 1u, false);
        }

        /* write back updated digest state */
        if (fifo8_is_empty(&s->input_fifo) || stop) {
            ot_hmac_writeback_digest_state(s);
        }

        /* assert FIFO Empty IRQ */
        if (fifo8_is_empty(&s->input_fifo)) {
            s->regs->intr_state |= INTR_FIFO_EMPTY_MASK;
        }
    }

    if (stop && ot_hmac_get_curlen(s) == 0) {
        s->regs->intr_state |= INTR_HMAC_DONE_MASK;
        s->regs->cmd = 0;
    }

    if (s->regs->cmd & R_CMD_HASH_PROCESS_MASK) {
        ot_hmac_compute_digest(s);
        s->regs->intr_state |= INTR_HMAC_DONE_MASK;
        s->regs->cmd = 0;
    }

    ot_hmac_update_irqs(s);

    ibex_irq_set(&s->clkmgr,
                 !fifo8_is_empty(&s->input_fifo) || (bool)s->regs->cmd);
}

static inline void ot_hmac_wipe_buffer(OtHMACState *s, uint32_t *buffer,
                                       size_t size)
{
    for (unsigned index = 0; index < size; index++) {
        buffer[index] = s->regs->wipe_secret;
    }
}

static uint64_t ot_hmac_fifo_read(void *opaque, hwaddr addr, unsigned size)
{
    (void)opaque;
    (void)addr;
    (void)size;
    qemu_log_mask(LOG_GUEST_ERROR, "%s: MSG_FIFO is write only\n", __func__);

    return 0;
}

static void ot_hmac_fifo_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    OtHMACState *s = OT_HMAC(opaque);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_hmac_fifo_write(s->ot_id, (uint32_t)addr, (uint32_t)value, size,
                             pc);

    if (!s->regs->cmd) {
        ot_hmac_report_error(s, R_ERR_CODE_PUSH_MSG_WHEN_DISALLOWED);
        return;
    }

    if (!(s->regs->cfg & R_CFG_SHA_EN_MASK)) {
        ot_hmac_report_error(s, R_ERR_CODE_PUSH_MSG_WHEN_SHA_DISABLED);
        return;
    }

    if (s->regs->cfg & R_CFG_ENDIAN_SWAP_MASK) {
        if (size == 4u) {
            value = bswap32((uint32_t)value);
        } else if (size == 2u) {
            value = bswap16((uint16_t)value);
        }
    }

    ibex_irq_set(&s->clkmgr, true);

    for (unsigned i = 0; i < size; i++) {
        uint8_t b = value;
        g_assert(!fifo8_is_full(&s->input_fifo));
        fifo8_push(&s->input_fifo, b);
        value >>= 8u;
    }

    s->regs->msg_length += (uint64_t)size * 8u;

    /*
     * Note: real HW may stall the bus till some room is available in the input
     * FIFO. In QEMU, we do not want to stall the I/O thread to emulate this
     * feature. The workaround is to let the FIFO fill up with an arbitrary
     * length, always smaller than the FIFO capacity, here half the size of the
     * FIFO then process the whole FIFO content in one step. This let the FIFO
     * depth register to update on each call as the real HW. However the FIFO
     * can never be full, which is not supposed to occur on the real HW anyway
     * since the HMAC is reportedly faster than the Ibex capability to fill in
     * the FIFO. Could be different with DMA access though.
     */
    if (fifo8_num_used(&s->input_fifo) >= OT_HMAC_FIFO_LENGTH / 2u) {
        ot_hmac_process_fifo(s);
    }
}

static uint64_t ot_hmac_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtHMACState *s = OT_HMAC(opaque);
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);
    switch (reg) {
    case R_INTR_STATE:
        val32 = s->regs->intr_state;
        break;
    case R_INTR_ENABLE:
        val32 = s->regs->intr_enable;
        break;
    case R_CFG:
        val32 = s->regs->cfg;
        break;
    case R_CMD:
        val32 = 0;
        /* always read 0: CMD is r0w1c */
        break;
    case R_STATUS: {
        uint32_t num_used = fifo8_num_used(&s->input_fifo);
        if (num_used == 0) {
            val32 = R_STATUS_FIFO_EMPTY_MASK;
        } else {
            val32 = ((num_used / 4u) << R_STATUS_FIFO_DEPTH_SHIFT) &
                    R_STATUS_FIFO_DEPTH_MASK;
            if (num_used == OT_HMAC_FIFO_LENGTH) {
                val32 |= R_STATUS_FIFO_FULL_MASK;
            }
        }
        if (!(s->regs->cmd)) {
            val32 |= R_STATUS_HMAC_IDLE_MASK;
        }
    } break;
    case R_ERR_CODE:
        val32 = s->regs->err_code;
        break;
    case R_DIGEST_0:
    case R_DIGEST_1:
    case R_DIGEST_2:
    case R_DIGEST_3:
    case R_DIGEST_4:
    case R_DIGEST_5:
    case R_DIGEST_6:
    case R_DIGEST_7:
    case R_DIGEST_8:
    case R_DIGEST_9:
    case R_DIGEST_10:
    case R_DIGEST_11:
    case R_DIGEST_12:
    case R_DIGEST_13:
    case R_DIGEST_14:
    case R_DIGEST_15:
        /*
         * We use a SHA library that computes in native (little) endian-ness,
         * but produces a big-endian digest upon termination. To ensure
         * consistency between digests that are read/written, we make sure the
         * value internally in s->regs is always big endian, to match the final
         * digest. So, we only need to swap if the swap config is 0 (i.e. the
         * digest should be output in little endian).
         */
        if (s->regs->cfg & R_CFG_DIGEST_SWAP_MASK) {
            val32 = s->regs->digest[reg - R_DIGEST_0];
        } else {
            val32 = bswap32(s->regs->digest[reg - R_DIGEST_0]);
        }
        break;
    case R_MSG_LENGTH_LOWER:
        val32 = s->regs->msg_length;
        break;
    case R_MSG_LENGTH_UPPER:
        val32 = s->regs->msg_length >> 32u;
        break;
    case R_INTR_TEST:
    case R_ALERT_TEST:
    case R_WIPE_SECRET:
    case R_KEY_0:
    case R_KEY_1:
    case R_KEY_2:
    case R_KEY_3:
    case R_KEY_4:
    case R_KEY_5:
    case R_KEY_6:
    case R_KEY_7:
    case R_KEY_8:
    case R_KEY_9:
    case R_KEY_10:
    case R_KEY_11:
    case R_KEY_12:
    case R_KEY_13:
    case R_KEY_14:
    case R_KEY_15:
    case R_KEY_16:
    case R_KEY_17:
    case R_KEY_18:
    case R_KEY_19:
    case R_KEY_20:
    case R_KEY_21:
    case R_KEY_22:
    case R_KEY_23:
    case R_KEY_24:
    case R_KEY_25:
    case R_KEY_26:
    case R_KEY_27:
    case R_KEY_28:
    case R_KEY_29:
    case R_KEY_30:
    case R_KEY_31:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: W/O register 0x%02" HWADDR_PRIx " (%s)\n", __func__,
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
    trace_ot_hmac_io_read_out(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32,
                              pc);

    return (uint64_t)val32;
}

static void ot_hmac_regs_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    OtHMACState *s = OT_HMAC(opaque);
    (void)size;
    uint32_t val32 = (uint32_t)value;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_hmac_io_write(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32, pc);

    OtHMACDigestSize digest_size;
    OtHMACKeyLength key_length;

    switch (reg) {
    case R_INTR_STATE:
        s->regs->intr_state &= ~(val32 & INTR_MASK);
        ot_hmac_update_irqs(s);
        break;
    case R_INTR_ENABLE:
        s->regs->intr_enable = val32 & INTR_MASK;
        ot_hmac_update_irqs(s);
        break;
    case R_INTR_TEST:
        s->regs->intr_state |= val32 & INTR_MASK;
        ot_hmac_update_irqs(s);
        break;
    case R_ALERT_TEST:
        s->regs->alert_test |= val32 & R_ALERT_TEST_FATAL_FAULT_MASK;
        ot_hmac_update_alert(s);
        break;
    case R_CFG:
        /* ignore write if engine is not idle */
        if (s->regs->cmd) {
            break;
        }

        val32 &=
            (R_CFG_HMAC_EN_MASK | R_CFG_SHA_EN_MASK | R_CFG_ENDIAN_SWAP_MASK |
             R_CFG_DIGEST_SWAP_MASK | R_CFG_KEY_SWAP_MASK |
             R_CFG_DIGEST_SIZE_MASK | R_CFG_KEY_LENGTH_MASK);

        /* If the digest size is invalid, it gets mapped to SHA2_NONE. */
        digest_size = ot_hmac_get_digest_size(val32);
        if (digest_size == HMAC_SHA2_NONE) {
            val32 &= ~R_CFG_DIGEST_SIZE_MASK;
            val32 |= OT_HMAC_CFG_DIGEST_SHA2_NONE << R_CFG_DIGEST_SIZE_SHIFT;
        }

        /* If the key length is invalid, it gets mapped to KEY_NONE. */
        key_length = ot_hmac_get_key_length(val32);
        if (key_length == HMAC_KEY_NONE) {
            val32 &= ~R_CFG_KEY_LENGTH_MASK;
            val32 |= OT_HMAC_CFG_KEY_LENGTH_NONE << R_CFG_KEY_LENGTH_SHIFT;
        }

        s->regs->cfg = val32;

        /* clear digest when SHA is disabled */
        if (!(s->regs->cfg & R_CFG_SHA_EN_MASK)) {
            ot_hmac_wipe_buffer(s, s->regs->digest,
                                ARRAY_SIZE(s->regs->digest));
        }
        break;
    case R_CMD:
        if (val32 & (R_CMD_HASH_START_MASK | R_CMD_HASH_CONTINUE_MASK)) {
            digest_size = ot_hmac_get_digest_size(s->regs->cfg);
            if (digest_size == HMAC_SHA2_NONE) {
                ot_hmac_report_error(s, R_ERR_CODE_INVALID_CONFIG);
                break;
            }

            if (s->regs->cfg & R_CFG_HMAC_EN_MASK) {
                key_length = ot_hmac_get_key_length(s->regs->cfg);
                if (key_length == HMAC_KEY_NONE ||
                    !ot_hmac_key_length_supported(digest_size, key_length)) {
                    ot_hmac_report_error(s, R_ERR_CODE_INVALID_CONFIG);
                    break;
                }
            }
        }

        if (val32 & R_CMD_HASH_START_MASK) {
            if (!(s->regs->cfg & R_CFG_SHA_EN_MASK)) {
                ot_hmac_report_error(s,
                                     R_ERR_CODE_HASH_START_WHEN_SHA_DISABLED);
                break;
            }
            if (s->regs->cmd) {
                ot_hmac_report_error(s, R_ERR_CODE_HASH_START_WHEN_ACTIVE);
                break;
            }
            s->regs->cmd = R_CMD_HASH_START_MASK;
            s->regs->msg_length = 0;

            ibex_irq_set(&s->clkmgr, true);

            /*
             * Hold the previous digest size until the HMAC is started with the
             * new digest size configured
             */
            s->ctx->digest_size_started = ot_hmac_get_digest_size(s->regs->cfg);

            ot_hmac_sha_init(s, true);

            /* HMAC mode, process input padding */
            if (s->regs->cfg & R_CFG_HMAC_EN_MASK) {
                size_t key_length_b = ot_hmac_get_key_bytes(s);
                size_t block_size_b = ot_hmac_get_block_size_bytes(s);
                /* pad key to right with 0s if smaller than the block size. */
                size_t pad_length_b = MAX(key_length_b, block_size_b);
                size_t pad_length_w = pad_length_b / sizeof(uint64_t);
                uint64_t ipad[OT_HMAC_MAX_KEY_LENGTH / sizeof(uint64_t)];
                memset(ipad, 0, sizeof(ipad));
                memcpy(ipad, s->regs->key, key_length_b);
                for (size_t idx = 0; idx < pad_length_w; idx++) {
                    ipad[idx] ^= 0x3636363636363636ull;
                }
                ot_hmac_sha_process(s, (const uint8_t *)ipad, pad_length_b,
                                    true);
            }
        }

        if (val32 & R_CMD_HASH_PROCESS_MASK) {
            if (!(s->regs->cmd &
                  (R_CMD_HASH_START_MASK | R_CMD_HASH_CONTINUE_MASK))) {
                qemu_log_mask(
                    LOG_GUEST_ERROR,
                    "%s: CMD.PROCESS requested but hash not started yet\n",
                    __func__);
                break;
            }
            if (s->regs->cmd & R_CMD_HASH_PROCESS_MASK) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "%s: CMD.PROCESS requested but hash is currently "
                              "processing\n",
                              __func__);
                break;
            }
            s->regs->cmd |= R_CMD_HASH_PROCESS_MASK;

            /* trigger delayed processing of FIFO */
            ibex_irq_set(&s->clkmgr, true);
            ot_hmac_process_fifo(s);
        }

        if (val32 & R_CMD_HASH_STOP_MASK) {
            s->regs->cmd = R_CMD_HASH_STOP_MASK;

            /*
             * trigger delayed processing of FIFO until the next block is
             * processed.
             */
            ibex_irq_set(&s->clkmgr, true);
            ot_hmac_process_fifo(s);
        }

        if (val32 & R_CMD_HASH_CONTINUE_MASK) {
            if (!(s->regs->cfg & R_CFG_SHA_EN_MASK)) {
                ot_hmac_report_error(s,
                                     R_ERR_CODE_HASH_START_WHEN_SHA_DISABLED);
                break;
            }
            if (s->regs->cmd) {
                ot_hmac_report_error(s, R_ERR_CODE_HASH_START_WHEN_ACTIVE);
                break;
            }

            s->regs->cmd = R_CMD_HASH_CONTINUE_MASK;

            /*
             * Hold the previous digest size until the HMAC is started with the
             * new digest size configured
             */
            s->ctx->digest_size_started = ot_hmac_get_digest_size(s->regs->cfg);

            ot_hmac_restore_context(s);

            /* trigger delayed processing of FIFO */
            ibex_irq_set(&s->clkmgr, true);
            ot_hmac_process_fifo(s);
        }

        break;
    case R_WIPE_SECRET:
        s->regs->wipe_secret = val32;
        ot_hmac_wipe_buffer(s, s->regs->key, ARRAY_SIZE(s->regs->key));
        ot_hmac_wipe_buffer(s, s->regs->digest, ARRAY_SIZE(s->regs->digest));
        break;
    case R_KEY_0:
    case R_KEY_1:
    case R_KEY_2:
    case R_KEY_3:
    case R_KEY_4:
    case R_KEY_5:
    case R_KEY_6:
    case R_KEY_7:
    case R_KEY_8:
    case R_KEY_9:
    case R_KEY_10:
    case R_KEY_11:
    case R_KEY_12:
    case R_KEY_13:
    case R_KEY_14:
    case R_KEY_15:
    case R_KEY_16:
    case R_KEY_17:
    case R_KEY_18:
    case R_KEY_19:
    case R_KEY_20:
    case R_KEY_21:
    case R_KEY_22:
    case R_KEY_23:
    case R_KEY_24:
    case R_KEY_25:
    case R_KEY_26:
    case R_KEY_27:
    case R_KEY_28:
    case R_KEY_29:
    case R_KEY_30:
    case R_KEY_31:
        /* ignore write and report error if engine is not idle */
        if (s->regs->cmd) {
            ot_hmac_report_error(s, R_ERR_CODE_UPDATE_SECRET_KEY_INPROCESS);
            break;
        }

        /*
         * We use a SHA library that operates in native (little) endian-ness,
         * so we only need to swap if the swap config is 0 (i.e. the input key
         * is big endian), to ensure the value in s->regs is little endian.
         */
        if (s->regs->cfg & R_CFG_KEY_SWAP_MASK) {
            s->regs->key[reg - R_KEY_0] = val32;
        } else {
            s->regs->key[reg - R_KEY_0] = bswap32(val32);
        }
        break;
    case R_STATUS:
    case R_ERR_CODE:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: R/O register 0x%02" HWADDR_PRIx " (%s)\n", __func__,
                      addr, REG_NAME(reg));
        break;
    case R_DIGEST_0:
    case R_DIGEST_1:
    case R_DIGEST_2:
    case R_DIGEST_3:
    case R_DIGEST_4:
    case R_DIGEST_5:
    case R_DIGEST_6:
    case R_DIGEST_7:
    case R_DIGEST_8:
    case R_DIGEST_9:
    case R_DIGEST_10:
    case R_DIGEST_11:
    case R_DIGEST_12:
    case R_DIGEST_13:
    case R_DIGEST_14:
    case R_DIGEST_15:
        /* ignore write and report error if engine is not idle */
        if (s->regs->cmd) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: Cannot W register 0x%02" HWADDR_PRIx
                          " (%s) whilst non-idle\n",
                          __func__, addr, REG_NAME(reg));
            break;
        } else if (s->regs->cfg & R_CFG_SHA_EN_MASK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: Cannot W register 0x%02" HWADDR_PRIx
                          " (%s) whilst SHA Engine is enabled\n",
                          __func__, addr, REG_NAME(reg));
        }

        /*
         * We use a SHA library that computes in native (little) endian-ness,
         * but produces a big-endian digest upon termination. To ensure
         * consistency between digests that are read/written, we make sure the
         * value internally in s->regs is always big endian, to match the final
         * digest. So, we only need to swap if the swap config is 0 (i.e. the
         * input digest is little endian).
         */
        if (s->regs->cfg & R_CFG_DIGEST_SWAP_MASK) {
            s->regs->digest[reg - R_DIGEST_0] = val32;
        } else {
            s->regs->digest[reg - R_DIGEST_0] = bswap32(val32);
        }
        break;
    case R_MSG_LENGTH_LOWER:
        /* ignore write and report error if engine is not idle */
        if (s->regs->cmd) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: Cannot W register 0x%02" HWADDR_PRIx
                          " (%s) whilst non-idle\n",
                          __func__, addr, REG_NAME(reg));
            break;
        } else if (s->regs->cfg & R_CFG_SHA_EN_MASK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: Cannot W register 0x%02" HWADDR_PRIx
                          " (%s) whilst SHA Engine is enabled\n",
                          __func__, addr, REG_NAME(reg));
        }
        s->regs->msg_length =
            (s->regs->msg_length & (0xFFFFFFFFull << 32u)) | val32;
        break;
    case R_MSG_LENGTH_UPPER:
        /* ignore write and report error if engine is not idle */
        if (s->regs->cmd) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: Cannot W register 0x%02" HWADDR_PRIx
                          " (%s) whilst non-idle\n",
                          __func__, addr, REG_NAME(reg));
            break;
        } else if (s->regs->cfg & R_CFG_SHA_EN_MASK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: Cannot W register 0x%02" HWADDR_PRIx
                          " (%s) whilst SHA Engine is enabled\n",
                          __func__, addr, REG_NAME(reg));
        }
        s->regs->msg_length =
            ((uint64_t)val32 << 32u) | (s->regs->msg_length & 0xFFFFFFFFull);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static Property ot_hmac_properties[] = {
    DEFINE_PROP_STRING("ot_id", OtHMACState, ot_id),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_hmac_regs_ops = {
    .read = &ot_hmac_regs_read,
    .write = &ot_hmac_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4u,
        .max_access_size = 4u,
    },
};

static const MemoryRegionOps ot_hmac_fifo_ops = {
    .read = &ot_hmac_fifo_read,
    .write = &ot_hmac_fifo_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1u,
        .max_access_size = 4u,
    },
};

static void ot_hmac_init(Object *obj)
{
    OtHMACState *s = OT_HMAC(obj);

    s->regs = g_new0(OtHMACRegisters, 1u);
    s->ctx = g_new(OtHMACContext, 1u);

    for (unsigned ix = 0; ix < PARAM_NUM_IRQS; ix++) {
        ibex_sysbus_init_irq(obj, &s->irqs[ix]);
    }
    qdev_init_gpio_out_named(DEVICE(obj), &s->alert, OT_DEVICE_ALERT, 1);
    ibex_qdev_init_irq(obj, &s->clkmgr, OT_CLOCK_ACTIVE);

    memory_region_init(&s->mmio, OBJECT(s), TYPE_OT_HMAC, OT_HMAC_WHOLE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    memory_region_init_io(&s->regs_mmio, obj, &ot_hmac_regs_ops, s,
                          TYPE_OT_HMAC ".regs", REGS_SIZE);
    memory_region_add_subregion(&s->mmio, OT_HMAC_REGS_BASE, &s->regs_mmio);

    memory_region_init_io(&s->fifo_mmio, obj, &ot_hmac_fifo_ops, s,
                          TYPE_OT_HMAC ".fifo", OT_HMAC_FIFO_SIZE);
    memory_region_add_subregion(&s->mmio, OT_HMAC_FIFO_BASE, &s->fifo_mmio);

    /* FIFO sizes as per OT Spec */
    fifo8_create(&s->input_fifo, OT_HMAC_FIFO_LENGTH);
}

static void ot_hmac_realize(DeviceState *dev, Error **errp)
{
    (void)errp;

    OtHMACState *s = OT_HMAC(dev);
    if (!s->ot_id) {
        s->ot_id =
            g_strdup(object_get_canonical_path_component(OBJECT(s)->parent));
    }
}

static void ot_hmac_reset(DeviceState *dev)
{
    OtHMACState *s = OT_HMAC(dev);
    OtHMACRegisters *r = s->regs;

    ibex_irq_set(&s->clkmgr, false);

    memset(s->ctx, 0, sizeof(*(s->ctx)));
    memset(s->regs, 0, sizeof(*(s->regs)));

    r->cfg = 0x4100u;

    ot_hmac_update_irqs(s);
    ot_hmac_update_alert(s);

    fifo8_reset(&s->input_fifo);
}

static void ot_hmac_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->reset = &ot_hmac_reset;
    dc->realize = &ot_hmac_realize;
    device_class_set_props(dc, ot_hmac_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_hmac_info = {
    .name = TYPE_OT_HMAC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtHMACState),
    .instance_init = &ot_hmac_init,
    .class_init = &ot_hmac_class_init,
};

static void ot_hmac_register_types(void)
{
    type_register_static(&ot_hmac_info);
}

type_init(ot_hmac_register_types);
