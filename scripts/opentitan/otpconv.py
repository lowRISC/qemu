#!/usr/bin/env python3

"""Convert a VMEM OTP file into a RAW file.
"""

# Copyright (c) 2023 Rivos, Inc.
# All rights reserved.

from argparse import ArgumentParser, FileType
from binascii import hexlify, unhexlify
from io import StringIO
from logging import DEBUG, ERROR, getLogger, Formatter, StreamHandler
from os.path import basename, splitext
from re import match as re_match, sub as re_sub
from struct import calcsize as scalc, pack as spack
from sys import exit as sysexit, modules, stderr, stdin, version_info
from textwrap import fill
from traceback import format_exc
from typing import BinaryIO, Callable, Dict, List, Optional, Set, TextIO, Tuple

#pylint: disable-msg=too-many-locals
#pylint: disable-msg=too-many-branches
#pylint: disable-msg=too-many-instance-attributes


class OtpConverter:
    """Simple OTP file format converter from Verilog/OpenTitan to QEMU RAW file.
    """

    HEADER_FORMAT = {
        'magic': '4s',  # "vOTP"
        'hlength': 'I', # count of header bytes after this point
        'version': 'I', # version of the header
        'eccbits': 'H', # count of ECC bits for each ECC granule
        'eccgran': 'H', # size in bytes of ECC granule
        'dlength': 'I', # count of data bytes (padded to 64-bit entries)
        'elength': 'I', # count of ecc bytes (padded to 64-bit entries)
    }

    RE_LOC = r'(?i)^@((?:[0-9a-f]{2})+)\s((?:[0-9a-f]{2})+)$'
    RE_REG = (r'^#define ([A-Z][\w]+)_(OFFSET|SIZE)\s+'
              r'((?:0x)?[A-Fa-f0-9]+)(?:\s|$)')
    MAXWIDTH = 20

    HARDENED_BOOLEANS = {
        0x739: True,
        0x1d4: False
    }

    EXTRA_SLOTS = {
        'lc_state': {
            'post_transition': None,
            'escalate': None,
            'invalid': None,
        }
    }

    def __init__(self, ecc_bits: int):
        self._log = getLogger('otpconv')
        self._ecc_bits = ecc_bits
        self._ecc_bytes = (ecc_bits + 7) // 8
        self._ecc_granule = 0
        self._data = b''
        self._ecc = b''
        self._regs: Dict[str, Tuple[int, int]] = {}
        self._tables: Dict[str, Dict[str, str]] = {}

    @classmethod
    def get_output_types(cls) -> List[str]:
        """Return a list of supported output generated formats.

           :return: list of formats
        """
        otypes = []
        for item in dir(cls):
            if not item.startswith('_save_'):
                continue
            if not isinstance(cls.__dict__[item], Callable):
                continue
            otypes.append(item[len('_save_'):])
        return otypes

    def parse(self, vfp: TextIO, swap: bool = True):
        """Parse a VMEM text stream.

           :param vfp: input VMEM stream
        """
        #pylint: disable-msg=too-many-locals
        data_buf: List[bytes] = []
        ecc_buf: List[bytes] = []
        last_addr = 0
        granule_sizes: Set[int] = set()
        for lno, line in enumerate(vfp, start=1):
            line = re_sub(r'//.*', '', line)
            line = line.strip()
            if not line:
                continue
            lmo = re_match(self.RE_LOC, line)
            if not lmo:
                self._log.error('Unexpected line @ %d: %s', lno, line)
                continue
            saddr, sdata = lmo.groups()
            addr = int(saddr, 16)
            if last_addr < addr:
                self._log.info("Padding addr from 0x%04x to 0x%04x",
                               last_addr, addr)
                data_buf.append(bytes(addr-last_addr))
            rdata = unhexlify(sdata)
            ecc, data = rdata[:self._ecc_bytes], rdata[self._ecc_bytes:]
            if swap:
                data = bytes(reversed(data))
            data_buf.append(data)
            ecc_buf.append(ecc)
            dlen = len(data)
            granule_sizes.add(dlen)
            last_addr = addr+dlen # ECC is not accounted for in address
        self._data = b''.join(data_buf)
        self._ecc = b''.join(ecc_buf)
        if granule_sizes:
            if len(granule_sizes) != 1:
                raise ValueError('Variable data size')
            self._ecc_granule = granule_sizes.pop()

    def load_registers(self, hfp: TextIO, show: bool) -> None:
        """Decode (part of) the OTP content.

           :param hfp: input register definition stream
           :param show: whether to output values to stdout (vs. log stream)
        """
        # consider that low address represents LSB, i.e. little endian
        # encoding
        radix = splitext(basename(hfp.name))[0]
        radix = radix.rsplit('_', 1)[0]
        radix_re = f'^{radix.upper()}_(?:PARAM_)?'
        defs = {}
        if show:
            def emit(fmt, *args):
                print(fmt % args)
        else:
            emit = self._log.info
        for line in hfp:
            line = line.strip()
            rmo = re_match(self.RE_REG, line)
            if not rmo:
                continue
            sname = rmo.group(1)
            skind = rmo.group(2).lower()
            sval = rmo.group(3)
            name = re_sub(radix_re, '', sname)
            val = int(sval, 16 if sval.startswith('0x') else 10)
            #self._log.debug("%s: 0x%x", sname, sval)
            if skind == 'offset':
                if name in defs:
                    self._log.error('Redefinition of %s: %x -> %x', name,
                                    defs[name][0], val)
                defs[name] = (val, 0)
            elif skind == 'size':
                if not name in defs:
                    self._log.info('Size w/o address of %s', name)
                    continue
                addr = defs[name][0]
                defs[name] = (addr, val)
        for name, (addr, size) in defs.items():
            if not size:
                continue
            value = self._data[addr:addr+size]
            if size > 8:
                value = bytes(reversed(value))
                sval = hexlify(value).decode()
                if name in self._tables:
                    dval = self._tables[name].get(sval)
                    if dval is not None:
                        emit('%-46s (decoded) %s', name, dval)
                        continue
                if not sum(value):
                    emit('%-46s [%d] 0...', name, size)
                else:
                    if size > self.MAXWIDTH:
                        sval = f'{sval[:self.MAXWIDTH*2]}...'
                    emit('%-46s [%d] %s', name, size, sval)
            else:
                ival = int.from_bytes(value, 'little')
                if size == 4 and ival in self.HARDENED_BOOLEANS:
                    emit('%-46s (decoded) %s',
                                   name, str(self.HARDENED_BOOLEANS[ival]))
                else:
                    emit('%-46s %x', name, ival)
        self._regs = defs

    def decode_lifecycle(self, svp: TextIO):
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

    def save(self, out_type: str, filename: str):
        """Save OTP content into as a specific format.

           :param out_type: type of generated file
           :param filenane: output filename
        """
        getattr(self, f'_save_{out_type}')(filename)

    def _save_raw(self, filename: str):
        """Save OTP content into as QEMU RAW file.

           :param filename: output RAW file
        """
        with open(filename, 'wb') as rfp:
            header = self._build_header()
            rfp.write(header)
            self._pad(rfp)
            rfp.write(self._data)
            self._pad(rfp)
            rfp.write(self._ecc)
            self._pad(rfp, 4096)

    def _save_lc_arrays(self, filename: str):
        """Save OTP life cycle definitions as a C file.

           :param filename: output RAW file
        """
        #pylint: disable-msg=unspecified-encoding
        with open(filename, 'wt') as cfp:
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
                    seqstr = ', '.join((f'0x{b:02x}u' for b in reversed(unhexlify(seq))))
                    defstr = fill(seqstr, width=80, initial_indent=pad,
                                  subsequent_indent=pad)
                    print(f'    [{slot}] = {{\n{defstr}\n    }},', file=array_io)
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

    def _pad(self, bfp: BinaryIO, padsize: Optional[int] = 8):
        tail = bfp.tell() % padsize
        if tail:
            bfp.write(bytes(padsize-tail))

    def _build_header(self) -> bytes:
        # dict in Python 3.7+ are kept ordered
        if version_info[:2] < (3, 7):
            raise RuntimeError('Unsupported Python version')
        # hlength is the length of header minus the two first items (T, L)
        hfmt = self.HEADER_FORMAT
        fhfmt = ''.join(hfmt.values())
        shfmt = ''.join(hfmt[k] for k in list(hfmt)[:2])
        hlen = scalc(fhfmt)-scalc(shfmt)
        dlen = (len(self._data)+7) & ~0x7
        elen = (len(self._ecc)+7) & ~0x7
        values = dict(magic=b'vOTP', hlength=hlen, version=1,
                      eccbits=self._ecc_bits, eccgran=self._ecc_granule,
                      dlength=dlen, elength=elen)
        args = [values[k] for k in hfmt]
        header = spack(f'<{fhfmt}', *args)
        return header


def main():
    """Main routine"""
    debug = False
    out_types = OtpConverter.get_output_types()
    try:
        desc = modules[__name__].__doc__.split('.', 1)[0].strip()
        argparser = ArgumentParser(description=f'{desc}.')
        argparser.add_argument('-i', '--input', type=FileType('rt'),
                               metavar='vmem', help='input VMEM file')
        argparser.add_argument('-c', '--check', type=FileType('rt'),
                               metavar='regfile',
                               help='decode OTP content w/ otp_ctrl_reg file')
        argparser.add_argument('-l', '--lc', type=FileType('rt'),
                               metavar='svfile',
                               help='decode OTP lifecycle w/ LC .sv file')
        argparser.add_argument('-O', '--output-type',
                               choices=out_types,  default='raw',
                               help='type of generated file, defaults to raw')
        argparser.add_argument('-o', '--output',
                               metavar='file', help='output file')
        argparser.add_argument('-e', '--ecc', type=int, default=6,
                               metavar='bits', help='ECC bit count')
        argparser.add_argument('-b', '--bswap', default=True,
                               action='store_false',
                               help='reverse data byte order (swap endianess)')
        argparser.add_argument('-s', '--show', default=False,
                               action='store_true',
                               help='dump decoded values to stdout')
        argparser.add_argument('-v', '--verbose', action='count',
                               help='increase verbosity')
        argparser.add_argument('-d', '--debug', action='store_true',
                               help='enable debug mode')
        args = argparser.parse_args()
        debug = args.debug

        loglevel = max(DEBUG, ERROR - (10 * (args.verbose or 0)))
        loglevel = min(ERROR, loglevel)
        formatter = Formatter('%(levelname)8s %(name)-10s %(message)s')
        log = getLogger('otpconv')
        logh = StreamHandler(stderr)
        logh.setFormatter(formatter)
        log.setLevel(loglevel)
        log.addHandler(logh)

        otp = OtpConverter(args.ecc)
        if args.input:
            otp.parse(args.input, args.bswap)
        elif args.output_type != 'lc_arrays':
            argparser.error('Missing input file')
        if args.lc:
            otp.decode_lifecycle(args.lc)
            if not args.check:
                print("Lifecycle decoding needs regfile option to run",
                      file=stderr)
        if args.check:
            otp.load_registers(args.check, args.show)
        if args.output:
            otp.save(args.output_type, args.output)

    except (IOError, ValueError, ImportError) as exc:
        print(f'\nError: {exc}', file=stderr)
        if debug:
            print(format_exc(chain=False), file=stderr)
        sysexit(1)
    except KeyboardInterrupt:
        sysexit(2)


if __name__ == '__main__':
    main()
