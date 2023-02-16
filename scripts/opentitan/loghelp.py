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

from argparse import ArgumentParser, FileType
from logging import DEBUG, ERROR, getLogger, Formatter, StreamHandler
from os import pardir, walk
from os.path import basename, dirname, join as joinpath, relpath, splitext
from re import compile as re_compile, sub as re_sub
from sys import exit as sysexit, modules, stderr
from traceback import format_exc
from typing import Dict, Optional, TextIO, Tuple


REG_CRE = re_compile(r'^#define ([A-Z][\w]+)_REG_(OFFSET|RESVAL)\s+'
                     r'((?:0x)?[A-Fa-f0-9]+)(?:\s|$)')

RegisterDefs = Dict[str, Tuple[int, int]]


def parse_defs(hfp: TextIO) -> RegisterDefs:
    log = getLogger('ot')
    radix = splitext(basename(hfp.name))[0]
    radix = radix.rsplit('_', 1)[0]
    radix_re = f'^{radix.upper()}_'
    defs = {}
    for line in hfp:
        line = line.strip()
        rmo = REG_CRE.match(line)
        if not rmo:
            continue
        sregname = rmo.group(1)
        sregkind = rmo.group(2)
        sregaddr = rmo.group(3)
        regname = re_sub(radix_re, '', sregname)
        regval = int(sregaddr, 16 if sregaddr.startswith('0x') else 10)
        if sregkind == 'OFFSET':
            defs[regname] = (regval, 0)
        else:
            defs[regname] = (defs[regname][0], regval)
            log.debug("%s @ 0x%x = 0x%x",
                      regname, defs[regname][0], defs[regname][1])
    return defs


def check(lfp: TextIO, comp: str, defs: RegisterDefs):
    prefix = f'ot_{comp}_io_'
    device = {}
    resvals = {v[0]: v[1] for v in defs.values()}
    regnames = {v[0]: k for k, v in defs.items()}
    for line in lfp:
        line = line.strip()
        if not line.startswith(prefix):
            continue
        parts = line[len(prefix):].split(' ', 1)
        items = dict(tuple(x.strip().split('=', 1)) for x in parts[1].split(','))
        vals = {k: int(v, 16) for k, v in items.items()}
        addr = vals['addr']
        val = vals['val']
        if parts[0] == 'write':
            device[addr] = val
        else:
            regname = regnames.get(addr, '?')
            if addr in device:
                if val != device[addr]:
                    print(f'Unexpected value @ 0x{addr:x}: 0x{val:x} '
                          f'(0x{device[addr]:x}) "{regname}"')
            elif defs:
                if addr not in resvals:
                    print(f'No known reset value @ 0x{addr:x} "{regname}"')
                elif val != resvals[addr]:
                    print(f'Reset value differ @ 0x{addr:x}: 0x{val:x} '
                          f'(0x{resvals[addr]:x}) "{regname}"')


def main():
    """Main routine"""
    #pylint: disable-msg=too-many-locals
    debug = False
    qemu_default_dir = dirname(dirname(dirname(__file__)))
    try:
        desc = modules[__name__].__doc__.split('.', 1)[0].strip()
        argparser = ArgumentParser(description=f'{desc}.')
        argparser.add_argument('log', nargs=1, metavar='file',
                               type=FileType('rt'),
                               help='log file')
        argparser.add_argument('-c', '--component', metavar='name',
                               required=True,
                               help='name of the OT component in log file')
        argparser.add_argument('-r', '--reg',metavar='file',
                               type=FileType('rt'),
                               help='register header file')
        argparser.add_argument('-v', '--verbose', action='count',
                               help='increase verbosity')
        argparser.add_argument('-d', '--debug', action='store_true',
                               help='enable debug mode')
        args = argparser.parse_args()
        debug = args.debug

        loglevel = max(DEBUG, ERROR - (10 * (args.verbose or 0)))
        loglevel = min(ERROR, loglevel)
        formatter = Formatter('%(asctime)s.%(msecs)03d %(levelname)8s '
                              '%(name)-10s %(message)s', '%H:%M:%S')
        log = getLogger('ot')
        logh = StreamHandler(stderr)
        logh.setFormatter(formatter)
        log.setLevel(loglevel)
        log.addHandler(logh)

        defs = parse_defs(args.reg) if args.reg else {}
        check(args.log[0], args.component, defs)

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
