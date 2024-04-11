# Copyright (c) 2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Miscellaneous helpers.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from typing import Any, Optional

try:
    # only available from Python 3.12+
    from collections.abc import Buffer
except ImportError:
    Buffer = [bytes | bytearray | memoryview]


class classproperty(property):
    """Getter property decorator for a class"""
    # pylint: disable=invalid-name
    def __get__(self, obj: Any, objtype=None) -> Any:
        return super().__get__(objtype)


class HexInt(int):
    """Simple wrapper to always represent an integer in hexadecimal format."""

    def __repr__(self) -> str:
        return f'0x{self:x}'

    @staticmethod
    def parse(val: Optional[str]) -> Optional[int]:
        """Simple helper to support hexadecimal integer in argument parser."""
        if val is None:
            return None
        return int(val, val.startswith('0x') and 16 or 10)


def dump_buffer(buffer: Buffer, addr: int) -> None:
    """Dump a binary buffer, same format as hexdump -C."""
    view = buffer.getbuffer()
    size = len(view)
    for pos in range(0, size, 16):
        chunks = view[pos:pos+8], view[pos+8:pos+16]
        buf = '  '.join(' '.join(f'{x:02x}' for x in c) for c in chunks)
        text = ''.join(chr(c) if 0x20 <= c < 0x7f else '.'
                       for c in view[pos:pos+16])
        print(f'{addr+pos:08x}  {buf}  |{text}|')


def round_up(value: int, rnd: int) -> int:
    """Round up a integer value."""
    return (value + rnd - 1) & -rnd
