/*
 * QEMU OpenTitan Present 128-bit key cipher implementation
 *
 * Copyright lowRISC contributors.
 * Reworked 2024 Emmanuel Blot <eblot@rivosinc.com> from Python implementation.

 * Licensed under the Apache License, Version 2.0, see LICENSE for details.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Simple unhardened reference implementation of the PRESENT cipher, following
 * the description in
 *
 *   [1] Bognadov et al, PRESENT: An Ultra-Lightweight Block Cipher. LNCS 4727:
 *       450–466. doi:10.1007/978-3-540-74735-2_31.
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "hw/opentitan/ot_present.h"

/* warning: not thread safe when enabled */
#undef OT_PRESENT_DEBUG

#define OT_PRESENT_ROUND 32u

typedef struct {
    uint64_t hi;
    uint64_t lo;
} OtPresentKey;

struct OtPresentState {
    uint64_t keys[OT_PRESENT_ROUND];
};

static const uint8_t OT_PRESENT_SBOX4[16u] = {
    12u, 5u, 6u, 11u, 9u, 0, 10u, 13u, 3u, 14u, 15u, 8u, 4u, 7u, 1u, 2u
};

static const uint8_t OT_PRESENT_SBOX4_INV[16u] = {
    5u, 14u, 15u, 8u, 12u, 1u, 2u, 13u, 11u, 4u, 6u, 3u, 0u, 7u, 9u, 10u
};

static const uint8_t OT_PRESENT_BIT_PERM[64u] = {
    0,   16u, 32u, 48u, 1u,  17u, 33u, 49u, 2u,  18u, 34u, 50u, 3u,
    19u, 35u, 51u, 4u,  20u, 36u, 52u, 5u,  21u, 37u, 53u, 6u,  22u,
    38u, 54u, 7u,  23u, 39u, 55u, 8u,  24u, 40u, 56u, 9u,  25u, 41u,
    57u, 10u, 26u, 42u, 58u, 11u, 27u, 43u, 59u, 12u, 28u, 44u, 60u,
    13u, 29u, 45u, 61u, 14u, 30u, 46u, 62u, 15u, 31u, 47u, 63u
};

static const uint8_t OT_PRESENT_BIT_PERM_INV[64u] = {
    0,   4u,  8u,  12u, 16u, 20u, 24u, 28u, 32u, 36u, 40u, 44u, 48u,
    52u, 56u, 60u, 1u,  5u,  9u,  13u, 17u, 21u, 25u, 29u, 33u, 37u,
    41u, 45u, 49u, 53u, 57u, 61u, 2u,  6u,  10u, 14u, 18u, 22u, 26u,
    30u, 34u, 38u, 42u, 46u, 50u, 54u, 58u, 62u, 3u,  7u,  11u, 15u,
    19u, 23u, 27u, 31u, 35u, 39u, 43u, 47u, 51u, 55u, 59u, 63u
};

#define MASK64(_x_) MAKE_64BIT_MASK(0, (unsigned)(_x_))

#ifdef OT_PRESENT_DEBUG
#define TRACE_PRESENT(msg, ...) \
    qemu_log("%s: " msg "\n", __func__, ##__VA_ARGS__);
#else
#define TRACE_PRESENT(msg, ...)
#endif

/*----------------------------------------------------------------------------*/
/* Helpers */
/*----------------------------------------------------------------------------*/

static OtPresentKey next_round_key(OtPresentKey k, unsigned round_count)
{
    assert((round_count >> 5u) == 0);

    uint64_t rot_hi = (((k.hi & MASK64(3u)) << 61u) | (k.lo >> 3u));
    uint64_t rot_lo = (((k.lo & MASK64(3u)) << 61u) | (k.hi >> 3u));

    uint64_t rot_nib124 = (rot_hi >> 60u) & MASK64(4u);
    uint64_t rot_nib120 = (rot_hi >> 56u) & MASK64(4u);

    uint64_t subst_hi = (((uint64_t)OT_PRESENT_SBOX4[rot_nib124] << 60u) |
                         ((uint64_t)OT_PRESENT_SBOX4[rot_nib120] << 56u) |
                         (rot_hi & MASK64(56u)));
    uint64_t subst_lo = rot_lo;

    uint64_t xored_hi = subst_hi ^ ((uint64_t)round_count >> 2u);
    uint64_t xored_lo = subst_lo ^ ((uint64_t)round_count << 62u);

    OtPresentKey next = { .hi = xored_hi, .lo = xored_lo };

    return next;
}

static inline uint64_t add_round_key(uint64_t data, uint64_t key)
{
    return data ^ key;
}

static uint64_t sbox_layer(uint64_t data)
{
    uint64_t ret = 0;

    for (unsigned npos = 0; npos < 64u / 4u; ++npos) {
        unsigned nibble = (data >> (4u * npos)) & 0xfu;
        ret |= ((uint64_t)OT_PRESENT_SBOX4[nibble]) << (4u * npos);
    }

    return ret;
}

static uint64_t sbox_inv_layer(uint64_t data)
{
    uint64_t ret = 0;

    for (unsigned npos = 0; npos < 64u / 4u; ++npos) {
        unsigned nibble = (data >> (4u * npos)) & 0xfu;
        ret |= ((uint64_t)OT_PRESENT_SBOX4_INV[nibble]) << (4u * npos);
    }

    return ret;
}

static uint64_t perm_layer(uint64_t data)
{
    uint64_t ret = 0;

    for (unsigned npos = 0; npos < 64u; ++npos) {
        uint64_t bit = (data >> npos) & 1u;
        ret |= bit << OT_PRESENT_BIT_PERM[npos];
    }

    return ret;
}

static uint64_t perm_inv_layer(uint64_t data)
{
    uint64_t ret = 0;

    for (unsigned npos = 0; npos < 64u; ++npos) {
        uint64_t bit = (data >> npos) & 1u;
        ret |= bit << OT_PRESENT_BIT_PERM_INV[npos];
    }

    return ret;
}

#ifdef OT_PRESENT_DEBUG
static char hexbuf[256u];
static const char *ot_present_hexdump(const void *data, size_t size)
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

/*----------------------------------------------------------------------------*/
/* Public API */
/*----------------------------------------------------------------------------*/

OtPresentState *ot_present_new(void)
{
    return g_new0(OtPresentState, 1u);
}

void ot_present_free(OtPresentState *ps)
{
    g_free(ps);
}

void ot_present_init(OtPresentState *ps, const uint8_t *key)
{
    OtPresentKey k128;

    TRACE_PRESENT("present init %s", ot_present_hexdump(key, 16u));

    memcpy(&k128.hi, &key[8u], sizeof(uint64_t));
    memcpy(&k128.lo, &key[0u], sizeof(uint64_t));

    unsigned round = 0;
    ps->keys[round++] = k128.hi;
    while (round < OT_PRESENT_ROUND) {
        k128 = next_round_key(k128, round);
        ps->keys[round] = k128.hi;
        round++;
    }
}

void ot_present_encrypt(const OtPresentState *ps, uint64_t src, uint64_t *dst)
{
    uint64_t state = src;

    for (unsigned round = 0; round < OT_PRESENT_ROUND - 1u; round++) {
        state = add_round_key(state, ps->keys[round]);
        state = sbox_layer(state);
        state = perm_layer(state);
    }
    state = add_round_key(state, ps->keys[OT_PRESENT_ROUND - 1u]);

    *dst = state;

    TRACE_PRESENT("present encrypt %016llx -> %016llx", src, *dst);
}

void ot_present_decrypt(const OtPresentState *ps, uint64_t src, uint64_t *dst)
{
    uint64_t state = src;

    for (unsigned round = OT_PRESENT_ROUND - 1u; round > 0; round--) {
        state = add_round_key(state, ps->keys[round]);
        state = sbox_inv_layer(state);
        state = perm_inv_layer(state);
    }
    state = add_round_key(state, ps->keys[0]);

    *dst = state;
}
