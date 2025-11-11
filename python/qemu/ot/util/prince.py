# Copyright (c) 2025 lowRISC contributors.
# SPDX-License-Identifier: Apache2

"""PRINCE cipher implementation.
"""

from functools import lru_cache

# pylint: disable=missing-docstring


class PrinceCipher:

    SBOX4 = [
        0xb, 0xf, 0x3, 0x2, 0xa, 0xc, 0x9, 0x1,
        0x6, 0x7, 0x8, 0x0, 0xe, 0x5, 0xd, 0x4
    ]

    SBOX4_INV = [
        0xb, 0x7, 0x3, 0x2, 0xf, 0xd, 0x8, 0x9,
        0xa, 0x6, 0x4, 0x0, 0x5, 0xe, 0xc, 0x1
    ]

    SHIFT_ROWS64 = [
        0x4, 0x9, 0xe, 0x3, 0x8, 0xd, 0x2, 0x7,
        0xc, 0x1, 0x6, 0xb, 0x0, 0x5, 0xa, 0xf
    ]

    SHIFT_ROWS64_INV = [
        0xc, 0x9, 0x6, 0x3, 0x0, 0xd, 0xa, 0x7,
        0x4, 0x1, 0xe, 0xb, 0x8, 0x5, 0x2, 0xf
    ]
    ROUND_CONSTS = [
        0x0000000000000000, 0x13198a2e03707344,
        0xa4093822299f31d0, 0x082efa98ec4e6c89,
        0x452821e638d01377, 0xbe5466cf34e90c6c,
        0x7ef84f78fd955cb1, 0x85840851f1ac43aa,
        0xc882d32f25323c54, 0x64a51195e0e3610d,
        0xd3b5a399ca0c2399, 0xc0ac29b7c97c50dd
    ]

    @classmethod
    def sbox(cls, in_: int, width: int, sbox: list[int]) -> int:
        full_mask = 0 if (width >= 64) else (1 << width) - 1
        width &= ~3
        sbox_mask = 0 if (width >= 64) else (1 << width) - 1

        ret = in_ & (full_mask & ~sbox_mask)

        for ix in range(0, width, 4):
            nibble = (in_ >> ix) & 0xf
            ret |= sbox[nibble] << ix

        return ret

    @classmethod
    @lru_cache(maxsize=65536)
    def nibble_red16(cls, data: int) -> int:
        nib0 = (data >> 0) & 0xf
        nib1 = (data >> 4) & 0xf
        nib2 = (data >> 8) & 0xf
        nib3 = (data >> 12) & 0xf
        return nib0 ^ nib1 ^ nib2 ^ nib3

    @staticmethod
    def mult_prim_const() -> list[tuple[int, tuple[int, int]]]:
        bconsts = []
        shift_rows_consts = [
            0x7bde, 0xbde7, 0xde7b, 0xe7bd
        ]
        for blk_idx in range(4):
            consts = []
            start_sr_idx = 0 if blk_idx in (0, 3) else 1
            blk_const = blk_idx * 16
            for nibble_idx in range(4):
                sr_idx = (start_sr_idx + 3 - nibble_idx) & 0x3
                sr_const = shift_rows_consts[sr_idx]
                shift_const = blk_const + nibble_idx * 4
                consts.append((shift_const, sr_const))
            bconsts.append((blk_const, consts))
        return bconsts

    MULT_PRIM_CONST = mult_prim_const()

    @classmethod
    def mult_prime(cls, data: int) -> int:
        ret = 0
        for blk_const, consts in cls.MULT_PRIM_CONST:
            data_hw = data >> blk_const
            for sh, sr in consts:
                ret |= cls.nibble_red16(data_hw & sr) << sh
        return ret

    @classmethod
    def shiftrows(cls, data: int, invert: bool) -> int:
        shifts = cls.SHIFT_ROWS64_INV if invert else cls.SHIFT_ROWS64
        ret = 0
        for nibble_idx in range(64//4):
            src_nibble_idx = shifts[nibble_idx]
            src_nibble = (data >> (4 * src_nibble_idx)) & 0xf
            ret |= src_nibble << (4 * nibble_idx)
        return ret

    @classmethod
    def fwd_round(cls, rc: int, key: int, data: int) -> int:
        data = cls.sbox(data, 64, cls.SBOX4)
        data = cls.mult_prime(data)
        data = cls.shiftrows(data, False)
        data ^= rc
        data ^= key
        return data

    @classmethod
    def inv_round(cls, rc: int, key: int, data: int) -> int:
        data ^= key
        data ^= rc
        data = cls.shiftrows(data, True)
        data = cls.mult_prime(data)
        data = cls.sbox(data, 64, cls.SBOX4_INV)
        return data

    # Run the PRINCE cipher.
    # This uses the new keyschedule proposed by Dinur in "Cryptanalytic
    # Time-Memory-Data Tradeoffs for FX-Constructions with Applications to
    # PRINCE and PRIDE".
    @classmethod
    def run(cls, data: int, khi: int, klo: int, num_rounds_half: int) -> int:
        khi_rot1 = ((khi & 1) << 63) | (khi >> 1)
        khi_prime = khi_rot1 ^ (khi >> 63)

        data ^= khi
        data ^= klo
        data ^= cls.ROUND_CONSTS[0]

        for hri in range(num_rounds_half):
            round_idx = 1 + hri
            rc = cls.ROUND_CONSTS[round_idx]
            rk = khi if (round_idx & 1) else klo
            data = cls.fwd_round(rc, rk, data)

        data = cls.sbox(data, 64, cls.SBOX4)
        data = cls.mult_prime(data)
        data = cls.sbox(data, 64, cls.SBOX4_INV)

        for hri in range(num_rounds_half):
            round_idx = 11 - num_rounds_half + hri
            rc = cls.ROUND_CONSTS[round_idx]
            rk = klo if (round_idx & 1) else khi
            data = cls.inv_round(rc, rk, data)

        data ^= cls.ROUND_CONSTS[11]
        data ^= klo
        data ^= khi_prime

        return data
