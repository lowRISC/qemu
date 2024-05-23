# Copyright (c) 2023-2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""OTP QEMU image.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from binascii import unhexlify
from io import BytesIO
from logging import getLogger
from re import match as re_match, sub as re_sub
from struct import calcsize as scalc, pack as spack, unpack as sunpack
from typing import Any, BinaryIO, Optional, TextIO

from .map import OtpMap
from .partition import OtpPartition, OtpLifecycleExtension
from ..util.misc import HexInt, classproperty


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
        self._header: dict[str, Any] = {}
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
        self._partitions: list[OtpPartition] = []

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
    def vmem_kinds(cls) -> list[str]:
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
        data_buf: list[bytes] = []
        ecc_buf: list[bytes] = []
        last_addr = 0
        granule_sizes: set[int] = set()
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
        results: dict[str, Optional[bool]] = {}
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

    def _load_header(self, bfp: BinaryIO) -> dict[str, Any]:
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
        values = {
            'magic': self._magic, 'hlength': hlen, 'version': 1+int(use_v2),
            'eccbits': self._ecc_bits, 'eccgran': self._ecc_granule,
            'dlength': dlen, 'elength': elen
        }
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
