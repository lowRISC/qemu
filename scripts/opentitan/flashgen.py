#!/usr/bin/env python3

# Copyright (c) 2023-2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Create/update an OpenTitan backend flash file.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from argparse import ArgumentParser, FileType
from os import rename
from os.path import dirname, exists, isfile, join as joinpath, normpath
from traceback import format_exc
import sys

QEMU_PYPATH = joinpath(dirname(dirname(dirname(normpath(__file__)))),
                       'python', 'qemu')
sys.path.append(QEMU_PYPATH)

from ot.eflash.gen import FlashGen
from ot.util.log import configure_loggers
from ot.util.misc import HexInt


def main():
    """Main routine"""
    debug = True
    banks = list(range(FlashGen.NUM_BANKS))
    try:
        desc = sys.modules[__name__].__doc__.split('.', 1)[0].strip()
        argparser = ArgumentParser(description=f'{desc}.')
        img = argparser.add_argument_group(title='Image')
        img.add_argument('flash', nargs=1, metavar='flash',
                         help='virtual flash file')
        img.add_argument('-D', '--discard', action='store_true',
                               help='Discard any previous flash file content')
        img.add_argument('-a', '--bank', type=int, choices=banks,
                         default=banks[0],
                         help=f'flash bank for data (default: {banks[0]})')
        img.add_argument('-s', '--offset', type=HexInt.parse,
                         default=FlashGen.CHIP_ROM_EXT_SIZE_MAX,
                         help=f'offset of the BL0 file (default: '
                              f'0x{FlashGen.CHIP_ROM_EXT_SIZE_MAX:x})')
        files = argparser.add_argument_group(title='Files')
        files.add_argument('-x', '--exec', type=FileType('rb'), metavar='file',
                           help='rom extension or application')
        files.add_argument('-X', '--exec-elf', metavar='elf',
                           help='ELF file for rom extension or application, '
                                'for symbol tracking (default: auto)')
        files.add_argument('-b', '--boot', type=FileType('rb'),
                           metavar='file', help='bootloader 0 file')
        files.add_argument('-B', '--boot-elf', metavar='elf',
                           help='ELF file for bootloader, for symbol tracking'
                                ' (default: auto)')
        files.add_argument('-t', '--otdesc', action='append', default=[],
                           help='OpenTitan style file descriptor, '
                                'may be repeated')
        files.add_argument('-T', '--ignore-time', action='store_true',
                           help='Discard time checking on ELF files')
        files.add_argument('-U', '--unsafe-elf', action='store_true',
                           help='Discard sanity checking on ELF files')
        files.add_argument('-A', '--accept-invalid', action='store_true',
                           help='Blindy accept invalid input files')
        extra = argparser.add_argument_group(title='Extra')
        extra.add_argument('-v', '--verbose', action='count',
                           help='increase verbosity')
        extra.add_argument('-d', '--debug', action='store_true',
                           help='enable debug mode')
        args = argparser.parse_args()
        debug = args.debug

        configure_loggers(args.verbose, 'flashgen', 'elf')

        use_bl0 = bool(args.boot) or len(args.otdesc) > 1
        gen = FlashGen(args.offset if use_bl0 else 0, args.unsafe_elf,
                       args.accept_invalid, args.ignore_time)
        flash_pathname = args.flash[0]
        backup_filename = None
        if args.otdesc and any(filter(None, (args.bank,
                                             args.boot, args.boot_elf,
                                             args.exec, args.exec_elf))):
            argparser.error('OT file descriptor mode is mutually exclusive '
                            'with boot and exec options')
        if args.boot_elf:
            if not args.boot:
                argparser.error('Bootloader ELF option requires bootloader '
                                'binary')
            if not isfile(args.boot_elf):
                argparser.error('No such Bootloader ELF file')
        if args.exec_elf:
            if not args.exec:
                argparser.error('Application ELF option requires application '
                                'binary')
            if not isfile(args.exec_elf):
                argparser.error('No such Bootloader ELF file')
        try:
            if args.discard:
                if exists(flash_pathname):
                    backup_filename = f'{flash_pathname}.bak'
                    rename(flash_pathname, backup_filename)
            gen.open(args.flash[0])
            if args.exec:
                gen.store_rom_ext(args.bank, args.exec, args.exec_elf)
            if args.boot:
                gen.store_bootloader(args.bank, args.boot, args.boot_elf)
            if args.otdesc:
                gen.store_ot_files(args.otdesc)
            backup_filename = None
        finally:
            gen.close()
            if backup_filename:
                print('Restoring previous file after error', file=sys.stderr)
                rename(backup_filename, flash_pathname)

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
