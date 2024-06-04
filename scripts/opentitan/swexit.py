#!/usr/bin/env python3

# Copyright (c) 2023-2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""OpenTitan exit code generator for QEMU.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from argparse import ArgumentParser, FileType
from struct import pack as spack
from sys import exit as sysexit, modules, stderr, stdout
from traceback import format_exc

# pylint: disable=missing-function-docstring


BASE_ADDRESS = {
    'earlgrey': 0x411f0000,
    'darjeeling': 0x211f0000,
}

LUI_MASK = (1 << 12) - 1


def to_int(value: str) -> int:
    return int(value.strip(), value.startswith('0x') and 16 or 10)


def main():
    debug = False
    try:
        desc = modules[__name__].__doc__.split('.', 1)[0].strip()
        argparser = ArgumentParser(description=f'{desc}')
        argparser.add_argument('-a', '--address', type=to_int,
                               help='Base address for swexit device (default: depends on SoC)')
        argparser.add_argument('-t', '--soc', choices=list(BASE_ADDRESS),
                               help='SoC type', required=True)
        argparser.add_argument('-o', '--output', type=FileType('wb'),
                               help='output file, default to stdout')
        argparser.add_argument('-d', '--debug', action='store_true',
                               help='enable debug mode')
        args = argparser.parse_args()
        debug = args.debug

        addr = args.address if args.address else BASE_ADDRESS[args.soc]
        addr &= ~LUI_MASK
        lui = addr | 0x537
        bincode = spack('<IIII',
                        lui,         # lui  a0,addr     # ibex_core_wrapper base
                        0xc0de05b7,  # lui  a1,0xc0de0  # exit code (0)
                        0x00b52423,  # sw   a1,8(a0)    # write to sw_fatal_err
                        0x10500073)  # wfi              # stop here
        out = args.output or stdout.buffer
        out.write(bincode)

    # pylint: disable=broad-except
    except Exception as exc:
        print(f'\nError: {exc}', file=stderr)
        if debug:
            print(format_exc(chain=False), file=stderr)
        sysexit(1)
    except KeyboardInterrupt:
        sysexit(2)


if __name__ == '__main__':
    main()
