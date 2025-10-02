/*
 * QEMU OpenTitan ROM controller
 *
 * Copyright (c) 2023-2025 Rivos, Inc.
 *
 * Author(s):
 *  Loïc Lefort <loic@rivosinc.com>
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
 * Notes:
 *  - KeyMgr interface (to send digest to Key Manager) is not yet supported
 *  - Unscrambling & ECC are performed at boot time when a VMEM or HEX file
 *    is loaded, not when the data are fetched from the system bus as on real
 *    HW, for execution performance reason. Moreover any ECC unrecoverable error
 *    discards the whole ROM content, whereas the real HW reports TL-UL error on
 *    a per-address basis. As any recoverable or unrecoverable error leads to an
 *    invalid digest and the ROM reporting an error to the PwrMfr and preventing
 *    execution, this should not be a real issue for emulation.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/memalign.h"
#include "qapi/error.h"
#include "elf.h"
#include "hw/core/rust_demangle.h"
#include "hw/loader.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_kmac.h"
#include "hw/opentitan/ot_prince.h"
#include "hw/opentitan/ot_rom_ctrl.h"
#include "hw/opentitan/ot_rom_ctrl_img.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "trace.h"

#define PARAM_NUM_ALERTS 1u

/* clang-format off */
REG32(ALERT_TEST, 0x0u)
    FIELD(ALERT_TEST, FATAL_ERROR, 0u, 1u)
REG32(FATAL_ALERT_CAUSE, 0x4u)
    FIELD(FATAL_ALERT_CAUSE, CHECKER_ERROR, 0u, 1u)
    FIELD(FATAL_ALERT_CAUSE, INTEGRITY_ERROR, 1u, 1u)
REG32(DIGEST_0, 0x8u)
REG32(DIGEST_1, 0xcu)
REG32(DIGEST_2, 0x10u)
REG32(DIGEST_3, 0x14u)
REG32(DIGEST_4, 0x18u)
REG32(DIGEST_5, 0x1cu)
REG32(DIGEST_6, 0x20u)
REG32(DIGEST_7, 0x24u)
REG32(EXP_DIGEST_0, 0x28u)
REG32(EXP_DIGEST_1, 0x2cu)
REG32(EXP_DIGEST_2, 0x30u)
REG32(EXP_DIGEST_3, 0x34u)
REG32(EXP_DIGEST_4, 0x38u)
REG32(EXP_DIGEST_5, 0x3cu)
REG32(EXP_DIGEST_6, 0x40u)
REG32(EXP_DIGEST_7, 0x44u)
/* clang-format on */

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_EXP_DIGEST_7)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    REG_NAME_ENTRY(ALERT_TEST),   REG_NAME_ENTRY(FATAL_ALERT_CAUSE),
    REG_NAME_ENTRY(DIGEST_0),     REG_NAME_ENTRY(DIGEST_1),
    REG_NAME_ENTRY(DIGEST_2),     REG_NAME_ENTRY(DIGEST_3),
    REG_NAME_ENTRY(DIGEST_4),     REG_NAME_ENTRY(DIGEST_5),
    REG_NAME_ENTRY(DIGEST_6),     REG_NAME_ENTRY(DIGEST_7),
    REG_NAME_ENTRY(EXP_DIGEST_0), REG_NAME_ENTRY(EXP_DIGEST_1),
    REG_NAME_ENTRY(EXP_DIGEST_2), REG_NAME_ENTRY(EXP_DIGEST_3),
    REG_NAME_ENTRY(EXP_DIGEST_4), REG_NAME_ENTRY(EXP_DIGEST_5),
    REG_NAME_ENTRY(EXP_DIGEST_6), REG_NAME_ENTRY(EXP_DIGEST_7),
};
#undef REG_NAME_ENTRY

#define OT_ROM_CTRL_NUM_ADDR_SUBST_PERM_ROUNDS 2u
#define OT_ROM_CTRL_NUM_DATA_SUBST_PERM_ROUNDS 2u
#define OT_ROM_CTRL_NUM_PRINCE_HALF_ROUNDS     3u

#define OT_ROM_CTRL_DATA_BITS  (sizeof(uint32_t) * 8u)
#define OT_ROM_CTRL_ECC_BITS   7u
#define OT_ROM_CTRL_WORD_BITS  (OT_ROM_CTRL_DATA_BITS + OT_ROM_CTRL_ECC_BITS)
#define OT_ROM_CTRL_WORD_BYTES ((OT_ROM_CTRL_WORD_BITS + 7u) / 8u)

#define ROM_DIGEST_WORDS (OT_ROM_DIGEST_BYTES / sizeof(uint32_t))

#define OT_ROM_CTRL_HEXSTR_SIZE (OT_ROM_DIGEST_BYTES * 2u + 2u)

/* clang-format off */
static const uint8_t SBOX4[16u] = {
    12u, 5u, 6u, 11u, 9u, 0u, 10u, 13u, 3u, 14u, 15u, 8u, 4u, 7u, 1u, 2u
};

static const uint64_t INV_HSIAO[7u] = {
    0x012606bd25ull,
    0x02deba8050ull,
    0x04413d89aaull,
    0x0831234ed1ull,
    0x10c2c1323bull,
    0x202dcc624cull,
    0x4098505586ull,
};
/* clang-format on */

#define INV_HSIAO_COUNT ARRAY_SIZE(INV_HSIAO)
#define HSIAO_MASK      0x2a00000000ull

static const OtKMACAppCfg KMAC_APP_CFG =
    OT_KMAC_CONFIG(CSHAKE, 256u, "", "ROM_CTRL");

typedef struct {
    uint64_t *buffer; /* OT_ROM_CTRL_WORD_BYTES array, in logical order */
    unsigned word_count; /* count of words in the buffer */
    unsigned word_pos; /* current index in for computed KMAC digest */
    bool local_gen; /* scrambling/ECC generated locally, not read from file */
} OtRomCtrlScrambled;

struct OtRomCtrlState {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    MemoryRegion mmio;
    IbexIRQ pwrmgr_good;
    IbexIRQ pwrmgr_done;
    IbexIRQ alert;

    uint32_t regs[REGS_COUNT];

    uint64_t keys[2u]; /* may be NULL */
    uint64_t nonce;
    uint64_t addr_nonce;
    uint64_t data_nonce;
    unsigned addr_width; /* bit count */
    unsigned data_nonce_width; /* bit count */
    OtRomCtrlScrambled scrambled;
    unsigned recovered_error_count;
    unsigned unrecoverable_error_count;
    char *hexstr;
    bool first_reset;
    bool loaded;

    char *ot_id;
    uint32_t size;
    OtKMACState *kmac;
    uint8_t kmac_app;
    char *nonce_xstr;
    char *key_xstr;
};

static void ot_rom_ctrl_get_mem_bounds(OtRomCtrlState *s, hwaddr *minaddr,
                                       hwaddr *maxaddr)
{
    *minaddr = s->mem.addr;
    *maxaddr = s->mem.addr + (hwaddr)memory_region_size(&s->mem);
}

static void ot_rom_ctrl_rust_demangle_fn(const char *st_name, int st_info,
                                         uint64_t st_value, uint64_t st_size)
{
    (void)st_info;
    (void)st_value;

    if (!st_size) {
        return;
    }

    rust_demangle_replace((char *)st_name);
}

static inline uint64_t
ot_rom_ctrl_bitswap(uint64_t in, uint64_t mask, unsigned shift)
{
    return ((in & mask) << shift) | ((in & ~mask) >> shift);
}

static uint64_t ot_rom_ctrl_bitswap64(uint64_t val)
{
    val = ot_rom_ctrl_bitswap(val, 0x5555555555555555ull, 1u);
    val = ot_rom_ctrl_bitswap(val, 0x3333333333333333ull, 2u);
    val = ot_rom_ctrl_bitswap(val, 0x0f0f0f0f0f0f0f0full, 4u);
    val = ot_rom_ctrl_bitswap(val, 0x00ff00ff00ff00ffull, 8u);
    val = ot_rom_ctrl_bitswap(val, 0x0000ffff0000ffffull, 16u);
    val = (val << 32u) | (val >> 32u);

    return val;
}

static uint64_t ot_rom_ctrl_sbox(uint64_t in, unsigned width,
                                 const uint8_t *sbox)
{
    g_assert(width < 64u);

    uint64_t full_mask = (1ull << width) - 1ull;
    width &= ~3ull;
    uint64_t sbox_mask = (1ull << width) - 1ull;

    uint64_t out = in & (full_mask & ~sbox_mask);
    for (unsigned ix = 0u; ix < width; ix += 4u) {
        uint64_t nibble = (in >> ix) & 0xfull;
        out |= ((uint64_t)sbox[nibble]) << ix;
    }

    return out;
}

static uint64_t ot_rom_ctrl_flip(uint64_t in, unsigned width)
{
    uint64_t out = ot_rom_ctrl_bitswap64(in);

    out >>= 64u - width;

    return out;
}

static uint64_t ot_rom_ctrl_perm(uint64_t in, unsigned width, bool invert)
{
    g_assert(width < 64u);

    uint64_t full_mask = (1ull << width) - 1ull;
    width &= ~1ull;
    uint64_t bfly_mask = (1ull << width) - 1ull;

    uint64_t out = in & (full_mask & ~bfly_mask);

    width >>= 1u;
    if (!invert) {
        for (unsigned ix = 0u; ix < width; ix++) {
            uint64_t bit = (in >> (ix << 1u)) & 1ull;
            out |= bit << ix;
            bit = (in >> ((ix << 1u) + 1u)) & 1ull;
            out |= bit << (width + ix);
        }
    } else {
        for (unsigned ix = 0u; ix < width; ix++) {
            uint64_t bit = (in >> ix) & 1ull;
            out |= bit << (ix << 1u);
            bit = (in >> (ix + width)) & 1ull;
            out |= bit << ((ix << 1u) + 1);
        }
    }

    return out;
}

static uint64_t ot_rom_ctrl_subst_perm_enc(uint64_t in, uint64_t key,
                                           unsigned width, unsigned num_rounds)
{
    uint64_t state = in;

    for (unsigned ix = 0u; ix < num_rounds; ix++) {
        state ^= key;
        state = ot_rom_ctrl_sbox(state, width, SBOX4);
        state = ot_rom_ctrl_flip(state, width);
        state = ot_rom_ctrl_perm(state, width, false);
    }

    state ^= key;

    return state;
}

static unsigned ot_rom_ctrl_addr_sp_enc(const OtRomCtrlState *s, unsigned addr)
{
    return ot_rom_ctrl_subst_perm_enc(addr, s->addr_nonce, s->addr_width,
                                      OT_ROM_CTRL_NUM_ADDR_SUBST_PERM_ROUNDS);
}

static uint64_t ot_rom_ctrl_data_sp_enc(const OtRomCtrlState *s, uint64_t in)
{
    (void)s;
    return ot_rom_ctrl_subst_perm_enc(in, 0, OT_ROM_CTRL_WORD_BITS,
                                      OT_ROM_CTRL_NUM_DATA_SUBST_PERM_ROUNDS);
}

static uint64_t
ot_rom_ctrl_get_keystream(const OtRomCtrlState *s, unsigned addr)
{
    uint64_t scramble = (s->data_nonce << s->addr_width) | addr;
    uint64_t stream = ot_prince_run(scramble, s->keys[1u], s->keys[0u],
                                    OT_ROM_CTRL_NUM_PRINCE_HALF_ROUNDS);
    return stream & ((1ull << OT_ROM_CTRL_WORD_BITS) - 1ull);
}

static uint64_t
ot_rom_ctrl_unscramble_word(const OtRomCtrlState *s, unsigned addr, uint64_t in)
{
    uint64_t keystream = ot_rom_ctrl_get_keystream(s, addr);
    return keystream ^ in;
}

static uint64_t
ot_rom_ctrl_scramble_word(const OtRomCtrlState *s, unsigned addr, uint64_t in)
{
    uint64_t keystream = ot_rom_ctrl_get_keystream(s, addr);
    return ot_rom_ctrl_data_sp_enc(s, keystream ^ in);
}

static uint32_t ot_rom_ctrl_verify_ecc_39_32_u32(
    const OtRomCtrlState *s, uint64_t data_i, unsigned *err_o)
{
    unsigned syndrome = 0u;

    for (unsigned int ix = 0u; ix < INV_HSIAO_COUNT; ix++) {
        syndrome |=
            __builtin_parityl((data_i ^ HSIAO_MASK) & INV_HSIAO[ix]) << ix;
    }

    unsigned err = __builtin_parity(syndrome);

    if (!(err & 0x1u) && syndrome) {
        err = 2u;
    }

    *err_o = err;

    if (!err) {
        return data_i & UINT32_MAX;
    }

    uint32_t data_o = 0u;

#define ROM_CTRL_RECOVER(_sy_, _di_, _ix_) \
    ((unsigned)((syndrome == (_sy_)) ^ (bool)((_di_) & (1ull << (_ix_)))) \
     << (_ix_))

    data_o |= ROM_CTRL_RECOVER(0x19u, data_i, 0u);
    data_o |= ROM_CTRL_RECOVER(0x54u, data_i, 1u);
    data_o |= ROM_CTRL_RECOVER(0x61u, data_i, 2u);
    data_o |= ROM_CTRL_RECOVER(0x34u, data_i, 3u);
    data_o |= ROM_CTRL_RECOVER(0x1au, data_i, 4u);
    data_o |= ROM_CTRL_RECOVER(0x15u, data_i, 5u);
    data_o |= ROM_CTRL_RECOVER(0x2au, data_i, 6u);
    data_o |= ROM_CTRL_RECOVER(0x4cu, data_i, 7u);
    data_o |= ROM_CTRL_RECOVER(0x45u, data_i, 8u);
    data_o |= ROM_CTRL_RECOVER(0x38u, data_i, 9u);
    data_o |= ROM_CTRL_RECOVER(0x49u, data_i, 10u);
    data_o |= ROM_CTRL_RECOVER(0x0du, data_i, 11u);
    data_o |= ROM_CTRL_RECOVER(0x51u, data_i, 12u);
    data_o |= ROM_CTRL_RECOVER(0x31u, data_i, 13u);
    data_o |= ROM_CTRL_RECOVER(0x68u, data_i, 14u);
    data_o |= ROM_CTRL_RECOVER(0x07u, data_i, 15u);
    data_o |= ROM_CTRL_RECOVER(0x1cu, data_i, 16u);
    data_o |= ROM_CTRL_RECOVER(0x0bu, data_i, 17u);
    data_o |= ROM_CTRL_RECOVER(0x25u, data_i, 18u);
    data_o |= ROM_CTRL_RECOVER(0x26u, data_i, 19u);
    data_o |= ROM_CTRL_RECOVER(0x46u, data_i, 20u);
    data_o |= ROM_CTRL_RECOVER(0x0eu, data_i, 21u);
    data_o |= ROM_CTRL_RECOVER(0x70u, data_i, 22u);
    data_o |= ROM_CTRL_RECOVER(0x32u, data_i, 23u);
    data_o |= ROM_CTRL_RECOVER(0x2cu, data_i, 24u);
    data_o |= ROM_CTRL_RECOVER(0x13u, data_i, 25u);
    data_o |= ROM_CTRL_RECOVER(0x23u, data_i, 26u);
    data_o |= ROM_CTRL_RECOVER(0x62u, data_i, 27u);
    data_o |= ROM_CTRL_RECOVER(0x4au, data_i, 28u);
    data_o |= ROM_CTRL_RECOVER(0x29u, data_i, 29u);
    data_o |= ROM_CTRL_RECOVER(0x16u, data_i, 30u);
    data_o |= ROM_CTRL_RECOVER(0x52u, data_i, 31u);

#undef OTP_ECC_RECOVER

    if (err > 1u) {
        trace_ot_rom_ctrl_unrecoverable_error(s->ot_id, (uint32_t)data_i);
    } else {
        if ((data_i & UINT32_MAX) != data_o) {
            trace_ot_rom_ctrl_recovered_error(s->ot_id, (uint32_t)data_i,
                                              (uint32_t)data_o);
        } else {
            /* ECC bit is corrupted */
            trace_ot_rom_ctrl_parity_error(s->ot_id, (uint32_t)data_i,
                                           (unsigned)(data_i >> 32u));
        }
    }

    return data_o;
}

static uint64_t ot_rom_ctrl_add_ecc_39_32_u64(uint64_t data_i)
{
    uint64_t ecc = 0u;
    bool inv = false;
    data_i &= UINT32_MAX;
    for (unsigned int ix = 0u; ix < INV_HSIAO_COUNT; ix++) {
        ecc <<= 1;
        bool parity = (bool)__builtin_parityl(
            data_i & INV_HSIAO[INV_HSIAO_COUNT - 1u - ix]);
        ecc |= (uint64_t)(parity ^ inv);
        inv = !inv;
    }
    return data_i | (ecc << 32u);
}

static void ot_rom_ctrl_unscramble(OtRomCtrlState *s, const uint64_t *src,
                                   uint32_t *dst)
{
    unsigned scr_word_size = (s->size - OT_ROM_DIGEST_BYTES) / sizeof(uint32_t);
    unsigned dword_count = (s->size * 2u) / sizeof(uint64_t);
    unsigned log_addr = 0u;
    /* unscramble the whole ROM, except the trailing ROM digest bytes */
    s->recovered_error_count = 0u;
    s->unrecoverable_error_count = 0u;
    for (; log_addr < scr_word_size; log_addr++) {
        unsigned phy_addr = ot_rom_ctrl_addr_sp_enc(s, log_addr);
        g_assert(phy_addr < dword_count);
        uint64_t scrdata = src[phy_addr];
        uint64_t clrdata = ot_rom_ctrl_unscramble_word(s, log_addr, scrdata);
        dst[log_addr] = (uint32_t)clrdata;
        unsigned err;
        uint32_t fixdata = ot_rom_ctrl_verify_ecc_39_32_u32(s, clrdata, &err);
        if (err & 0x1u) {
            s->recovered_error_count += 1u;
            dst[log_addr] = fixdata;
        }
        if (err & 0x2u) {
            s->unrecoverable_error_count += 1u;
        }
    }
    /* recover the ROM digest bytes, which are not scrambled */
    for (unsigned wix = 0u; wix < ROM_DIGEST_WORDS; wix++, log_addr++) {
        unsigned phy_addr = ot_rom_ctrl_addr_sp_enc(s, log_addr);
        g_assert(phy_addr < dword_count);
        s->regs[R_EXP_DIGEST_0 + wix] = (uint32_t)src[phy_addr];
        /* note: ECC is not used for DIGEST words */
    }
}

static void ot_rom_ctrl_scramble(OtRomCtrlState *s, const uint32_t *src,
                                 uint64_t *dst)
{
    if (!s->key_xstr || !s->nonce_xstr) {
        trace_ot_rom_ctrl_missing(s->ot_id,
                                  "missing key/nonce to fake scrambling");
        return;
    }

    unsigned scr_word_size = (s->size - OT_ROM_DIGEST_BYTES) / sizeof(uint32_t);
    for (unsigned log_addr = 0u; log_addr < scr_word_size; log_addr++) {
        uint64_t clrdata = src[log_addr];
        uint64_t eclrdata = ot_rom_ctrl_add_ecc_39_32_u64(clrdata);
        uint64_t scrdata = ot_rom_ctrl_scramble_word(s, log_addr, eclrdata);
        dst[log_addr] = scrdata;
    }
}

static void ot_rom_ctrl_compare_and_notify(OtRomCtrlState *s)
{
    /* compare digests */
    bool rom_good = true;
    for (unsigned ix = 0u; ix < ROM_DIGEST_WORDS; ix++) {
        if (s->regs[R_EXP_DIGEST_0 + ix] != s->regs[R_DIGEST_0 + ix]) {
            rom_good = false;
            error_setg(&error_fatal,
                       "ot_rom_ctrl: %s: Digest mismatch (expected 0x%08x got "
                       "0x%08x) @ %u, errors: %u single-bit, %u double-bit\n",
                       s->ot_id, s->regs[R_EXP_DIGEST_0 + ix],
                       s->regs[R_DIGEST_0 + ix], ix, s->recovered_error_count,
                       s->unrecoverable_error_count);
        }
    }

    trace_ot_rom_ctrl_notify(s->ot_id, rom_good);

    /* notify end of check */
    ibex_irq_set(&s->pwrmgr_good, rom_good);
    ibex_irq_set(&s->pwrmgr_done, true);
}

static void ot_rom_ctrl_send_kmac_req(OtRomCtrlState *s)
{
    g_assert(s->scrambled.buffer);
    g_assert(s->scrambled.word_pos <= s->scrambled.word_count);

    OtKMACAppReq req = {
        .msg_len = OT_ROM_CTRL_WORD_BYTES,
    };
    stq_le_p(req.msg_data, s->scrambled.buffer[s->scrambled.word_pos]);
    req.last = ++s->scrambled.word_pos == s->scrambled.word_count;

    OtKMACClass *kc = OT_KMAC_GET_CLASS(s->kmac);
    kc->app_request(s->kmac, s->kmac_app, &req);
}

static void
ot_rom_ctrl_handle_kmac_response(void *opaque, const OtKMACAppRsp *rsp)
{
    OtRomCtrlState *s = OT_ROM_CTRL(opaque);

    if (!rsp->done) {
        ot_rom_ctrl_send_kmac_req(s);
        return;
    }
    g_assert(s->scrambled.word_pos == s->scrambled.word_count);

    qemu_vfree(s->scrambled.buffer);
    s->scrambled.word_count = 0u;

    /*
     * switch to ROMD mode if no unrecoverable ECC error has been detected.
     * Note that real HW does this on a per 32-bit address basis, but as any
     * error triggers an invalid digest and prevents the Ibex core from booting,
     * this use case is mostly useless anyway.
     */
    memory_region_rom_device_set_romd(&s->mem,
                                      s->unrecoverable_error_count == 0u);

    /* retrieve digest */
    for (unsigned ix = 0u; ix < ROM_DIGEST_WORDS; ix++) {
        uint32_t share0;
        uint32_t share1;
        memcpy(&share0, &rsp->digest_share0[ix * sizeof(uint32_t)],
               sizeof(uint32_t));
        memcpy(&share1, &rsp->digest_share1[ix * sizeof(uint32_t)],
               sizeof(uint32_t));
        s->regs[R_DIGEST_0 + ix] = share0 ^ share1;
        if (s->scrambled.local_gen) {
            s->regs[R_EXP_DIGEST_0 + ix] = s->regs[R_DIGEST_0 + ix];
        }
    }

    if (trace_event_get_state(TRACE_OT_ROM_CTRL_COMPUTED_DIGEST)) {
        uint8_t digest[OT_ROM_DIGEST_BYTES];
        for (unsigned ix = 0u; ix < OT_ROM_DIGEST_BYTES; ix++) {
            digest[ix] = rsp->digest_share0[ix] ^ rsp->digest_share1[ix];
        }
        trace_ot_rom_ctrl_computed_digest(
            s->ot_id, ot_common_lhexdump(digest, OT_ROM_DIGEST_BYTES, true,
                                         s->hexstr, OT_ROM_CTRL_HEXSTR_SIZE));
    }

    trace_ot_rom_ctrl_digest_mode(s->ot_id, "stored");

    /* compare digests and send notification */
    ot_rom_ctrl_compare_and_notify(s);
}

static void ot_rom_ctrl_spawn_hash_calculation(
    OtRomCtrlState *s, uintptr_t scramble_ptr, bool generate)
{
    g_assert(scramble_ptr % sizeof(uint64_t) == 0ull);

    OtRomCtrlScrambled *scrambled = &s->scrambled;
    unsigned word_count = (s->size - OT_ROM_DIGEST_BYTES) / sizeof(uint32_t);
    scrambled->buffer = (uint64_t *)scramble_ptr;
    scrambled->word_count = word_count;
    scrambled->word_pos = 0u;
    scrambled->local_gen = generate;

    ot_rom_ctrl_send_kmac_req(s);
}

static void ot_rom_ctrl_load_elf(OtRomCtrlState *s, const OtRomImg *ri)
{
    AddressSpace *as = ot_common_get_local_address_space(DEVICE(s));
    hwaddr minaddr;
    hwaddr maxaddr;
    ot_rom_ctrl_get_mem_bounds(s, &minaddr, &maxaddr);
    uint64_t loaddr;

    if (load_elf_ram_sym_nosz(ri->filename, NULL, NULL, NULL, NULL, &loaddr,
                              NULL, NULL, 0, EM_RISCV, 1, 0, as, false,
                              &ot_rom_ctrl_rust_demangle_fn, true) <= 0) {
        error_setg(&error_fatal,
                   "ot_rom_ctrl: %s: ROM image '%s', ELF loading failed",
                   s->ot_id, ri->filename);
        return;
    }
    if ((loaddr < minaddr) || (loaddr > maxaddr)) {
        /* cannot test upper load address as QEMU loader returns VMA, not LMA */
        error_setg(&error_fatal, "ot_rom_ctrl: %s: ELF cannot fit into ROM",
                   s->ot_id);
    }

    uintptr_t hostptr = (uintptr_t)memory_region_get_ram_ptr(&s->mem);

    unsigned load_size = s->size * 2u;
    uintptr_t tmpptr = (uintptr_t)qemu_memalign(sizeof(uint64_t), load_size);
    ot_rom_ctrl_scramble(s, (const uint32_t *)hostptr, (uint64_t *)tmpptr);

    ot_rom_ctrl_spawn_hash_calculation(s, tmpptr, true);
}

static void ot_rom_ctrl_load_binary(OtRomCtrlState *s, const OtRomImg *ri)
{
    if (ri->raw_size > s->size) {
        error_setg(&error_fatal, "%s: %s: cannot fit into ROM", __func__,
                   s->ot_id);
        return;
    }

    /* NOLINTNEXTLINE(misc-redundant-expression) */
    int fd = open(ri->filename, O_RDONLY | O_BINARY | O_CLOEXEC);
    if (fd == -1) {
        error_setg(&error_fatal, "%s: %s: could not open ROM '%s': %s",
                   __func__, s->ot_id, ri->filename, strerror(errno));
        return;
    }

    uint8_t *data = g_malloc0(ri->raw_size);
    ssize_t rc = read(fd, data, ri->raw_size);
    close(fd);

    if (rc != (ssize_t)ri->raw_size) {
        g_free(data);
        error_setg(&error_fatal,
                   "%s: %s: file %s: read error: rc=%zd (expected %u)",
                   __func__, s->ot_id, ri->filename, rc, ri->raw_size);
        return;
    }

    uintptr_t hostptr = (uintptr_t)memory_region_get_ram_ptr(&s->mem);
    memcpy((void *)hostptr, data, ri->raw_size);
    g_free(data);

    memory_region_set_dirty(&s->mem, 0, ri->raw_size);

    unsigned load_size = s->size * 2u;
    uintptr_t tmpptr = (uintptr_t)qemu_memalign(sizeof(uint64_t), load_size);
    ot_rom_ctrl_scramble(s, (const uint32_t *)hostptr, (uint64_t *)tmpptr);

    ot_rom_ctrl_spawn_hash_calculation(s, tmpptr, true);
}

static char *ot_rom_ctrl_read_text_file(OtRomCtrlState *s, const OtRomImg *ri)
{
    if (!s->key_xstr || !s->nonce_xstr) {
        error_setg(&error_fatal,
                   "%s: %s: cannot unscrambled ROM '%s' w/o key and nonce\n",
                   __func__, ri->filename, s->ot_id);
        return NULL;
    }

    /* NOLINTNEXTLINE(misc-redundant-expression) */
    int fd = open(ri->filename, O_RDONLY | O_BINARY | O_CLOEXEC);
    if (fd == -1) {
        error_setg(&error_fatal, "%s: %s: could not open ROM '%s': %s\n",
                   __func__, ri->filename, s->ot_id, strerror(errno));
        return NULL;
    }

    char *buffer = g_malloc0(ri->raw_size);
    ssize_t rc = read(fd, buffer, ri->raw_size);
    close(fd);

    if (rc != ri->raw_size) {
        g_free(buffer);
        error_setg(&error_fatal,
                   "%s: %s: file %s: read error: rc=%zd (expected %u)",
                   __func__, s->ot_id, ri->filename, rc, ri->raw_size);
        return NULL;
    }

    return buffer;
}

static void ot_rom_ctrl_load_vmem(OtRomCtrlState *s, const OtRomImg *ri,
                                  bool scrambled_n_ecc)
{
    char *buffer = ot_rom_ctrl_read_text_file(s, ri);
    if (!buffer) {
        return;
    }

    uintptr_t baseptr;
    unsigned load_size;
    if (!scrambled_n_ecc) {
        load_size = s->size;
        baseptr = (uintptr_t)memory_region_get_ram_ptr(&s->mem);
    } else {
        /*
         * allocate a temporary buffer to store scrambled data and ECC.
         * The buffer needs to be twice as large as the 32-bit data since ECC
         * byte is stored in b39..b32, so storage is managed with 64-bit values.
         * This buffer is descrambled and ECC verified in a post-postprocessing
         * stage where clear data are copied back to the device memory region
         * and ECC data are discarded once used.
         */
        load_size = s->size * 2u;
        baseptr = (uintptr_t)qemu_memalign(sizeof(uint64_t), load_size);
    }

    uintptr_t lastptr = baseptr + load_size;
    uintptr_t memptr = baseptr;
    unsigned exp_addr = 0u;
    unsigned blk_size = scrambled_n_ecc ? sizeof(uint64_t) : sizeof(uint32_t);
    const char *sep = "\r\n";
    char *brks;
    char *line;
    for (line = strtok_r(buffer, sep, &brks); line;
         line = strtok_r(NULL, sep, &brks)) {
        if (strlen(line) == 0u) {
            continue;
        }

        gchar **items = g_strsplit_set(line, " ", 0);
        if (items[0u][0u] != '@') { /* block address */
            g_strfreev(items);
            continue;
        }

        unsigned blk_addr = (unsigned)g_ascii_strtoull(&items[0u][1], NULL, 16);
        if (blk_addr < exp_addr) {
            g_strfreev(items);
            g_free(buffer);
            error_setg(&error_fatal,
                       "%s: %s: address discrepancy in VMEM file '%s'",
                       __func__, s->ot_id, ri->filename);
            return;
        }
        if (blk_addr != exp_addr) {
            /* each block contains 32-bit of data */
            unsigned pad_size = (blk_addr - exp_addr) * sizeof(uint32_t);
            memptr += pad_size;
        }

        unsigned blk_count = 0u;
        while (items[1u + blk_count]) {
            blk_count++;
        }

        if ((memptr + (uintptr_t)(blk_count * blk_size)) > lastptr) {
            g_free(buffer);
            error_setg(&error_fatal, "%s: %s: VMEM file '%s' too large",
                       __func__, s->ot_id, ri->filename);
            return;
        }

        for (unsigned blk = 0u; blk < blk_count; blk++) {
            uint64_t value = g_ascii_strtoull(items[1u + blk], NULL, 16);
            if (!scrambled_n_ecc) {
                /* direct store to ROM controller memory */
                stl_le_p((void *)memptr, value & UINT32_MAX);
                memptr += sizeof(uint32_t);
            } else {
                /* store to an intermediate buffer for delayed descrambling */
                stq_le_p((void *)memptr, value);
                memptr += sizeof(uint64_t);
            }
        }
        exp_addr += blk_count;
        g_strfreev(items);
    }
    g_free(buffer);

    uintptr_t scrptr;

    if (scrambled_n_ecc) {
        uintptr_t dst = (uintptr_t)memory_region_get_ram_ptr(&s->mem);
        g_assert((dst & 0x3u) == 0u);
        ot_rom_ctrl_unscramble(s, (const uint64_t *)baseptr, (uint32_t *)dst);

        /*
         * hash buffer needs to be in logicial order, input file is in physical
         * order. Create an intermediate copy
         */
        uint64_t *logbuf = qemu_memalign(sizeof(uint64_t), load_size);
        const uint64_t *phybuf = (const uint64_t *)baseptr;
        unsigned dword_count = load_size / sizeof(uint64_t);
        for (unsigned log_addr = 0u; log_addr < dword_count; log_addr++) {
            unsigned phy_addr = ot_rom_ctrl_addr_sp_enc(s, log_addr);
            g_assert(phy_addr < dword_count);
            logbuf[log_addr] = phybuf[phy_addr];
        }
        /* physical buffer is not longer needed */
        qemu_vfree((void *)baseptr);

        /* hash completion discard temporary baseptr */
        scrptr = (uintptr_t)logbuf;
    } else {
        load_size = s->size * 2u;
        uintptr_t tmpptr =
            (uintptr_t)qemu_memalign(sizeof(uint64_t), load_size);
        ot_rom_ctrl_scramble(s, (const uint32_t *)baseptr, (uint64_t *)tmpptr);
        /*
         * hash completion discard temporary tmpptr,
         * baseptr points to the real QEMU host memory and should be kept.
         */
        scrptr = tmpptr;
    }

    ot_rom_ctrl_spawn_hash_calculation(s, scrptr, !scrambled_n_ecc);
}

static void ot_rom_ctrl_load_hex(OtRomCtrlState *s, const OtRomImg *ri)
{
    char *buffer = ot_rom_ctrl_read_text_file(s, ri);
    if (!buffer) {
        return;
    }

    /*
     * allocate a temporary buffer to store scrambled data and ECC.
     * The buffer needs to be twice as large as the 32-bit data since ECC
     * byte is stored in b39..b32, so storage is managed with 64-bit values.
     * This buffer is descrambled and ECC verified in a post-postprocessing
     * stage where clear data are copied back to the device memory region
     * and ECC data are discarded once used.
     */
    unsigned load_size = s->size * 2u;
    uintptr_t baseptr = (uintptr_t)qemu_memalign(sizeof(uint64_t), load_size);
    uintptr_t lastptr = baseptr + load_size;
    uintptr_t memptr = baseptr;

    const char *sep = "\r\n";
    char *brks;
    char *line;
    for (line = strtok_r(buffer, sep, &brks); line;
         line = strtok_r(NULL, sep, &brks)) {
        if (strlen(line) == 0u) {
            continue;
        }

        if (memptr >= lastptr) {
            g_free(buffer);
            error_setg(&error_fatal, "%s: %s: HEX file '%s' too large",
                       __func__, s->ot_id, ri->filename);
            return;
        }

        char *end;
        uint64_t value = g_ascii_strtoull(line, &end, 16);
        if (((uintptr_t)end - (uintptr_t)line) != 10u) {
            g_free(buffer);
            error_setg(&error_fatal, "%s: %s: invalid line in HEX file '%s'",
                       __func__, s->ot_id, ri->filename);
            return;
        }
        stq_le_p((void *)memptr, value);
        memptr += sizeof(uint64_t);
    }
    g_free(buffer);

    uintptr_t dst = (uintptr_t)memory_region_get_ram_ptr(&s->mem);
    g_assert((dst & 0x3u) == 0u);
    ot_rom_ctrl_unscramble(s, (const uint64_t *)baseptr, (uint32_t *)dst);

    if (memptr > baseptr) {
        memory_region_set_dirty(&s->mem, 0, memptr - baseptr);

        unsigned loaded_size = (unsigned)(memptr - baseptr);
        if (loaded_size != load_size) {
            error_setg(&error_fatal,
                       "%s: %s: incomplete HEX file '%s': %u bytes", __func__,
                       s->ot_id, ri->filename, loaded_size / 2u);
            return;
        }

        /*
         * hash buffer needs to be in logicial order, input file is in physical
         * order. Create an intermediate copy
         */
        uint64_t *logbuf = qemu_memalign(sizeof(uint64_t), load_size);
        const uint64_t *phybuf = (const uint64_t *)baseptr;
        unsigned dword_count = load_size / sizeof(uint64_t);
        for (unsigned log_addr = 0u; log_addr < dword_count; log_addr++) {
            unsigned phy_addr = ot_rom_ctrl_addr_sp_enc(s, log_addr);
            g_assert(phy_addr < dword_count);
            logbuf[log_addr] = phybuf[phy_addr];
        }
        /* physical buffer is not longer needed */
        qemu_vfree((void *)baseptr);

        /* hash completion takes care of freeing the buffer */
        ot_rom_ctrl_spawn_hash_calculation(s, (uintptr_t)logbuf, false);
    }
}

static void ot_rom_ctrl_load_rom(OtRomCtrlState *s)
{
    Object *obj = NULL;
    OtRomImg *rom_img = NULL;

    /* try to find our ROM image object */
    obj = object_resolve_path_component(object_get_objects_root(), s->ot_id);
    if (!obj) {
        trace_ot_rom_ctrl_load_rom_no_image(s->ot_id);
        /*
         * when no ROM is present, fake an empty digest. This use case is not
         * valid on real HW. In QEMU, it is a shortcut to run the platform
         * for specific test cases. Note that the KeyManager may not produce
         * valid results in such a case.
         */
        memset(&s->regs[R_EXP_DIGEST_0], 0,
               ROM_DIGEST_WORDS * sizeof(uint32_t));
        memset(&s->regs[R_DIGEST_0], 0, ROM_DIGEST_WORDS * sizeof(uint32_t));
        ot_rom_ctrl_compare_and_notify(s);
        return;
    }
    rom_img = (OtRomImg *)object_dynamic_cast(obj, TYPE_OT_ROM_IMG);
    if (!rom_img) {
        error_setg(&error_fatal, "%s: %s: Object is not a ROM Image", __func__,
                   s->ot_id);
        return;
    }

    const char *basename = strrchr(rom_img->filename, '/');
    basename = basename ? basename + 1 : rom_img->filename;

    switch (rom_img->format) {
    case OT_ROM_IMG_FORMAT_VMEM_PLAIN:
        trace_ot_rom_ctrl_image_identify(s->ot_id, basename, "plain VMEM");
        ot_rom_ctrl_load_vmem(s, rom_img, false);
        break;
    case OT_ROM_IMG_FORMAT_VMEM_SCRAMBLED_ECC:
        trace_ot_rom_ctrl_image_identify(s->ot_id, basename,
                                         "scrambled VMEM w/ ECC");
        ot_rom_ctrl_load_vmem(s, rom_img, true);
        break;
    case OT_ROM_IMG_FORMAT_HEX_SCRAMBLED_ECC:
        trace_ot_rom_ctrl_image_identify(s->ot_id, basename,
                                         "scrambled HEX w/ ECC");
        ot_rom_ctrl_load_hex(s, rom_img);
        break;
    case OT_ROM_IMG_FORMAT_ELF:
        trace_ot_rom_ctrl_image_identify(s->ot_id, basename, "ELF32");
        ot_rom_ctrl_load_elf(s, rom_img);
        break;
    case OT_ROM_IMG_FORMAT_BINARY:
        trace_ot_rom_ctrl_image_identify(s->ot_id, basename, "Binary");
        ot_rom_ctrl_load_binary(s, rom_img);
        break;
    case OT_ROM_IMG_FORMAT_NONE:
    default:
        error_setg(&error_fatal, "%s: %s: unable to read binary file '%s'",
                   __func__, s->ot_id, rom_img->filename);
    }
}

static uint64_t ot_rom_ctrl_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtRomCtrlState *s = opaque;
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_FATAL_ALERT_CAUSE:
    case R_DIGEST_0:
    case R_DIGEST_1:
    case R_DIGEST_2:
    case R_DIGEST_3:
    case R_DIGEST_4:
    case R_DIGEST_5:
    case R_DIGEST_6:
    case R_DIGEST_7:
    case R_EXP_DIGEST_0:
    case R_EXP_DIGEST_1:
    case R_EXP_DIGEST_2:
    case R_EXP_DIGEST_3:
    case R_EXP_DIGEST_4:
    case R_EXP_DIGEST_5:
    case R_EXP_DIGEST_6:
    case R_EXP_DIGEST_7:
        val32 = s->regs[reg];
        break;
    case R_ALERT_TEST:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: W/O register 0x%02" HWADDR_PRIx " (%s)\n", __func__,
                      addr, REG_NAME(reg));
        val32 = 0u;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        val32 = 0u;
        break;
    }

    uint32_t pc = ibex_get_current_pc();
    trace_ot_rom_ctrl_io_read_out(s->ot_id, (uint32_t)addr, REG_NAME(reg),
                                  val32, pc);

    return (uint64_t)val32;
};

static void ot_rom_ctrl_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                                   unsigned size)
{
    OtRomCtrlState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_rom_ctrl_io_write(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32,
                               pc);

    switch (reg) {
    case R_ALERT_TEST:
        val32 &= R_ALERT_TEST_FATAL_ERROR_MASK;
        s->regs[reg] = val32;
        ibex_irq_set(&s->alert, (int)(bool)val32);
        break;
    case R_FATAL_ALERT_CAUSE:
    case R_DIGEST_0:
    case R_DIGEST_1:
    case R_DIGEST_2:
    case R_DIGEST_3:
    case R_DIGEST_4:
    case R_DIGEST_5:
    case R_DIGEST_6:
    case R_DIGEST_7:
    case R_EXP_DIGEST_0:
    case R_EXP_DIGEST_1:
    case R_EXP_DIGEST_2:
    case R_EXP_DIGEST_3:
    case R_EXP_DIGEST_4:
    case R_EXP_DIGEST_5:
    case R_EXP_DIGEST_6:
    case R_EXP_DIGEST_7:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: R/O register 0x%02" HWADDR_PRIx " (%s)\n", __func__,
                      addr, REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
};

static void ot_rom_ctrl_get_rom_digest(const OtRomCtrlState *s,
                                       uint8_t digest[OT_ROM_DIGEST_BYTES])
{
    g_assert(s != NULL);
    for (unsigned wix = 0; wix < ROM_DIGEST_WORDS; wix++) {
        stl_le_p(&digest[wix * sizeof(uint32_t)], s->regs[R_DIGEST_0 + wix]);
    }
}

static void ot_rom_ctrl_mem_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    OtRomCtrlState *s = opaque;
    uint32_t pc = ibex_get_current_pc();

    trace_ot_rom_ctrl_mem_write(s->ot_id, (uint32_t)addr, (uint32_t)value, pc);

    uint8_t *rom_ptr = (uint8_t *)memory_region_get_ram_ptr(&s->mem);

    if ((addr + size) <= s->size) {
        stn_le_p(&rom_ptr[addr], (int)size, value);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Bad offset 0x%" HWADDR_PRIx ", pc=0x%x\n",
                      __func__, s->ot_id, addr, pc);
    }
}

static bool ot_rom_ctrl_mem_accepts(void *opaque, hwaddr addr, unsigned size,
                                    bool is_write, MemTxAttrs attrs)
{
    OtRomCtrlState *s = opaque;
    (void)attrs;
    uint32_t pc = ibex_get_current_pc();

    if (!is_write) {
        /*
         * only allow reads during first reset (after complete check, MR gets
         * turned to ROMD mode where mem_ops->valid.accepts is no longer called.
         */
        return s->first_reset;
    }

    bool accept = ((addr + size) <= s->size && s->first_reset);

    if (!accept) {
        trace_ot_rom_ctrl_mem_rejects(s->ot_id, (uint32_t)addr, is_write, pc);
    }

    return accept;
}

static void ot_rom_ctrl_parse_hexstr(const char *name, uint8_t **buf,
                                     const char *hexstr, unsigned size)
{
    if (!hexstr) {
        *buf = NULL;
        return;
    }

    size_t len = strlen(hexstr);
    if ((unsigned)len != size * 2u) {
        *buf = NULL;
        /* 1 char for each nibble */
        error_setg(&error_fatal, "%s: Invalid %s string length: %zu", __func__,
                   name, len);
        return;
    }

    uint8_t *out = g_new0(uint8_t, size);
    for (unsigned ix = 0u; ix < len; ix++) {
        if (!g_ascii_isxdigit(hexstr[ix])) {
            g_free(out);
            *buf = NULL;
            error_setg(&error_fatal, "%s: %s must only contain hex digits",
                       __func__, name);
            return;
        }
        uint8_t digit = g_ascii_xdigit_value(hexstr[ix]);
        digit = (ix & 1u) ? (digit & 0xfu) : (digit << 4u);
        out[ix / 2] |= digit;
    }

    *buf = out;
}

static Property ot_rom_ctrl_properties[] = {
    DEFINE_PROP_STRING(OT_COMMON_DEV_ID, OtRomCtrlState, ot_id),
    DEFINE_PROP_UINT32("size", OtRomCtrlState, size, 0u),
    DEFINE_PROP_LINK("kmac", OtRomCtrlState, kmac, TYPE_OT_KMAC, OtKMACState *),
    DEFINE_PROP_UINT8("kmac-app", OtRomCtrlState, kmac_app, UINT8_MAX),
    DEFINE_PROP_STRING("nonce", OtRomCtrlState, nonce_xstr),
    DEFINE_PROP_STRING("key", OtRomCtrlState, key_xstr),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_rom_ctrl_mem_ops = {
    .write = &ot_rom_ctrl_mem_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 1u,
    .impl.max_access_size = 4u,
    .valid.accepts = &ot_rom_ctrl_mem_accepts,
};

static const MemoryRegionOps ot_rom_ctrl_regs_ops = {
    .read = &ot_rom_ctrl_regs_read,
    .write = &ot_rom_ctrl_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_rom_ctrl_reset_hold(Object *obj, ResetType type)
{
    OtRomCtrlClass *c = OT_ROM_CTRL_GET_CLASS(obj);
    OtRomCtrlState *s = OT_ROM_CTRL(obj);

    trace_ot_rom_ctrl_reset(s->ot_id, "hold");

    if (c->parent_phases.hold) {
        c->parent_phases.hold(obj, type);
    }

    s->loaded = false;

    /* reset all registers on first reset, otherwise keep digests */
    if (s->first_reset) {
        memset(memory_region_get_ram_ptr(&s->mem), 0, s->size);
        memset(s->regs, 0, REGS_SIZE);
    } else {
        s->regs[R_ALERT_TEST] = 0u;
        s->regs[R_FATAL_ALERT_CAUSE] = 0u;
    }

    ibex_irq_set(&s->pwrmgr_good, false);
    ibex_irq_set(&s->pwrmgr_done, false);

    /* connect to KMAC */
    OtKMACClass *kc = OT_KMAC_GET_CLASS(s->kmac);
    kc->connect_app(s->kmac, s->kmac_app, &KMAC_APP_CFG,
                    ot_rom_ctrl_handle_kmac_response, s);
}

static void ot_rom_ctrl_set_load(Object *obj, bool value, Error **errp)
{
    OtRomCtrlState *s = OT_ROM_CTRL(obj);
    (void)errp;

    if (!value) {
        return;
    }

    if (!s->loaded) {
        s->loaded = true;

        /* on initial reset, load ROM then set it read-only */
        if (s->first_reset) {
            /* load ROM from file */
            ot_rom_ctrl_load_rom(s);

            /* ensure ROM can no longer be written */
            s->first_reset = false;
        } else {
            /* compare existing digests and send notification to pwrmgr */
            ot_rom_ctrl_compare_and_notify(s);
        }
    }
}

static void ot_rom_ctrl_realize(DeviceState *dev, Error **errp)
{
    OtRomCtrlState *s = OT_ROM_CTRL(dev);

    g_assert(s->ot_id);
    g_assert(s->size);
    g_assert(s->kmac);
    g_assert(s->kmac_app != UINT8_MAX);
    OtKMACClass *kc = OT_KMAC_GET_CLASS(s->kmac);
    g_assert(kc->connect_app);
    g_assert(kc->app_request);

    memory_region_init_rom_device_nomigrate(&s->mem, OBJECT(dev),
                                            &ot_rom_ctrl_mem_ops, s,
                                            TYPE_OT_ROM_CTRL ".mem", s->size,
                                            errp);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mem);

    /*
     * at creation, set to read-write and disable ROMD mode:
     * - read-write required for initial loading of ROM content
     * - ROMD mode disabled effectively disables all reads until ROMD is enabled
     * again after a successful digest check (mem_ops.valid.accepts rejects
     * reads).
     */
    s->first_reset = true;
    memory_region_rom_device_set_romd(&s->mem, false);

    size_t wsize = (size_t)s->size / sizeof(uint32_t);
    unsigned addrbits = ctz32(wsize);
    g_assert((wsize & ~(1ull << addrbits)) == 0ull);

    uint8_t *bytes;

    ot_rom_ctrl_parse_hexstr("nonce", &bytes, s->nonce_xstr, sizeof(s->nonce));
    if (bytes) {
        s->nonce = ldq_be_p(bytes);
        s->data_nonce_width = (sizeof(s->nonce) * 8u) - addrbits;
        s->addr_nonce = s->nonce >> s->data_nonce_width;
        s->data_nonce = s->nonce & ((1ull << s->data_nonce_width) - 1u);
        g_free(bytes);
    }

    ot_rom_ctrl_parse_hexstr("key", &bytes, s->key_xstr, sizeof(s->keys));
    if (bytes) {
        s->keys[0u] = ldq_be_p(&bytes[8u]);
        s->keys[1u] = ldq_be_p(&bytes[0u]);
        g_free(bytes);
    }

    s->addr_width = addrbits;
}

static void ot_rom_ctrl_init(Object *obj)
{
    OtRomCtrlState *s = OT_ROM_CTRL(obj);

    ibex_qdev_init_irq(obj, &s->pwrmgr_good, OT_ROM_CTRL_GOOD);
    ibex_qdev_init_irq(obj, &s->pwrmgr_done, OT_ROM_CTRL_DONE);

    memory_region_init_io(&s->mmio, obj, &ot_rom_ctrl_regs_ops, s,
                          TYPE_OT_ROM_CTRL ".regs", REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    ibex_qdev_init_irq(obj, &s->alert, OT_DEVICE_ALERT);

    object_property_add_bool(obj, "load", NULL, &ot_rom_ctrl_set_load);
    object_property_set_description(obj, "load", "Trigger initial ROM loading");

    s->hexstr = g_new0(char, OT_ROM_CTRL_HEXSTR_SIZE);
}

static void ot_rom_ctrl_class_init(ObjectClass *klass, void *data)
{
    (void)data;

    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(dc);
    OtRomCtrlClass *rcc = OT_ROM_CTRL_CLASS(klass);

    resettable_class_set_parent_phases(rc, NULL, &ot_rom_ctrl_reset_hold, NULL,
                                       &rcc->parent_phases);
    dc->realize = &ot_rom_ctrl_realize;
    device_class_set_props(dc, ot_rom_ctrl_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    rcc->get_rom_digest = &ot_rom_ctrl_get_rom_digest;
}

static const TypeInfo ot_rom_ctrl_info = {
    .name = TYPE_OT_ROM_CTRL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtRomCtrlState),
    .instance_init = &ot_rom_ctrl_init,
    .class_size = sizeof(OtRomCtrlClass),
    .class_init = &ot_rom_ctrl_class_init,
};

static void ot_rom_ctrl_register_types(void)
{
    type_register_static(&ot_rom_ctrl_info);
}

type_init(ot_rom_ctrl_register_types);
