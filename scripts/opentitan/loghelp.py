#!/usr/bin/env python3

# Copyright (c) 2023-2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Verify register definitions.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from argparse import ArgumentParser, FileType
from logging import getLogger
from os.path import basename, dirname, splitext, join as joinpath, normpath
from traceback import format_exc
from typing import TextIO
import re
import sys

QEMU_PYPATH = joinpath(dirname(dirname(dirname(normpath(__file__)))),
                       'python', 'qemu')
sys.path.append(QEMU_PYPATH)

from ot.util.log import configure_loggers

# pylint: disable=missing-function-docstring

REG_CRE = re.compile(r'^#define ([A-Z][\w]+)_REG_(OFFSET|RESVAL)\s+'
                     r'((?:0x)?[A-Fa-f0-9]+)(?:\s|$)')

RegisterDefs = dict[str, tuple[int, int]]


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
        regname = re.sub(radix_re, '', sregname)
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
        items = dict(tuple(x.strip().split('=', 1))
                     for x in parts[1].split(','))
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
    debug = False
    try:
        desc = sys.modules[__name__].__doc__.split('.', 1)[0].strip()
        argparser = ArgumentParser(description=f'{desc}.')
        argparser.add_argument('log', nargs=1, metavar='file',
                               type=FileType('rt'),
                               help='log file')
        argparser.add_argument('-c', '--component', metavar='name',
                               required=True,
                               help='name of the OT component in log file')
        argparser.add_argument('-r', '--reg', metavar='file',
                               type=FileType('rt'),
                               help='register header file')
        argparser.add_argument('-v', '--verbose', action='count',
                               help='increase verbosity')
        argparser.add_argument('-d', '--debug', action='store_true',
                               help='enable debug mode')
        args = argparser.parse_args()
        debug = args.debug

        configure_loggers(args.verbose, 'ot', ms=True)

        defs = parse_defs(args.reg) if args.reg else {}
        check(args.log[0], args.component, defs)

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
