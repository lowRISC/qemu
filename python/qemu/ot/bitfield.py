# Copyright (c) 2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Simple BitField container.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from typing import Any

from .util.misc import HexInt


class BitField:
    """BitField container
    """

    def __init__(self, *args, **kwargs):
        self._bits = {}
        self._named_bits = {}
        for bits in args:
            self._bits.update(bits)
        for name, bits in kwargs.items():
            self._named_bits[name] = {}
            self._named_bits[name].update(bits)
        self._selector = None
        sels = {k: v for k, v in self._bits.items() if len(v) > 3 and v[3]}
        if sels:
            if len(sels) > 1:
                raise ValueError(f'Too many selectors: {", ".join(sels)}')
            name, desc = sels.popitem()
            enum_ = desc[2]
            if name != enum_.__name__:
                raise ValueError(f'Invalid selector name: {enum_.__name__}')
            self._selector = enum_

    def decode(self, value: int) -> dict[str, Any]:
        """Decode a value into a dictionary."""
        bits = dict(self._bits)
        if self._selector:
            offset, length, enum_ = self._bits[self._selector.__name__][:3]
            mask = (1 << length) - 1
            val = (value >> offset) & mask
            try:
                sel = enum_(val).name
            except ValueError:
                sel = None
            if sel:
                bits.update(self._named_bits[sel])
        values = {}
        for name, code in bits.items():
            offset, length = code[:2]
            enum = code[2] if len(code) > 2 else None
            mask = (1 << length) - 1
            val = (value >> offset) & mask
            if enum:
                values[name] = enum(val)
            else:
                if length == 1:
                    values[name] = bool(val)
                else:
                    values[name] = HexInt(val)
        return values

    def encode(self, *init, **values: dict[str, Any]) -> HexInt:
        """Encode a dictionary into a value."""
        if init:
            value = init[0]
            if len(init) > 1:
                raise ValueError('Unknown argument')
        else:
            value = 0
        values = dict(values)  # duplicate as entries are removed
        bits = dict(self._bits)  # duplicate as selector may be folded into
        if self._selector:
            selname = self._selector.__name__
            sel = values.get(selname)
            if sel:
                bits.update(self._named_bits[sel])
        for name, code in bits.items():
            if name not in values:
                continue
            val = values[name]
            del values[name]
            offset, length = code[:2]
            enum = code[2] if len(code) > 2 else None
            if enum and isinstance(val, str):
                val = enum[val]
            if length == 1 and isinstance(val, bool):
                val = int(val)
            mask = (1 << length) - 1
            val &= mask
            value &= ~(mask << offset)
            value |= val << offset
        if values:
            raise ValueError(f'Unknown field {", ".join(values)}')
        return HexInt(value)
