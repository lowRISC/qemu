#!/usr/bin/env python3

"""Verify register definitions.
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

from argparse import ArgumentParser
from logging import DEBUG, ERROR, getLogger, Formatter, StreamHandler
from os import pardir, walk
from os.path import basename, dirname, join as joinpath, relpath, splitext
from re import compile as re_compile, sub as re_sub
from sys import exit as sysexit, modules, stderr
from traceback import format_exc
from typing import Dict, Optional, TextIO, Tuple


RegisterDefs = Dict[str, Tuple[int, int]]

#pylint: disable-msg=unspecified-encoding
#pylint: disable-msg=missing-function-docstring


class OtRegisters:
    """Simple class to parse and compare register definitions
    """

    REG_CRE = re_compile(r'^#define ([A-Z][\w]+)_REG_OFFSET\s+'
                         r'((?:0x)?[A-Fa-f0-9]+)(?:\s|$)')
    REGFIELD_CRE = re_compile(r'^\s*REG32\(([A-Z][\w]+),\s+'
                              r'((?:0x)?[A-Fa-f0-9]+)u?\)(?:\s|$)')
    DEFMAP = {
        'alert_handler': 'alert',
        'flash_ctrl': 'flash',
        'lc_ctrl': 'lifecycle',
        'otp_ctrl': 'otp',
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
        radix_re = f'^{radix.upper()}_'
        defs = {}
        for lno, line in enumerate(hfp, start=1):
            line = line.strip()
            rmo = self.REG_CRE.match(line)
            if not rmo:
                continue
            sregname = rmo.group(1)
            sregaddr = rmo.group(2)
            regname = re_sub(radix_re, '', sregname)
            regaddr = int(sregaddr, 16 if sregaddr.startswith('0x') else 10)
            self._log.debug("%s: 0x%x", regname, regaddr)
            if regname in defs:
                self._log.error('Redefinition of %s: %x -> %x', regname,
                               defs[regname][0], regaddr)
            defs[regname] = (regaddr, lno)
        return defs

    def find_qemu_impl(self, filename: str, basedir: str, nomap: bool) \
            -> Optional[str]:
        filename = basename(filename)
        radix = re_sub(r'_regs$', '', splitext(filename)[0])
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
        for lno, line in enumerate(qfp, start=1):
            line = line.strip()
            rmo = self.REGFIELD_CRE.match(line)
            if not rmo:
                continue
            regname = rmo.group(1)
            sregaddr = rmo.group(2)
            regaddr = int(sregaddr, 16 if sregaddr.startswith('0x') else 10)
            self._log.debug("%s: 0x%x", regname, regaddr)
            if regname in defs:
                self._log.error('Redefinition of %s: %x -> %x', regname,
                               defs[regname][0], regaddr)
            defs[regname] = (regaddr, lno)
        return defs

    def compare(self, name: str, hdefs: RegisterDefs,
                qdefs: RegisterDefs, show_all: bool) -> int:
        name = basename(name)
        chdefs = {k: v[0] for k, v in hdefs.items()}
        cqdefs = {k: v[0] for k, v in qdefs.items()}
        if chdefs == cqdefs:
            self._log.info('%s: ok, %d register definitions', name, len(hdefs))
            return 0
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
                if show_all:
                    for miss in sorted(hmissing, key=lambda e: qdefs[e][1]):
                        self._log.warning('.. %s (0x%x)', miss, qdefs[miss][0])
                mismatch_count += missing
            qmissing = hentries - qentries
            if qmissing:
                missing = len(qmissing)
                self._log.warning('QEMU %s is missing %d defs', name, missing)
                if show_all:
                    for miss in sorted(qmissing, key=lambda e: hdefs[e][1]):
                        self._log.warning('.. %s (0x%x)', miss, hdefs[miss][0])
                mismatch_count += missing
        entries = hentries & qentries
        for entry in sorted(entries, key=lambda e: hdefs[e][0]):
            if hdefs[entry][0] != qdefs[entry][0]:
                self._log.warning('Mismatched definition for %s: '
                                  'OT: 0x%x @ line %d / QEMU 0x%x @ line %d',
                                  entry, hdefs[entry][0], hdefs[entry][1],
                                  qdefs[entry][0], qdefs[entry][1])
                mismatch_count += 1
        self._log.error('%s: %d discrepancies', name, mismatch_count)
        return mismatch_count


def main():
    """Main routine"""
    #pylint: disable-msg=too-many-locals
    debug = False
    qemu_default_dir = dirname(dirname(dirname(__file__)))
    try:
        desc = modules[__name__].__doc__.split('.', 1)[0].strip()
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
        logh = StreamHandler(stderr)
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
            mismatch_count += otr.compare(qemu_impl, hdefs, qdefs, args.all)

        if mismatch_count:
            print(f'{mismatch_count} differences', file=stderr)
            sysexit(1)
        print('No differences', file=stderr)

    #pylint: disable-msg=broad-except
    except Exception as exc:
        print(f'\nError: {exc}', file=stderr)
        if debug:
            print(format_exc(chain=False), file=stderr)
        sysexit(1)
    except KeyboardInterrupt:
        sysexit(2)


if __name__ == '__main__':
    main()
