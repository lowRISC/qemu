# Copyright (c) 2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Miscellaneous helpers.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from sys import stdout
from typing import Any, Iterable, Optional, TextIO

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


class EasyDict(dict):
    """Dictionary whose members can be accessed as instance members
    """

    def __init__(self, dictionary=None, **kwargs):
        if dictionary is not None:
            self.update(dictionary)
        self.update(kwargs)

    def __getattr__(self, name):
        try:
            return self.__getitem__(name)
        except KeyError as exc:
            raise AttributeError(f"'{self.__class__.__name__}' object has no "
                                 f"attribute '{name}'") from exc

    def __setattr__(self, name, value):
        self.__setitem__(name, value)

    def __dir__(self) -> Iterable[Any]:
        items = set(super().__dir__())
        items.update(set(self))
        yield from sorted(items)


def dump_buffer(buffer: Buffer, addr: int = 0, file: Optional[TextIO] = None) \
        -> None:
    """Dump a binary buffer, same format as hexdump -C."""
    if isinstance(buffer, memoryview):
        view = buffer.getbuffer()
    else:
        view = buffer
    size = len(view)
    if not file:
        file = stdout
    for pos in range(0, size, 16):
        chunks = view[pos:pos+8], view[pos+8:pos+16]
        buf = '  '.join(' '.join(f'{x:02x}' for x in c) for c in chunks)
        if len(buf) < 48:
            buf = f'{buf}{" " * (48 - len(buf))}'
        chunk = view[pos:pos+16]
        text = ''.join(chr(c) if 0x20 <= c < 0x7f else '.' for c in chunk)
        if len(text) < 16:
            text = f'{text}{" " * (16-len(text))}'
        print(f'{addr+pos:08x}  {buf}  |{text}|', file=file)


def round_up(value: int, rnd: int) -> int:
    """Round up a integer value."""
    return (value + rnd - 1) & -rnd
