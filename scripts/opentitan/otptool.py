#!/usr/bin/env python3

# Copyright (c) 2023-2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""QEMU OT tool to manage OTP files.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from argparse import ArgumentParser, FileType
from binascii import hexlify, unhexlify
from io import BytesIO, StringIO
from logging import getLogger
from os.path import basename
from re import match as re_match, sub as re_sub
from struct import calcsize as scalc, pack as spack, unpack as sunpack
from sys import argv, exit as sysexit, modules, stderr, stdout, version_info
from textwrap import fill
from traceback import format_exc
from typing import (Any, BinaryIO, Dict, Iterator, List, Optional, Set, TextIO,
                    Tuple, Union)

from ot.util.log import configure_loggers
from ot.util.misc import HexInt

try:
    # try to load HJSON if available
    from hjson import load as hjload
except ImportError:
    hjload = None

try:
    # try to load Present if available
    from present import Present
except ImportError:
    Present = None

# requirement: Python 3.7+: dict entries are kept in creation order
if version_info[:2] < (3, 7):
    raise RuntimeError('Unsupported Python version')


def round_up(value: int, rnd: int) -> int:
    """Round up a integer value."""
    return (value + rnd - 1) & -rnd


class classproperty(property):
    """Getter property decorator for a class"""
    # pylint: disable=invalid-name
    def __get__(self, obj: Any, objtype=None) -> Any:
        return super().__get__(objtype)


class OtpPartitionDecoder:
    """Custom partition value decoder."""

    def decode(self, category: str, seq: str) -> Union[str, int, None]:
        """Decode a value (if possible)."""
        raise NotImplementedError('abstract base class')


class OtpPartition:
    """Partition abstract base class.

       :param params: initial partition attributes.
    """
    # pylint: disable=no-member

    DIGEST_SIZE = 8  # bytes

    MAX_DATA_WIDTH = 20

    def __init__(self, params):
        self.__dict__.update(params)
        self._decoder = None
        self._log = getLogger('otptool.part')
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

    def load(self, bfp: BinaryIO) -> None:
        """Load the content of the partition from a binary stream."""
        data = bfp.read(self.size)
        if len(data) != self.size:
            raise IOError(f'{self.name} Cannot load {self.size} from stream')
        if self.has_digest:
            data, digest = data[:-self.DIGEST_SIZE], data[-self.DIGEST_SIZE:]
            self._digest_bytes = digest
        self._data = data

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

    def decode(self, decode: bool = True, wide: int = 0,
               ofp: Optional[TextIO] = None) -> None:
        """Decode the content of the partition."""
        buf = BytesIO(self._data)
        if ofp:
            def emit(fmt, *args):
                print(fmt % args, file=ofp)
        else:
            emit = self._log.info
        pname = self.name
        for itname, itdef in self.items.items():
            itsize = itdef['size']
            itvalue = buf.read(itsize)
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
                        emit('%-46s (decoded) %s', name, dval)
                        continue
                if not sum(itvalue) and wide < 2:
                    emit('%-46s [%d] 0...', name, itsize)
                else:
                    if not wide and itsize > self.MAX_DATA_WIDTH:
                        sval = f'{sval[:self.MAX_DATA_WIDTH*2]}...'
                    emit('%-46s [%d] %s', name, itsize, sval)
            else:
                ival = int.from_bytes(itvalue, 'little')
                if decode:
                    if itdef.get('ismubi'):
                        emit('%-46s (decoded) %s',
                             name, str(OtpMap.MUBI8_BOOLEANS.get(ival, ival)))
                    elif itsize == 4 and ival in OtpMap.HARDENED_BOOLEANS:
                        emit('%-46s (decoded) %s',
                             name, str(OtpMap.HARDENED_BOOLEANS[ival]))
                else:
                    emit('%-46s %x', name, ival)


class OtpLifecycleExtension(OtpPartitionDecoder):
    """Decoder for Lifecyle bytes sequences.
    """

    EXTRA_SLOTS = {
        'lc_state': {
            'post_transition': None,
            'escalate': None,
            'invalid': None,
        }
    }

    def __init__(self):
        self._log = getLogger('otptool.lc')
        self._tables: Dict[str, Dict[str, str]] = {}

    def decode(self, category: str, seq: str) -> Union[str, int, None]:
        return self._tables.get(category, {}).get(seq, None)

    def load(self, svp: TextIO):
        """Decode LifeCycle information.

           :param svp: System Verilog stream with OTP definitions.
        """
        ab_re = (r"\s*parameter\s+logic\s+\[\d+:\d+\]\s+"
                 r"([ABCD]\d+|ZRO)\s+=\s+\d+'(b(?:[01]+)|h(?:[0-9a-fA-F]+));")
        tbl_re = r"\s*Lc(St|Cnt)(\w+)\s+=\s+\{([^\}]+)\}\s*,?"
        codes: Dict[str, int] = {}
        sequences: Dict[str, List[str]] = {}
        for line in svp:
            cmt = line.find('//')
            if cmt >= 0:
                line = line[:cmt]
            line = line.strip()
            abmo = re_match(ab_re, line)
            if not sequences and abmo:
                name = abmo.group(1)
                sval = abmo.group(2)
                val = int(sval[1:], 2 if sval.startswith('b') else 16)
                if name in codes:
                    self._log.error('Redefinition of %s', name)
                    continue
                codes[name] = val
                continue
            smo = re_match(tbl_re, line)
            if smo:
                kind = smo.group(1).lower()
                name = smo.group(2)
                seq = smo.group(3)
                items = [x.strip() for x in seq.split(',')]
                inv = [it for it in items if it not in codes]
                if inv:
                    self._log.error('Unknown state seq: %s', ', '.join(inv))
                if kind not in sequences:
                    sequences[kind] = {}
                sequences[kind][name] = items
                continue
        for kind, seqs in sequences.items():
            mkind, conv = dict(st=('LC_STATE', str),
                               cnt=('LC_TRANSITION_CNT', int))[kind]
            self._tables[mkind] = {}
            for ref, seq in seqs.items():
                seq = ''.join((f'{x:04x}'for x in map(codes.get, seq)))
                self._tables[mkind][seq] = conv(ref)

    def save(self, cfp: TextIO):
        """Save OTP life cycle definitions as a C file.

           :param cfp: output text stream
        """
        print(f'/* Section auto-generated with {basename(__file__)} '
              f'script */', file=cfp)
        for kind, table in self._tables.items():
            enum_io = StringIO()
            array_io = StringIO()
            count = len(table)
            length = max(len(x) for x in table.keys())//2
            print(f'static const char {kind.lower()}s[{count}u][{length}u]'
                  f' = {{', file=array_io)
            pad = ' ' * 8
            for seq, ref in table.items():
                if isinstance(ref, str):
                    slot = f'{kind}_{ref}'.upper()
                    print(f'    {slot},', file=enum_io)
                else:
                    slot = f'{ref}u'
                seqstr = ', '.join((f'0x{b:02x}u' for b in
                                    reversed(unhexlify(seq))))
                defstr = fill(seqstr, width=80, initial_indent=pad,
                              subsequent_indent=pad)
                print(f'    [{slot}] = {{\n{defstr}\n    }},',
                      file=array_io)
            print('};', file=array_io)
            for extra in self.EXTRA_SLOTS.get(kind.lower(), {}):
                slot = f'{kind}_{extra}'.upper()
                print(f'    {slot},', file=enum_io)
            enum_str = enum_io.getvalue()
            if enum_str:
                # likely to be moved to a header file
                print(f'enum {kind.lower()} {{\n{enum_str}}};\n', file=cfp)
            print(f'{array_io.getvalue()}', file=cfp)
        print('/* End of auto-generated section */', file=cfp)


class OtpMap:
    """OTP configuration.

       Assume partition file does not contain any error or missing information,
       it should have been validated by OT tools first.
    """
    BLOCK_SIZE = 8

    HARDENED_BOOLEANS = {
        0x739: True,
        0x1d4: False
    }

    MUBI8_BOOLEANS = {
        0x96: False,
        0x69: True,
        0x00: None
    }

    def __init__(self):
        self._log = getLogger('otptool.map')
        self._map: Dict = {}
        self._otp_size = 0
        self._partitions: List[OtpPartition] = []

    def load(self, hfp: TextIO) -> None:
        """Parse a HJSON configuration file, typically otp_ctrl_mmap.hjson
        """
        if hjload is None:
            raise ImportError('HJSON module is required')
        self._map = hjload(hfp, object_pairs_hook=dict)
        otp = self._map['otp']
        self._otp_size = int(otp['width']) * int(otp['depth'])
        self._generate_partitions()
        self._compute_locations()

    @property
    def partitions(self) -> Dict[str, Any]:
        """Return the partitions (in any)"""
        return self._map.get('partitions', {})

    @classmethod
    def part_offset(cls, part: Dict[str, Any]) -> int:
        """Get the offset of a partition."""
        # expect a KeyError if missing
        return int(part['offset'])

    def enumerate_partitions(self) -> Iterator[OtpPartition]:
        """Enumerate the partitions in their address order."""
        return iter(self._partitions)

    def _generate_partitions(self) -> None:
        parts = self.partitions
        have_offset = all('offset' in p for p in parts)
        if not have_offset:
            # either all or no partition should have an offset definition
            if any('offset' in p for p in parts):
                raise RuntimeError('Incoherent offset use in partitions')
        if have_offset:
            # if offset are defined, first create a shallow copy of the
            # partition in sorted order
            parts = list(sorted(parts, key=OtpMap.part_offset))
        self._partitions = []
        for part in parts:
            # shallow copy of the partition
            part = dict(part)
            name = part['name']
            # remove the name from the dict
            del part['name']
            desc = part.get('desc', '').replace('\n', ' ')
            # remove description from partition
            if desc:
                del part['desc']
            # remove descriptions from items
            items = {}
            for item in part.get('items', []):
                assert isinstance(item, dict)
                # shallow copy
                item = dict(item)
                if 'desc' in item:
                    del item['desc']
                # assume name & size are always defined for each item
                item_name = item['name']
                del item['name']
                item_size = int(item['size'])
                # handle very weird case where the size define the number of
                # a multibit bool but not its size in bytes
                item_size = round_up(item_size, 4)
                item['size'] = item_size
                assert item_name not in items
                items[item_name] = item
            part['items'] = items
            # size are always encoded as strings, not integers
            items_size = sum(int(i.get('size')) for i in items.values())
            # some partitions define their overall size, most don't
            # if the size is defined, it takes precedence over the sum of its
            # items
            part_size = int(part.get('size', '0'))
            has_digest = any(part.get(f'{k}w_digest') for k in 'sh')
            if has_digest:
                items_size += OtpPartition.DIGEST_SIZE
            if part_size:
                assert items_size <= part_size
            else:
                part_size = round_up(items_size, self.BLOCK_SIZE)
            # update the partition with is actual size
            part['size'] = part_size
            # special ugly case as configuration file defines is_keymgr per item
            # but RTL defines it per partition for some reason
            kmm = self._check_keymgr_materials(name, part['items'])
            if kmm:
                part[kmm[0]] = kmm[1]
            prefix = name.title().replace('_', '')
            partname = f'{prefix}Part'
            newpart = type(partname, (OtpPartition,),
                           dict(name=name, __doc__=desc))
            self._partitions.append(newpart(part))

    def _check_keymgr_materials(self, partname: str, items: Dict[str, Dict]) \
            -> Optional[Tuple[str, bool]]:
        """Check partition for key manager material fields."""
        kms: Dict[str, bool] = {}
        kmprefix = 'iskeymgr'
        for props in items.values():
            for prop, value in props.items():
                if prop.startswith(kmprefix):
                    kind = prop[len(kmprefix):]
                    if kind not in kms:
                        kms[kind] = set()
                    kms[kind].add(value)
        kind_count = len(kms)
        if not kind_count:
            return None
        if kind_count > 1:
            raise ValueError(f'Incoherent key manager material definition in '
                             f'{partname} partition')
        kind = set(kms).pop()
        enable = any(kms[kind])
        return f'{kmprefix}{kind}', enable

    def _compute_locations(self) -> None:
        """Update partitions with their location within the OTP map."""
        absorb_parts = [p for p in self._partitions
                        if getattr(p, 'absorb', False)]
        total_size = sum(p.size for p in self._partitions)
        rem_size = self._otp_size - total_size
        rem_blocks = rem_size // self.BLOCK_SIZE
        absorb_count = len(absorb_parts)
        blk_per_part = rem_blocks // absorb_count
        extra_blocks = rem_blocks % absorb_count
        self._log.info("%d bytes (%d blocks) to absorb into %d partition%s",
                       rem_size, rem_blocks, absorb_count,
                       's' if absorb_count > 1 else '')
        for part in absorb_parts:
            psize = part.size
            part.size += self.BLOCK_SIZE * blk_per_part
            if extra_blocks:
                part.size += self.BLOCK_SIZE
                extra_blocks -= 1
            self._log.info('Partition %s size augmented from %u to %u',
                           part.name, psize, part.size)
        for part in self._partitions:
            part_offset = 0
            for part in self._partitions:
                if part.sw_digest or part.hw_digest:
                    digest_offset = part_offset + part.size - 8
                else:
                    digest_offset = None
                setattr(part, 'offset', part_offset)
                setattr(part, 'digest_offset', digest_offset)
                part_offset += part.size
        assert part_offset == self._otp_size, "Unexpected partition offset"


class OTPPartitionDesc:
    """OTP Partition descriptor generator."""

    ATTRS = dict(
        size=None,
        offset=None,
        digest_offset=None,
        hw_digest='',
        sw_digest='',
        secret='',
        variant='buffer',
        write_lock='wlock',
        read_lock='rlock',
        integrity='',
        iskeymgr='',
        iskeymgr_creator='',
        iskeymgr_owner='',
        wide=''
    )

    def __init__(self, otpmap: OtpMap):
        self._log = getLogger('otptool.partdesc')
        self._otpmap = otpmap

    def save(self, hjname: str, cfp: TextIO) -> None:
        """Generate a C file with a static description for the partitions."""
        # pylint: disable=f-string-without-interpolation
        attrs = {n: getattr(self, f'_convert_to_{k}') if k else lambda x: x
                 for n, k in self.ATTRS.items() if k is not None}
        scriptname = basename(argv[0])
        print(f'/* Generated from {hjname} with {scriptname} */', file=cfp)
        print(file=cfp)
        print('/* clang-format off */', file=cfp)
        print('/* NOLINTBEGIN */', file=cfp)
        print('static const OtOTPPartDesc OtOTPPartDescs[] = {', file=cfp)
        for part in self._otpmap.enumerate_partitions():
            print(f'    [OTP_PART_{part.name}] = {{', file=cfp)
            print(f'        .size = {part.size}u,', file=cfp)
            print(f'        .offset = {part.offset}u,', file=cfp)
            if part.digest_offset is not None:
                print(f'        .digest_offset = {part.digest_offset}u,',
                      file=cfp)
            else:
                print(f'        .digest_offset = UINT16_MAX,',   # noqa: F541
                      file=cfp)
            for attr in attrs:
                value = getattr(part, attr, None)
                if value is None:
                    continue
                convs = attrs[attr](value)
                if not isinstance(convs, list):
                    convs = [convs]
                for conv in convs:
                    if isinstance(conv, tuple):
                        attr_name = conv[0]
                        attr_val = conv[1]
                    else:
                        attr_name = attr
                        attr_val = conv
                    if isinstance(attr_val, bool):
                        attr_val = str(attr_val).lower()
                    print(f'        .{attr_name} = {attr_val},', file=cfp)
            print(f'    }},', file=cfp)  # noqa: F541
        print('};', file=cfp)
        print('', file=cfp)
        print('#define OTP_PART_COUNT ARRAY_SIZE(OtOTPPartDescs)', file=cfp)
        print(file=cfp)
        print('/* NOLINTEND */', file=cfp)
        print('/* clang-format on */', file=cfp)
        # pylint: enable=f-string-without-interpolation

    @classmethod
    def _convert_to_bool(cls, value) -> str:
        return str(value).lower()

    @classmethod
    def _convert_to_buffer(cls, value) -> Tuple[str, bool]:
        return {
            'unbuffered': ('buffered', False),
            'buffered': ('buffered', True),
            'lifecycle': ('buffered', True),
        }[value.lower()]

    @classmethod
    def _convert_to_wlock(cls, value) -> bool:
        return value == 'digest'

    @classmethod
    def _convert_to_rlock(cls, value) -> List[Tuple[str, bool]]:
        value = value.lower()
        if value == 'csr':
            return [('read_lock_csr', True), ('read_lock', True)]
        if value == 'digest':
            return 'read_lock', True
        if value == 'none':
            return 'read_lock', False
        assert False, 'Unknown RLOCK type'


class OTPRegisterDef:
    """OTP Partition register generator."""

    def __init__(self, otpmap: OtpMap):
        self._log = getLogger('otptool.reg')
        self._otpmap = otpmap

    def save(self, hjname: str, cfp: TextIO) -> None:
        """Generate a C file with register definition for the partitions."""
        reg_offsets = []
        reg_sizes = []
        part_names = []
        for part in self._otpmap.enumerate_partitions():
            part_names.append(f'OTP_PART_{part.name}')
            offset = part.offset
            reg_sizes.append((f'{part.name}_SIZE', part.size))
            for itname, itdict in part.items.items():
                size = itdict['size']
                if not itname.startswith(f'{part.name}_'):
                    name = f'{part.name}_{itname}'.upper()
                else:
                    name = itname
                reg_offsets.append((name, offset))
                reg_sizes.append((f'{name}_SIZE', size))
                offset += size
        scriptname = basename(argv[0])
        print(f'/* Generated from {hjname} with {scriptname} */')
        print(file=cfp)
        print('/* clang-format off */', file=cfp)
        for reg, off in reg_offsets:
            print(f'REG32({reg}, {off}u)', file=cfp)
        print(file=cfp)
        regwidth = max(len(r[0]) for r in reg_sizes)
        for reg, size in reg_sizes:
            print(f'#define {reg:{regwidth}s} {size}u', file=cfp)
        print(file=cfp)
        pcount = len(part_names)
        part_names.extend((
            '_OTP_PART_COUNT',
            'OTP_ENTRY_DAI = _OTP_PART_COUNT',
            'OTP_ENTRY_KDI',
            '_OTP_ENTRY_COUNT'))
        print('typedef enum {', file=cfp)
        for pname in part_names:
            print(f'    {pname},', file=cfp)
        print('} OtOTPPartitionType;', file=cfp)
        print(file=cfp)
        print('static const char *PART_NAMES[] = {', file=cfp)
        for pname in part_names[:pcount]:
            print(f'    OTP_NAME_ENTRY({pname}),', file=cfp)
        print('};', file=cfp)
        print('/* clang-format on */', file=cfp)
        print(file=cfp)


class OtpImage:
    """QEMU 'RAW' OTP image."""

    HEADER_FORMAT = {
        'magic': '4s',  # "vOTP"
        'hlength': 'I',  # count of header bytes after this point
        'version': 'I',  # version of the header
        'eccbits': 'H',  # count of ECC bits for each ECC granule
        'eccgran': 'H',  # size in bytes of ECC granule
        'dlength': 'I',  # count of data bytes (padded to 64-bit entries)
        'elength': 'I',  # count of ecc bytes (padded to 64-bit entries)
    }

    HEADER_FORMAT_V2_EXT = {
        'digiv':   '8s',  # Present digest scrambler IV
        'digfc':   '16s',  # Present digest scrambler finalization constant
    }

    KINDS = {
        'OTP MEM': 'otp',
        'FUSEMAP': 'fuz',
    }

    RE_VMEMLOC = r'(?i)^@((?:[0-9a-f]{2})+)\s((?:[0-9a-f]{2})+)$'
    RE_VMEMDESC = r'(?i)^//\s?([\w\s]+) file with (\d+)[^\d]*(\d+)\s?bit layout'

    DEFAULT_ECC_BITS = 6

    def __init__(self, ecc_bits: Optional[int] = None):
        self._log = getLogger('otptool.img')
        self._header: Dict[str, Any] = {}
        self._magic = b''
        self._data = b''
        self._ecc = b''
        if ecc_bits is None:
            ecc_bits = self.DEFAULT_ECC_BITS
        self._ecc_bits = ecc_bits
        self._ecc_bytes = (ecc_bits + 7) // 8
        self._ecc_granule = 0
        self._digest_iv: Optional[int] = None
        self._digest_constant: Optional[int] = None
        self._partitions: List[OtpPartition] = []

    @property
    def version(self) -> int:
        """Provide the version of the RAW image."""
        return self._header.get('version', 0)

    @property
    def loaded(self) -> int:
        """Report whether data have been loaded into the image."""
        return len(self._data) > 0

    @property
    def is_opentitan(self) -> bool:
        """Report whether the current image contains OpenTitan OTP data."""
        return self._magic == b'vOTP'

    @classproperty
    def vmem_kinds(cls) -> List[str]:
        """Reports the supported content kinds of VMEM files."""
        # pylint: disable=no-self-argument
        return ['auto'] + list(cls.KINDS.values())

    @classproperty
    def logger(self):
        """Return logger instance."""
        # pylint: disable=no-self-argument
        return getLogger('otptool')

    def load_raw(self, rfp: BinaryIO) -> None:
        """Load OTP image from a QEMU 'RAW' image stream."""
        header = self._load_header(rfp)
        self._header = header
        self._data = rfp.read(header['dlength'])
        self._ecc = rfp.read(header['elength'])
        if header['version'] > 1:
            self._digest_iv = header['digiv']
            self._digest_constant = header['digfc']

    def save_raw(self, rfp: BinaryIO) -> None:
        """Save OTP image as a QEMU 'RAW' image stream."""
        header = self._build_header()
        rfp.write(header)
        self._pad(rfp)
        rfp.write(self._data)
        self._pad(rfp)
        rfp.write(self._ecc)
        self._pad(rfp, 4096)

    def load_vmem(self, vfp: TextIO, vmem_kind: Optional[str] = None,
                  swap: bool = True):
        """Parse a VMEM '24' text stream."""
        data_buf: List[bytes] = []
        ecc_buf: List[bytes] = []
        last_addr = 0
        granule_sizes: Set[int] = set()
        vkind: Optional[str] = None
        row_count = 0
        byte_count = 0
        line_count = 0
        if vmem_kind:
            vmem_kind = vmem_kind.lower()
            if vmem_kind == 'auto':
                vmem_kind = None
        if vmem_kind and vmem_kind not in self.KINDS.values():
            raise ValueError(f"Unknown VMEM file kind '{vmem_kind}'")
        for lno, line in enumerate(vfp, start=1):
            if vkind is None:
                kmo = re_match(self.RE_VMEMDESC, line)
                if kmo:
                    vkind = kmo.group(1)
                    row_count = int(kmo.group(2))
                    bits = int(kmo.group(3))
                    byte_count = bits // 8
                    continue
            line = re_sub(r'//.*', '', line)
            line = line.strip()
            if not line:
                continue
            lmo = re_match(self.RE_VMEMLOC, line)
            if not lmo:
                self._log.error('Unexpected line @ %d: %s', lno, line)
                continue
            line_count += 1
            saddr, sdata = lmo.groups()
            addr = int(saddr, 16)
            if last_addr < addr:
                self._log.info('Padding addr from 0x%04x to 0x%04x',
                               last_addr, addr)
                data_buf.append(bytes(addr-last_addr))
            rdata = unhexlify(sdata)
            if byte_count != len(rdata):
                self._log.warning('Expected %d bytes @ line %s, found %d',
                                  byte_count, lno, len(sdata))
            ecc, data = rdata[:self._ecc_bytes], rdata[self._ecc_bytes:]
            if swap:
                data = bytes(reversed(data))
            data_buf.append(data)
            ecc_buf.append(ecc)
            dlen = len(data)
            granule_sizes.add(dlen)
            last_addr = addr+dlen  # ECC is not accounted for in address
        self._data = b''.join(data_buf)
        self._ecc = b''.join(ecc_buf)
        if granule_sizes:
            if len(granule_sizes) != 1:
                raise ValueError('Variable data size')
            self._ecc_granule = granule_sizes.pop()
        if row_count and row_count != line_count:
            self._log.error('Should have parsed %d lines, found %d',
                            row_count, line_count)
        if not vkind:
            if vmem_kind:
                vkind = vmem_kind
        else:
            vkind = self.KINDS.get(vkind.upper(), None)
            if vmem_kind:
                if vkind and vkind != vmem_kind:
                    self._log.warning("Detected VMEM kind '%s' differs from "
                                      "'%s'", vkind, vmem_kind)
                # use user provided type, even if it is not the one detected
                vkind = vmem_kind
        if not vkind:
            raise ValueError('Unable to detect VMEM find, please specify')
        self._magic = f'v{vkind[:3].upper()}'.encode()

    def load_lifecycle(self, lcext: OtpLifecycleExtension) -> None:
        """Load lifecyle values."""
        for part in self._partitions:
            if part.name == 'LIFE_CYCLE':
                part.set_decoder(lcext)

    # pylint: disable=invalid-name
    def set_digest_iv(self, iv: int) -> None:
        """Set the Present digest initialization 64-bit vector."""
        if iv >> 64:
            raise ValueError('Invalid digest initialization vector')
        self._digest_iv = iv

    def set_digest_constant(self, constant: int) -> None:
        """Set the Present digest finalization 128-bit constant."""
        if constant >> 128:
            raise ValueError('Invalid digest finalization constant')
        self._digest_constant = constant

    @property
    def has_present_constants(self) -> bool:
        """Reports whether the Present scrambler constants are known/defined."""
        return self._digest_iv is not None and self._digest_constant is not None

    def dispatch(self, cfg: OtpMap) -> None:
        """Dispatch RAW image data into the partitions."""
        bfp = BytesIO(self._data)
        for part in cfg.enumerate_partitions():
            self._log.debug('%s %d', part.name, bfp.tell())
            part.load(bfp)
            self._partitions.append(part)
        # all data bytes should have been dispatched into the partitions
        assert bfp.tell() == len(self._data), 'Unexpected remaining data bytes'
        if self._header:
            data_size = self._header.get('dlength', 0)
            assert bfp.tell() == data_size, 'Unexpected remaining data bytes'

    def verify(self, show: bool = False) -> bool:
        """Verify the partition digests, if any."""
        if any(c is None for c in (self._digest_iv, self._digest_constant)):
            raise RuntimeError('Missing Present constants')
        results: Dict[str, Optional[bool]] = {}
        for part in self._partitions:
            if not part.hw_digest:
                continue
            results[part.name] = part.verify(self._digest_iv,
                                             self._digest_constant)
        if show:
            print('HW digests:')
            width = max(len(x) for x in results)
            for name, result in results.items():
                if result is None:
                    status = 'No digest'
                elif result:
                    status = 'OK'
                else:
                    status = 'Failed'
                print(f' * {name:{width}s}: {status}')
        # any partition with a defined digest should be valid
        return not any(r is False for r in results.values())

    def decode(self, decode: bool = True, wide: int = 0,
               ofp: Optional[TextIO] = None) -> None:
        """Decode the content of the image, one partition at a time."""
        version = self.version
        if version:
            print(f'OTP image v{version}')
            if version > 1:
                print(f' * present iv       {self._digest_iv:016x}')
                print(f' * present constant {self._digest_constant:032x}')
        for part in self._partitions:
            part.decode(decode, wide, ofp)

    def _load_header(self, bfp: BinaryIO) -> Dict[str, Any]:
        hfmt = self.HEADER_FORMAT
        fhfmt = ''.join(hfmt.values())
        # hlength is the length of header minus the two first items (T, L)
        fhsize = scalc(fhfmt)
        hdata = bfp.read(fhsize)
        parts = sunpack(f'<{fhfmt}', hdata)
        header = dict(zip(hfmt.keys(), parts))
        magics = set(f'v{k.upper()}'.encode() for k in self.KINDS.values())
        if header['magic'] not in magics:
            raise ValueError(f'{bfp.name} is not a QEMU OTP RAW image')
        self._magic = header['magic']
        version = header['version']
        if version > 2:
            raise ValueError(f'{bfp.name} is not a valid QEMU OTP RAW image')
        if version > 1:
            hfmt = self.HEADER_FORMAT_V2_EXT
            fhfmt = ''.join(hfmt.values())
            fhsize = scalc(fhfmt)
            hdata = bfp.read(fhsize)
            parts = sunpack(f'<{fhfmt}', hdata)
            headerv2 = dict(zip(hfmt.keys(),
                                (HexInt(int.from_bytes(v, 'little'))
                                    for v in parts)))
            header.update(headerv2)
        return header

    def _build_header(self) -> bytes:
        assert self._magic, "File kind unknown"
        hfmt = self.HEADER_FORMAT
        # use V2 image format if Present scrambling constants are available,
        # otherwise use V1
        use_v2 = bool(self._digest_iv) or bool(self._digest_constant)
        if use_v2:
            hfmt.update(self.HEADER_FORMAT_V2_EXT)
        fhfmt = ''.join(hfmt.values())
        shfmt = ''.join(hfmt[k] for k in list(hfmt)[:2])
        # hlength is the length of header minus the two first items (T, L)
        hlen = scalc(fhfmt)-scalc(shfmt)
        dlen = (len(self._data)+7) & ~0x7
        elen = (len(self._ecc)+7) & ~0x7
        values = dict(magic=self._magic, hlength=hlen, version=1+int(use_v2),
                      eccbits=self._ecc_bits, eccgran=self._ecc_granule,
                      dlength=dlen, elength=elen)
        if use_v2:
            values['digiv'] = self._digest_iv.to_bytes(8, byteorder='little')
            values['digfc'] = self._digest_constant.to_bytes(16,
                                                             byteorder='little')
        args = [values[k] for k in hfmt]
        header = spack(f'<{fhfmt}', *args)
        return header

    def _pad(self, bfp: BinaryIO, padsize: Optional[int] = None):
        if padsize is None:
            padsize = OtpMap.BLOCK_SIZE
        tail = bfp.tell() % padsize
        if tail:
            bfp.write(bytes(padsize-tail))


def main():
    """Main routine"""
    debug = True
    try:
        desc = modules[__name__].__doc__.split('.', 1)[0].strip()
        argparser = ArgumentParser(description=f'{desc}.')
        files = argparser.add_argument_group(title='Files')
        files.add_argument('-j', '--otp-map', type=FileType('rt'),
                           metavar='HJSON',
                           help='input OTP controller memory map file')
        files.add_argument('-m', '--vmem', type=FileType('rt'),
                           help='input VMEM file')
        files.add_argument('-l', '--lifecycle', type=FileType('rt'),
                           metavar='SV',
                           help='input lifecycle system verilog file')
        files.add_argument('-o', '--output', metavar='C', type=FileType('wt'),
                           help='output C file')
        files.add_argument('-r', '--raw',
                           help='QEMU OTP raw image file')
        params = argparser.add_argument_group(title='Parameters')
        # pylint: disable=unsubscriptable-object
        params.add_argument('-k', '--kind',
                            choices=OtpImage.vmem_kinds,
                            help=f'kind of content in VMEM input file, '
                                 f'default: {OtpImage.vmem_kinds[0]}')
        params.add_argument('-e', '--ecc', type=int,
                            default=OtpImage.DEFAULT_ECC_BITS,
                            metavar='BITS', help='ECC bit count')
        params.add_argument('-c', '--constant', type=HexInt.parse,
                            metavar='INT',
                            help='finalization constant for Present scrambler')
        params.add_argument('-i', '--iv', type=HexInt.parse, metavar='INT',
                            help='initialization vector for Present scrambler')
        params.add_argument('-w', '--wide', action='count', default=0,
                            help='use wide output, non-abbreviated content')
        params.add_argument('-n', '--no-decode', action='store_true',
                            default=False,
                            help='do not attempt to decode OTP fields')
        commands = argparser.add_argument_group(title='Commands')
        commands.add_argument('-s', '--show', action='store_true',
                              help='show the OTP content')
        commands.add_argument('-D', '--digest', action='store_true',
                              help='check the OTP HW partition digest')
        generate = commands.add_mutually_exclusive_group()
        generate.add_argument('-L', '--generate-lc', action='store_true',
                              help='generate lc_ctrl C arrays')
        generate.add_argument('-P', '--generate-parts', action='store_true',
                              help='generate partition descriptor C arrays')
        generate.add_argument('-R', '--generate-regs', action='store_true',
                              help='generate partition register C definitions')
        extra = argparser.add_argument_group(title='Extras')
        extra.add_argument('-v', '--verbose', action='count',
                           help='increase verbosity')
        extra.add_argument('-d', '--debug', action='store_true',
                           help='enable debug mode')
        args = argparser.parse_args()
        debug = args.debug

        configure_loggers(args.verbose, 'otptool')

        otp = OtpImage(args.ecc)

        if not (args.vmem or args.raw):
            if args.show or args.digest:
                argparser.error('At least one raw or vmem file is required')

        if not args.vmem and args.kind:
            argparser.error('VMEM kind only applies for VMEM input files')

        otpmap: Optional[OtpMap] = None
        lcext: Optional[OtpLifecycleExtension] = None
        partdesc: Optional[OTPPartitionDesc] = None

        if not args.otp_map:
            if args.generate_parts or args.generate_regs:
                argparser.error('Generator require an OTP map')
            if args.show:
                argparser.error('Cannot decode OTP values without an OTP map')
            if args.digest:
                argparser.error('Cannot verify OTP digests without an OTP map')
        else:
            otpmap = OtpMap()
            otpmap.load(args.otp_map)

        if args.lifecycle:
            lcext = OtpLifecycleExtension()
            lcext.load(args.lifecycle)

        output = stdout if not args.output else args.output

        if args.generate_parts:
            partdesc = OTPPartitionDesc(otpmap)
            partdesc.save(basename(args.otp_map.name), output)

        if args.generate_regs:
            regdef = OTPRegisterDef(otpmap)
            regdef.save(basename(args.otp_map.name), output)

        if args.generate_lc:
            if not lcext:
                argparser.error('Cannot generate LC array w/o a lifecycle file')
            lcext.save(output)

        if args.vmem:
            otp.load_vmem(args.vmem, args.kind)
        if args.raw:
            # if no VMEM is provided, select the RAW file as an input file
            # otherwise it is selected as an output file
            if not args.vmem:
                with open(args.raw, 'rb') as rfp:
                    otp.load_raw(rfp)

        if otp.loaded:
            if not otp.is_opentitan:
                ot_opts = ('iv', 'constant', 'digest', 'generate_parts',
                           'generate_regs', 'generate_lc', 'otp_map',
                           'lifecycle')
                if any(getattr(args, a) for a in ot_opts):
                    argparser.error('Selected option only applies to OpenTitan '
                                    'images')
                if args.show:
                    argparser.error('Showing content of non-OpenTitan image is '
                                    'not supported')
            if args.iv:
                otp.set_digest_iv(args.iv)
            if args.constant:
                otp.set_digest_constant(args.constant)
            if otpmap:
                otp.dispatch(otpmap)
            if lcext:
                otp.load_lifecycle(lcext)
            if args.show:
                otp.decode(not args.no_decode, args.wide, stdout)
            if args.digest:
                if not otp.has_present_constants:
                    if args.raw and otp.version == 1:
                        msg = '; OTP v1 image does not track them'
                    else:
                        msg = ''
                    # can either be defined on the CLI or in an existing QEWU
                    # image
                    argparser.error(f'Present scrambler constants are required '
                                    f'to verify the partition digest{msg}')
                otp.verify(True)
            if args.raw and args.vmem:
                # when both RAW and VMEM are selected, QEMU RAW image file
                # should be generated
                with open(args.raw, 'wb') as rfp:
                    otp.save_raw(rfp)

    except (IOError, ValueError, ImportError) as exc:
        print(f'\nError: {exc}', file=stderr)
        if debug:
            print(format_exc(chain=False), file=stderr)
        sysexit(1)
    except KeyboardInterrupt:
        sysexit(2)


if __name__ == '__main__':
    main()
