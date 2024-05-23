#!/usr/bin/env python3

# Python PRESENT implementation
#
# Copyright (c) 2008
#     Christophe Oosterlynck <christophe.oosterlynck_AT_gmail.com> $
#     NXP ( Philippe Teuwen <philippe.teuwen_AT_nxp.com> )
# Updated 2024 Emmanuel Blot <eblot@rivosinc.com> for QEMU/OT
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

""" PRESENT block cipher implementation.

>> from pypresent import Present

Encrypting with a 128-bit key:
------------------------------
>>> key = 0x0123456789abcdef0123456789abcdef
>>> plain = 0x0123456789abcdef
>>> cipher = Present(key)
>>> encrypted = cipher.encrypt(plain)
>>> hex(encrypted)
'0xe9d28685e671dd6'
>>> decrypted = cipher.decrypt(encrypted)
>>> hex(decrypted)
'0x123456789abcdef'
"""

from typing import list


def _tinvert(tpl):
    return tuple(tpl.index(x) for x in range(len(tpl)))


class Present:
    """PRESENT cipher object

       :param key: the 128-bit key as an integer
       :param rounds: the number of rounds as an integer
    """

    SBOX = (12, 5, 6, 11, 9, 0, 10, 13, 3, 14, 15, 8, 4, 7, 1, 2)

    SBOX_INV = _tinvert(SBOX)

    PBOX = (0, 16, 32, 48, 1, 17, 33, 49, 2, 18, 34, 50, 3, 19, 35, 51, 4, 20,
            36, 52, 5, 21, 37, 53, 6, 22, 38, 54, 7, 23, 39, 55, 8, 24, 40, 56,
            9, 25, 41, 57, 10, 26, 42, 58, 11, 27, 43, 59, 12, 28, 44, 60, 13,
            29, 45, 61, 14, 30, 46, 62, 15, 31, 47, 63)

    PBOX_INV = _tinvert(PBOX)

    def __init__(self, key, rounds=32):
        self._roundkeys = self._generate_roundkeys(key, rounds)

    @property
    def rounds(self) -> int:
        """Return the number of rounds

           :return: the number of rounds
        """
        return len(self._roundkeys)

    def encrypt(self, block: int) -> int:
        """Encrypt 1 block (8 bytes)

           :param block: plaintext block
           :return: ciphertext block
        """
        state = block
        for i in range(self.rounds - 1):
            state = self._add_round_key(state, self._roundkeys[i])
            state = self._sbox_layer(state)
            state = self._p_layer(state)
        cipher = self._add_round_key(state, self._roundkeys[-1])
        return cipher

    def decrypt(self, block: int) -> int:
        """Decrypt 1 block (8 bytes)

           :param block: ciphertext block
           :return: plaintext block
        """
        state = block
        for i in range(self.rounds - 1):
            state = self._add_round_key(state, self._roundkeys[-i - 1])
            state = self._p_layer_dec(state)
            state = self._sbox_layer_dec(state)
        decipher = self._add_round_key(state, self._roundkeys[0])
        return decipher

    @classmethod
    def _generate_roundkeys(cls, key: int, rounds: int) -> list[int]:
        """Generate the roundkeys for a 128-bit key

           :param key: the key as a 128-bit integer
           :param rounds: the number of rounds
           :return: list of 64-bit roundkeys
        """
        roundkeys = []
        for rnd in range(rounds):  # (K1 ... K32)
            # rawkey: used in comments to show what happens at bitlevel
            roundkeys.append(key >> 64)
            # 1. Shift
            key = ((key & (2**67 - 1)) << 61) + (key >> 67)
            # 2. SBox
            key = (cls.SBOX[key >> 124] << 124) + \
                  (cls.SBOX[(key >> 120) & 0xF] << 120) + (key & (2**120 - 1))
            # 3. Salt
            # rawKey[62:67] ^ i
            key ^= (rnd + 1) << 62
        return roundkeys

    @classmethod
    def _add_round_key(cls, state: int, roundkey: int) -> int:
        return state ^ roundkey

    @classmethod
    def _sbox_layer(cls, state: int) -> int:
        """SBox function for encryption

           :param state: 64-bit integer
           :return: 64-bit integer
        """

        output = 0
        for idx in range(16):
            output += cls.SBOX[(state >> (idx * 4)) & 0xF] << (idx * 4)
        return output

    @classmethod
    def _sbox_layer_dec(cls, state: int) -> int:
        """Inverse SBox function for decryption

           :param state: 64-bit integer
           :return: 64-bit integer
        """
        output = 0
        for idx in range(16):
            output += cls.SBOX_INV[(state >> (idx * 4)) & 0xF] << (idx * 4)
        return output

    @classmethod
    def _p_layer(cls, state: int) -> int:
        """Permutation layer for encryption

           :param state: 64-bit integer
           :return: 64-bit integer
        """
        output = 0
        for i in range(64):
            output += ((state >> i) & 0x01) << cls.PBOX[i]
        return output

    @classmethod
    def _p_layer_dec(cls, state: int) -> int:
        """Permutation layer for decryption

           :param state: 64-bit integer
           :return: 64-bit integer
        """
        output = 0
        for i in range(64):
            output += ((state >> i) & 0x01) << cls.PBOX_INV[i]
        return output


if __name__ == '__main__':
    from doctest import testmod
    testmod()
