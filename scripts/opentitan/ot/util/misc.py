# Copyright (c) 2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Miscellaneous helpers.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from io import BytesIO
from re import sub
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
    def parse(val: Optional[str], base: Optional[int] = None) -> Optional[int]:
        """Simple helper to support hexadecimal integer in argument parser."""
        if val is None:
            return None
        if base is not None:
            return HexInt(int(val, base))
        return HexInt(int(val, val.startswith('0x') and 16 or 10))


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


def group(lst, count):
    """Group a list into consecutive count-tuples. Incomplete tuples are
    discarded.

    `group([0,3,4,10,2,3], 2) => [(0,3), (4,10), (2,3)]`

    From: http://aspn.activestate.com/ASPN/Cookbook/Python/Recipe/303060
    """
    return list(zip(*[lst[i::count] for i in range(count)]))


def dump_buffer(buffer: Buffer, addr: int = 0, file: Optional[TextIO] = None) \
        -> None:
    """Dump a binary buffer, same format as hexdump -C."""
    if isinstance(buffer, BytesIO):
        view = buffer.getbuffer()
    elif isinstance(buffer, memoryview):
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


def camel_to_snake_case(camel: str) -> str:
    """Convert CamelString string into snake_case lower string."""
    pattern = r'(?<=[a-z])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])'
    return sub(pattern, '_', camel).lower()
