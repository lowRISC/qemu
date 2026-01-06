#!/usr/bin/env python3

"""Verify register definitions.
"""

# Copyright (c) 2023-2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

from argparse import ArgumentParser
from logging import DEBUG, ERROR, getLogger, Formatter, StreamHandler
from os import pardir, walk
from os.path import (basename, dirname, join as joinpath, normpath, relpath,
                     splitext)
from traceback import format_exc
from typing import NamedTuple, Optional, TextIO
import re
import sys


class ValueLocation(NamedTuple):
    """Location of a defined value."""
    line: int  # line
    start: int  # start column
    end: int  # end column


RegisterDefs = dict[str, tuple[int, ValueLocation]]
"""Definition of a register value (name, value, location)."""

# pylint: disable=missing-function-docstring


class OtRegisters:
    """Simple class to parse and compare register definitions
    """

    DEFMAP = {
        'alert_handler': 'alert',
        'flash_ctrl': 'flash',
        'lc_ctrl': 'lifecycle',
        'rv_core_ibex': 'ibex_wrapper',
        'rv_timer': 'timer',
        'sensor_ctrl': 'sensor'
    }

    def __init__(self):
        self._log = getLogger('ot.regs')

    def parse_defs(self, filename: str) -> RegisterDefs:
        with open(filename, 'rt') as hfp:
            return self._parse_defs(hfp)

    def _parse_defs(self, hfp: TextIO) -> RegisterDefs:
        radix = splitext(basename(hfp.name))[0]
        radix = radix.rsplit('_', 1)[0]
        defs = {}
        rre = f'{radix.upper()}_'
        # the following RE matches two kinds of definition:
        #   #define <COMPONENT>_<RADIX>_REG_OFFSET <HEXVAL>
        #   #define <COMPONENT>_PARAM_<RADIX>_OFFSET <HEXVAL>
        reg_cre = re.compile(rf'^#define {rre}(?P<param>PARAM_)?'
                             r'(?P<name>[A-Z][\w]+?)(?(param)|_REG)_OFFSET\s+'
                             r'(?P<val>(?:0x)?[A-Fa-f0-9]+)(?:\s|$)')
        for lno, line in enumerate(hfp, start=1):
            line = line.strip()
            rmo = reg_cre.match(line)
            if not rmo:
                continue
            regname = rmo.group('name')
            sregaddr = rmo.group('val')
            vstart, vend = rmo.start('val'), rmo.end('val')
            regaddr = int(sregaddr, 16 if sregaddr.startswith('0x') else 10)
            self._log.debug("%s: 0x%x", regname, regaddr)
            if regname in defs:
                self._log.error('Redefinition of %s: %x -> %x', regname,
                                defs[regname][0], regaddr)
            defs[regname] = (regaddr, ValueLocation(lno, vstart, vend))
        return defs

    def find_qemu_impl(self, filename: str, basedir: str, nomap: bool) \
            -> Optional[str]:
        filename = basename(filename)
        radix = re.sub(r'_regs$', '', splitext(filename)[0])
        if not nomap:
            radix = self.DEFMAP.get(radix, radix)
        impl_name = f'ot_{radix}.c'
        self._log.debug('Looking up %s for %s in %s',
                        impl_name, filename, basedir)
        for dirpath, _, filenames in walk(basedir):
            if impl_name in filenames:
                impl_path = joinpath(dirpath, impl_name)
                self._log.debug('Found as %s', impl_path)
                return impl_path
        return None

    def parse_ot_qemu(self, filename: str) -> RegisterDefs:
        with open(filename, 'rt') as qfp:
            return self._parse_ot_qemu(qfp)

    def _parse_ot_qemu(self, qfp: TextIO) -> RegisterDefs:
        defs = {}
        regfield_cre = re.compile(r'^\s*REG32\(([A-Z][\w]+),\s+'
                                  r'((?:0x)?[A-Fa-f0-9]+)u?\)(?:\s|$)')
        for lno, line in enumerate(qfp, start=1):
            line = line.strip()
            rmo = regfield_cre.match(line)
            if not rmo:
                continue
            regname = rmo.group(1)
            sregaddr = rmo.group(2)
            vstart, vend = rmo.start(2), rmo.end(2)
            regaddr = int(sregaddr, 16 if sregaddr.startswith('0x') else 10)
            self._log.debug("%s: 0x%x", regname, regaddr)
            if regname in defs:
                self._log.error('Redefinition of %s: %x -> %x', regname,
                                defs[regname][0], regaddr)
            defs[regname] = (regaddr, ValueLocation(lno, vstart, vend))
        return defs

    def compare(self, name: str, hdefs: RegisterDefs,
                qdefs: RegisterDefs, show_all: bool) \
            -> tuple[int, dict[ValueLocation, int], set[int]]:
        name = basename(name)
        chdefs = {k: v[0] for k, v in hdefs.items()}
        cqdefs = {k: v[0] for k, v in qdefs.items()}
        deprecated: set[int] = set()
        appendable: dict[str, int] = {}
        fixes: dict[ValueLocation, int] = {}
        if chdefs == cqdefs:
            self._log.info('%s: ok, %d register definitions', name, len(hdefs))
            return 0, fixes, deprecated, appendable
        if len(hdefs) == len(qdefs):
            self._log.debug('%s: %d register definitions', name, len(hdefs))
        hentries = set(hdefs)
        qentries = set(qdefs)
        mismatch_count = 0
        if hentries != qentries:
            hmissing = qentries - hentries
            if hmissing:
                missing = len(hmissing)
                self._log.warning('QEMU %s contains %s non-existing defs',
                                  name, missing)
                for miss in sorted(hmissing, key=lambda e: qdefs[e][1]):
                    deprecated.add(qdefs[miss][1].line)
                    if show_all:
                        self._log.warning('.. %s (0x%x)', miss, qdefs[miss][0])
                mismatch_count += missing
            qmissing = hentries - qentries
            if qmissing:
                missing = len(qmissing)
                self._log.warning('QEMU %s is missing %d defs', name, missing)
                for miss in sorted(qmissing, key=lambda e: hdefs[e][1]):
                    appendable[miss] = hdefs[miss][0]
                    if show_all:
                        self._log.warning('.. %s (0x%x)', miss, hdefs[miss][0])
                mismatch_count += missing
        entries = hentries & qentries
        for entry in sorted(entries, key=lambda e: hdefs[e][0]):
            if hdefs[entry][0] != qdefs[entry][0]:
                self._log.warning('Mismatched definition for %s: '
                                  'OT: 0x%x @ line %d / QEMU 0x%x @ line %d',
                                  entry, hdefs[entry][0], hdefs[entry][1].line,
                                  qdefs[entry][0], qdefs[entry][1].line)
                fixes[qdefs[entry][1]] = hdefs[entry][0]
                mismatch_count += 1
        self._log.error('%s: %d discrepancies', name, mismatch_count)
        return mismatch_count, fixes, deprecated, appendable

    def fix(self, filename: str, suffix: str, fixes: dict[ValueLocation, int],
            deprecated: set[int], newvalues: dict[str, int]) \
            -> None:
        fix_lines = {loc.line: (loc.start, loc.end, val)
                     for loc, val in fixes.items()}
        parts = splitext(filename)
        outfilename = f'{parts[0]}_{suffix}{parts[1]}'
        with open(filename, 'rt') as ifp:
            with open(outfilename, 'wt') as ofp:
                for lno, line in enumerate(ifp, start=1):
                    if lno in deprecated:
                        # use a C++ comment to stress on this line, since QEMU
                        # prohibit the use of this comment style, the output
                        # file should be rejected by checkpatch.pl
                        line = f'// {line.rstrip()} [deprecated]\n'
                    elif lno in fix_lines:
                        start, end, val = fix_lines[lno]
                        lhs = line[:start]
                        rhs = line[end:]
                        hexfmt = line[start:].startswith('0x')
                        if hexfmt:
                            line = f'{lhs}0x{val:x}{rhs}'
                        else:
                            line = f'{lhs}{val}{rhs}'
                    print(line, file=ofp, end='')
                if newvalues:
                    print('', file=ofp)
                    print('// New registers', file=ofp)
                    print('#ifdef USE_DECIMAL_VALUES', file=ofp)
                    for name, val in newvalues.items():
                        print(f'REG32({name}, {val}u)', file=ofp)
                    print('#else // USE_DECIMAL_VALUES', file=ofp)
                    for name, val in newvalues.items():
                        print(f'REG32({name}, 0x{val:x}u)', file=ofp)
                    print('#endf // !USE_DECIMAL_VALUES', file=ofp)


def main():
    """Main routine"""
    debug = False
    qemu_default_dir = dirname(dirname(dirname(normpath(__file__))))
    try:
        desc = sys.modules[__name__].__doc__.split('.', 1)[0].strip()
        argparser = ArgumentParser(description=f'{desc}.')
        argparser.add_argument('regs', nargs='+', metavar='file',
                               help='register header file')
        argparser.add_argument('-q', '--qemu', default=qemu_default_dir,
                               metavar='dir',
                               help='QEMU directory to seek')
        argparser.add_argument('-a', '--all', action='store_true',
                               default=False,
                               help='list all discrepancies')
        argparser.add_argument('-k', '--keep', action='store_true',
                               default=False,
                               help='keep verifying if QEMU impl. is not found')
        argparser.add_argument('-f', '--fix', metavar='SUFFIX', default=False,
                               help='create a file with specified suffix '
                                    'with possible fixes')
        argparser.add_argument('-M', '--no-map', action='store_true',
                               default=False,
                               help='do not convert regs into QEMU impl. path')
        argparser.add_argument('-v', '--verbose', action='count',
                               help='increase verbosity')
        argparser.add_argument('-d', '--debug', action='store_true',
                               help='enable debug mode')
        args = argparser.parse_args()
        debug = args.debug

        loglevel = max(DEBUG, ERROR - (10 * (args.verbose or 0)))
        loglevel = min(ERROR, loglevel)
        formatter = Formatter('%(levelname)8s %(name)-10s %(message)s')
        log = getLogger('ot')
        logh = StreamHandler(sys.stderr)
        logh.setFormatter(formatter)
        log.setLevel(loglevel)
        log.addHandler(logh)

        mismatch_count = 0
        for regfile in args.regs:
            otr = OtRegisters()
            qemu_impl = otr.find_qemu_impl(regfile, args.qemu, args.no_map)
            if not qemu_impl:
                pretty_file = relpath(regfile)
                if pretty_file.startswith(pardir):
                    pretty_file = regfile
                msg = f'Unable to locate implementation file for {pretty_file}'
                if not args.keep:
                    raise ValueError(msg)
                log.info('%s', msg)
                continue
            hdefs = otr.parse_defs(regfile)
            qdefs = otr.parse_ot_qemu(qemu_impl)
            mm_count, fixes, deprecated, newvalues = \
                otr.compare(qemu_impl, hdefs, qdefs, args.all)
            if mm_count and args.fix:
                otr.fix(qemu_impl, args.fix, fixes, deprecated, newvalues)
            mismatch_count += mm_count

        if mismatch_count:
            print(f'{mismatch_count} differences', file=sys.stderr)
            sys.exit(1)
        print('No differences', file=sys.stderr)

    # pylint: disable=broad-except
    except Exception as exc:
        print(f'\nError: {exc}', file=sys.stderr)
        if debug:
            print(format_exc(chain=False), file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(2)


if __name__ == '__main__':
    main()
