#!/usr/bin/env python3

"""Create/update an OpenTitan backend flash file.
"""

# Copyright (c) 2023 Rivos, Inc.
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
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

from argparse import ArgumentParser, FileType
from binascii import hexlify
from hashlib import sha256
from itertools import repeat
from io import BytesIO
from logging import DEBUG, ERROR, getLogger, Formatter, StreamHandler
from os import SEEK_END, SEEK_SET, rename, stat
from os.path import abspath, basename, exists, isfile
from re import sub as re_sub
from struct import calcsize as scalc, pack as spack, unpack as sunpack
from sys import exit as sysexit, modules, stderr, version_info
from traceback import format_exc
from typing import (Any, BinaryIO, Dict, Iterator, List, NamedTuple, Optional,
                    Tuple, Union)

try:
    # note: pyelftools package is an OpenTitan toolchain requirement, see
    # python-requirements.txt file from OT top directory.
    from elftools.common.exceptions import ELFError
    from elftools.elf.constants import SH_FLAGS
    from elftools.elf.elffile import ELFFile
    from elftools.elf.sections import Section
    from elftools.elf.segments import Segment
except ImportError:
    ELFError = BaseException
    ELFFile = None

#pylint: disable=missing-function-docstring


class BootLocation(NamedTuple):
    """Boot location entry (always in two first pages of first info part)
    """
    bank: int
    page: int
    seq: int


class ElfBlob:
    """Load ELF application."""

    def __init__(self):
        self._log = getLogger('flashgen.elf')
        self._elf: Optional[ELFFile] = None
        self._payload_address: int = 0
        self._payload_size: int = 0
        self._payload: bytes = b''

    def load(self, efp: BinaryIO) -> None:
        """Load the content of an ELF file.

           The ELF file stream is no longer accessed once this method
           completes.

           :param efp: a File-like (binary read access)
        """
        # use a copy of the stream to release the file pointer.
        try:
            self._elf = ELFFile(BytesIO(efp.read()))
        except ELFError as exc:
            raise ValueError(f'Invalid ELF file: {exc}') from exc
        if self._elf['e_machine'] != 'EM_RISCV':
            raise ValueError('Not an RISC-V ELF file')
        if self._elf['e_type'] != 'ET_EXEC':
            raise ValueError('Not an executable ELF file')
        self._log.debug('entry point: 0x%X', self.entry_point)
        self._log.debug('data size: %d', self.raw_size)

    @property
    def address_size(self) -> int:
        """Provide the width of address value used in the ELFFile.

           :return: the address width in bits (not bytes!)
        """
        return self._elf.elfclass if self._elf else 0

    @property
    def entry_point(self) -> Optional[int]:
        """Provide the entry point of the application, if any.

           :return: the entry point address
        """
        return self._elf and self._elf.header.get('e_entry', None)

    @property
    def raw_size(self) -> int:
        """Provide the size of the Secure Boot Header section, if any.

           :return: the data/payload size in bytes
        """
        if not self._payload_size:
            self._payload_address, self._payload_size = self._parse_segments()
        return self._payload_size

    @property
    def load_address(self) -> int:
        """Provide the first destination address on target to copy the
           application blob.

           :return: the load address
        """
        if not self._payload_address:
            self._payload_address, self._payload_size = self._parse_segments()
        return self._payload_address

    @property
    def blob(self) -> bytes:
        """Provide the application blob, i.e. the whole loadable binary.

           :return: the raw application binary.
        """
        if not self._payload:
            self._payload = self._build_payload()
        if len(self._payload) != self.raw_size:
            raise RuntimeError('Internal error: size mismatch')
        return self._payload

    @property
    def code_span(self) -> Tuple[int, int]:
        """Report the extent of the executable portion of the ELF file.

           :return: (start address, end address)
        """
        loadable_segments = list(self._loadable_segments())
        base_addr = None
        last_addr = None
        for section in self._elf.iter_sections():
            if not self.is_section_executable(section):
                continue
            for segment in loadable_segments:
                if segment.section_in_segment(section):
                    break
            else:
                continue
            addr = section.header['sh_addr']
            size = section.header['sh_size']
            if base_addr is None or base_addr > addr:
                base_addr = addr
            last = addr + size
            if last_addr is None or last_addr < last:
                last_addr = last
            self._log.debug('Code section @ 0x%08x 0x%08x bytes', addr, size)
        return base_addr, last_addr

    def is_section_executable(self, section: 'Section') -> bool:
        """Report whether the section is flagged as executable.

           :return: True is section is executable
        """
        return bool(section.header['sh_flags'] & SH_FLAGS.SHF_EXECINSTR)

    def _loadable_segments(self) -> Iterator['Segment']:
        """Provide an iterator on segments that should be loaded into the final
           binary.
        """
        if not self._elf:
            raise RuntimeError('No ELF file loaded')
        for segment in sorted(self._elf.iter_segments(),
                              key=lambda seg: seg['p_paddr']):
            if segment['p_type'] not in ('PT_LOAD', ):
                continue
            if not segment['p_filesz']:
                continue
            yield segment

    def _parse_segments(self) -> Tuple[int, int]:
        """Parse ELF segments and extract physical location and size.

           :return: the location of the first byte and the overall payload size
                    in bytes
        """
        size = 0
        phy_start = None
        for segment in self._loadable_segments():
            seg_size = segment['p_filesz']
            if not seg_size:
                continue
            phy_addr = segment['p_paddr']
            if phy_start is None:
                phy_start = phy_addr
            else:
                if phy_addr > phy_start+size:
                    self._log.debug('fill gap with previous segment')
                    size = phy_addr-phy_start
            size += seg_size
        if phy_start is None:
            raise ValueError('No loadable segment found')
        return phy_start, size

    def _build_payload(self) -> bytes:
        """Extract the loadable payload from the ELF file and generate a
           unique, contiguous binary buffer.

           :return: the payload to store as the application blob
        """
        buf = BytesIO()
        phy_start = None
        for segment in self._loadable_segments():
            phy_addr = segment['p_paddr']
            if phy_start is None:
                phy_start = phy_addr
            else:
                current_addr = phy_start+buf.tell()
                if phy_addr > current_addr:
                    fill_size = phy_addr-current_addr
                    buf.write(bytes(fill_size))
            buf.write(segment.data())
        data = buf.getvalue()
        buf.close()
        return data


class RuntimeDescriptor(NamedTuple):
    """Description of an executable binary.
    """
    code_start: int
    code_end: int
    raw_size: int
    entry_point: int


class FlashGen:
    """Generate a flash image file.

       :param bl_offset: offset of the BL0 storage within the data partition.
                         if forced to 0, do not reserve any space for BL0, i.e.
                         dedicated all storage space to ROM_EXT section.
       :discard_elf_check: whether to ignore mismatching binary/elf files.
    """

    NUM_BANKS = 2
    PAGES_PER_BANK = 256
    NUM_REGIONS = 8
    INFOS = [10, 1, 2]
    WORDS_PER_PAGE = 256
    BYTES_PER_WORD = 8
    BYTES_PER_PAGE = 2048
    BYTES_PER_BANK = 524288
    CHIP_ROM_EXT_SIZE_MAX = 0x10000

    HEADER_FORMAT = {
        'magic': '4s',  # "vFSH"
        'hlength': 'I',  # count of header bytes after this point
        'version': 'I',  # version of the header
        'bank': 'B',  # count of bank
        'info': 'B',  # count of info partitions per bank
        'page': 'H',  # count of pages per bank
        'psize': 'I', # page size in bytes
        'ipp': '12s', # count of pages for each info partition (up to 12 parts)
    }

    BOOT_HEADER_FORMAT = {
        'sha': '32s',  # SHA-256 digest of boot data
        'valid': 'Q',  # Invalidate a previously entry
        'identifier': 'I', # Boot data identifier (i.e. magic)
        'counter': 'I',  # used to determine the newest entry
        'min_ver_rom_ext': 'I',  # Minimum required security version for ROM_EXT
        'min_ver_bl0': 'I', # Minimum required security version for BL0
        'padding': 'Q', # Padding to make the size of header a power of 2
    }

    MANIFEST_FORMAT = {
        # SigverifyRsaBuffer
        'signature': '384s',  # 96u32
        # ManifestUsageConstraints
        'selector_bits': 'I',
        'device_id': '32s',  # 8u32
        'manuf_state_creator': 'I',
        'manuf_state_owner': 'I',
        'life_cycle_state': 'I',
        # SigverifyRsaBuffer
        'modulus': '384s',
        'address_translation': 'I',
        'identifier': '4s',
        # ManifestVersion
        'manifest_version_minor': 'H',
        'manifest_version_major': 'H',
        'signed_region_end': 'I',
        'length': 'I',
        'version_major': 'I',
        'version_minor': 'I',
        'security_version': 'I',
        # Timestamp
        'timestamp': '8s',  # cannot use 'Q', no longer aligned on 64-bit type
        # KeymgrBindingValue
        'binding_value': '32s',  # 8u32
        'max_key_version': 'I',
        'code_start': 'I',
        'code_end': 'I',
        'entry_point': 'I',
        # ManifestExtTable
        'entries': '120s'  # 15*(2u32)
    }

    MANIFEST_SIZE = 1024
    MANIFEST_VERSION_MINOR1 = 0x6c47
    MANIFEST_VERSION_MAJOR1 = 0x71c3
    MANIFEST_EXT_TABLE_COUNT = 15

    MANIFEST_TRUE =  0x739  # 'true' value for address_translation field
    MANIFEST_FALSE = 0x1d4  # 'false' value for address_translation field

    IDENTIFIERS = {
        None: b'\x00\x00\x00\x00',
        'rom_ext': b'OTRE',
        'bl0': b'OTB0',
    }

    DEBUG_TRAILER_FORMAT = {
        'otre0': '256s', # optional path to the rom_ext filename in bank A
        'otb00': '256s', # optional path to the bl0 filename in bank A
        'otre1': '256s', # optional path to the rom_ext filename in bank B
        'otb01': '256s', # optional path to the bl0 filename in bank B
    }

    BOOT_IDENTIFIER = 0x41444f42
    BOOT_INVALID = 0
    BOOT_VALID = (1<<64) - 1
    BOOT_BANK = 1
    BOOT_PARTS = 2

    def __init__(self, bl_offset: Optional[int] = None,
                 discard_elf_check: Optional[bool] = False):
        self._log = getLogger('flashgen')
        self._check_manifest_size()
        self._bl_offset = bl_offset if bl_offset is not None \
            else self.CHIP_ROM_EXT_SIZE_MAX
        self._check_elf = not bool(discard_elf_check)
        hfmt = ''.join(self.HEADER_FORMAT.values())
        header_size = scalc(hfmt)
        assert header_size == 32
        # dict in Python 3.7+ are kept ordered
        if version_info[:2] < (3, 7):
            raise RuntimeError('Unsupported Python version')
        self._header_size = header_size
        bhfmt = ''.join(self.BOOT_HEADER_FORMAT.values())
        self._boot_header_size = scalc(bhfmt)
        tfmt = ''.join(self.DEBUG_TRAILER_FORMAT.values())
        trailer_size = scalc(tfmt)
        self._image_size = ((self.BYTES_PER_BANK + self.info_part_size()) *
            self.NUM_BANKS + self._header_size + trailer_size)
        self._ffp: Optional[BinaryIO] = None

    def open(self, path: str) -> None:
        """Prepare flash content into a QEMU RAW stream.
        """
        mode = 'r+b' if exists(path) else 'w+b'
        # cannot use a context manager here
        #pylint: disable=consider-using-with
        self._ffp = open(path, mode)
        self._ffp.seek(0, SEEK_END)
        vsize = self._ffp.tell()
        if vsize < self._image_size:
            if vsize and mode.startswith('r'):
                self._log.info('File image too short, expanding')
            else:
                self._log.info('Creating new image file')
            header = self._build_header()
            self._write(0, header)
            vsize += len(header)
            self._write(len(header),
                        bytes(repeat(0xff, self._image_size-vsize)))
        self._ffp.seek(0)
        if vsize > self._image_size:
            self._log.info('File image too long, truncating')
            self._ffp.truncate(self._image_size)

    def close(self):
        if self._ffp:
            pos = self._ffp.seek(0, SEEK_END)
            self._ffp.close()
            self._ffp = None
            if pos != self._image_size:
                self._log.error('Invalid image size (%d bytes)', pos)

    @property
    def logger(self):
        return self._log

    @classmethod
    def info_part_size(cls) -> int:
        return sum(cls.INFOS) * cls.BYTES_PER_PAGE

    def read_boot_info(self) -> Dict[BootLocation,
                                     Dict[str, Union[int, bytes]]]:
        size = self._boot_header_size
        fmt = ''.join(self.BOOT_HEADER_FORMAT.values())
        boot_entries = {}
        boot_bank = 1
        for page in range(self.BOOT_PARTS):
            base = page * self.BYTES_PER_PAGE
            for offset in range(0, self.BYTES_PER_PAGE, size):
                bdata = self.read_info_partition(boot_bank, base+offset, size)
                if len(bdata) != size:
                    raise ValueError(f'Cannot read header: {len(bdata)} '
                        f'bytes @ page {page} offset {base+offset}')
                values = sunpack(f'<{fmt}', bdata)
                boot = dict(zip(self.BOOT_HEADER_FORMAT, values))
                if boot['identifier'] != self.BOOT_IDENTIFIER:
                    continue
                if boot['valid'] != self.BOOT_VALID:
                    continue
                boot_entries[BootLocation(boot_bank, page, offset//size)] = boot
            offset += size
        return boot_entries

    def read_info_partition(self, bank: int, offset: int, size: int) -> bytes:
        offset += (self._header_size + self.NUM_BANKS * self.BYTES_PER_BANK +
                   bank * self.info_part_size())
        pos = self._ffp.tell()
        self._ffp.seek(offset)
        data = self._ffp.read(size)
        self._ffp.seek(pos)
        return data

    def store_rom_ext(self, bank: int, dfp: BinaryIO,
                      elfpath: Optional[str] = None) -> None:
        #pylint: disable=too-many-locals
        #pylint: disable=too-many-branches
        #pylint: disable=too-many-statements
        if not 0 <= bank < self.NUM_BANKS:
            raise ValueError(f'Invalid bank {bank}')
        data = dfp.read()
        if len(data) > self.BYTES_PER_BANK:
            raise ValueError('Data too large')
        bindesc = self._check_rom_ext(data)
        boot_entries = self.read_boot_info()
        if not boot_entries:
            next_loc = BootLocation(self.BOOT_BANK, 0, 0)
            next_count = 5
            self._log.info('No pre-existing BootLocation')
        else:
            sorted_locs = sorted(boot_entries,
                                 key=lambda e: boot_entries[e]['counter'])
            mr_loc = sorted_locs[-1]
            self._log.info('Last boot location %s', mr_loc)
            mr_entry = boot_entries[mr_loc]
            mr_bank = mr_loc.bank
            next_op_bank = mr_bank
            op_locs = [loc for loc in sorted_locs if loc.bank == next_op_bank]
            if op_locs:
                last_op_loc = op_locs[-1]
                next_op_seq = last_op_loc.seq + 1
                next_op_page = last_op_loc.page
            else:
                next_op_seq = 0
                next_op_page = 0
            if next_op_seq >= self.BYTES_PER_PAGE/self._boot_header_size:
                next_op_page += 1
                next_op_seq = 0
            if next_op_page >= self.BOOT_PARTS:
                # erase the flash?
                raise ValueError('No more room to store boot location')
            next_loc = BootLocation(next_op_bank, next_op_page, next_op_seq)
            next_count = mr_entry['counter'] + 1
        self._write(self._header_size + bank * self.BYTES_PER_BANK, data)
        boot_header = self._build_boot_header(next_count)
        offset = self._get_boot_location_offset(next_loc)
        self._write(offset, boot_header)
        info_offset = (offset - self.NUM_BANKS * self.BYTES_PER_BANK -
                       self._header_size)
        self._log.info('New %s stored @ abs:0x%06x / rel:0x%06x',
                       next_loc, offset, info_offset)
        field_offset, field_data = self._build_field(self.BOOT_HEADER_FORMAT,
                                                     'valid', self.BOOT_INVALID)
        for loc, entry in boot_entries.items():
            if loc.bank != next_op_bank:
                continue
            if entry['valid'] != self.BOOT_INVALID:
                offset = self._get_boot_location_offset(loc)
                offset += field_offset
                self._write(offset, field_data)
        ename = f'otre{bank}'
        if not elfpath:
            elfpath = self._get_elf_filename(dfp.name)
        if elfpath:
            elftime = stat(elfpath).st_mtime
            bintime = stat(dfp.name).st_mtime
            if bintime < elftime:
                msg = 'Application binary file is older than ELF file'
                if self._check_elf:
                    raise RuntimeError(msg)
                self._log.warning(msg)
            be_match = self._compare_bin_elf(bindesc, elfpath)
            if not be_match:
                if be_match is None:
                    msg = 'Cannot verify ELF file (pyelftools not installed)'
                else:
                    msg = 'Application ELF file does not match binary file'
                if self._check_elf:
                    raise RuntimeError(msg)
                self._log.warning(msg)
        self._store_debug_info(ename, elfpath)

    def store_bootloader(self, bank: int, dfp: BinaryIO,
                         elfpath: Optional[str] = None) -> None:
        #pylint: disable=too-many-locals
        if self._bl_offset == 0:
            raise ValueError('Bootloader cannot be used')
        if not 0 <= bank < self.NUM_BANKS:
            raise ValueError(f'Invalid bank {bank}')
        data = dfp.read()
        if len(data) > self.BYTES_PER_BANK:
            raise ValueError('Data too large')
        bindesc = self._check_bootloader(data)
        self._write(self._header_size + self._bl_offset, data)
        ename = f'otb0{bank}'
        if not elfpath:
            elfpath = self._get_elf_filename(dfp.name)
        if elfpath:
            elftime = stat(elfpath).st_mtime
            bintime = stat(dfp.name).st_mtime
            if bintime < elftime:
                msg = 'Boot binary file is older than ELF file'
                if self._check_elf:
                    raise RuntimeError(msg)
                self._log.warning(msg)
            be_match = self._compare_bin_elf(bindesc, elfpath)
            if not be_match:
                if be_match is None:
                    msg = 'Cannot verify ELF file (pyelftools not installed)'
                else:
                    msg = 'Boot ELF file does not match binary file'
                if self._check_elf:
                    raise RuntimeError(msg)
                self._log.warning(msg)
        self._store_debug_info(ename, elfpath)

    def store_ot_files(self, otdescs: List[str]) -> None:
        for dpos, otdesc in enumerate(otdescs, start=1):
            parts = otdesc.rsplit(':', 1)
            if len(parts) > 1:
                otdesc = parts[0]
                elf_filename = parts[1]
            else:
                elf_filename = None
            parts = otdesc.split('@', 1)
            if len(parts) < 2:
                raise ValueError('Missing address in OT descriptor')
            bin_filename = parts[0]
            if not isfile(bin_filename):
                raise ValueError(f'No such file {bin_filename}')
            try:
                address = int(parts[1], 16)
            except ValueError as exc:
                raise ValueError('Invalid address in OT descriptor') from exc
            bank = address // self.BYTES_PER_BANK
            address %= self.BYTES_PER_BANK
            kind = 'rom_ext' if address < self.CHIP_ROM_EXT_SIZE_MAX else \
                'bootloader'
            self._log.info(
                'Handling file #%d as %s @ 0x%x in bank %d with%s ELF',
                dpos, kind, address, bank, '' if elf_filename else 'out')
            with open(bin_filename, 'rb') as bfp:
                # func decode should never fail, so no error handling here
                getattr(self, f'store_{kind}')(bank, bfp, elf_filename)

    def _compare_bin_elf(self, bindesc: RuntimeDescriptor, elfpath: str) \
            -> Optional[bool]:
        if ELFFile is None:
            return None
        with open(elfpath, 'rb') as efp:
            elfdesc = self._load_elf_info(efp)
        if not elfdesc:
            return False
        binep = bindesc.entry_point & (self.CHIP_ROM_EXT_SIZE_MAX - 1)
        elfep = elfdesc.entry_point & (self.CHIP_ROM_EXT_SIZE_MAX - 1)
        if binep != elfep:
            self._log.warning('Cannot compare bin vs. elf files')
            return False
        offset = elfdesc.entry_point - bindesc.entry_point
        self._log.debug('ELF base offset 0x%08x', offset)
        relfdesc = RuntimeDescriptor(elfdesc.code_start - offset,
                                     elfdesc.code_end - offset,
                                     elfdesc.raw_size,
                                     elfdesc.entry_point - offset)
        match = bindesc == relfdesc
        logfunc = self._log.debug if match else self._log.warning
        logfunc('start bin %08x / elf %08x',
                bindesc.code_start, relfdesc.code_start)
        logfunc('end   bin %08x / elf %08x',
                bindesc.code_end, relfdesc.code_end)
        logfunc('size  bin %08x / elf %08x',
                bindesc.raw_size, relfdesc.raw_size)
        logfunc('entry bin %08x / elf %08x',
                bindesc.entry_point, relfdesc.entry_point)
        return match

    def _write(self, offset: Optional[int], data: bytes) -> None:
        pos = self._ffp.tell()
        if offset is None:
            offset = pos
        if offset + len(data) > self._image_size:
            raise ValueError(f'Invalid offset {offset}+{len(data)}, '
                             f'max {self._image_size}')
        self._ffp.seek(offset, SEEK_SET)
        self._ffp.write(data)
        self._ffp.seek(pos, SEEK_SET)

    def _get_info_part_offset(self, part: int, info: int) -> int:
        offset = self._header_size + self.NUM_BANKS * self.BYTES_PER_BANK
        partition = 0
        while partition < part:
            offset += self.INFOS[partition]*self.BYTES_PER_PAGE
            partition += 1
        offset += info * self.BYTES_PER_PAGE
        return offset

    def _get_boot_location_offset(self, loc: BootLocation) -> int:
        return (loc.bank * self.info_part_size() +
                self._get_info_part_offset(0, 0) +
                loc.page * self.BYTES_PER_PAGE +
                loc.seq * self._boot_header_size)

    def _build_field(self, fmtdict: Dict[str, Any], field: str, value: Any) \
            -> Tuple [int, bytes]:
        offset = 0
        for name, fmt in fmtdict.items():
            if name == field:
                return offset, spack(f'<{fmt}', value)
            offset += scalc(fmt)
        raise ValueError(f'No such field: {field}')

    def _build_header(self) -> bytes:
        # hlength is the length of header minus the two first items (T, L)
        hfmt = self.HEADER_FORMAT
        fhfmt = ''.join(hfmt.values())
        shfmt = ''.join(hfmt[k] for k in list(hfmt)[:2])
        hlen = scalc(fhfmt) - scalc(shfmt)
        ipp = bytearray(self.INFOS)
        ipp.extend([0] * (12 - len(ipp)))
        values = dict(magic=b'vFSH', hlength=hlen, version=1,
                      bank=self.NUM_BANKS, info=len(self.INFOS),
                      page=self.PAGES_PER_BANK, psize=self.BYTES_PER_PAGE,
                      ipp=bytes(ipp))
        args = [values[k] for k in hfmt]
        header = spack(f'<{fhfmt}', *args)
        return header

    def _build_boot_header(self, counter) -> bytes:
        min_sec_ver_rom_ext = 0
        min_sec_ver_bl0 = 0
        padding = 0
        fmts = list(self.BOOT_HEADER_FORMAT.values())
        sha_fmt, pld_fmt = fmts[0], ''.join(fmts[1:])
        payload = spack(f'<{pld_fmt}', self.BOOT_VALID, self.BOOT_IDENTIFIER,
            counter, min_sec_ver_rom_ext, min_sec_ver_bl0, padding)
        sha = spack(sha_fmt, sha256(payload).digest())
        header = b''.join((sha, payload))
        return header

    def _get_elf_filename(self, filename: str) -> str:
        pathname = abspath(filename)
        radix = re_sub(r'.[a-z_]+_0.signed.bin$', '', pathname)
        elfname = f'{radix}.elf'
        if not exists(elfname):
            self._log.warning('No ELF debug info found')
            return ''
        self._log.info('Using ELF %s for %s',
                       basename(elfname), basename(filename))
        return elfname

    def _load_elf_info(self, efp: BinaryIO) \
            -> Optional[RuntimeDescriptor]:
        if not ELFFile:
            # ELF tools are not available
            self._log.warning('ELF file cannot be verified')
            return None
        elf = ElfBlob()
        elf.load(efp)
        if elf.address_size != 32:
            raise ValueError('Spefified ELF file {} is not an ELF32 file')
        elfstart, elfend = elf.code_span
        return RuntimeDescriptor(elfstart, elfend, elf.raw_size,
                                 elf.entry_point)

    def _store_debug_info(self, entryname: str, filename: Optional[str]) \
            -> None:
        fnp = filename.encode('utf8') if filename else b''
        lfnp = len(fnp)
        tfmt = ''.join(self.DEBUG_TRAILER_FORMAT.values())
        trailer_size = scalc(tfmt)
        trailer_offset = self._image_size - trailer_size
        for name, fmt in self.DEBUG_TRAILER_FORMAT.items():
            lfmt = scalc(fmt)
            if name != entryname:
                trailer_offset += lfmt
                continue
            if lfnp < lfmt:
                fnp = b''.join((fnp, bytes(lfmt-lfnp)))
            elif lfnp > lfmt:
                self._log.warning('ELF pathname too long to store')
                return
            fnp = spack(fmt, fnp) # useless, used as sanity check
            self._write(trailer_offset, fnp)
            break
        else:
            self._log.warning('Unable to find a matching debug entry: %s',
                              entryname)

    def _check_rom_ext(self, data: bytes) -> RuntimeDescriptor:
        max_size = self._bl_offset or self.BYTES_PER_BANK
        return self._check_manifest(data, 'rom_ext', max_size)

    def _check_bootloader(self, data: bytes) -> RuntimeDescriptor:
        assert self._bl_offset
        max_size =  self.BYTES_PER_BANK - self._bl_offset
        return self._check_manifest(data, 'bl0', max_size)

    def _check_manifest(self, data: bytes, kind: str, max_size: int) \
            -> RuntimeDescriptor:
        if len(data) > max_size:
            raise ValueError(f'{kind} too large')
        mfmt = ''.join(self.MANIFEST_FORMAT.values())
        slen = scalc(mfmt)
        if len(data) <= slen:
            raise ValueError(f'{kind} too short')
        manifest = dict(zip(self.MANIFEST_FORMAT,
                            sunpack(f'<{mfmt}', data[:slen])))
        self._log_manifest(manifest)
        if (manifest['manifest_version_major'] !=
                self.MANIFEST_VERSION_MAJOR1
            or manifest['manifest_version_minor'] !=
                self.MANIFEST_VERSION_MINOR1):
            raise ValueError('Unsupported manifest version')
        self._log.info('%s code start 0x%05x, end 0x%05x, exec 0x%05x',
            kind, manifest['code_start'], manifest['code_end'],
            manifest['entry_point'])
        if manifest['identifier'] != self.IDENTIFIERS[kind]:
            if manifest['identifier'] != self.IDENTIFIERS[None]:
                manifest_str = hexlify(manifest["identifier"]).decode().upper()
                raise ValueError(f'Specified file is not a {kind} file: '
                                 f'{manifest_str}')
            self._log.warning('Empty %s manifest, cannot verify', kind)
        return RuntimeDescriptor(manifest['code_start'], manifest['code_end'],
                                 manifest['length'], manifest['entry_point'])

    @classmethod
    def _check_manifest_size(cls):
        slen = scalc(''.join(cls.MANIFEST_FORMAT.values()))
        assert cls.MANIFEST_SIZE == slen, 'Invalid Manifest size'

    def _log_manifest(self, manifest):
        for item, value in manifest.items():
            if isinstance(value, int):
                self._log.debug('%s: 0x%08x', item, value)
            elif isinstance(value, bytes):
                self._log.debug('%s: (%d) %s', item, len(value),
                               hexlify(value).decode())
            else:
                self._log.debug('%s: (%d) %s', item, len(value), value)


def hexint(val: str) -> int:
    return int(val, val.startswith('0x') and 16 or 10)


def main():
    """Main routine"""
    #pylint: disable=too-many-statements
    #pylint: disable=too-many-locals
    #pylint: disable=too-many-branches
    debug = True
    banks = list(range(FlashGen.NUM_BANKS))
    try:
        desc = modules[__name__].__doc__.split('.', 1)[0].strip()
        argparser = ArgumentParser(description=f'{desc}.')
        img = argparser.add_argument_group(title='Image')
        img.add_argument('flash', nargs=1, metavar='flash',
                         help='virtual flash file')
        img.add_argument('-D', '--discard', action='store_true',
                               help='Discard any previous flash file content')
        img.add_argument('-a', '--bank', type=int, choices=banks,
                         default=banks[0],
                         help=f'flash bank for data (default: {banks[0]})')
        img.add_argument('-s', '--offset', type=hexint,
                         default=FlashGen.CHIP_ROM_EXT_SIZE_MAX,
                         help=f'offset of the BL0 file (default: '
                              f'0x{FlashGen.CHIP_ROM_EXT_SIZE_MAX:x})')
        files = argparser.add_argument_group(title='Files')
        files.add_argument('-x', '--exec', type=FileType('rb'), metavar='file',
                           help='rom extension or application')
        files.add_argument('-X', '--exec-elf', metavar='elf',
                           help='ELF file for rom extension or application, for'
                                ' symbol tracking (default: auto)')
        files.add_argument('-b', '--boot', type=FileType('rb'),
                          metavar='file', help='bootloader 0 file')
        files.add_argument('-B', '--boot-elf', metavar='elf',
                           help='ELF file for bootloader, for symbol tracking'
                                ' (default: auto)')
        files.add_argument('-t', '--otdesc', action='append', default=[],
                           help='OpenTitan style file descriptor, '
                                'may be repeated')
        extra = argparser.add_argument_group(title='Extra')
        extra.add_argument('-v', '--verbose', action='count',
                           help='increase verbosity')
        extra.add_argument('-d', '--debug', action='store_true',
                           help='enable debug mode')
        extra.add_argument('-U', '--unsafe-elf', action='store_true',
                           help='Discard sanity checking on ELF files')
        args = argparser.parse_args()
        debug = args.debug

        loglevel = max(DEBUG, ERROR - (10 * (args.verbose or 0)))
        loglevel = min(ERROR, loglevel)
        formatter = Formatter('%(levelname)8s [%(lineno)d] %(name)-12s '
                              '%(message)s')
        log = getLogger('flashgen')
        logh = StreamHandler(stderr)
        logh.setFormatter(formatter)
        log.setLevel(loglevel)
        log.addHandler(logh)

        use_bl0 = bool(args.boot) or len(args.otdesc) > 1
        gen = FlashGen(args.offset if use_bl0 else 0, bool(args.unsafe_elf))
        flash_pathname = args.flash[0]
        backup_filename = None
        if args.otdesc and any(filter(None, (args.bank,
                                             args.boot, args.boot_elf,
                                             args.exec, args.exec_elf))):
            argparser.error('OT file descriptor mode is mutually exclusive with'
                            'boot and exec options')
        if args.boot_elf:
            if not args.boot:
                argparser.error('Bootloader ELF option requires bootloader '
                                'binary')
            if not isfile(args.boot_elf):
                argparser.error('No such Bootloader ELF file')
        if args.exec_elf:
            if not args.exec:
                argparser.error('Application ELF option requires application '
                                'binary')
            if not isfile(args.exec_elf):
                argparser.error('No such Bootloader ELF file')
        try:
            if args.discard:
                if exists(flash_pathname):
                    backup_filename = f'{flash_pathname}.bak'
                    rename(flash_pathname, backup_filename)
            gen.open(args.flash[0])
            if args.exec:
                gen.store_rom_ext(args.bank, args.exec, args.exec_elf)
            if args.boot:
                gen.store_bootloader(args.bank, args.boot, args.boot_elf)
            if args.otdesc:
                gen.store_ot_files(args.otdesc)
            backup_filename = None
        finally:
            gen.close()
            if backup_filename:
                print('Restoring previous file after error', file=stderr)
                rename(backup_filename, flash_pathname)

    #pylint: disable=broad-except
    except Exception as exc:
        print(f'\nError: {exc}', file=stderr)
        if debug:
            print(format_exc(chain=False), file=stderr)
        sysexit(1)
    except KeyboardInterrupt:
        sysexit(2)


if __name__ == '__main__':
    main()
