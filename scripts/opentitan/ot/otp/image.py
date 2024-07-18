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
from typing import Any, BinaryIO, Optional, Sequence, TextIO, Union

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

    SYNDROME_HAMMING_22_16 = (
        (0x01ad5b, 0x02366d, 0x04c78e, 0x0807f0, 0x10f800, 0x3fffff),
        (0x23, 0x25, 0x26, 0x27, 0x29, 0x2a, 0x2b, 0x2c,
         0x2d, 0x2e, 0x2f, 0x31, 0x32, 0x33, 0x34, 0x35)
    )

    def __init__(self, ecc_bits: Optional[int] = None):
        self._log = getLogger('otptool.img')
        self._header: dict[str, Any] = {}
        self._magic = b''
        self._data = bytearray()
        self._ecc = bytearray()
        if ecc_bits is None:
            ecc_bits = self.DEFAULT_ECC_BITS
        self._ecc_bits = ecc_bits
        self._ecc_bytes = (ecc_bits + 7) // 8
        self._ecc_granule = 0
        self._digest_iv: Optional[int] = None
        self._digest_constant: Optional[int] = None
        self._partitions: list[OtpPartition] = []
        self._part_offsets: list[int] = []

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
        self._data = bytearray(rfp.read(header['dlength']))
        self._ecc = bytearray(rfp.read(header['elength']))
        self._ecc_bits = header['eccbits']
        self._ecc_bytes = header['dlength']
        self._ecc_granule = header['eccgran']
        self._ecc_bytes = (self._ecc_bits + 7) // 8
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
        self._data = bytearray(b''.join(data_buf))
        self._ecc = bytearray(b''.join(ecc_buf))
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
            pos = bfp.tell()
            self._log.debug('%s %d', part.name, pos)
            part.load(bfp)
            self._partitions.append(part)
            self._part_offsets.append(pos)
        self._part_offsets.append(bfp.tell())
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

    def clear_bits(self, bitdefs: Sequence[tuple[int, int]]) -> None:
        """Clear one or more bits.

           :param bitdefs: a sequence of bits to clear, where each bit is
                           specified as a 2-uple (offset, bit position)
                           If bit position is larger than data width for the
                           specified offset, it indicates an ECC bit to change
        """
        self._change_bits(bitdefs, False)

    def set_bits(self, bitdefs: Sequence[tuple[int, int]]) -> None:
        """Set one or more bits.

           :param bitdefs: a sequence of bits to set, where each bit is
                           specified as a 2-uple (offset, bit position)
                           If bit position is larger than data width for the
                           specified offset, it indicates an ECC bit to change
        """
        self._change_bits(bitdefs, True)

    def toggle_bits(self, bitdefs: Sequence[tuple[int, int]]) -> None:
        """Toggle one or more bits.

           :param bitdefs: a sequence of bits to toggle, where each bit is
                           specified as a 2-uple (offset, bit position)
                           If bit position is larger than data width for the
                           specified offset, it indicates an ECC bit to change
        """
        self._change_bits(bitdefs, None)

    @staticmethod
    def bit_parity(data: int) -> int:
        """Compute the bit parity of an integer, i.e. reduce the vector to a
           single bit.
        """
        data ^= data >> 16  # useless for 16 bits data
        data ^= data >> 8
        data ^= data >> 4
        data ^= data >> 2
        data ^= data >> 1
        return data & 1

    def verify_ecc(self, recover: bool) -> tuple[int, int]:
        """Verify data with ECC.

           :return: 2-uple of count of uncorrectable and corrected errors.
        """
        granule = self._ecc_granule
        bitgran = granule * 8
        ecclen = (self._ecc_bits + 7) // 8
        bitcount = bitgran + self._ecc_bits
        self._log.info('ECC check (%d, %d)', bitcount, bitgran)
        if len(self._ecc) * granule != len(self._data):
            self._log.critical('Incoherent ECC size w/ granule %d',
                               granule)
        try:
            ecc_fn = getattr(self, f'_decode_ecc_{bitcount}_{bitgran}')
        except AttributeError as exc:
            raise NotImplementedError('ECC function for {self._ecc.bits}'
                                      'not supported') from exc
        err_cnt = fatal_cnt = 0
        updated_parts: set[OtpPartition] = set()
        for off in range(0, len(self._data), granule):
            chunk = int.from_bytes(self._data[off:off+granule], 'little')
            eccoff = off//granule
            if ecclen == 1:
                ecc = self._ecc[eccoff]
            else:
                ecc = int.from_bytes(self._ecc[eccoff:eccoff+ecclen], 'little')
            if not chunk and not ecc:
                continue
            partition = self._get_partition_at_offset(off)
            err, fchunk = ecc_fn(chunk, ecc)
            self._log.debug("ECC check @ %u data:%04x ecc:%02x",
                            off, chunk, ecc)
            if err > 0:
                partinfo = f' in {partition.name}' if partition else ''
                if getattr(partition, 'integrity', False):
                    self._log.warning('Ignoring ECC error%s @ '
                                      '0x%04x', partinfo, off)
                    continue
                if chunk == fchunk:
                    # error is in ECC bits
                    self._log.warning('ECC single bit corruption @ 0x%04x%s, '
                                      'ignored: data:%04x ecc:%02x', off,
                                      partinfo, chunk, ecc)
                else:
                    self._log.warning('ECC recover%s error @ 0x%04x%s: '
                                      'data:%04x->%04x ecc:%02x',
                                      'ed' if recover else 'able',
                                      off, partinfo, chunk, fchunk, ecc)
                if recover:
                    self._data[off:off+granule] = fchunk.to_bytes(granule,
                                                                  'little')
                    updated_parts.add(partition)
                err_cnt += 1
            elif err < 0:
                self._log.critical('Unrecoverable ECC error @ 0x%04x%s: '
                                   'data:%04x, ecc:%02x', off, partinfo, chunk,
                                   ecc)
                fatal_cnt += 1
        for part in updated_parts:
            bounds = self._get_partition_bounds(part)
            if not bounds:
                self._log.warning('Unknown partiton bounds for %s, '
                                  'cannot updated recovered data', part.name)
                continue
            bfp = BytesIO(self._data[bounds[0]:bounds[1]])
            self._log.info('Updating partition %s with recover data', part.name)
            part.load(bfp)
        return fatal_cnt, err_cnt

    def _compute_ecc_22_16(self, data: int) -> int:

        data |= self.bit_parity(data & 0x00ad5b) << 16
        data |= self.bit_parity(data & 0x00366d) << 17
        data |= self.bit_parity(data & 0x00c78e) << 18
        data |= self.bit_parity(data & 0x0007f0) << 19
        data |= self.bit_parity(data & 0x00f800) << 20
        data |= self.bit_parity(data & 0x1fffff) << 21

        return (data & 0x3f0000) >> 16

    def _decode_ecc_22_16(self, data: int, ecc: int) -> tuple[int, int]:
        """Check and fix 16-bit data with 6 bits ECC.

           :param data: a 16-bit integer
           :return: 2-uple error, data, where err is
                    0 if no error has been spotted
                    1 if a single bit error has been detected and recovered
                    -1 if a double bit error has been detected and not fixed
        """
        assert (data >> 16) == 0

        idata = data | (ecc << 16)

        synd_mask, synd_code = self.SYNDROME_HAMMING_22_16

        syndrome = sum(1 << b for b, m in enumerate(synd_mask)
                       if self.bit_parity(idata & m))

        err = (syndrome >> 5) & 1
        if not err:
            err = -int(syndrome & 0x1f != 0)
        if not err:
            return 0, data

        odata = sum(1 << b for b, s in enumerate(synd_code)
                    if (syndrome == s) ^ ((idata >> b) & 1))

        return err, odata

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

    def _change_bits(self, bits: Sequence[tuple[int, int]],
                     level: Optional[bool]) -> None:
        granule = self._ecc_granule
        bitgran = granule * 8
        bitcount = bitgran + self._ecc_bits
        ecclen = (self._ecc_bits + 7) // 8
        for off, bit in bits:
            off -= off % granule
            if off > len(self._data):
                raise ValueError(f'Invalid bit offset: 0x{off:x}')
            if bit >= bitcount:
                raise ValueError(f'Invalid bit position: {bit}')
            if bit >= bitgran:  # ECC bit
                eccoff = off//granule
                if ecclen == 1:
                    ecc = self._ecc[eccoff]
                else:
                    ecc = int.from_bytes(self._ecc[eccoff:eccoff+ecclen],
                                         'little')
                bitval = 1 << (bit - bitgran)
                old = ecc
                if level is None:
                    ecc ^= bitval
                elif level:
                    ecc |= bitval
                else:
                    ecc &= ~bitval
                self._log.info('Changed ECC bit %d @ 0x%x: 0x%x -> 0x%x',
                               bit-bitgran, off, old, ecc)
                if ecclen == 1:
                    self._ecc[eccoff] = ecc
                else:
                    self._ecc[eccoff] = ecc.to_bytes(ecclen, 'little')
                    ecc = int.from_bytes(self._ecc[eccoff:eccoff+ecclen],
                                         'little')
            else:  # Data bit
                chunk = int.from_bytes(self._data[off:off+granule], 'little')
                bitval = 1 << bit
                old = chunk
                if level is None:
                    chunk ^= bitval
                elif level:
                    chunk |= bitval
                else:
                    chunk &= ~bitval
                self._log.info('Changed data bit %d @ 0x%x: 0x%x -> 0x%x',
                               bit, off, old, chunk)
                self._data[off:off+granule] = chunk.to_bytes(granule, 'little')

    def _get_partition_bounds(self, partref: Union[str|OtpPartition]) \
            -> Optional[tuple[int, int]]:
        if isinstance(partref, str):
            name = partref
        elif isinstance(partref, OtpPartition):
            name = partref.name
        else:
            raise TypeError('Unsupported partition definition')
        for pos, (part, start) in enumerate(zip(self._partitions,
                                                self._part_offsets)):
            if part.name == name:
                return (start, self._part_offsets[pos+1])
        return None

    def _get_partition_at_offset(self, off: int) -> Optional[OtpPartition]:
        for pos, (part, start) in enumerate(zip(self._partitions,
                                                self._part_offsets)):
            if off >= start:
                end = self._part_offsets[pos+1]
                if off < end:
                    return part
        return None
