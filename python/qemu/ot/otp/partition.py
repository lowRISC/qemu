# Copyright (c) 2023-2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""OTP partitions.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from binascii import hexlify, unhexlify, Error as hexerror
from io import BytesIO
from logging import getLogger
from re import IGNORECASE, match
from typing import BinaryIO, NamedTuple, Optional, Sequence, TextIO, Union

from .lifecycle import OtpLifecycle

try:
    # try to load Present if available
    from ot.util.present import Present
except ImportError:
    Present = None


class OtpPartitionDecoder:
    """Custom partition value decoder."""

    def decode(self, category: str, seq: str) -> Union[str, int, None]:
        """Decode a value (if possible)."""
        raise NotImplementedError('abstract base class')


class OtpPartitionItemProp(NamedTuple):
    """Item/field properties."""

    name: str
    """Name of the item."""

    offset: int
    """Offset of the first data byte of the item within the partition."""

    size: int
    """Length in bytes of the item within the partition."""

    end: int
    """Offset past the last data byte in the partition, i.e. offset+length."""

    mubi8: bool = False
    """Whether the field is a known 8-bit boolean value (which differs
       from hardened boolean values and other complex boolean types)."""


class OtpPartition:
    """Partition abstract base class.

       :param params: initial partition attributes. Those parameters are added
                      to the instance dictionary, which means that most of the
                      partition attributes are explicitly listed in __init__
    """
    # pylint: disable=no-member

    DIGEST_SIZE = 8  # bytes

    ZER_SIZE = 8  # bytes

    MAX_DATA_WIDTH = 20

    def __init__(self, params):
        self.__dict__.update(params)
        self._decoder = None
        self._log = getLogger('otp.part')
        self._data = b''
        self._digest_bytes: Optional[bytes] = None
        self._zer_bytes: Optional[bytes] = None

    @property
    def has_digest(self) -> bool:
        """Check if the partition supports any kind of digest (SW or HW)."""
        return any(getattr(self, f'{k}w_digest', False) for k in 'sh')

    @property
    def has_hw_digest(self) -> bool:
        """Check if the partition supports HW digest."""
        return getattr(self, 'hw_digest', False)

    @property
    def is_locked(self) -> bool:
        """Check if the partition is locked, based on its digest."""
        return (self.has_digest and self._digest_bytes and
                self._digest_bytes != bytes(self.DIGEST_SIZE))

    @property
    def is_zeroizable(self) -> bool:
        """Check if the partition supports zeroization."""
        return getattr(self, 'zeroizable', False)

    @property
    def is_empty(self) -> bool:
        """Report if the partition is empty."""
        if self._digest_bytes and sum(self._digest_bytes):
            return False
        if self._zer_bytes and sum(self._zer_bytes):
            return False
        return sum(self._data) == 0

    def __repr__(self) -> str:
        return repr(self.__dict__)

    def load(self, bfp: BinaryIO) -> None:
        """Load the content of the partition from a binary stream."""
        data = bfp.read(self.size)
        if len(data) != self.size:
            raise IOError(f'{self.name} Cannot load {self.size} from stream')
        zer_data = None
        if self.is_zeroizable:
            data, zer_data = data[:-self.ZER_SIZE], data[-self.ZER_SIZE:]
            self._zer_bytes = zer_data
        if self.has_digest:
            data, digest = data[:-self.DIGEST_SIZE], data[-self.DIGEST_SIZE:]
            self._digest_bytes = digest
        self._data = data

    def save(self, bfp: BinaryIO) -> None:
        """Save the content of the partition to a binary stream."""
        pos = bfp.tell()
        bfp.write(self._data)
        bfp.write(self._digest_bytes)
        bfp.write(self._zer_bytes)
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
        assert block_sz == Present.BLOCK_BIT_SIZE // 8
        if len(data) % block_sz != 0:
            # this case is valid but not yet implemented (paddding)
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
               ofp: Optional[TextIO] = None,
               filters: Optional[Sequence[str]] = None) -> None:
        """Decode the content of the partition."""
        buf = BytesIO(self._data)
        if ofp:
            def emit(fmt, *args):
                print(f'%-52s %s {fmt}' % args, file=ofp)
        else:
            def emit(fmt, *args):
                fmt = f'%-52s %s {fmt}'
                self._log.info(fmt, *args)
        pname = self.name
        offset = 0
        soff = 0
        if filters:
            fre = '|'.join(f.replace('*', '.*') for f in filters)
            filter_re = f'^({fre})$'
        else:
            filter_re = r'.*'
        for itname, itdef in self.items.items():
            itsize = itdef['size']
            if not itsize:
                self._log.error('Zero sized %s.%s', self.name, itname)
            itvalue = buf.read(itsize)
            soff = f'[{f"{base+offset:d}":>5s}]' if base is not None else ''
            offset += itsize
            if itname.startswith(f'{pname}_'):
                name = f'{pname}:{itname[len(pname)+1:]}'
            else:
                name = f'{pname}:{itname}'
            if not match(filter_re, itname, IGNORECASE):
                continue
            if itsize > 8:
                rvalue = bytes(reversed(itvalue))
                sval = hexlify(rvalue).decode()
                if decode and self._decoder:
                    dval = self._decoder.decode(itname, sval)
                    if dval is not None:
                        emit('(decoded) %s', name, soff, dval)
                        continue
                ssize = f'{{{itsize}}}'
                if not sum(itvalue) and wide < 2:
                    if decode:
                        emit('%5s (empty)', name, soff, ssize)
                    else:
                        emit('%5s 0...', name, soff, ssize)
                else:
                    if not wide and itsize > self.MAX_DATA_WIDTH:
                        sval = f'{sval[:self.MAX_DATA_WIDTH*2]}...'
                    emit('%5s %s', name, soff, ssize, sval)
            else:
                ival = int.from_bytes(itvalue, 'little')
                if decode:
                    if itdef.get('ismubi'):
                        emit('(decoded) %s',
                             name, soff,
                             str(OtpMap.MUBI8_BOOLEANS.get(ival, ival)))
                        continue
                    if itsize == 4 and ival in OtpMap.HARDENED_BOOLEANS:
                        emit('(decoded) %s',
                             name, soff, str(OtpMap.HARDENED_BOOLEANS[ival]))
                        continue
                emit('%x', name, soff, ival)
        offset = (offset + OtpMap.BLOCK_SIZE - 1) & ~(OtpMap.BLOCK_SIZE - 1)
        rpos = self.size
        dsoff = zsoff = f'[{"":5s}]'
        if self.is_zeroizable:
            rpos -= self.ZER_SIZE
            if base is not None:
                zsoff = f'[{f"{base+rpos:d}":>5s}]'
        if self.has_digest:
            rpos -= self.DIGEST_SIZE
            if base is not None:
                dsoff = f'[{f"{base+rpos:d}":>5s}]'
        if self._digest_bytes is not None:
            if match(filter_re, 'DIGEST', IGNORECASE):
                if not sum(self._digest_bytes) and decode:
                    val = '(empty)'
                else:
                    val = hexlify(self._digest_bytes).decode()
                ssize = f'{{{len(self._digest_bytes)}}}'
                emit('%5s %s', f'{pname}:DIGEST', dsoff, ssize, val)
        if self._zer_bytes is not None:
            if match(filter_re, 'ZER', IGNORECASE):
                if not sum(self._zer_bytes) and decode:
                    val = '(empty)'
                else:
                    val = hexlify(self._zer_bytes).decode()
                ssize = f'{{{len(self._zer_bytes)}}}'
                emit('%5s %s', f'{pname}:ZER', zsoff, ssize, val)
        if offset != rpos:
            self._log.warning('%s: offset %d, size %d', self.name, offset, rpos)

    def empty(self) -> None:
        """Empty the partition, including its digest if any."""
        self._data = bytes(len(self._data))
        if self.has_digest:
            self._digest_bytes = bytes(self.DIGEST_SIZE)

    def change_field(self, field: str,
                     value: Union[bytes, bytearray, bool, int, str]) -> None:
        """Change the content of a field.

           :param field: the name of the field to erase
           :param value: the new value of the field

           Valid value types depend on the field.
        """
        prop = self._retrieve_properties(field)
        if isinstance(value, str):
            value = value.encode('utf8')
        elif isinstance(value, bytearray):
            value = bytes(value)
        if isinstance(value, bytes):
            size = len(value)
            if size > prop.size:
                raise ValueError(f'{self.name}:{field} value cannot fit in '
                                 f'partition field')
            if size < prop.size:
                # pad value with NUL bytes
                value = b''.join((value, bytes(prop.size-size)))
        elif isinstance(value, bool):
            if not prop.mubi8:
                bool_type = 'hardened'
                bool_size = 4
            else:
                bool_type = 'mubi8'
                bool_size = 1
            if prop.size != bool_size:
                raise ValueError(f'{self.name}:{field} does not expect a '
                                 f'boolean value')
            bool_map = getattr(OtpMap, f'{bool_type.upper()}_BOOLEANS')
            ivalue = {v: k for k, v in bool_map.items()}[value]
            value = ivalue.to_bytes(length=bool_size, byteorder='little')
        elif isinstance(value, int):
            try:
                value = value.to_bytes(length=prop.size, byteorder='little')
            except ValueError as exc:
                raise ValueError(f'{self.name}:{field} cannot encode {value}') \
                    from exc
        self._log.info('Updating 0x%x..0x%x from %s', prop.offset, prop.end,
                       self.name)
        self._data = b''.join((self._data[:prop.offset], value,
                               self._data[prop.end:]))

    def decode_field(self, field: str) -> None:
        """Decode the content of a field.

           :param field: the name of the field to decode
           :param value: the value to decode
        """
        if not self._decoder:
            return None
        val = self.get_field(field)
        sval = hexlify(bytes(reversed(val)))
        return self._decoder.decode(field, sval).upper()

    def get_field(self, field: str) -> bytes:
        """Retrieve the content of a field.

           :param field: the name of the field to retrieve
        """
        prop = self._retrieve_properties(field)
        return self._data[prop.offset:prop.end]

    def erase_field(self, field: str) -> None:
        """Erase (reset) the content of a field.

           :param field: the name of the field to erase
        """
        prop = self._retrieve_properties(field)
        self._log.info('Erasing 0x%x..0x%x from %s', prop.offset, prop.end,
                       self.name)
        self._data = b''.join((self._data[:prop.offset], bytes(prop.size),
                               self._data[prop.end:]))

    def build_digest(self, digest_iv: int, digest_constant: int, erase: bool) \
            -> None:
        """Rebuild the digest of a partition from its content.

           :param erase: whether to erase any existing digest or combine it
        """
        if not self.has_hw_digest:
            raise ValueError(f'Partition {self.name} does not have a HW digest')
        assert self._digest_bytes is not None
        digest = self.compute_digest(self._data, digest_iv, digest_constant)
        self._log.info('New partition %s digest: %016x', self.name, digest)
        digest_len = len(self._digest_bytes)
        if erase:
            self._digest_bytes = bytes(digest_len)
        bdigest = digest.to_bytes(length=digest_len, byteorder='little')
        self._digest_bytes = bytes((a | b)
                                   for a, b in zip(self._digest_bytes, bdigest))

    def has_field(self, field: str) -> bool:
        """Tell whehther the partition has a field by its name.

           :param field: the name of the field to locate
           :return: true if the field is defined, false otherwise
        """
        try:
            self._retrieve_properties(field)
            return True
        except ValueError:
            return False

    def _retrieve_properties(self, field: str) -> tuple[int, int]:
        is_digest = self.has_digest and field.upper() == 'DIGEST'
        if not is_digest:
            if field not in self.items:
                # for some reason, some partitions are defined with their own
                # partition name as a prefix for their field name, some other
                # are not; try to workaround these inconsistent definitions
                search_field = f'{self.name}_{field}'
                if search_field not in self.items:
                    raise ValueError(f"No such field: '{field}'")
            else:
                search_field = field
        offset = 0
        itsize = 0
        mubi8 = False
        for itname, itdef in self.items.items():
            itsize = itdef['size']
            if itname == search_field:
                mubi8 = itsize == 1 and itdef.get('ismubi', False)
                break
            offset += itsize
        end = offset + itsize
        return OtpPartitionItemProp(field, offset, itsize, end, mubi8)


class OtpLifecycleExtension(OtpLifecycle, OtpPartitionDecoder):
    """Decoder for Lifecyle bytes sequences.
    """

    def decode(self, category: str, seq: str) -> Union[str, int, None]:
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
