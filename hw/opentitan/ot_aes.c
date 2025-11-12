/*
 * QEMU OpenTitan AES device
 *
 * Copyright (c) 2022-2025 Rivos, Inc.
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
 */

#include "qemu/osdep.h"
#include "qemu/bitmap.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "hw/opentitan/ot_aes.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_clkmgr.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_edn.h"
#include "hw/opentitan/ot_key_sink.h"
#include "hw/opentitan/ot_prng.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_clock_src.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "tomcrypt.h"
#include "trace.h"

#undef DEBUG_AES

#define PARAM_NUM_REGS_KEY  8u
#define PARAM_NUM_REGS_IV   4u
#define PARAM_NUM_REGS_DATA 4u
#define PARAM_NUM_ALERTS    2u

/* clang-format off */
REG32(ALERT_TEST, 0x0u)
    SHARED_FIELD(ALERT_RECOV_CTRL_UPDATE_ERR, 0u, 1u)
    SHARED_FIELD(ALERT_FATAL_FAULT, 1u, 1u)
REG32(KEY_SHARE0_0, 0x4u)
REG32(KEY_SHARE0_1, 0x8u)
REG32(KEY_SHARE0_2, 0xcu)
REG32(KEY_SHARE0_3, 0x10u)
REG32(KEY_SHARE0_4, 0x14u)
REG32(KEY_SHARE0_5, 0x18u)
REG32(KEY_SHARE0_6, 0x1cu)
REG32(KEY_SHARE0_7, 0x20u)
REG32(KEY_SHARE1_0, 0x24u)
REG32(KEY_SHARE1_1, 0x28u)
REG32(KEY_SHARE1_2, 0x2cu)
REG32(KEY_SHARE1_3, 0x30u)
REG32(KEY_SHARE1_4, 0x34u)
REG32(KEY_SHARE1_5, 0x38u)
REG32(KEY_SHARE1_6, 0x3cu)
REG32(KEY_SHARE1_7, 0x40u)
REG32(IV_0, 0x44u)
REG32(IV_1, 0x48u)
REG32(IV_2, 0x4cu)
REG32(IV_3, 0x50u)
REG32(DATA_IN_0, 0x54u)
REG32(DATA_IN_1, 0x58u)
REG32(DATA_IN_2, 0x5cu)
REG32(DATA_IN_3, 0x60u)
REG32(DATA_OUT_0, 0x64u)
REG32(DATA_OUT_1, 0x68u)
REG32(DATA_OUT_2, 0x6cu)
REG32(DATA_OUT_3, 0x70u)
REG32(CTRL_SHADOWED, 0x74u)
    FIELD(CTRL_SHADOWED, OPERATION, 0u, 2u)
    FIELD(CTRL_SHADOWED, MODE, 2u, 6u)
    FIELD(CTRL_SHADOWED, KEY_LEN, 8u, 3u)
    FIELD(CTRL_SHADOWED, SIDELOAD, 11u, 1u)
    FIELD(CTRL_SHADOWED, PRNG_RESEED_RATE, 12u, 3u)
    FIELD(CTRL_SHADOWED, MANUAL_OPERATION, 15u, 1u)
    FIELD(CTRL_SHADOWED, FORCE_ZERO_MASKS, 16u, 1u)
REG32(CTRL_AUX_SHADOWED, 0x78u)
    FIELD(CTRL_AUX_SHADOWED, KEY_TOUCH_FORCES_RESEED, 0u, 1u)
    FIELD(CTRL_AUX_SHADOWED, FORCE_MASKS, 1u, 1u)
REG32(CTRL_AUX_REGWEN, 0x7cu)
    FIELD(CTRL_AUX_REGWEN, CTRL_AUX_REGWEN, 0u, 1u)
REG32(TRIGGER, 0x80u)
    FIELD(TRIGGER, START, 0u, 1u)
    FIELD(TRIGGER, KEY_IV_DATA_IN_CLEAR, 1u, 1u)
    FIELD(TRIGGER, DATA_OUT_CLEAR, 2u, 1u)
    FIELD(TRIGGER, PRNG_RESEED, 3u, 1u)
REG32(STATUS, 0x84u)
    FIELD(STATUS, IDLE, 0u, 1u)
    FIELD(STATUS, STALL, 1u, 1u)
    FIELD(STATUS, OUTPUT_LOST, 2u, 1u)
    FIELD(STATUS, OUTPUT_VALID, 3u, 1u)
    FIELD(STATUS, INPUT_READY, 4u, 1u)
    FIELD(STATUS, ALERT_RECOV_CTRL_UPDATE_ERR, 5u, 1u)
    FIELD(STATUS, ALERT_FATAL_FAULT, 6u, 1u)
/* clang-format on */

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define OT_AES_KEYSHARE_BM_MASK ((1u << (PARAM_NUM_REGS_KEY * 2u)) - 1u)
#define OT_AES_IV_BM_MASK       ((1u << (PARAM_NUM_REGS_IV)) - 1u)
#define OT_AES_DATA_BM_MASK     ((1u << (PARAM_NUM_REGS_DATA)) - 1u)

#define OT_AES_ALERT_STATUS_OFFSET \
    (R_STATUS_ALERT_RECOV_CTRL_UPDATE_ERR_SHIFT - \
     ALERT_RECOV_CTRL_UPDATE_ERR_SHIFT)

#define OT_AES_DATA_SIZE (PARAM_NUM_REGS_DATA * sizeof(uint32_t))
#define OT_AES_IV_SIZE   (PARAM_NUM_REGS_IV * sizeof(uint32_t))

static_assert(OT_AES_KEY_SIZE == (PARAM_NUM_REGS_KEY * sizeof(uint32_t)),
              "Invalid key size");

#define OT_AES_CLOCK_ACTIVE "clock-active"
#define OT_AES_CLOCK_INPUT  "clock-in"

/* arbitrary value long enough to give back execution to vCPU */
#define OT_AES_RETARD_DELAY_NS 10000u /* 10 us */

/* size of the debug buffer: hex key + NUL + extra */
#define AES_DEBUG_HEXBUF_SIZE (OT_AES_DATA_SIZE * 2u + 4u)

#define R_LAST_REG (R_STATUS)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) < REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(KEY_SHARE0_0),
    REG_NAME_ENTRY(KEY_SHARE0_1),
    REG_NAME_ENTRY(KEY_SHARE0_2),
    REG_NAME_ENTRY(KEY_SHARE0_3),
    REG_NAME_ENTRY(KEY_SHARE0_4),
    REG_NAME_ENTRY(KEY_SHARE0_5),
    REG_NAME_ENTRY(KEY_SHARE0_6),
    REG_NAME_ENTRY(KEY_SHARE0_7),
    REG_NAME_ENTRY(KEY_SHARE1_0),
    REG_NAME_ENTRY(KEY_SHARE1_1),
    REG_NAME_ENTRY(KEY_SHARE1_2),
    REG_NAME_ENTRY(KEY_SHARE1_3),
    REG_NAME_ENTRY(KEY_SHARE1_4),
    REG_NAME_ENTRY(KEY_SHARE1_5),
    REG_NAME_ENTRY(KEY_SHARE1_6),
    REG_NAME_ENTRY(KEY_SHARE1_7),
    REG_NAME_ENTRY(IV_0),
    REG_NAME_ENTRY(IV_1),
    REG_NAME_ENTRY(IV_2),
    REG_NAME_ENTRY(IV_3),
    REG_NAME_ENTRY(DATA_IN_0),
    REG_NAME_ENTRY(DATA_IN_1),
    REG_NAME_ENTRY(DATA_IN_2),
    REG_NAME_ENTRY(DATA_IN_3),
    REG_NAME_ENTRY(DATA_OUT_0),
    REG_NAME_ENTRY(DATA_OUT_1),
    REG_NAME_ENTRY(DATA_OUT_2),
    REG_NAME_ENTRY(DATA_OUT_3),
    REG_NAME_ENTRY(CTRL_SHADOWED),
    REG_NAME_ENTRY(CTRL_AUX_SHADOWED),
    REG_NAME_ENTRY(CTRL_AUX_REGWEN),
    REG_NAME_ENTRY(TRIGGER),
    REG_NAME_ENTRY(STATUS),
};
#undef REG_NAME_ENTRY

static const char *OT_AES_MODE_NAMES[6u] = {
    "NONE", "ECB", "CBC", "CFB", "OFB", "CTR",
};

#define OT_AES_KEY_DWORD_COUNT (OT_AES_KEY_SIZE / sizeof(uint64_t))
#define OT_AES_IV_DWORD_COUNT  (OT_AES_IV_SIZE / sizeof(uint64_t))

typedef struct OtAESRegisters {
    /* public registers */
    uint32_t keyshare[PARAM_NUM_REGS_KEY * 2u]; /* wo */
    uint32_t iv[PARAM_NUM_REGS_IV]; /* rw */
    uint32_t data_in[PARAM_NUM_REGS_DATA]; /* wo */
    uint32_t data_out[PARAM_NUM_REGS_DATA]; /* ro */
    OtShadowReg ctrl;
    OtShadowReg ctrl_aux;
    uint32_t ctrl_aux_regwen;
    uint32_t trigger;
    uint32_t status;

    /* implementation */
    DECLARE_BITMAP(keyshare_bm, (uint64_t)(PARAM_NUM_REGS_KEY * 2u));
    DECLARE_BITMAP(iv_bm, PARAM_NUM_REGS_IV);
    DECLARE_BITMAP(data_in_bm, PARAM_NUM_REGS_DATA);
    DECLARE_BITMAP(data_out_bm, PARAM_NUM_REGS_DATA);
    uint32_t key[PARAM_NUM_REGS_KEY];
    bool data_out_rdy; /* AES output data exist, not yet published */
} OtAESRegisters;

typedef struct OtAESContext {
    union {
        symmetric_ECB ecb;
        symmetric_CFB cfb;
        symmetric_OFB ofb;
        symmetric_CBC cbc;
        symmetric_CTR ctr;
    };
    uint64_t key[OT_AES_KEY_DWORD_COUNT];
    uint64_t iv[OT_AES_IV_DWORD_COUNT];
    uint8_t src[OT_AES_DATA_SIZE];
    uint8_t dst[OT_AES_DATA_SIZE];
    bool key_ready; /* Key has been fully loaded */
    bool iv_ready; /* IV has been fully loaded */
    bool di_full; /* Input DATA FIFO fully filled */
    bool do_full; /* Output DATA FIFO not empty */
    int aes_cipher; /* AES handle for tomcrypt */
} OtAESContext;

typedef struct OtAESEDN {
    OtEDNState *device;
    uint8_t ep;
    bool connected;
    bool scheduled;
} OtAESEDN;

typedef struct {
    uint64_t share0[OT_AES_KEY_DWORD_COUNT];
    uint64_t share1[OT_AES_KEY_DWORD_COUNT];
    bool valid;
} OtAESKey;

typedef enum {
    AES_NONE,
    AES_ECB,
    AES_CBC,
    AES_CFB,
    AES_OFB,
    AES_CTR,
} OtAESMode;

struct OtAESState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    IbexIRQ alerts[PARAM_NUM_ALERTS];
    IbexIRQ clock_active;
    QEMUBH *process_bh;
    QEMUTimer *retard_timer; /* only used with disabled fast-mode */

    OtAESRegisters *regs;
    OtAESContext *ctx;
    OtAESKey *sl_key;
    OtAESEDN edn;
    OtPrngState *prng;
    const char *clock_src_name; /* IRQ name once connected */
    char *hexstr;
    unsigned pclk; /* Current input clock */
    unsigned reseed_count;
    bool fast_mode;

    char *ot_id;
    char *clock_name;
    DeviceState *clock_src;
};

struct OtAESClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

#ifdef DEBUG_AES
#define trace_ot_aes_buf(_s_, _a_, _m_, _b_) \
    trace_ot_aes_buffer((_s_)->ot_id, (_a_), (_m_), \
                        ot_common_uhexdump((const uint8_t *)(_b_), \
                                           OT_AES_DATA_SIZE, false, \
                                           (_s_)->hexstr, \
                                           AES_DEBUG_HEXBUF_SIZE))
#define trace_ot_aes_key(_s_, _a_, _b_, _l_) \
    trace_ot_aes_buffer((_s_)->ot_id, (_a_), "key", \
                        ot_common_uhexdump((const uint8_t *)(_b_), (_l_), \
                                           false, (_s_)->hexstr, \
                                           AES_DEBUG_HEXBUF_SIZE))
#define trace_ot_aes_iv(_s_, _a_, _b_) \
    trace_ot_aes_buffer((_s_)->ot_id, (_a_), "iv", \
                        ot_common_uhexdump((const uint8_t *)(_b_), \
                                           OT_AES_IV_SIZE, false, \
                                           (_s_)->hexstr, \
                                           AES_DEBUG_HEXBUF_SIZE))
#else
#define trace_ot_aes_buf(_s_, _a_, _m_, _b_)
#define trace_ot_aes_key(_s_, _a_, _b_, _l_)
#define trace_ot_aes_iv(_s_, _a_, _b_)
#endif /* DEBUG_AES */
#define xtrace_ot_aes_debug(_otid_, _msg_) \
    trace_ot_aes_debug(__func__, __LINE__, _otid_, _msg_)
#define xtrace_ot_aes_info(_otid_, _msg_) \
    trace_ot_aes_info(__func__, __LINE__, _otid_, _msg_)
#define xtrace_ot_aes_error(_otid_, _msg_) \
    trace_ot_aes_error(__func__, __LINE__, _otid_, _msg_)

static void ot_aes_reseed(OtAESState *s);

static void ot_aes_randomize(OtAESState *s, uint32_t *regs, size_t count)
{
    ot_prng_random_u32_array(s->prng, regs, count);
}

static inline size_t ot_aes_get_key_length(OtAESRegisters *r)
{
    uint32_t ctrl = ot_shadow_reg_peek(&r->ctrl);
    switch (FIELD_EX32(ctrl, CTRL_SHADOWED, KEY_LEN)) {
    case 0x01u:
        return 16u; /* 128 bits */
    case 0x02u:
        return 24u; /* 192 bits */
    case 0x04u:
    default:
        return 32u; /* 256bits */
    }
};

static inline uint32_t ot_aes_get_key_mask(OtAESRegisters *r)
{
    uint32_t ctrl = ot_shadow_reg_peek(&r->ctrl);
    switch (FIELD_EX32(ctrl, CTRL_SHADOWED, KEY_LEN)) {
    case 0x01u:
        return 0x0fu; /* 128 bits, 16 bytes, 4 words */
    case 0x02u:
        return 0x3fu; /* 192 bits, 24 bytes, 6 words */
    case 0x04u:
    default:
        return 0xffu; /* 256bits, 32 bytes, 8 words */
    }
};

static void ot_aes_update_alert(OtAESState *s)
{
    for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
        bool level =
            (bool)(s->regs->status & (1u << (ix + OT_AES_ALERT_STATUS_OFFSET)));
        ibex_irq_set(&s->alerts[ix], (int)level);
    }
}

static inline bool ot_aes_is_manual(OtAESRegisters *r)
{
    uint32_t ctrl = ot_shadow_reg_peek(&r->ctrl);
    return FIELD_EX32(ctrl, CTRL_SHADOWED, MANUAL_OPERATION) == 1u;
}

static bool ot_aes_is_idle(OtAESState *s)
{
    return s->regs->trigger == 0u &&
           /* retard_timer is never used if fast_mode is enabled */
           (s->fast_mode || !timer_pending(s->retard_timer));
}

static inline bool ot_aes_is_encryption(OtAESRegisters *r)
{
    uint32_t ctrl = ot_shadow_reg_peek(&r->ctrl);
    return FIELD_EX32(ctrl, CTRL_SHADOWED, OPERATION) != 0x2u;
}

static inline OtAESMode ot_aes_get_mode(OtAESRegisters *r)
{
    uint32_t ctrl = ot_shadow_reg_peek(&r->ctrl);
    switch (FIELD_EX32(ctrl, CTRL_SHADOWED, MODE)) {
    case 0x01u:
        return AES_ECB;
    case 0x02u:
        return AES_CBC;
    case 0x04u:
        return AES_CFB;
    case 0x08u:
        return AES_OFB;
    case 0x10u:
        return AES_CTR;
    case 0x20u:
    default:
        return AES_NONE;
    }
};

static inline void ot_aes_load_reseed_rate(OtAESState *s)
{
    OtAESRegisters *r = s->regs;
    uint32_t ctrl = ot_shadow_reg_peek(&r->ctrl);
    uint32_t rate = FIELD_EX32(ctrl, CTRL_SHADOWED, PRNG_RESEED_RATE);
    unsigned reseed;

    switch (rate) {
    case 0x3u:
        /* should be "approximately" 8192 */
        reseed = 8192u;
        break;
    case 0x2u:
        /* should be "approximately" 64 */
        reseed = 64u;
        break;
    case 0x1u:
    case 0x0u:
    default:
        reseed = 1u;
        break;
    }

    trace_ot_aes_reseed_rate(s->ot_id, reseed);

    s->reseed_count = reseed;
}

static inline bool ot_aes_is_sideload(OtAESRegisters *r)
{
    uint32_t ctrl = ot_shadow_reg_peek(&r->ctrl);
    return FIELD_EX32(ctrl, CTRL_SHADOWED, SIDELOAD) == 1u;
}

static inline bool ot_aes_is_data_in_ready(OtAESRegisters *r)
{
    return (r->data_in_bm[0] & OT_AES_DATA_BM_MASK) == OT_AES_DATA_BM_MASK;
}

static inline bool ot_aes_key_touch_force_reseed(OtAESRegisters *r)
{
    return (bool)(ot_shadow_reg_peek(&r->ctrl_aux) &
                  R_CTRL_AUX_SHADOWED_KEY_TOUCH_FORCES_RESEED_MASK);
}

static void ot_aes_init_keyshare(OtAESState *s, bool randomize)
{
    OtAESRegisters *r = s->regs;
    OtAESContext *c = s->ctx;

    if (randomize) {
        trace_ot_aes_init(s->ot_id, "keyshare init (randomize data)");
        ot_aes_randomize(s, r->keyshare, ARRAY_SIZE(r->keyshare));
    } else {
        trace_ot_aes_init(s->ot_id, "keyshare init (data preserved)");
    }
    bitmap_zero(r->keyshare_bm, (int64_t)(PARAM_NUM_REGS_KEY * 2u));
    c->key_ready = false;
}

static void ot_aes_init_iv(OtAESState *s, bool randomize)
{
    OtAESRegisters *r = s->regs;
    OtAESContext *c = s->ctx;

    if (randomize) {
        trace_ot_aes_init(s->ot_id, "iv init (randomize data)");
        ot_aes_randomize(s, r->iv, ARRAY_SIZE(r->iv));
    } else {
        trace_ot_aes_init(s->ot_id, "iv init (data preserved)");
    }
    bitmap_zero(r->iv_bm, PARAM_NUM_REGS_IV);
    c->iv_ready = false;
}

static void ot_aes_init_data(OtAESState *s, bool io)
{
    OtAESRegisters *r = s->regs;

    if (!io) {
        trace_ot_aes_init(s->ot_id, "data_in");
        ot_aes_randomize(s, r->data_in, ARRAY_SIZE(r->data_in));
        bitmap_zero(r->data_in_bm, PARAM_NUM_REGS_DATA);
    } else {
        trace_ot_aes_init(s->ot_id, "data_out");
        ot_aes_randomize(s, r->data_out, ARRAY_SIZE(r->data_out));
        bitmap_zero(r->data_out_bm, PARAM_NUM_REGS_DATA);
    }
}

static bool ot_aes_is_mode_ready(OtAESRegisters *r, bool *need_iv)
{
    OtAESMode mode = ot_aes_get_mode(r);

    switch (mode) {
    case AES_ECB:
        if (need_iv) {
            *need_iv = false;
        }
        return true;
    case AES_CBC:
    case AES_CFB:
    case AES_OFB:
    case AES_CTR:
        if (need_iv) {
            *need_iv = true;
        }
        return true;
    case AES_NONE:
    default:
        return false;
    }
}

static void ot_aes_trigger_reseed(OtAESState *s)
{
    OtAESRegisters *r = s->regs;
    uint32_t ctrl_aux = ot_shadow_reg_peek(&r->ctrl_aux);

    if (!(ctrl_aux & R_CTRL_AUX_SHADOWED_FORCE_MASKS_MASK)) {
        ot_aes_reseed(s);
    } else {
        r->trigger &= ~R_TRIGGER_PRNG_RESEED_MASK;
        xtrace_ot_aes_info(s->ot_id, "reseed on trigger disabled");
    }
}

static void ot_aes_sideload_key(OtAESState *s)
{
    OtAESKey *key = s->sl_key;
    OtAESContext *c = s->ctx;

    for (unsigned ix = 0u; ix < OT_AES_KEY_DWORD_COUNT; ix++) {
        c->key[ix] = key->share0[ix] ^ key->share1[ix];
    }

    c->key_ready = key->valid;
}

static void ot_aes_update_key(OtAESState *s)
{
    OtAESRegisters *r = s->regs;

    uint32_t key_mask;
    if (ot_aes_is_manual(r)) {
        key_mask = ot_aes_get_key_mask(r);
    } else {
        key_mask = (1u << (PARAM_NUM_REGS_KEY * 2u)) - 1u;
    }
    if ((r->keyshare_bm[0u] & key_mask) != key_mask) {
        /* missing key parts, nothing to do */
        return;
    }

    OtAESContext *c = s->ctx;

    for (unsigned ix = 0u; ix < ARRAY_SIZE(r->keyshare) / 2u; ix += 2u) {
        uint64_t key;

        key = (uint64_t)(r->keyshare[ix + 1u] ^
                         r->keyshare[PARAM_NUM_REGS_KEY + ix + 1u]);
        key <<= 32u;
        key |= (uint64_t)(r->keyshare[ix + 0u] ^
                          r->keyshare[PARAM_NUM_REGS_KEY + ix + 0u]);

        c->key[ix >> 1u] = key;
    }

    if (!c->key_ready && ot_aes_key_touch_force_reseed(r)) {
        r->trigger |= R_TRIGGER_PRNG_RESEED_MASK;
        trace_ot_aes_reseed(s->ot_id, "new key");
        ot_aes_trigger_reseed(s);
    }

    c->key_ready = true;
}

static void ot_aes_update_iv(OtAESState *s)
{
    OtAESRegisters *r = s->regs;

    if ((r->iv_bm[0u] & OT_AES_IV_BM_MASK) != OT_AES_IV_BM_MASK) {
        /* missing key parts, nothing to do */
        return;
    }

    OtAESContext *c = s->ctx;

    for (unsigned ix = 0u; ix < ARRAY_SIZE(r->iv); ix += 2u) {
        c->iv[ix >> 1u] =
            ((uint64_t)(r->iv[ix + 1u])) << 32u | (uint64_t)r->iv[ix + 0u];
    }

    c->iv_ready = true;
}

static inline bool ot_aes_can_process(const OtAESState *s)
{
    OtAESRegisters *r = s->regs;
    bool need_iv;

    if (!ot_aes_is_mode_ready(r, &need_iv)) {
        xtrace_ot_aes_debug(s->ot_id, "mode not ready");
        return false;
    }

    OtAESContext *c = s->ctx;

    if (need_iv && !c->iv_ready) {
        xtrace_ot_aes_debug(s->ot_id, "IV not ready");
        return false;
    }

    if (ot_aes_is_manual(r)) {
        if (!(r->trigger & R_TRIGGER_START_MASK)) {
            /* cannot execute in manual mode w/o an explicit trigger */
            xtrace_ot_aes_debug(s->ot_id, "manual not triggered");
            return false;
        }

        /* No other checks for inputs or key validity in manual mode */
        return true;
    }
    /* auto mode */

    if (!c->key_ready) {
        xtrace_ot_aes_debug(s->ot_id, "key not ready");
        return false;
    }

    if (c->do_full) {
        /* cannot schedule a round if output FIFO has not been emptied */
        xtrace_ot_aes_debug(s->ot_id, "DO full");
        return false;
    }

    if (!c->di_full) {
        xtrace_ot_aes_debug(s->ot_id, "DI not filled");
    }

    return c->di_full;
}

static void ot_aes_handle_trigger(OtAESState *s)
{
    /*
     * @todo looks like these operations needs to use a background worker
     * as IDLE may need to be actionned
     */
    OtAESRegisters *r = s->regs;

    ibex_irq_set(&s->clock_active, (int)true);

    if (r->trigger & R_TRIGGER_PRNG_RESEED_MASK) {
        trace_ot_aes_reseed(s->ot_id, "trigger write");
        ot_aes_trigger_reseed(s);
        if (s->edn.scheduled) {
            xtrace_ot_aes_debug(s->ot_id, "EDN scheduled, defer");
            return;
        }
    }

    if (r->trigger & R_TRIGGER_KEY_IV_DATA_IN_CLEAR_MASK) {
        ot_aes_init_keyshare(s, true);
        ot_aes_init_iv(s, true);
        ot_aes_init_data(s, false);
        r->trigger &= ~R_TRIGGER_KEY_IV_DATA_IN_CLEAR_MASK;
    }

    if (r->trigger & R_TRIGGER_DATA_OUT_CLEAR_MASK) {
        ot_aes_init_data(s, true);
        r->trigger &= ~R_TRIGGER_DATA_OUT_CLEAR_MASK;
    }

    if (r->trigger & R_TRIGGER_START_MASK) {
        if (ot_aes_get_mode(r) == AES_NONE || !ot_aes_is_manual(r)) {
            /* ignore */
            xtrace_ot_aes_debug(s->ot_id, "start trigger ignored");
            return;
        }
    }

    /* an AES round might have been delayed */
    if (ot_aes_can_process(s)) {
        trace_ot_aes_schedule(s->ot_id);
        qemu_bh_schedule(s->process_bh);
    }

    xtrace_ot_aes_debug(s->ot_id, ot_aes_is_idle(s) ? "IDLE" : "NOT IDLE");
    ibex_irq_set(&s->clock_active, (int)!ot_aes_is_idle(s));
}

static void ot_aes_update_config(OtAESState *s)
{
    OtAESRegisters *r = s->regs;

    bool need_iv;

    xtrace_ot_aes_debug(s->ot_id, "CONFIG");

    if (!ot_aes_is_mode_ready(r, &need_iv)) {
        return;
    }

    OtAESContext *c = s->ctx;

    if (!ot_aes_is_manual(s->regs) && !c->key_ready) {
        return;
    }

    if (need_iv && !c->iv_ready) {
        return;
    }

    size_t key_size = ot_aes_get_key_length(r);
    OtAESMode mode = ot_aes_get_mode(r);

    int rc;

    switch (mode) {
    case AES_ECB:
        rc = ecb_start(c->aes_cipher, (const uint8_t *)c->key, (int)key_size, 0,
                       &c->ecb);
        break;
    case AES_CBC:
        rc = cbc_start(c->aes_cipher, (const uint8_t *)c->iv,
                       (const uint8_t *)c->key, (int)key_size, 0, &c->cbc);
        break;
    case AES_CFB:
        rc = cfb_start(c->aes_cipher, (const uint8_t *)c->iv,
                       (const uint8_t *)c->key, (int)key_size, 0, &c->cfb);
        break;
    case AES_OFB:
        rc = ofb_start(c->aes_cipher, (const uint8_t *)c->iv,
                       (const uint8_t *)c->key, (int)key_size, 0, &c->ofb);
        break;
    case AES_CTR:
        rc = ctr_start(c->aes_cipher, (const uint8_t *)c->iv,
                       (const uint8_t *)c->key, (int)key_size, 0,
                       CTR_COUNTER_BIG_ENDIAN, &c->ctr);
        break;
    default:
        /* cannot happen */
        return;
    }

    trace_ot_aes_key(s, OT_AES_MODE_NAMES[mode], c->key, key_size);
    trace_ot_aes_iv(s, OT_AES_MODE_NAMES[mode], c->iv);

    if (rc != CRYPT_OK) {
        error_report("OpenTitan AES [%s]: Unable to initialize AES API: %d",
                     OT_AES_MODE_NAMES[mode], rc);
        /* @todo how to report this? */
    }
}

static void ot_aes_finalize(OtAESState *s, OtAESMode mode)
{
    int rc;

    OtAESContext *c = s->ctx;

    /*
     * libtomcrypt's cipher filed needs to be tested, as the initialization
     * function may have never been called prior to the finalization, in which
     * case the matching *_done function should not be called
     */

    switch (mode) {
    case AES_NONE:
        return;
    case AES_ECB:
        rc = c->ecb.cipher == c->aes_cipher ? ecb_done(&c->ecb) : CRYPT_OK;
        break;
    case AES_CBC:
        rc = c->cbc.cipher == c->aes_cipher ? cbc_done(&c->cbc) : CRYPT_OK;
        break;
    case AES_CFB:
        rc = c->cfb.cipher == c->aes_cipher ? cfb_done(&c->cfb) : CRYPT_OK;
        break;
    case AES_OFB:
        rc = c->ofb.cipher == c->aes_cipher ? ofb_done(&c->ofb) : CRYPT_OK;
        break;
    case AES_CTR:
        rc = c->ctr.cipher == c->aes_cipher ? ctr_done(&c->ctr) : CRYPT_OK;
        break;
    default:
        /* cannot happen */
        return;
    }

    if (rc != CRYPT_OK) {
        error_report("OpenTitan AES [%s]: Unable to finalize AES API: %d",
                     OT_AES_MODE_NAMES[mode], rc);
        /* @todo how to report this? */
    }

    c->di_full = false;
    c->do_full = false;
}

static void ot_aes_compute_ctr_iv(OtAESState *s, uint8_t *iv)
{
    OtAESContext *c = s->ctx;
    uint8_t liv[OT_AES_IV_SIZE];

    unsigned long length = OT_AES_IV_SIZE;
    ctr_getiv(liv, &length, &c->ctr);

    g_assert(c->ctr.mode == CTR_COUNTER_BIG_ENDIAN);
    g_assert(c->ctr.ctrlen == 0);

    unsigned ix = OT_AES_IV_SIZE - 1u;
    do {
        liv[ix] = liv[ix] + 0x1u;
        if (liv[ix] != 0) {
            break;
        }
    } while (ix--);

    memcpy(iv, liv, sizeof(liv));
}

static void ot_aes_pop(OtAESState *s)
{
    OtAESRegisters *r = s->regs;
    OtAESContext *c = s->ctx;

    memcpy(c->src, r->data_in, sizeof(c->src));
    bitmap_zero(r->data_in_bm, PARAM_NUM_REGS_DATA);

    c->di_full = true;
}

static void ot_aes_push(OtAESState *s)
{
    OtAESRegisters *r = s->regs;
    OtAESContext *c = s->ctx;

    memcpy(r->data_out, c->dst, sizeof(c->dst));
    memcpy(r->iv, c->iv, sizeof(c->iv));
    r->data_out_rdy = true;
}

static void ot_aes_process(OtAESState *s)
{
    OtAESRegisters *r = s->regs;
    OtAESContext *c = s->ctx;

    OtAESMode mode = ot_aes_get_mode(s->regs);
    bool encrypt = ot_aes_is_encryption(r);

    int rc;

    xtrace_ot_aes_debug(s->ot_id, "process");

    if (encrypt) {
        trace_ot_aes_buf(s, OT_AES_MODE_NAMES[mode], "enc/in ", c->src);
        switch (mode) {
        case AES_ECB:
            rc = ecb_encrypt(c->src, c->dst, OT_AES_DATA_SIZE, &c->ecb);
            break;
        case AES_CBC:
            rc = cbc_encrypt(c->src, c->dst, OT_AES_DATA_SIZE, &c->cbc);
            break;
        case AES_CFB:
            rc = cfb_encrypt(c->src, c->dst, OT_AES_DATA_SIZE, &c->cfb);
            break;
        case AES_OFB:
            rc = ofb_encrypt(c->src, c->dst, OT_AES_DATA_SIZE, &c->ofb);
            break;
        case AES_CTR:
            rc = ctr_encrypt(c->src, c->dst, OT_AES_DATA_SIZE, &c->ctr);
            break;
        case AES_NONE:
        default:
            rc = CRYPT_ERROR;
            break;
        }
        trace_ot_aes_buf(s, OT_AES_MODE_NAMES[mode], "enc/out", c->dst);
    } else {
        trace_ot_aes_buf(s, OT_AES_MODE_NAMES[mode], "dec/in ", c->src);
        switch (mode) {
        case AES_ECB:
            rc = ecb_decrypt(c->src, c->dst, OT_AES_DATA_SIZE, &c->ecb);
            break;
        case AES_CBC:
            rc = cbc_decrypt(c->src, c->dst, OT_AES_DATA_SIZE, &c->cbc);
            break;
        case AES_CFB:
            rc = cfb_decrypt(c->src, c->dst, OT_AES_DATA_SIZE, &c->cfb);
            break;
        case AES_OFB:
            rc = ofb_decrypt(c->src, c->dst, OT_AES_DATA_SIZE, &c->ofb);
            break;
        case AES_CTR:
            rc = ctr_decrypt(c->src, c->dst, OT_AES_DATA_SIZE, &c->ctr);
            break;
        case AES_NONE:
        default:
            rc = CRYPT_ERROR;
            break;
        }
        trace_ot_aes_buf(s, OT_AES_MODE_NAMES[mode], "dec/out", c->dst);
    }

    c->di_full = false;

    if (rc == CRYPT_OK) {
        /*
         * IV registers are updated on each round. For details, see:
         * https://opentitan.org/book/hw/ip/aes/doc/theory_of_operation.html
         * https://en.wikipedia.org/wiki/Block_cipher_mode_of_operation
         */
        switch (mode) {
        case AES_CBC:
            memcpy(c->iv, c->cbc.IV, sizeof(c->iv));
            break;
        case AES_CFB:
            /* In CFB mode the next IV register value is the ciphertext */
            memcpy(c->iv, encrypt ? c->dst : c->src, sizeof(c->iv));
            break;
        case AES_OFB:
            memcpy(c->iv, c->ofb.IV, sizeof(c->iv));
            break;
        case AES_CTR:
            ot_aes_compute_ctr_iv(s, (uint8_t *)&c->iv[0]);
            break;
        default:
            break;
        }
        c->do_full = true;
    } else {
        error_report("OpenTitan AES [%s]: Unable to run AES: %d",
                     OT_AES_MODE_NAMES[mode], rc);
        /* @todo how to report this? */
    }
}

static void ot_aes_commit_data_out(OtAESRegisters *r)
{
    if (r->data_out_rdy) {
        bitmap_fill(r->data_out_bm, PARAM_NUM_REGS_DATA);
        r->status |= R_STATUS_OUTPUT_VALID_MASK;
        r->data_out_rdy = false;
    }
}

static inline void ot_aes_do_process(OtAESState *s)
{
    ot_aes_process(s);
    ot_aes_push(s);
    if (s->reseed_count) {
        s->reseed_count -= 1u;
    }
    if (!s->reseed_count) {
        /*
         * delay availability of pushed data till completion of reseed
         * otherwise, IDLE status may be false once the vCPU has read the data,
         * which would not match the HW behavior
         */
        trace_ot_aes_reseed(s->ot_id, "reseed_count reached");
        s->regs->trigger |= R_TRIGGER_PRNG_RESEED_MASK;
        ot_aes_trigger_reseed(s);
        ot_aes_load_reseed_rate(s);
    } else {
        /* flag pushed data as immediately available */
        ot_aes_commit_data_out(s->regs);
    }

    OtAESRegisters *r = s->regs;
    if (ot_aes_is_manual(r)) {
        xtrace_ot_aes_info(s->ot_id, "end of manual seq");
        s->regs->trigger &= ~R_TRIGGER_START_MASK;
    }
}

static void ot_aes_process_cond(OtAESState *s)
{
    if (!s->edn.scheduled) {
        if (ot_aes_can_process(s)) {
            if (!s->fast_mode) {
                /*
                 * if fast mode is disabled, delay execution of AES round
                 * to give back execution to vCPU using a short timer. This
                 * allows the vCPU to run and time-sensitive guest code in
                 * OpenTitan libraries to poll for HW status, at the expense of
                 * AES throughput.
                 */
                timer_del(s->retard_timer);
                uint64_t now = qemu_clock_get_ns(OT_VIRTUAL_CLOCK);
                timer_mod(s->retard_timer,
                          (int64_t)(now + OT_AES_RETARD_DELAY_NS));
            } else {
                /*
                 * if fast mode is enabled, vCPU cannot resume execution before
                 * the AES round is completed, which increase throughput and
                 * reduce latency for AES compution, but may fail time-sensitive
                 * OpenTitan library that expect the real CPU to execute code
                 * while AES is performing in HW.
                 */
                ot_aes_do_process(s);
            }
        }
    } else {
        xtrace_ot_aes_info(s->ot_id, "defer exec, waiting for EDN");
    }
}

static void ot_aes_fill_entropy(void *opaque, uint32_t bits, bool fips)
{
    /* called from EDN */
    (void)fips;

    OtAESState *s = opaque;
    OtAESEDN *edn = &s->edn;
    OtAESRegisters *r = s->regs;

    if (!edn->scheduled) {
        xtrace_ot_aes_error(s->ot_id, "unexpected entropy");
        return;
    }
    trace_ot_aes_fill_entropy(s->ot_id, bits, fips);
    edn->scheduled = false;
    r->trigger &= ~R_TRIGGER_PRNG_RESEED_MASK;

    /*
     * if a previous AES data output generation had completed, flag the output
     * as valid, as this state was delayed till entropy collection was
     * completed to maintain a coherent IDLE state.
     */
    ot_aes_commit_data_out(r);

    ot_prng_reseed(s->prng, bits);

    ot_aes_handle_trigger(s);
}

static void ot_aes_handle_process(void *opaque)
{
    OtAESState *s = opaque;

    ot_aes_do_process(s);
}

static void ot_aes_reseed(OtAESState *s)
{
    OtAESEDN *edn = &s->edn;

    if (!edn->connected) {
        ot_edn_connect_endpoint(edn->device, edn->ep, &ot_aes_fill_entropy, s);
        edn->connected = true;
    }
    if (!edn->scheduled) {
        trace_ot_aes_request_entropy(s->ot_id);
        if (!ot_edn_request_entropy(edn->device, edn->ep)) {
            edn->scheduled = true;
        } else {
            xtrace_ot_aes_error(s->ot_id, "cannot request new entropy");
        }
    }
}

static void ot_aes_clock_input(void *opaque, int irq, int level)
{
    OtAESState *s = opaque;

    g_assert(irq == 0);

    s->pclk = (unsigned)level;

    /* TODO: disable AES execution when PCLK is 0 */
}

static void ot_aes_push_key(OtKeySinkIf *ifd, const uint8_t *share0,
                            const uint8_t *share1, size_t key_len, bool valid)
{
    g_assert(!key_len || key_len == OT_AES_KEY_SIZE);

    OtAESState *s = OT_AES(ifd);
    OtAESKey *key = s->sl_key;

    if (key_len && share0) {
        memcpy(key->share0, share0, key_len);
    } else {
        memset(key->share0, 0, OT_AES_KEY_SIZE);
    }
    if (key_len && share1) {
        memcpy(key->share1, share1, key_len);
    } else {
        memset(key->share1, 0, OT_AES_KEY_SIZE);
    }
    key->valid = valid;

    if (ot_aes_is_sideload(s->regs)) {
        ot_aes_sideload_key(s);
        ot_aes_update_config(s);
    }
}

static uint64_t ot_aes_read(void *opaque, hwaddr addr, unsigned size)
{
    OtAESState *s = opaque;
    (void)size;
    OtAESRegisters *r = s->regs;

    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_ALERT_TEST:
    case R_KEY_SHARE0_0:
    case R_KEY_SHARE0_1:
    case R_KEY_SHARE0_2:
    case R_KEY_SHARE0_3:
    case R_KEY_SHARE0_4:
    case R_KEY_SHARE0_5:
    case R_KEY_SHARE0_6:
    case R_KEY_SHARE0_7:
    case R_KEY_SHARE1_0:
    case R_KEY_SHARE1_1:
    case R_KEY_SHARE1_2:
    case R_KEY_SHARE1_3:
    case R_KEY_SHARE1_4:
    case R_KEY_SHARE1_5:
    case R_KEY_SHARE1_6:
    case R_KEY_SHARE1_7:
    case R_DATA_IN_0:
    case R_DATA_IN_1:
    case R_DATA_IN_2:
    case R_DATA_IN_3:
    case R_TRIGGER:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: W/O register 0x%02" HWADDR_PRIx " (%s)\n",
                      __func__, s->ot_id, addr, REG_NAME(reg));
        val32 = 0u;
        break;
    case R_IV_0:
    case R_IV_1:
    case R_IV_2:
    case R_IV_3:
        val32 = r->iv[reg - R_IV_0];
        break;
    case R_DATA_OUT_0:
    case R_DATA_OUT_1:
    case R_DATA_OUT_2:
    case R_DATA_OUT_3:
        val32 = r->data_out[reg - R_DATA_OUT_0];
        clear_bit((int64_t)(reg - R_DATA_OUT_0), r->data_out_bm);
        if (bitmap_empty(r->data_out_bm, PARAM_NUM_REGS_DATA)) {
            r->status &= ~R_STATUS_OUTPUT_VALID_MASK;
            s->ctx->do_full = false;
            ot_aes_process_cond(s);
        }
        break;
    case R_CTRL_SHADOWED:
        val32 = ot_shadow_reg_read(&r->ctrl);
        break;
    case R_CTRL_AUX_SHADOWED:
        val32 = ot_shadow_reg_read(&r->ctrl_aux);
        break;
    case R_CTRL_AUX_REGWEN:
        val32 = r->ctrl_aux_regwen;
        break;
    case R_STATUS:
        val32 = r->status;
        if (ot_aes_is_idle(s)) {
            val32 |= R_STATUS_IDLE_MASK;
        }
        if (!ot_aes_is_data_in_ready(r)) {
            val32 |= R_STATUS_INPUT_READY_MASK;
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Bad offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, addr);
        val32 = 0u;
        break;
    }

    uint32_t pc = ibex_get_current_pc();
    trace_ot_aes_io_read_out(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32,
                             pc);

    return (uint64_t)val32;
};

static void ot_aes_write(void *opaque, hwaddr addr, uint64_t val64,
                         unsigned size)
{
    OtAESState *s = opaque;
    (void)size;
    OtAESRegisters *r = s->regs;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_aes_io_write(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32, pc);

    switch (reg) {
    case R_ALERT_TEST:
        if (val32 & ALERT_RECOV_CTRL_UPDATE_ERR_MASK) {
            r->status |= R_STATUS_ALERT_RECOV_CTRL_UPDATE_ERR_MASK;
        }
        if (val32 & ALERT_FATAL_FAULT_MASK) {
            r->status |= R_STATUS_ALERT_FATAL_FAULT_MASK;
        }
        ot_aes_update_alert(s);
        break;
    case R_DATA_OUT_0:
    case R_DATA_OUT_1:
    case R_DATA_OUT_2:
    case R_DATA_OUT_3:
    case R_STATUS:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: R/O register 0x%02" HWADDR_PRIx " (%s)\n",
                      __func__, s->ot_id, addr, REG_NAME(reg));
        break;
    case R_KEY_SHARE0_0:
    case R_KEY_SHARE0_1:
    case R_KEY_SHARE0_2:
    case R_KEY_SHARE0_3:
    case R_KEY_SHARE0_4:
    case R_KEY_SHARE0_5:
    case R_KEY_SHARE0_6:
    case R_KEY_SHARE0_7:
    case R_KEY_SHARE1_0:
    case R_KEY_SHARE1_1:
    case R_KEY_SHARE1_2:
    case R_KEY_SHARE1_3:
    case R_KEY_SHARE1_4:
    case R_KEY_SHARE1_5:
    case R_KEY_SHARE1_6:
    case R_KEY_SHARE1_7:
        if (ot_aes_is_sideload(r)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: key share disabled, sideload is active\n",
                          __func__, s->ot_id);
            break;
        }
        if (ot_aes_is_idle(s)) {
            r->keyshare[reg - R_KEY_SHARE0_0] = val32;
            set_bit((int64_t)(reg - R_KEY_SHARE0_0), r->keyshare_bm);
            ot_aes_update_key(s);
            ot_aes_update_config(s);
        }
        break;
    case R_IV_0:
    case R_IV_1:
    case R_IV_2:
    case R_IV_3:
        if (ot_aes_is_idle(s)) {
            r->iv[reg - R_IV_0] = val32;
            set_bit((int64_t)(reg - R_IV_0), r->iv_bm);
            ot_aes_update_iv(s);
            ot_aes_update_config(s);
        }
        break;
    case R_DATA_IN_0:
    case R_DATA_IN_1:
    case R_DATA_IN_2:
    case R_DATA_IN_3:
        r->data_in[reg - R_DATA_IN_0] = val32;
        set_bit((int64_t)(reg - R_DATA_IN_0), r->data_in_bm);
        if (ot_aes_is_data_in_ready(r)) {
            ibex_irq_set(&s->clock_active, (int)true);
            ot_aes_pop(s);
        }
        if (!ot_aes_is_manual(r)) {
            ot_aes_process_cond(s);
        }
        break;
    case R_CTRL_SHADOWED:
        if (!ot_aes_is_idle(s)) {
            break;
        }
        val32 &= R_CTRL_SHADOWED_OPERATION_MASK | R_CTRL_SHADOWED_MODE_MASK |
                 R_CTRL_SHADOWED_KEY_LEN_MASK | R_CTRL_SHADOWED_SIDELOAD_MASK |
                 R_CTRL_SHADOWED_PRNG_RESEED_RATE_MASK |
                 R_CTRL_SHADOWED_MANUAL_OPERATION_MASK |
                 R_CTRL_SHADOWED_FORCE_ZERO_MASKS_MASK;
        OtAESMode prev_mode = ot_aes_get_mode(s->regs);
        switch (ot_shadow_reg_write(&r->ctrl, val32)) {
        case OT_SHADOW_REG_STAGED:
            break;
        case OT_SHADOW_REG_COMMITTED:
            /*
             * "A write to the Control Register is considered the start
             * of a new message. Hence, software needs to provide new key,
             * IV and input data afterwards."
             */
            ot_aes_finalize(s, prev_mode);
            ot_aes_init_keyshare(s, false);
            ot_aes_init_iv(s, false);
            ot_aes_load_reseed_rate(s);
            break;
        case OT_SHADOW_REG_ERROR:
        default:
            r->status |= R_STATUS_ALERT_RECOV_CTRL_UPDATE_ERR_MASK;
            ot_aes_update_alert(s);
            break;
        }
        if (ot_aes_is_sideload(s->regs)) {
            ot_aes_sideload_key(s);
        }
        ot_aes_update_config(s);
        break;
    case R_CTRL_AUX_SHADOWED:
        if (!r->ctrl_aux_regwen) {
            break;
        }
        val32 &= R_CTRL_AUX_SHADOWED_KEY_TOUCH_FORCES_RESEED_MASK |
                 R_CTRL_AUX_SHADOWED_FORCE_MASKS_MASK;
        switch (ot_shadow_reg_write(&r->ctrl_aux, val32)) {
        case OT_SHADOW_REG_STAGED:
        case OT_SHADOW_REG_COMMITTED:
            break;
        case OT_SHADOW_REG_ERROR:
        default:
            r->status |= R_STATUS_ALERT_RECOV_CTRL_UPDATE_ERR_MASK;
            ot_aes_update_alert(s);
        }
        break;
    case R_CTRL_AUX_REGWEN:
        val32 &= R_CTRL_AUX_REGWEN_CTRL_AUX_REGWEN_MASK;
        r->ctrl_aux_regwen = val32;
        break;
    case R_TRIGGER:
        val32 &= R_TRIGGER_START_MASK | R_TRIGGER_KEY_IV_DATA_IN_CLEAR_MASK |
                 R_TRIGGER_DATA_OUT_CLEAR_MASK | R_TRIGGER_PRNG_RESEED_MASK;
        r->trigger |= val32;
        ot_aes_handle_trigger(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Bad offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, addr);
        break;
    }
};

static Property ot_aes_properties[] = {
    DEFINE_PROP_STRING(OT_COMMON_DEV_ID, OtAESState, ot_id),
    DEFINE_PROP_STRING("clock-name", OtAESState, clock_name),
    DEFINE_PROP_LINK("clock-src", OtAESState, clock_src, TYPE_DEVICE,
                     DeviceState *),
    DEFINE_PROP_LINK("edn", OtAESState, edn.device, TYPE_OT_EDN, OtEDNState *),
    DEFINE_PROP_UINT8("edn-ep", OtAESState, edn.ep, UINT8_MAX),
    /*
     * some time-sensitive OT tests requires vCPU to be scheduled before AES
     * round is executed, in which case this property should be reset. In most
     * cases, using fast-mode reduces latency and increase AES throughput
     */
    DEFINE_PROP_BOOL("fast-mode", OtAESState, fast_mode, true),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_aes_regs_ops = {
    .read = &ot_aes_read,
    .write = &ot_aes_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_aes_reset_enter(Object *obj, ResetType type)
{
    OtAESClass *c = OT_AES_GET_CLASS(obj);
    OtAESState *s = OT_AES(obj);
    OtAESEDN *e = &s->edn;
    OtAESRegisters *r = s->regs;

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    timer_del(s->retard_timer);

    memset(s->ctx, 0, sizeof(*s->ctx));
    memset(r, 0, sizeof(*r));
    memset(s->sl_key, 0, sizeof(*s->sl_key));

    ot_shadow_reg_init(&r->ctrl, 0x1181u);
    ot_shadow_reg_init(&r->ctrl_aux, 1u);
    r->ctrl_aux_regwen = 1u;
    r->trigger = 0xeu;
    r->status = 0u;
    r->data_out_rdy = false;
    e->scheduled = false;
    ot_aes_load_reseed_rate(s);

    for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
        ibex_irq_set(&s->alerts[ix], 0);
    }

    if (!s->clock_src_name) {
        IbexClockSrcIfClass *ic = IBEX_CLOCK_SRC_IF_GET_CLASS(s->clock_src);
        IbexClockSrcIf *ii = IBEX_CLOCK_SRC_IF(s->clock_src);

        s->clock_src_name =
            ic->get_clock_source(ii, s->clock_name, DEVICE(s), &error_fatal);
        qemu_irq in_irq =
            qdev_get_gpio_in_named(DEVICE(s), OT_AES_CLOCK_INPUT, 0);
        qdev_connect_gpio_out_named(s->clock_src, s->clock_src_name, 0, in_irq);

        if (object_dynamic_cast(OBJECT(s->clock_src), TYPE_OT_CLKMGR)) {
            char *hint_name =
                g_strdup_printf(OT_CLOCK_HINT_PREFIX "%s", s->clock_name);
            qemu_irq hint_irq =
                qdev_get_gpio_in_named(s->clock_src, hint_name, 0);
            g_assert(hint_irq);
            qdev_connect_gpio_out_named(DEVICE(s), OT_AES_CLOCK_ACTIVE, 0,
                                        hint_irq);
            g_free(hint_name);
        }
    }
}

static void ot_aes_reset_exit(Object *obj, ResetType type)
{
    OtAESClass *c = OT_AES_GET_CLASS(obj);
    OtAESState *s = OT_AES(obj);

    if (c->parent_phases.exit) {
        c->parent_phases.exit(obj, type);
    }

    qemu_bh_cancel(s->process_bh);

    trace_ot_aes_reseed(s->ot_id, "reset");
    ot_aes_handle_trigger(s);
}

static void ot_aes_realize(DeviceState *dev, Error **errp)
{
    OtAESState *s = OT_AES(dev);
    OtAESEDN *e = &s->edn;

    (void)errp;

    g_assert(e->device);
    g_assert(e->ep != UINT8_MAX);
    g_assert(s->ot_id);
    g_assert(s->clock_name);
    g_assert(s->clock_src);

    qdev_init_gpio_in_named(DEVICE(s), &ot_aes_clock_input, OT_AES_CLOCK_INPUT,
                            1);

    s->prng = ot_prng_allocate();
}

static void ot_aes_init(Object *obj)
{
    OtAESState *s = OT_AES(obj);

    memory_region_init_io(&s->mmio, obj, &ot_aes_regs_ops, s, TYPE_OT_AES,
                          REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    s->regs = g_new0(OtAESRegisters, 1u);
    s->ctx = g_new0(OtAESContext, 1u);
    s->sl_key = g_new0(OtAESKey, 1u);

    /* aes_desc is defined in libtomcrypt */
    s->ctx->aes_cipher = register_cipher(&aes_desc);
    if (s->ctx->aes_cipher < 0) {
        error_report("OpenTitan AES: Unable to use libtomcrypt AES API");
    }

    for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
        ibex_qdev_init_irq(obj, &s->alerts[ix], OT_DEVICE_ALERT);
    }

    ibex_qdev_init_irq(obj, &s->clock_active, OT_AES_CLOCK_ACTIVE);

    s->process_bh = qemu_bh_new(&ot_aes_handle_process, s);
    s->retard_timer = timer_new_ns(OT_VIRTUAL_CLOCK, &ot_aes_handle_process, s);

#ifdef DEBUG_AES
    s->hexstr = g_new0(char, AES_DEBUG_HEXBUF_SIZE);
#endif
}

static void ot_aes_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->realize = &ot_aes_realize;
    device_class_set_props(dc, ot_aes_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    ResettableClass *rc = RESETTABLE_CLASS(klass);
    OtAESClass *ac = OT_AES_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_aes_reset_enter, NULL,
                                       &ot_aes_reset_exit, &ac->parent_phases);

    OtKeySinkIfClass *kc = OT_KEY_SINK_IF_CLASS(klass);
    kc->push_key = &ot_aes_push_key;
}

static const TypeInfo ot_aes_info = {
    .name = TYPE_OT_AES,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtAESState),
    .instance_init = &ot_aes_init,
    .class_size = sizeof(OtAESClass),
    .class_init = &ot_aes_class_init,
    .interfaces =
        (InterfaceInfo[]){
            { TYPE_OT_KEY_SINK_IF },
            {},
        },
};

static void ot_aes_register_types(void)
{
    type_register_static(&ot_aes_info);
}

type_init(ot_aes_register_types);
