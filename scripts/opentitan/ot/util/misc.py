# Copyright (c) 2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Miscellaneous helpers.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

class HexInt(int):
    """Simple wrapper to always represent an integer in hexadecimal format."""

    def __repr__(self) -> str:
        return f'0x{self:x}'

    @staticmethod
    def parse(val: str) -> int:
        """Simple helper to support hexadecimal integer in argument parser."""
        return int(val, val.startswith('0x') and 16 or 10)
