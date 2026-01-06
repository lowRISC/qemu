/*
 * QEMU OpenTitan Prince implementation
 *
 * Copyright (c) lowRISC contributors.
 * Licensed under the Apache License, Version 2.0, see LICENSE for details.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "hw/opentitan/ot_prince.h"

/* clang-format off */

static const uint8_t PRINCE_SBOX4[] = {
    0xbu, 0xfu, 0x3u, 0x2u,
    0xau, 0xcu, 0x9u, 0x1u,
    0x6u, 0x7u, 0x8u, 0x0u,
    0xeu, 0x5u, 0xdu, 0x4u
};

static const uint8_t PRINCE_SBOX4_INV[] = {
    0xbu, 0x7u, 0x3u, 0x2u,
    0xfu, 0xdu, 0x8u, 0x9u,
    0xau, 0x6u, 0x4u, 0x0u,
    0x5u, 0xeu, 0xcu, 0x1u
};

static const uint8_t PRINCE_SHIFT_ROWS64[] = {
    0x4u, 0x9u, 0xeu, 0x3u,
    0x8u, 0xdu, 0x2u, 0x7u,
    0xcu, 0x1u, 0x6u, 0xbu,
    0x0u, 0x5u, 0xau, 0xfu
};

static const uint8_t PRINCE_SHIFT_ROWS64_INV[] = {
    0xcu, 0x9u, 0x6u, 0x3u,
    0x0u, 0xdu, 0xau, 0x7u,
    0x4u, 0x1u, 0xeu, 0xbu,
    0x8u, 0x5u, 0x2u, 0xfu
};

static const uint64_t PRINCE_ROUND_CONSTS[] = {
    0x0000000000000000ull,
    0x13198a2e03707344ull,
    0xa4093822299f31d0ull,
    0x082efa98ec4e6c89ull,
    0x452821e638d01377ull,
    0xbe5466cf34e90c6cull,
    0x7ef84f78fd955cb1ull,
    0x85840851f1ac43aaull,
    0xc882d32f25323c54ull,
    0x64a51195e0e3610dull,
    0xd3b5a399ca0c2399ull,
    0xc0ac29b7c97c50ddull
};

static const uint16_t PRINCE_SHIFT_ROWS_CONSTS[] = {
    0x7bdeu, 0xbde7u, 0xde7bu, 0xe7bdu
};

/* clang-format on */

static uint64_t ot_prince_sbox(uint64_t in, unsigned width, const uint8_t *sbox)
{
    uint64_t full_mask = ((width >= 64u) ? 0ull : (1ull << width)) - 1ull;
    width &= ~3ull;
    uint64_t sbox_mask = ((width >= 64u) ? 0ull : (1ull << width)) - 1ull;

    uint64_t ret = in & (full_mask & ~sbox_mask);

    for (unsigned ix = 0; ix < width; ix += 4u) {
        uint64_t nibble = (in >> ix) & 0xfull;
        ret |= ((uint64_t)sbox[nibble]) << ix;
    }

    return ret;
}

static inline uint64_t ot_prince_nibble_red16(uint64_t data)
{
    uint64_t nib0 = (data >> 0u) & 0xfull;
    uint64_t nib1 = (data >> 4u) & 0xfull;
    uint64_t nib2 = (data >> 8u) & 0xfull;
    uint64_t nib3 = (data >> 12u) & 0xfull;

    return nib0 ^ nib1 ^ nib2 ^ nib3;
}

static uint64_t ot_prince_mult_prime(uint64_t data)
{
    uint64_t ret = 0u;
    for (unsigned blk_idx = 0u; blk_idx < 4u; blk_idx++) {
        uint64_t data_hw = (data >> (16u * blk_idx)) & 0xffffull;
        unsigned start_sr_idx = (blk_idx == 0u || blk_idx == 3u) ? 0u : 1u;
        for (unsigned nibble_idx = 0u; nibble_idx < 4u; nibble_idx++) {
            unsigned sr_idx = (start_sr_idx + 3u - nibble_idx) & 0x3u;
            uint64_t sr_const = (uint64_t)PRINCE_SHIFT_ROWS_CONSTS[sr_idx];
            uint64_t nibble = ot_prince_nibble_red16(data_hw & sr_const);
            ret |= nibble << (16u * blk_idx + 4u * nibble_idx);
        }
    }
    return ret;
}

static uint64_t ot_prince_shiftrows(uint64_t data, bool invert)
{
    const uint8_t *shifts =
        invert ? PRINCE_SHIFT_ROWS64_INV : PRINCE_SHIFT_ROWS64;

    uint64_t ret = 0u;
    for (unsigned nibble_idx = 0; nibble_idx < 64u / 4u; nibble_idx++) {
        unsigned src_nibble_idx = shifts[nibble_idx];
        uint64_t src_nibble = (data >> (4u * src_nibble_idx)) & 0xfu;
        ret |= src_nibble << (4u * nibble_idx);
    }
    return ret;
}

static uint64_t ot_prince_fwd_round(uint64_t rc, uint64_t key, uint64_t data)
{
    data = ot_prince_sbox(data, 64u, PRINCE_SBOX4);
    data = ot_prince_mult_prime(data);
    data = ot_prince_shiftrows(data, false);
    data ^= rc;
    data ^= key;
    return data;
}

static uint64_t ot_prince_inv_round(uint64_t rc, uint64_t key, uint64_t data)
{
    data ^= key;
    data ^= rc;
    data = ot_prince_shiftrows(data, true);
    data = ot_prince_mult_prime(data);
    data = ot_prince_sbox(data, 64u, PRINCE_SBOX4_INV);
    return data;
}

/*
 * Run the PRINCE cipher.
 * This uses the new keyschedule proposed by Dinur in "Cryptanalytic
 * Time-Memory-Data Tradeoffs for FX-Constructions with Applications to PRINCE
 * and PRIDE".
 */
uint64_t ot_prince_run(uint64_t data, uint64_t khi, uint64_t klo,
                       unsigned num_rounds_half)
{
    uint64_t khi_rot1 = ((khi & 1u) << 63u) | (khi >> 1u);
    uint64_t khi_prime = khi_rot1 ^ (khi >> 63u);

    data ^= khi;
    data ^= klo;
    data ^= PRINCE_ROUND_CONSTS[0];

    for (unsigned hri = 0u; hri < num_rounds_half; hri++) {
        unsigned round_idx = 1u + hri;
        uint64_t rc = PRINCE_ROUND_CONSTS[round_idx];
        uint64_t rk = (round_idx & 1u) ? khi : klo;
        data = ot_prince_fwd_round(rc, rk, data);
    }

    data = ot_prince_sbox(data, 64u, PRINCE_SBOX4);
    data = ot_prince_mult_prime(data);
    data = ot_prince_sbox(data, 64u, PRINCE_SBOX4_INV);

    for (unsigned hri = 0u; hri < num_rounds_half; hri++) {
        unsigned round_idx = 11u - num_rounds_half + hri;
        uint64_t rc = PRINCE_ROUND_CONSTS[round_idx];
        uint64_t rk = (round_idx & 1u) ? klo : khi;
        data = ot_prince_inv_round(rc, rk, data);
    }

    data ^= PRINCE_ROUND_CONSTS[11u];
    data ^= klo;

    data ^= khi_prime;

    return data;
}
