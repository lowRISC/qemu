# Copyright (c) 2019 Emmanuel Blot <emmanuel.blot@free.fr>
# Copyright (c) 2025 lowRISC contributors.
# SPDX-License-Identifier: MIT License

"""Text-record tools
"""

from array import array as Array
from binascii import hexlify, unhexlify
from io import BytesIO, StringIO
from os import stat, linesep, SEEK_SET
from re import compile as re_compile
from struct import pack as spack
from typing import BinaryIO, Optional, TextIO

import sys


# pylint: disable-msg=broad-except,invalid-name


class RecordError(ValueError):
    """Error in text record content"""


class SRecError(RecordError):
    """Error in SREC content"""


class IHexError(RecordError):
    """Error in iHex content"""


class TItxtError(RecordError):
    """Error in TI-txt content"""


class VMemError(RecordError):
    """Error in VMem content"""


class RecordSegment:
    """Data container for a consecutive sequence of bytes.

       ..note:: RecordSegment methods are extensively called, so the code has
                been optimized to decrease Python call overhead.
    """

    def __init__(self, baseaddr=0):
        self._baseaddr = baseaddr
        self._size = 0
        self._buffer = BytesIO()

    @property
    def size(self):
        """Return the size in bytes of a segment."""
        return self._size

    @property
    def baseaddr(self):
        """Return the address of the first byte of the segment."""
        return self._baseaddr

    @property
    def absaddr(self):
        """Return the absolute address of this segment."""
        return self._baseaddr + self._size

    @property
    def reladdr(self):
        """Return the relative address of this segment."""
        return self._size

    @property
    def data(self) -> bytes:
        """Return the segment payload."""
        return self._buffer.getvalue()

    def __str__(self):
        return f'Data segment @ {self._baseaddr:08x} {self._size} bytes'

    def write(self, data: bytes, offset=None):
        """Write new data to the segment, at the specified offset."""
        self.write_with_size(data, len(data), offset)

    def write_with_size(self, data, size, offset):
        """Write new data to the segment, at the specified offset, with a
           specific size."""
        if offset is not None and (offset - self._baseaddr) != self._size:
            offset -= self._baseaddr
            self._buffer.seek(offset, SEEK_SET)
            size += offset - self._size
        self._buffer.write(data)
        self._size += size


class RecordParser:
    """Abstract record file parser.

      :param src: file object for sourcing stream
      :param offset: byte offset to substract to encoded address
      :param min_addr: lowest address to consider
      :param max_addr: highest address to consider
      :param segment_gap: distance between non-consecutive address to trigger
                          a new segment
      :param verbose: emit extra information while processing the stream
      :param verify: verify the checksum with calculated one
    """

    (INFO, DATA, EXECUTE, EOF) = range(1, 5)

    def __init__(self, src: TextIO, offset: int = 0, min_addr: int = 0x0,
                 max_addr: int = 0xffffffff, segment_gap: int = 16,
                 verbose: bool = False, verify: bool = True):
        if segment_gap < 1:
            raise ValueError("Invalid segment gap")
        self._src: TextIO = src
        self._offset: int = offset
        self._min_addr: int = min_addr
        self._max_addr: int = max_addr
        self._verbose: bool = verbose
        self._exec_addr: Optional[int] = None
        self._segments: list[RecordSegment] = []
        self._info: Optional[RecordSegment] = None
        self._gap: int = segment_gap
        self._seg: Optional[RecordSegment] = None
        self._bytes: int = 0
        self._verify: bool = verify

    def parse(self):
        """Parse the stream"""
        for (record, address, value) in self:
            if record == RecordParser.DATA:
                addr = address - self._offset
                if self._seg and (abs(addr - self._seg.absaddr) >= self._gap):
                    self._store_segment()
                if not self._seg:
                    self._seg = RecordSegment(addr)
                self._seg.write(value, addr)
            elif record == RecordParser.EXECUTE:
                self._exec_addr = address
            elif record == RecordParser.INFO:
                if not self._info:
                    self._info = RecordSegment(address)
                self._info.write(value, address)
            elif record == RecordParser.EOF:
                pass
            else:
                raise RuntimeError("Internal error")
        self._store_segment()

    def __iter__(self):
        return self._get_next_chunk()

    def _get_next_chunk(self):
        raise NotImplementedError()

    def get_data_segments(self) -> list[RecordSegment]:
        """Return all segments."""
        return self._segments

    def get_info(self) -> bytes:
        """Return the info segment data, if there is such a segment."""
        return bytes(self._info.data) if self._info else bytes()

    def getexec(self) -> Optional[int]:
        """Return the execution address (entry point) if any."""
        return self._exec_addr

    @classmethod
    def is_valid_syntax(cls, file):
        """Tell whether the file contains a valid syntax.

           :param file: either a filepath or a file-like object
           :return: True if the file content looks valid
        """
        cre = getattr(cls, 'SYNTAX_CRE', None)
        if not cre:
            return False
        last = False
        with isinstance(file, str) and open(file, 'rt') or file as hfp:
            try:
                for line in hfp:
                    line = line.strip()
                    if not line:
                        last = True
                        continue
                    if not cre.match(line) or last:
                        # there should be no empty line but the last one(s)
                        return False
            except Exception:
                return False
        return True

    def _verify_address(self, address):
        if (address < self._min_addr) or (address > self._max_addr):
            raise RecordError(f"Address out of range [0x{self._min_addr:08x}.."
                              f"0x{self._max_addr:08x}: 0x{address:08x}")
        if address < self._offset:
            raise RecordError(f"Invalid address in file: 0x{address:08x}")

    def _store_segment(self):
        if self._seg and self._seg.size:
            self._segments.append(self._seg)
        self._seg = None


class SRecParser(RecordParser):
    """S-record file parser.
    """

    SYNTAX_CRE = re_compile('(?i)^S([0-9])((?:[0-9A-F]{2})+)$')

    def _get_next_chunk(self):
        # test if the file size can be found...
        try:
            self._bytes = stat(self._src.name)[6]
        except Exception:
            pass
        bc = 0
        try:
            for (lno, line) in enumerate(self._src, start=1):
                line = line.strip()
                if self._verbose and self._bytes:
                    opc = (50 * bc) // self._bytes
                    bc += len(line)
                    pc = (50 * bc) // self._bytes
                    if pc > opc:
                        info = f"\rAnalysing SREC file [{2*pc:3d}%%] {'.' * pc}"
                        sys.stdout.write(info)
                        sys.stdout.flush()
                try:
                    # avoid line stripping, SREC files always use DOS format
                    if len(line) < 5:
                        continue
                    if line[0] != 'S':
                        raise SRecError("Invalid SREC header")
                    record = int(line[1])
                    if record == 1:
                        addrend = 3
                        address = int(line[4:8], 16)
                        type_ = RecordParser.DATA
                    elif record == 2:
                        addrend = 4
                        address = int(line[4:10], 16)
                        type_ = RecordParser.DATA
                    elif record == 3:
                        addrend = 5
                        address = int(line[4:12], 16)
                        type_ = RecordParser.DATA
                    elif record == 7:
                        addrend = 5
                        address = int(line[4:12], 16)
                        type_ = RecordParser.EXECUTE
                    elif record == 8:
                        addrend = 4
                        address = int(line[4:10], 16)
                        type_ = RecordParser.EXECUTE
                    elif record == 9:
                        addrend = 3
                        address = int(line[4:8], 16)
                        type_ = RecordParser.EXECUTE
                    elif record == 0:
                        addrend = 3
                        address = int(line[4:8], 16)
                        type_ = RecordParser.INFO
                    else:
                        raise SRecError("Unsupported SREC record")
                    try:
                        bytes_ = unhexlify(line[2:-2])
                    except TypeError as exc:
                        raise SRecError(f"{exc} @ line {lno}") from exc
                    size = int(line[2:4], 16)
                    effsize = len(bytes_)
                    if size != effsize:
                        raise SRecError(f"Expected {size} bytes, got {effsize} "
                                        f"@ line {lno}")
                    if self._verify:
                        csum = sum(Array('B', bytes_))
                        csum &= 0xff
                        csum ^= 0xff
                        rsum = int(line[-2:], 16)
                        if rsum != csum:
                            raise SRecError(f"Invalid checksum: 0x{rsum:02x} / "
                                            f"0x{csum:02x}")
                    if self._verify and record:
                        self._verify_address(address)
                    yield (type_, address, bytes_[addrend:])
                except RecordError as exc:
                    raise exc.__class__(f"{exc} @ line {lno}:'{line}'") from exc
        finally:
            if self._verbose:
                print('')


class IHexParser(RecordParser):
    """Intel Hex record file parser.
    """

    SYNTAX_CRE = re_compile('(?i)^:[0-9A-F]+$')

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._offset_addr = 0

    def _get_next_chunk(self):
        # test if the file size can be found...
        try:
            self._bytes = stat(self._src.name)[6]
        except Exception:
            pass
        bc = 0
        try:
            for (lno, line) in enumerate(self._src, start=1):
                line = line.strip()
                if self._verbose and self._bytes:
                    opc = (50 * bc) // self._bytes
                    bc += len(line)
                    pc = (50 * bc) // self._bytes
                    if pc > opc:
                        info = f"\rAnalysing iHEX file [{2*pc:3d}%%] {'.' * pc}"
                        sys.stdout.write(info)
                        sys.stdout.flush()
                try:
                    if len(line) < 5:
                        continue
                    if line[0] != ':':
                        raise IHexError("Invalid IHEX header")
                    size = int(line[1:3], 16)
                    address = int(line[3:7], 16)
                    record = int(line[7:9])
                    if record == 0:
                        type_ = RecordParser.DATA
                    elif record == 1:
                        type_ = RecordParser.EOF
                        if address != 0:
                            print(f"Unexpected non-zero address in EOF: "
                                  f"0x{address:04x}", file=sys.stderr)
                    elif record == 2:
                        self._offset_addr &= ~((1 << 20) - 1)
                        self._offset_addr |= int(line[9:-2], 16) << 4
                        continue
                    elif record == 4:
                        self._offset_addr = int(line[9:-2], 16) << 16
                        continue
                    elif record == 3:
                        type_ = RecordParser.EXECUTE
                        cs = int(line[9:13], 16)
                        ip = int(line[13:-2], 16)
                        address = (cs << 4) + ip
                    else:
                        raise IHexError(f"Unsupported IHEX record: {record}")
                    try:
                        bytes_ = unhexlify(line[9:-2])
                    except TypeError as exc:
                        raise IHexError(f"{exc} @ line {lno}") from exc
                    effsize = len(bytes_)
                    if size != effsize:
                        raise IHexError(f"Expected {size} bytes, got {effsize} "
                                        f"@ line {lno}")
                    if self._verify:
                        csum = sum(Array('B', unhexlify(line[1:-2])))
                        csum = (-csum) & 0xff
                        rsum = int(line[-2:], 16)
                        if rsum != csum:
                            raise IHexError(f"Invalid checksum: 0x{rsum:02x} / "
                                            f"0x{csum:02x}")
                    if type_ == RecordParser.DATA:
                        address += self._offset_addr
                    if self._verify and record:
                        self._verify_address(address)
                    yield (type_, address, bytes_)
                except RecordError as exc:
                    raise exc.__class__(f"{exc} @ line {lno}:'{line}'") from exc
        finally:
            if self._verbose:
                print('')


class IHexFastParser(RecordParser):
    """Intel Hex record file parser.

       Faster implementation than IHexParser, but less readable.
    """

    # pylint: disable=abstract-method

    HEXCRE = re_compile(r'(?aim)^:((?:[0-9A-F][0-9A-F]){5,})$')

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._offset_addr = 0

    @classmethod
    def is_valid_syntax(cls, file):
        """Tell whether the file contains a valid HEX syntax.

           :param file: either a filepath or a file-like object
           :return: True if the file content looks valid
        """
        valid = False
        with isinstance(file, str) and open(file, 'rt') or file as hfp:
            try:
                data = hfp.read()
                # it seems there is no easy way to full match a multiline re
                # so compare the count of line vs, the count of matching ihex
                # valid lines as a quick workaround. This could give false
                # positive or negative, but this approximation is for now
                # sufficient to fast match a file.
                ihex_count = len(cls.HEXCRE.findall(data))
                lf_count = data.count('\n')
                valid = ihex_count == lf_count
            except Exception:
                pass
        return valid

    def parse(self):
        for pos, mo in enumerate(self.HEXCRE.finditer(self._src.read()),
                                 start=1):
            bvalues = unhexlify(mo.group(0)[1:])
            size = bvalues[0]
            rsum = bvalues[-1]
            data = bvalues[4:-1]
            if len(data) != size:
                raise IHexError(f'Invalid line @ {pos} in HEX file')
            if self._verify:
                csum = sum(bvalues[:-1])
                csum = (-csum) & 0xff
                if rsum != csum:
                    raise IHexError(f"Invalid checksum: 0x{rsum:02x} /"
                                    f" 0x{csum:02x}")
            address = (bvalues[1] << 8) + bvalues[2]
            record = bvalues[3]
            if self._verify and record:
                self._verify_address(address)
            if record == 0:
                # RecordParser.DATA
                address += self._offset_addr
                addr = address - self._offset
                if self._seg:
                    gap = addr - self._seg.absaddr
                    if gap < 0:
                        gap = -gap
                    if gap >= self._gap:
                        self._store_segment()
                if not self._seg:
                    self._seg = RecordSegment(addr)
                self._seg.write_with_size(data, size, addr)
            elif record == 1:
                # RecordParser.EOF
                if address != 0:
                    print(f"Unexpected non-zero address @ EOF: "
                          f"0x:{address:04x}", file=sys.stderr)
            elif record == 2:
                if size != 2:
                    raise IHexError('Invalid segment address')
                self._offset_addr &= ~((1 << 20) - 1)
                self._offset_addr |= ((data[0] << 8) + data[1]) << 4
                continue
            elif record == 4:
                if size != 2:
                    raise IHexError('Invalid linear address')
                self._offset_addr = ((data[0] << 8) + data[1]) << 16
                continue
            elif record == 3:
                # RecordParser.EXECUTE
                if size != 4:
                    raise IHexError('Invalid start address')
                cs = (data[0] << 8) + data[1]
                ip = (data[2] << 8) + data[3]
                address = (cs << 4) + ip
                self._exec_addr = address
        self._store_segment()


class TItxtParser(RecordParser):
    """TI txt record file parser.
    """

    def _get_next_chunk(self):
        # test if the file size can be found...
        try:
            self._bytes = stat(self._src.name)[6]
        except Exception:
            pass
        bc = 0
        try:
            for (lno, line) in enumerate(self._src, start=1):
                line = line.strip()
                if self._verbose and self._bytes:
                    opc = (50 * bc) // self._bytes
                    bc += len(line)
                    pc = (50 * bc) // self._bytes
                    if pc > opc:
                        info = (f"\rAnalysing TItxt file [{2*pc:3d}%%] "
                                f"{'.' * pc}")
                        sys.stdout.write(info)
                        sys.stdout.flush()
                try:
                    if line.startswith('@'):
                        address = int(line[1:], 16)
                        continue
                    if line == 'q':
                        yield (RecordParser.EOF, 0, b'')
                        break
                    try:
                        bytes_ = unhexlify(line)
                    except TypeError as exc:
                        raise IHexError(f'{exc} @ line {lno}') from exc
                    self._verify_address(address)
                    yield (RecordParser.DATA, address, bytes_)
                    address += len(bytes_)
                except RecordError as exc:
                    raise exc.__class__(f"{exc} @ line {lno}:'{line}'") from exc
        finally:
            if self._verbose:
                print('')


class VMemParser(RecordParser):
    """VMEM record file parser.

       Additional named arguments:

       :param eccbits: count of trailing bits in each data chunk
       :param byteorder: either 'little' or 'big', default to big
    """

    def __init__(self, src, *args, **kwargs):
        if 'eccbits' in kwargs:
            try:
                self._eccbits = int(kwargs.pop('eccbits'))
            except ValueError as exc:
                raise ValueError('Invalid ecc bit count') from exc
        else:
            self._eccbits = 0
        self._ecc_bytes = (self._eccbits + 7) // 8
        if 'byteorder' in kwargs:
            byteorder = kwargs.pop('byteorder')
            try:
                self._reverse = {'little': True, 'big': False}[byteorder]
            except KeyError as exc:
                raise ValueError('Invalid byte order') from exc
        else:
            self._reverse = False
        super().__init__(src, *args, **kwargs)

    def _get_next_chunk(self):
        cmt_re = re_compile(r'(#|//).*$')
        address = 0
        block_count = 0
        if self._ecc_bytes:
            bs = slice(0, -self._ecc_bytes)
            be = slice(-self._ecc_bytes, None)
        else:
            bs, be = slice(None), slice(0)

        if self._reverse:
            def conv(data):
                return bytes(reversed(data))
        else:
            def conv(data):
                return data

        for lno, line in enumerate(self._src, start=1):
            line = cmt_re.sub('', line).rstrip()
            if not line:
                continue
            if not line.startswith('@'):
                raise VMemError(f'Invalid line @ {lno}')
            parts = line[1:].split(' ')
            part_count = len(parts)
            try:
                block = int(parts[0], 16)
            except ValueError as exc:
                raise VMemError(f"Invalid address @ line {lno}: {exc}") from exc
            if block != block_count:
                raise VMemError(f"Unexpected block {block} @ line {lno}")
            try:
                bmap = map(unhexlify, parts[1:])
            except (TypeError, ValueError) as exc:
                raise VMemError(f"{exc} @ line {lno}") from exc
            blocks = ((conv(b[bs]), conv(b[be])) for b in bmap)
            # _ecc is not yet managed/verified, only discarded
            data, _ecc = (b''.join(x) for x in zip(*blocks))
            self._verify_address(address)
            yield (RecordParser.DATA, address, data)
            address += len(data)
            block_count += part_count - 1


class RecordBuilder:
    """Abstract record generator.

       :param crlf: whether to force CRLF line terminators or use host default
    """

    def __init__(self, crlf=False):
        self._linesep = '\r\n' if crlf else linesep
        self._buffer = StringIO()

    def build(self, datasegs, infoseg=None, execaddr=None, offset=0):
        """Build the SREC stream from a binary stream"""
        if infoseg:
            self._buffer.write(self._create_info(infoseg))
            self._buffer.write(self._linesep)
        for dataseg in datasegs:
            for line in self._create_data(offset, dataseg):
                self._buffer.write(line)
                self._buffer.write(self._linesep)
        if execaddr is not None:
            self._buffer.write(self._create_exec(execaddr))
            self._buffer.write(self._linesep)
        eof = self._create_eof()
        if eof:
            self._buffer.write(eof)
            self._buffer.write(self._linesep)

    def getvalue(self) -> str:
        """Return the record as as string."""
        return self._buffer.getvalue()

    def _create_info(self, segment):
        raise NotImplementedError()

    def _create_data(self, offset, segment):
        raise NotImplementedError()

    def _create_exec(self, address):
        raise NotImplementedError()

    def _create_eof(self):
        raise NotImplementedError()


class SRecBuilder(RecordBuilder):
    """Intel Hex generator.
    """

    @classmethod
    def checksum(cls, hexastr: str) -> int:
        """Compute the checksum of an hexa string."""
        dsum = sum(Array('B', unhexlify(hexastr)))
        dsum &= 0xff
        return dsum ^ 0xff

    def _create_info(self, segment):
        msg = segment.data[:16]
        line = f'S0{len(msg)+2+1:02x}{0:04x}'
        line += hexlify(msg)
        line += f'{SRecBuilder.checksum(line[2:]):02x}'
        return line.upper()

    def _create_data(self, offset, segment):
        data = segment.data
        upaddr = segment.baseaddr+len(data)
        if upaddr < (1 << 16):
            prefix = 'S1%02x%04x'
        elif upaddr < (1 << 24):
            prefix = 'S2%02x%06x'
        else:
            prefix = 'S3%02x%08x'
        for pos in range(0, len(data), 16):
            chunk = data[pos:pos+16]
            hexachunk = hexlify(chunk).decode()
            line = prefix % (len(chunk) + int(prefix[1]) + 1 + 1,
                             offset + segment.baseaddr) + hexachunk
            line += f'{SRecBuilder.checksum(line[2:]):02x}'
            yield line.upper()
            offset += 16

    def _create_exec(self, address):
        if address < (1 << 16):
            prefix = 'S903%04x'
        elif address < (1 << 24):
            prefix = 'S804%06x'
        else:
            prefix = 'S705%08x'
        line = prefix % address
        line += f'{SRecBuilder.checksum(line[2:]):02x}'
        return line.upper()

    def _create_eof(self):
        return ''


class IHexBuilder(RecordBuilder):
    """S-record generator.
    """

    @classmethod
    def checksum(cls, hexastr: str) -> int:
        """Compute the checksum of an hexa string."""
        csum = sum(Array('B', unhexlify(hexastr)))
        csum = (-csum) & 0xff
        return csum

    def _create_info(self, segment):
        return ''

    def _create_data(self, offset, segment):
        data = segment.data
        address = offset + segment.baseaddr
        high_addr = None
        for pos in range(0, len(data), 16):
            high = address >> 16
            if high != high_addr:
                hi_bytes = spack('>H', high)
                yield self._create_line(4, 0, hi_bytes)
                high_addr = high
            chunk = data[pos:pos+16]
            yield self._create_line(0, address & 0xffff, chunk)
            address += 16

    def _create_exec(self, address):
        if address < (1 << 20):
            cs = address >> 4
            ip = address & 0xFFFF
            addr = (cs << 16) | ip
            addr = spack('>I', addr)
            return self._create_line(3, 0, addr)
        addr = spack('>I', address)
        return self._create_line(5, 0, addr)

    def _create_eof(self):
        return self._create_line(1)

    def _create_line(self, type_, address=0, data=None):
        if not data:
            data = b''
        hexdat = hexlify(data).decode()
        length = len(data)
        datastr = f'{length:02X}{address:04X}{type_:02X}{hexdat}'
        checksum = self.checksum(datastr)
        line = f':{datastr}{checksum:02X}'
        return line.upper()


class TItxtBuilder(RecordBuilder):
    """TI-txt generator.
    """

    # pylint: disable=abstract-method

    def _create_data(self, offset, segment):
        data = segment.data
        if data:
            yield f'@{segment.baseaddr + offset:04x}'
        for pos in range(0, len(data), 16):
            chunk = data[pos:pos+16]
            line = ' '.join((f'{b:02x}' for b in chunk))
            yield line.upper()

    def _create_eof(self):
        return 'q'


class VMemBuilder(RecordBuilder):
    """VMEM generator.

       :param crlf: whether to force CRLF line terminators or use host default
       :param byteorder: either 'little' or 'big', default to big
       :param chunksize: how many bytes to encode per chunk
       :param offsetize: how many chars to encode the VMEM offset (0: auto)
       :param linewidth: maximum character per output line
    """

    # pylint: disable=abstract-method

    def __init__(self, crlf=False, byteorder: Optional[str] = None,
                 chunksize: int = 4, offsetsize: int = 0, linewidth: int = 80):
        super().__init__(crlf)
        self._chunk_size = chunksize
        self._offset_size = offsetsize
        self._line_width = linewidth
        try:
            self._reverse = {'little': True, 'big': False}[byteorder]
        except KeyError as exc:
            raise ValueError('Invalid byte order') from exc

    def _create_data(self, offset, segment):
        if offset:
            raise ValueError('VMEM format does not support offsets')
        data = segment.data
        if not data:
            return
        total_chunk_count = ((segment.size + self._chunk_size - 1) //
                             self._chunk_size)
        max_bit_count = total_chunk_count.bit_length()
        off_char_len = 2 * ((max_bit_count + 7) // 8)
        if self._offset_size:
            if self._offset_size < off_char_len:
                raise ValueError('Not enough char to encode VMEM offset')
            off_char_len = self._offset_size
        off_len = len(f'@{0:0{off_char_len}X} ')
        chunk_len = 2 * self._chunk_size + 1
        chunk_per_line = (self._line_width - off_len) // chunk_len
        line_byte_count = self._chunk_size * chunk_per_line
        chunk_indices = [x * self._chunk_size for x in range(0, chunk_per_line)]
        offpos = 0
        cksize = self._chunk_size

        if self._reverse:
            def conv(data):
                return bytes(reversed(data))
        else:
            def conv(data):
                return data

        for pos in range(0, len(data), line_byte_count):
            chunks = (hexlify(conv(data[pos+ck:pos+ck+cksize])).upper().decode()
                      for ck in chunk_indices)
            yield f'@{offpos:0{off_char_len}X} {" ".join(chunks)}'
            offpos += chunk_per_line

    def _create_eof(self):
        return ''


class BinaryBuilder:
    """Raw binary generator.
    """

    # pylint: disable=missing-function-docstring

    def __init__(self, maxsize):
        self._iofp = BytesIO()
        self._maxsize = maxsize

    def build(self, datasegs):
        addr_offset = None
        for segment in sorted(datasegs, key=lambda seg: seg.baseaddr):
            if addr_offset is None:
                addr_offset = segment.baseaddr
            offset = segment.baseaddr-addr_offset
            if not 0 <= offset < self._maxsize:
                # segment cannot start outside flash area
                raise ValueError('Invalid HEX file')
            if offset + segment.size > self._maxsize:
                raise ValueError('Invalid HEX file')
            self._iofp.seek(segment.baseaddr-addr_offset)
            self._iofp.write(segment.data)
        self._iofp.seek(0, SEEK_SET)

    def getvalue(self) -> bytes:
        return self._iofp.getvalue()

    @property
    def io(self) -> BinaryIO:
        return self._iofp
