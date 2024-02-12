# Copyright (c) 2010-2024 Emmanuel Blot <emmanuel.blot@free.fr>
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

"""Bit sequence helpers for JTAG/DTM.

   BitSequence is a wrapper to help manipulating bits with the JTAG tools.

   >>> empty = BitSequence()

   >>> len(empty)
   0
   >>> int(empty)
   0
   >>> bool(empty)
   False
   >>> bs = BitSequence(0xC4, 8)

   >>> bs
   [1, 1, 0, 0, 0, 1, 0, 0]
   >>> str(bs)
   '11000100'
   >>> bs = BitSequence('11000100')

   >>> bs
   [1, 1, 0, 0, 0, 1, 0, 0]
   >>> int(bs)
   196
   >>> bs = BitSequence(b'11000100')

   >>> bs
   [1, 1, 0, 0, 0, 1, 0, 0]
   >>> bs = BitSequence([1, 1, 0, 0, 0, 1, 0, 0])

   >>> bs
   [1, 1, 0, 0, 0, 1, 0, 0]
   >>> bs.pop_right()
   [0]
   >>> bs.pop_right(2)
   [1, 0]
   >>> bs.pop_right_bit()
   False
   >>> bs.push_right('0111')
   [1, 1, 0, 0, 0, 1, 1, 1]
   >>> bs.pop_left_bit()
   True
   >>> bs.pop_left(2)
   [1, 0]
   >>> bs
   [0, 0, 1, 1, 1]
   >>> len(bs)
   5
   >>> bs.push_left([False, True, True])
   [0, 1, 1, 0, 0, 1, 1, 1]
   >>> bs.rll()
   [1, 1, 0, 0, 1, 1, 1, 0]
   >>> bs.rrl(3)
   [1, 1, 0, 1, 1, 0, 0, 1]
   >>> bs.inc()
   [1, 1, 0, 1, 1, 0, 1, 0]
   >>> bs.invert()
   [0, 0, 1, 0, 0, 1, 0, 1]
   >>> bs.reverse()
   [1, 0, 1, 0, 0, 1, 0, 0]
   >>> bs = BitSequence(0xff, 8)
   >>> bs.inc()
   [0, 0, 0, 0, 0, 0, 0, 0]
   >>> bs.invariant()
   False
   >>> len(bs)
   8
   >>> bs.dec()
   [1, 1, 1, 1, 1, 1, 1, 1]
   >>> bs.invariant()
   True
   >>> len(bs)
   8
   >>> bs1 = BitSequence(0x13, 5)  # 10011

   >>> bs2 = BitSequence(0x19, 5)  # 11001

   >>> bs3 = bs2.copy().reverse()

   >>> bs1 == bs3
   True
   >>> bs1 == bs2
   False
   >>> bs1 != bs2
   True
   >>> bs1 < bs2
   True
   >>> bs1 <= bs2
   True
   >>> bs1 > bs2
   False
   >>> bs1 | bs2
   [1, 1, 0, 1, 1]
   >>> bs1 & bs2
   [1, 0, 0, 0, 1]
   >>> bs1 ^ bs2
   [0, 1, 0, 1, 0]
   >>> ~(bs1 ^ bs2)
   [1, 0, 1, 0, 1]
"""

from typing import Any, Iterable, List, Union


class BitSequenceError(Exception):
    """Bit sequence error"""


BitSequenceInitializer = Union['BitSequence', str, int, bytes, bytearray,
                               Iterable[int], Iterable[bool], None]
"""Supported types to initialize a BitSequence."""


class BitSequence:
    """Bit sequence.

       Support most of the common bit operations and conversion from and to
       integral values, as well as sequence of boolean, int and characters.

       Bit sequence objects are iterable.

       :param value:  initial value
       :param length: count of signficant bits in the bit sequence
    """

    # pylint: disable=protected-access

    def __init__(self, value: BitSequenceInitializer = None,
                 width: int = 0):
        if value is None:
            self._int = 0
            self._width = width
            return
        if isinstance(value, BitSequence):
            self._int, self._width = value._int, value._width
            return
        if isinstance(value, int):
            bseq = self.from_int(value, width)
            self._int, self._width = bseq._int, bseq._width
            return
        if is_iterable(value):
            bseq = self.from_iterable(value)
            if width and width != bseq._width:
                raise ValueError('Specified width does not match input value')
            self._int, self._width = bseq._int, bseq._width
            return
        raise BitSequenceError(f'Cannot initialize from a {type(value)}')

    @classmethod
    def from_iterable(cls, iterable: Iterable) -> 'BitSequence':
        """Instanciate a BitSequence from an iterable."""
        # pylint: disable=duplicate-key
        smap = {
            0: 0, 1: 1,  # as int
            '0': 0, '1': 1,  # as string
            0x30: 0, 0x31: 1,  # as bytes
            False: 0, True: 1,  # as bool
        }
        value = 0
        width = 0
        for bit in iterable:
            try:
                value <<= 1
                value |= smap[bit]
                width += 1
            except KeyError as exc:
                raise ValueError(f"Invalid item '{bit}' in iterable at pos "
                                 f"{width}") from exc
        return BitSequence.from_int(value, width)

    @classmethod
    def from_int(cls, value: int, width: int) -> 'BitSequence':
        """Instanciate a BitSequence from an integer value."""
        bseq = BitSequence()
        bseq._int = value
        bseq._width = width
        return bseq

    def __len__(self):
        return self._width

    def __bool__(self):
        # report wether the Bit Sequence is empty or not.
        # to fold a single bit sequence into a boolean, see #to_bit
        return bool(self._width)

    def __int__(self):
        return self._int

    def __repr__(self) -> str:
        sseq = ', '.join((f'{b:0d}' for b in self))
        return f'[{sseq}]'

    def __str__(self) -> str:
        return f'{self._int:0{self._width}b}'

    def copy(self) -> 'BitSequence':
        """Duplicate bitsequence."""
        bseq = self.__class__()
        bseq._int = self._int
        bseq._width = self._width
        return bseq

    @property
    def mask(self) -> int:
        """Bit mask."""
        return (1 << self._width) - 1

    def to_bit(self) -> bool:
        """Fold the sequence into a single bit, if possible"""
        if self._width != 1:
            raise BitSequenceError("BitSequence too large")
        return bool(self._int & 1)

    def to_byte(self, msb: bool = False) -> int:
        """Convert the sequence into a single byte value, if possible"""
        if self._width > 8:
            raise BitSequenceError("Cannot fit into a single byte")
        if not msb:
            bseq = BitSequence(self)
            bseq.reverse()
        else:
            bseq = self
        return self._int

    def to_bytes(self) -> bytes:
        """Return the internal representation as bytes"""
        return bytes((int(b) for b in self))

    def to_bool_list(self) -> List[bool]:
        """Convert the sequence into a list of boolean values."""
        return list(self)

    def to_bytestream(self, msb: bool = False, msby: bool = False) -> bytes:
        """Convert the sequence into a sequence of byte values"""
        out: List[int] = []
        bseq = BitSequence(self)
        if not msb:
            bseq.reverse()
        while bseq._width:
            out.append(self._int & 0xff)
            self._int >>= 8
        if msby and not msb:
            out.reverse()
        return bytes(out)

    def reverse(self) -> 'BitSequence':
        """In-place reverse.

           :return: self
        """
        bseq = self.__class__.from_iterable(reversed(self))
        assert bseq._width == self._width
        self._int = bseq._int
        return self

    def invert(self) -> 'BitSequence':
        """In-place invert of each sequence value.

           :return: self
        """
        self._int = ~self._int & self.mask
        return self

    def push_right(self, bseq: BitSequenceInitializer) -> 'BitSequence':
        """Push a bit sequence to the right side.

           :param bseq: the bit sequence to push
           :return: self
        """
        if not isinstance(bseq, BitSequence):
            bseq = BitSequence(bseq)
        self._int <<= len(bseq)
        self._int |= bseq._int
        self._width += len(bseq)
        return self

    def push_left(self, bseq: BitSequenceInitializer) -> 'BitSequence':
        """Push a bit sequence to the left side.

           :param bseq: the bit sequence to push
           :return: self
        """
        if not isinstance(bseq, BitSequence):
            bseq = BitSequence(bseq)
        self._int = (bseq._int << self._width) | self._int
        self._width += len(bseq)
        return self

    def pop_right(self, count: int = 1) -> 'BitSequence':
        """Pop bits from the right side.

           :param count: how many bits to pop
           :return: popped bits a a new BitSequence
        """
        if count > self._width:
            raise ValueError('Count too large')
        if count == 0:
            return BitSequence()
        if count < 0:
            raise ValueError('Negative shift is not defined')
        bseq = self.__class__.from_int(self._int & ((1 << count) - 1), count)
        self._int >>= count
        self._width -= count
        return bseq

    def pop_left(self, count: int = 1) -> 'BitSequence':
        """Pop bits from the left side.

           :param count: how many bits to pop
           :return: popped bits a a new BitSequence
        """
        if count > self._width:
            raise ValueError('Count too large')
        if count == 0:
            return BitSequence()
        if count < 0:
            raise ValueError('Negative shift is not defined')
        shift = self._width - count
        bseq = self.__class__.from_int(self._int >> shift, count)
        self._int &= (1 << shift) - 1
        self._width -= count
        return bseq

    def pop_right_bit(self) -> bool:
        """Pop a single bit from the right side.

           :return: popped bit
        """
        if self._width == 0:
            raise RuntimeError('Empty bit sequence')
        bit = bool(self._int & 1)
        self._int >>= 1
        self._width -= 1
        return bit

    def pop_left_bit(self) -> bool:
        """Pop a single bit from the left side.

           :return: popped bit
        """
        if self._width == 0:
            raise RuntimeError('Empty bit sequence')
        bit = bool(self._int >> (self._width - 1))
        self._width -= 1
        self._int &= self.mask
        return bit

    def rll(self, count: int = 1) -> 'BitSequence':
        """Rotate Left Logical.

           :return: self
        """
        count %= self._width
        bseq = self.pop_left(count)
        self.push_right(bseq)
        return self

    def rrl(self, count: int = 1) -> 'BitSequence':
        """Rotate Right Logical.

           :return: self
        """
        count %= self._width
        bseq = self.pop_right(count)
        self.push_left(bseq)
        return self

    def inc(self, wrap: bool = True) -> 'BitSequence':
        """Increment the sequence."""
        self._int += 1
        if not wrap:
            if self._int >> self._width:
                self._width += 1
        else:
            self._int &= self.mask
        return self

    def dec(self, wrap: bool = True) -> 'BitSequence':
        """Decrement the sequence"""
        self._int -= 1
        if not wrap:
            if not self._int >> self._width:
                self._width -= 1
        else:
            self._int &= self.mask
        return self

    def invariant(self) -> bool:
        """Tells whether all bits of the sequence are of the same value.

           Return the value, or ValueError if the bits are not of the same
           value
        """
        if self._int == 0:
            return False
        if self._int == self.mask:
            return True
        raise ValueError('Bits do no match')

    class Iterator:
        """BitSequence iterator.

           Iterate from left to right (MSB to LSB) if reverse is not set.

           :param bseq: the BitSequence to iterate
           :param reverse: whether to create a reverse iterator
        """

        def __init__(self, bseq: 'BitSequence', reverse: bool = False):
            self._bseq = bseq
            self._reverse = reverse
            self._width = bseq._width
            self._pos = 0

        def __iter__(self) -> 'BitSequence.Iterator':
            return self

        def __next__(self) -> bool:
            if self._width != self._bseq._width:
                raise RuntimeError('BitSequence modified while iterating')
            if self._pos >= self._bseq._width:
                raise StopIteration()
            if self._reverse:
                bit = bool((self._bseq._int >> self._pos) & 1)
            else:
                pos = self._width - self._pos - 1
                bit = bool((self._bseq._int >> pos) & 1)
            self._pos += 1
            return bit

    def __iter__(self) -> 'BitSequence.Iterator':
        """Iterate from left to right, i.e. MSB to LSB."""
        return self.__class__.Iterator(self)

    def __reversed__(self):
        """Iterate from right to left, i.e. LSB to MSB."""
        return self.__class__.Iterator(self, reverse=True)

    def __eq__(self, other: 'BitSequence') -> bool:
        if not isinstance(other, self.__class__):
            raise ValueError(f'Cannot compare with {type(other)}')
        return self._cmp(other) == 0

    def __ne__(self, other: 'BitSequence') -> bool:
        if not isinstance(other, self.__class__):
            raise ValueError(f'Cannot compare with {type(other)}')
        return not self == other

    def __le__(self, other: 'BitSequence') -> bool:
        if not isinstance(other, self.__class__):
            raise ValueError(f'Cannot compare with {type(other)}')
        return self._cmp(other) <= 0

    def __lt__(self, other: 'BitSequence') -> bool:
        if not isinstance(other, self.__class__):
            raise ValueError(f'Cannot compare with {type(other)}')
        return self._cmp(other) < 0

    def __ge__(self, other: 'BitSequence') -> bool:
        if not isinstance(other, self.__class__):
            raise ValueError(f'Cannot compare with {type(other)}')
        return self._cmp(other) >= 0

    def __gt__(self, other: 'BitSequence') -> bool:
        if not isinstance(other, self.__class__):
            raise ValueError(f'Cannot compare with {type(other)}')
        return self._cmp(other) > 0

    def _cmp(self, other: 'BitSequence') -> int:
        # the bit sequence should be of the same length
        ld = self._width - other._width
        if ld:
            return ld
        return self._int - other._int

    def __and__(self, other: 'BitSequence') -> 'BitSequence':
        if not isinstance(other, self.__class__):
            raise ValueError('Need a BitSequence to combine')
        if self._width != other._width:
            raise ValueError('Sequences must be the same size')
        value = self._int & other._int
        return self.__class__.from_int(value, self._width)

    def __or__(self, other: 'BitSequence') -> 'BitSequence':
        if not isinstance(other, self.__class__):
            raise ValueError('Need a BitSequence to combine')
        if self._width != other._width:
            raise ValueError('Sequences must be the same size')
        value = self._int | other._int
        return self.__class__.from_int(value, self._width)

    def __xor__(self, other: 'BitSequence') -> 'BitSequence':
        if not isinstance(other, self.__class__):
            raise ValueError('Need a BitSequence to combine')
        if self._width != other._width:
            raise ValueError('Sequences must be the same size')
        value = self._int ^ other._int
        return self.__class__.from_int(value, self._width)

    def __invert__(self) -> 'BitSequence':
        value = (~self._int) & self.mask
        return self.__class__.from_int(value, self._width)

    def __ilshift__(self, count) -> 'BitSequence':
        self.pop_left()
        return self

    def __irshift__(self, count) -> 'BitSequence':
        self.pop_right()
        return self

    def __getitem__(self, index) -> 'BitSequence':
        # TODO: not yet validated, likely buggy
        if isinstance(index, slice):
            bits: List[int] = []
            for bpos in range(index.start, index.stop, index.step):
                if bpos < 0:
                    continue
                if bpos >= self._width:
                    break
                bits.append((self._int >> bpos) & 1)
            return self.__class__.from_iterable(reversed(bits))
        if not isinstance(index, int):
            raise TypeError(f'{self.__class__.__name__} indices must be '
                            f'integers or slices, not {type(index)}')
        if ~index >= self._width:
            raise IndexError(f'{self.__class__.__name__} index out of range')
        if index >= 0:
            value = (self._int >> index) & 1
        else:
            value = (self._int >> (self._width - index - 1)) & 1
        return self.__class__.from_int(value, 1)

    def __setitem__(self, index, value) -> None:
        # TODO: not yet validated, likely buggy
        if isinstance(index, slice):
            if not isinstance(value, BitSequence):
                if not is_iterable(value):
                    raise TypeError(f'Cannot set item with {type(value)}')
                value = self.from_iterable(value)
            else:
                value = value.copy()
            for bpos in range(index.start, index.stop, index.step):
                if bpos < 0:
                    continue
                if bpos >= self._width:
                    break
                self._int &= ~(1 << bpos)
                self._int |= int(value.pop_right()) << bpos
                if value._width == 0:
                    break
        if not isinstance(index, int):
            raise TypeError(f'{self.__class__.__name__} indices must be '
                            f'integers or slices, not {type(index)}')
        if isinstance(value, BitSequence):
            value = value.tobit()
        elif isinstance(value, int):
            if value not in (0, 1):
                raise ValueError('Invalid value')
        value = int(value)
        if ~index >= self._width:
            raise IndexError(f'{self.__class__.__name__} index out of '
                             f'range')
        if index < 0:
            index = self._width - index - 1
        self._int[index] = value


def is_iterable(obj: Any) -> bool:
    """Tells whether an instance is iterable or not.

       :param obj: the instance to test
       :type obj: object
       :return: True if the object is iterable
       :rtype: bool
    """
    try:
        iter(obj)
        return True
    except TypeError:
        return False


if __name__ == "__main__":
    import doctest
    doctest.testmod()
