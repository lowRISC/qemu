# Copyright (c) 2023-2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""OTP partitions.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from binascii import hexlify, unhexlify, Error as hexerror
from io import BytesIO
from logging import getLogger
from typing import BinaryIO, Optional, TextIO

from .lifecycle import OtpLifecycle

try:
    # try to load Present if available
    from present import Present
except ImportError:
    Present = None


class OtpPartitionDecoder:
    """Custom partition value decoder."""

    def decode(self, category: str, seq: str) -> Optional[str | int]:
        """Decode a value (if possible)."""
        raise NotImplementedError('abstract base class')


class OtpPartition:
    """Partition abstract base class.

       :param params: initial partition attributes. Those parameters are added
                      to the instance dictionary, which means that most of the
                      partition attributes are explicitly listed in __init__
    """
    # pylint: disable=no-member

    DIGEST_SIZE = 8  # bytes

    MAX_DATA_WIDTH = 20

    def __init__(self, params):
        self.__dict__.update(params)
        self._decoder = None
        self._log = getLogger('otp.part')
        self._data = b''
        self._digest_bytes: Optional[bytes] = None

    @property
    def has_digest(self) -> bool:
        """Check if the partition supports any kind of digest (SW or HW)."""
        return any(getattr(self, f'{k}w_digest', False) for k in 'sh')

    @property
    def is_locked(self) -> bool:
        """Check if the partition is locked, based on its digest."""
        return (self.has_digest and self._digest_bytes and
                self._digest_bytes != bytes(self.DIGEST_SIZE))

    @property
    def is_empty(self) -> bool:
        """Report if the partition is empty."""
        if self._digest_bytes and sum(self._digest_bytes):
            return False
        return sum(self._data) == 0

    def __repr__(self) -> str:
        return repr(self.__dict__)

    def load(self, bfp: BinaryIO) -> None:
        """Load the content of the partition from a binary stream."""
        data = bfp.read(self.size)
        if len(data) != self.size:
            raise IOError(f'{self.name} Cannot load {self.size} from stream')
        if self.has_digest:
            data, digest = data[:-self.DIGEST_SIZE], data[-self.DIGEST_SIZE:]
            self._digest_bytes = digest
        self._data = data

    def save(self, bfp: BinaryIO) -> None:
        """Save the content of the partition to a binary stream."""
        pos = bfp.tell()
        bfp.write(self._data)
        bfp.write(self._digest_bytes)
        size = bfp.tell() - pos
        if size != self.size:
            raise RuntimeError(f"Failed to save partition {self.name} content")

    def verify(self, digest_iv: int, digest_constant: int) -> Optional[bool]:
        """Verify if the digest matches the content of the partition, if any.
        """
        self._log.debug('Verify %s', self.name)
        if not self.is_locked:
            self._log.info('%s has no stored digest', self.name)
            return None
        return self.check_digest(digest_iv, digest_constant)

    def check_digest(self, digest_iv: int, digest_constant: int) \
            -> Optional[bool]:
        """Verify if the digest matches the content of the partition."""
        # don't ask about the byte order. Something is inverted somewhere, and
        # this is all that matters for now
        assert self._digest_bytes is not None
        idigest = int.from_bytes(self._digest_bytes, byteorder='little')
        if idigest == 0:
            self._log.warning('Partition %s digest empty', self.name)
            return None
        lidigest = self.compute_digest(self._data, digest_iv, digest_constant)
        if lidigest != idigest:
            self._log.error('Partition %s digest mismatch (%016x/%016x)',
                            self.name, lidigest, idigest)
            return False
        self._log.info('Partition %s digest match (%016x)', self.name, lidigest)
        return True

    @classmethod
    def compute_digest(cls, data: bytes, digest_iv: int, digest_constant: int) \
            -> int:
        """Compute the HW digest of the partition."""
        if Present is None:
            raise RuntimeError('Cannot check digest, Present module not found')
        block_sz = OtpMap.BLOCK_SIZE
        assert block_sz == 8  # should be 64 bits for Present to work
        if len(data) % block_sz != 0:
            # this case is valid but not yet impplemented (paddding)
            raise RuntimeError('Invalid partition size')
        block_count = len(data) // block_sz
        if block_count & 1:
            data = b''.join((data, data[-block_sz:]))
        state = digest_iv
        for offset in range(0, len(data), 16):
            chunk = data[offset:offset+16]
            b128 = int.from_bytes(chunk, byteorder='little')
            present = Present(b128)
            tmp = present.encrypt(state)
            state ^= tmp
        present = Present(digest_constant)
        state ^= present.encrypt(state)
        return state

    def set_decoder(self, decoder: OtpPartitionDecoder) -> None:
        """Assign a custom value decoder."""
        self._decoder = decoder

    def decode(self, base: Optional[int], decode: bool = True, wide: int = 0,
               ofp: Optional[TextIO] = None) -> None:
        """Decode the content of the partition."""
        buf = BytesIO(self._data)
        if ofp:
            def emit(fmt, *args):
                print(fmt % args, file=ofp)
        else:
            emit = self._log.info
        pname = self.name
        offset = 0
        for itname, itdef in self.items.items():
            itsize = itdef['size']
            itvalue = buf.read(itsize)
            soff = f'[{f"{base+offset:d}":>5s}]' if base is not None else ''
            if itname.startswith(f'{pname}_'):
                name = f'{pname}:{itname[len(pname)+1:]}'
            else:
                name = f'{pname}:{itname}'
            if itsize > 8:
                rvalue = bytes(reversed(itvalue))
                sval = hexlify(rvalue).decode()
                if decode and self._decoder:
                    dval = self._decoder.decode(itname, sval)
                    if dval is not None:
                        emit('%-48s %s (decoded) %s', name, soff, dval)
                        continue
                if not sum(itvalue) and wide < 2:
                    emit('%-48s %s {%d} 0...', name, soff, itsize)
                else:
                    if not wide and itsize > self.MAX_DATA_WIDTH:
                        sval = f'{sval[:self.MAX_DATA_WIDTH*2]}...'
                    emit('%-48s %s {%d} %s', name, soff, itsize, sval)
            else:
                ival = int.from_bytes(itvalue, 'little')
                if decode:
                    if itdef.get('ismubi'):
                        emit('%-48s %s (decoded) %s',
                             name, soff,
                             str(OtpMap.MUBI8_BOOLEANS.get(ival, ival)))
                        continue
                    if itsize == 4 and ival in OtpMap.HARDENED_BOOLEANS:
                        emit('%-48s %s (decoded) %s',
                             name, soff, str(OtpMap.HARDENED_BOOLEANS[ival]))
                        continue
                emit('%-48s %s %x', name, soff, ival)
            offset += itsize
        if self._digest_bytes is not None:
            emit('%-48s %s %s', f'{pname}:DIGEST', soff,
                 hexlify(self._digest_bytes).decode())

    def empty(self) -> None:
        """Empty the partition, including its digest if any."""
        self._data = bytes(len(self._data))
        if self.has_digest:
            self._digest_bytes = bytes(self.DIGEST_SIZE)


class OtpLifecycleExtension(OtpLifecycle, OtpPartitionDecoder):
    """Decoder for Lifecyle bytes sequences.
    """

    def decode(self, category: str, seq: str) -> Optional[str | int]:
        try:
            iseq = hexlify(bytes(reversed(unhexlify(seq)))).decode()
        except (ValueError, TypeError, hexerror) as exc:
            self._log.error('Unable to parse LC data: %s', str(exc))
            return None
        return self._tables.get(category, {}).get(iseq, None)


# imported here to avoid Python circular dependency issue
# pylint: disable=cyclic-import
# pylint: disable=wrong-import-position
from .map import OtpMap  # noqa: E402
