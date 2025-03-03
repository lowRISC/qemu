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


# pylint: disable-msg=broad-except,invalid-name


class RecordError(ValueError):
    """Error in text record content"""


class SRecError(RecordError):
    """Error in SREC content"""


class IHexError(RecordError):
    """Error in iHex content"""


class TItxtError(RecordError):
    """Error in TI-txt content"""


class RecordSegment:
    """Data container for a consecutive sequence of bytes.RecordSegment

       ..note:: RecordSegment methods are extensively called, so the code has
                been optimized to decrease Python call overhead.
    """

    def __init__(self, baseaddr=0):
        self._baseaddr = baseaddr
        self._size = 0
        self._buffer = BytesIO()

    @property
    def size(self):
        return self._size

    @property
    def baseaddr(self):
        return self._baseaddr

    @property
    def absaddr(self):
        return self._baseaddr+self._size

    @property
    def reladdr(self):
        return self._size

    @property
    def data(self):
        return self._buffer.getvalue()

    def __str__(self):
        return 'Data segment @ %08x %d bytes' % (self._baseaddr, self._size)

    def write(self, data, offset=None):
        self.write_with_size(data, len(data), offset)

    def write_with_size(self, data, size, offset):
        if offset is not None and (offset - self._baseaddr) != self._size:
            offset -= self._baseaddr
            self._buffer.seek(offset, SEEK_SET)
            size += offset - self._size
        self._buffer.write(data)
        self._size += size


class RecordParser:
    """Abstract record file parser.

      :param src: file object for sourcing SREC stream
      :param offset: byte offset to substract to encoded address
      :param min_addr: lowest address to consider
      :param max_addr: highest address to consider
      :param segment_gap: distance between non-consecutive address to trigger
                          a new segment
      :param verbose: emit extra information while processing the SREC stream
      :param verify: verify the SREC checksum with calculated one
    """

    (INFO, DATA, EXECUTE, EOF) = range(1, 5)

    def __init__(self, src, offset=0, min_addr=0x0, max_addr=0xffffffff,
                 segment_gap=16, verbose=False, verify=True):
        if segment_gap < 1:
            raise ValueError("Invalid segment gap")
        self._src = src
        self._offset = offset
        self._min_addr = min_addr
        self._max_addr = max_addr
        self._verbose = verbose
        self._exec_addr = None
        self._segments = []
        self._info = None
        self._gap = segment_gap
        self._seg = None
        self._bytes = 0
        self._verify = verify

    def parse(self, shift=False):
        """Parse the SREC stream"""
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

    def get_data_segments(self):
        return self._segments

    def get_info(self):
        return self._info.data if self._info else bytearray()

    def getexec(self):
        return self._exec_addr

    @classmethod
    def is_valid_syntax(cls, file):
        """Tell whether the file contains a valid HEX syntax.

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
            raise RecordError("Address out of range [0x%08x..0x%08x]: 0x%08x" %
                              (self._min_addr, self._max_addr, address))
        if address < self._offset:
            raise RecordError("Invalid address in file: 0x%08x" % address)

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
            import sys
            self._bytes = stat(self._src.name)[6]
        except Exception:
            pass
        bc = 0
        try:
            for (l, line) in enumerate(self._src, start=1):
                line = line.strip()
                if self._verbose and self._bytes:
                    opc = (50*bc)//self._bytes
                    bc += len(line)
                    pc = (50*bc)//self._bytes
                    if pc > opc:
                        info = '\rAnalysing SREC file [%3d%%] %s' % \
                                       (2*pc, '.' * pc)
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
                    except TypeError:
                        raise SRecError("%s @ line %s" % (str(e), l))
                    size = int(line[2:4], 16)
                    effsize = len(bytes_)
                    if size != effsize:
                        raise SRecError("Expected %d bytes, got %d "
                                        "@ line %d" % (size, effsize, l))
                    if self._verify:
                        csum = sum(Array('B', bytes_))
                        csum &= 0xff
                        csum ^= 0xff
                        rsum = int(line[-2:], 16)
                        if rsum != csum:
                            raise SRecError("Invalid checksum: 0x%02x / "
                                            "0x%02x" % (rsum, csum))
                    if self._verify and record:
                        self._verify_address(address)
                    yield (type_, address, bytes_[addrend:])
                except RecordError as ex:
                    raise ex.__class__("%s @ line %d:'%s'" % (ex, l, line))
        finally:
            if self._verbose:
                print('')


class IHexParser(RecordParser):
    """Intel Hex record file parser.
    """

    SYNTAX_CRE = re_compile('(?i)^:[0-9A-F]+$')

    def __init__(self, *args, **kwargs):
        super(IHexParser, self).__init__(*args, **kwargs)
        self._offset_addr = 0

    def _get_next_chunk(self):
        # test if the file size can be found...
        try:
            import sys
            self._bytes = stat(self._src.name)[6]
        except Exception:
            pass
        bc = 0
        try:
            for (lpos, line) in enumerate(self._src, start=1):
                line = line.strip()
                if self._verbose and self._bytes:
                    opc = (50*bc)//self._bytes
                    bc += len(line)
                    pc = (50*bc)//self._bytes
                    if pc > opc:
                        info = '\rAnalysing iHEX file [%3d%%] %s' % \
                                       (2*pc, '.' * pc)
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
                            print("Unexpected non-zero address in EOF: %04x" %
                                  address, file=sys.stderr)
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
                        raise IHexError("Unsupported IHEX record: %d" % record)
                    try:
                        bytes_ = unhexlify(line[9:-2])
                    except TypeError:
                        raise IHexError("%s @ line %s" % (str(e), lpos))
                    effsize = len(bytes_)
                    if size != effsize:
                        raise IHexError("Expected %d bytes, got %d "
                                        "@ line %d" % (size, effsize, lpos))
                    if self._verify:
                        csum = sum(Array('B', unhexlify(line[1:-2])))
                        csum = (-csum) & 0xff
                        rsum = int(line[-2:], 16)
                        if rsum != csum:
                            raise IHexError("Invalid checksum: 0x%02x / "
                                            "0x%02x" % (rsum, csum))
                    if type_ == RecordParser.DATA:
                        address += self._offset_addr
                    if self._verify and record:
                        self._verify_address(address)
                    yield (type_, address, bytes_)
                except RecordError as ex:
                    raise ex.__class__("%s @ line %d:'%s'" % (ex, lpos, line))
        finally:
            if self._verbose:
                print('')


class IHexFastParser(RecordParser):
    """Intel Hex record file parser.

       Faster implementation than IHexParser, but less readable.
    """

    HEXCRE = re_compile(r'(?aim)^:((?:[0-9A-F][0-9A-F]){5,})$')

    def __init__(self, *args, **kwargs):
        super(IHexFastParser, self).__init__(*args, **kwargs)
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
            except Exception as exc:
                pass
        return valid

    def parse(self, shift=False):
        for pos, mo in enumerate(self.HEXCRE.finditer(self._src.read()),
                                 start=1):
            bvalues = unhexlify(mo.group(0)[1:])
            size = bvalues[0]
            rsum = bvalues[-1]
            data = bvalues[4:-1]
            if len(data) != size:
                raise IHexError('Invalid line @ %d in HEX file' % pos)
            if self._verify:
                csum = sum(bvalues[:-1])
                csum = (-csum) & 0xff
                if rsum != csum:
                    raise IHexError("Invalid checksum: 0x%02x / 0x%02x" %
                                    (rsum, csum))
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
                    print("Unexpected non-zero address in EOF: %04x" %
                          address, file=sys.stderr)
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
            import sys
            self._bytes = stat(self._src.name)[6]
        except Exception:
            pass
        bc = 0
        try:
            for (l, line) in enumerate(self._src, start=1):
                line = line.strip()
                if self._verbose and self._bytes:
                    opc = (50*bc)//self._bytes
                    bc += len(line)
                    pc = (50*bc)//self._bytes
                    if pc > opc:
                        info = '\rAnalysing TItxt file [%3d%%] %s' % \
                                       (2*pc, '.' * pc)
                        sys.stdout.write(info)
                        sys.stdout.flush()
                try:
                    if line.startswith('@'):
                        address = int(line[1:], 16)
                        continue
                    if line == 'q':
                        yield(RecordParser.EOF, 0, b'')
                        break
                    try:
                        bytes_ = unhexlify(line)
                    except TypeError as e:
                        raise IHexError("%s @ line %s" % (str(e), l))
                    self._verify_address(address)
                    yield (RecordParser.DATA, address, bytes_)
                    address += len(bytes_)
                except RecordError as e:
                    raise e.__class__("%s @ line %d:'%s'" % (e, l, line))
        finally:
            if self._verbose:
                print('')


class RecordBuilder:
    """Abstract record generator.

       :param crlf: whether to force CRLF line terminators or use host default
    """

    def __init__(self, crlf=False):
        self._linesep = crlf and '\r\n' or linesep
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

    def getvalue(self):
        return self._buffer.getvalue()

    @classmethod
    def _create_info(cls, segment):
        raise NotImplementedError()

    @classmethod
    def _create_data(cls, offset, segment):
        raise NotImplementedError()

    @classmethod
    def _create_exec(cls, address):
        raise NotImplementedError()

    @classmethod
    def _create_eof(cls):
        raise NotImplementedError()


class SRecBuilder(RecordBuilder):
    """Intel Hex generator.
    """

    @classmethod
    def checksum(cls, hexastr):
        dsum = sum(Array('B', unhexlify(hexastr)))
        dsum &= 0xff
        return dsum ^ 0xff

    @classmethod
    def _create_info(cls, segment):
        msg = segment.data[:16]
        line = 'S0%02X%04X' % (len(msg)+2+1, 0)
        line += hexlify(msg)
        line += "%02x" % SRecBuilder.checksum(line[2:])
        return line.upper()

    @classmethod
    def _create_data(cls, offset, segment):
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
            line = prefix % (len(chunk) + int(prefix[1])+1 + 1,
                             offset + segment.baseaddr) + hexachunk
            line += "%02x" % SRecBuilder.checksum(line[2:])
            yield line.upper()
            offset += 16

    @classmethod
    def _create_exec(cls, address):
        if address < (1 << 16):
            prefix = 'S903%04x'
        elif address < (1 << 24):
            prefix = 'S804%06x'
        else:
            prefix = 'S705%08x'
        line = prefix % address
        line += "%02x" % SRecBuilder.checksum(line[2:])
        return line.upper()

    @classmethod
    def _create_eof(cls):
        return ''


class IHexBuilder(RecordBuilder):
    """S-record generator.
    """

    @classmethod
    def checksum(cls, hexastr):
        csum = sum(Array('B', unhexlify(hexastr)))
        csum = (-csum) & 0xff
        return csum

    @classmethod
    def _create_info(cls, segment):
        return ''

    @classmethod
    def _create_data(cls, offset, segment):
        data = segment.data
        address = offset + segment.baseaddr
        high_addr = None
        for pos in range(0, len(data), 16):
            high = address >> 16
            if high != high_addr:
                hi_bytes = spack('>H', high)
                yield cls._create_line(4, 0, hi_bytes)
                high_addr = high
            chunk = data[pos:pos+16]
            yield cls._create_line(0, address & 0xffff, chunk)
            address += 16

    @classmethod
    def _create_exec(cls, address):
        if address < (1 << 20):
            cs = address >> 4
            ip = address & 0xFFFF
            addr = (cs << 16) | ip
            addr = spack('>I', addr)
            return cls._create_line(3, 0, addr)
        addr = spack('>I', address)
        return cls._create_line(5, 0, addr)

    @classmethod
    def _create_eof(cls):
        return cls._create_line(1)

    @classmethod
    def _create_line(cls, type_, address=0, data=None):
        if not data:
            data = b''
        hexdat = hexlify(data).decode()
        length = len(data)
        datastr = '%02X%04X%02X%s' % (length, address, type_, hexdat)
        checksum = cls.checksum(datastr)
        line = ':%s%02X' % (datastr, checksum)
        return line.upper()


class TItxtBuilder(RecordBuilder):
    """TI-txt generator.
    """

    @classmethod
    def _create_data(cls, offset, segment):
        data = segment.data
        if data:
            yield '@%04x' % (segment.baseaddr + offset)
        for pos in range(0, len(data), 16):
            chunk = data[pos:pos+16]
            line = ' '.join(['%02x' % b for b in chunk])
            yield line.upper()

    @classmethod
    def _create_eof(cls):
        return 'q'


class BinaryBuilder:
    """Raw binary generator.
    """

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

    def getvalue(self):
        return self._iofp.getvalue()

    @property
    def io(self):
        return self._iofp
