#!/usr/bin/env python3

# Copyright (c) 2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""QEMU OT tool to generate a scrambled ROM image.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

# pylint: disable=invalid-name
# pylint: enable=invalid-name

from argparse import ArgumentParser, FileType
from os.path import dirname, join as joinpath, normpath
from traceback import format_exception
import sys

QEMU_PYPATH = joinpath(dirname(dirname(dirname(normpath(__file__)))),
                       'python', 'qemu')
sys.path.append(QEMU_PYPATH)

# ruff: noqa: E402
from ot.rom.image import ROMImage
from ot.util.arg import ArgError
from ot.util.log import configure_loggers
from ot.util.misc import HexInt


def main():
    """Main routine"""
    debug = True
    desc = sys.modules[__name__].__doc__.split('.', 1)[0].strip()
    argparser = ArgumentParser(description=f'{desc}.')
    out_formats = list(ROMImage.save_formats)
    # make hex format, if supported, the first (default) one
    out_formats.sort(key=lambda x: x if x != 'HEX' else '')
    try:

        files = argparser.add_argument_group(title='Files')
        files.add_argument('rom', nargs=1, type=FileType('rb'),
                           help='input ROM image file')
        files.add_argument('-c', '--config', type=FileType('rt'),
                           metavar='CFG', required=True,
                           help='input QEMU OT config file')
        files.add_argument('-o', '--output',
                           help='output ROM image file')

        params = argparser.add_argument_group(title='Parameters')
        params.add_argument('-D', '--digest', action='store_true',
                            help='Show the ROM digest')
        params.add_argument('-f', '--output-format', choices=out_formats,
                            default=out_formats[0],
                            help=f'Output file format '
                                 f'(default: {out_formats[0]})')
        params.add_argument('-i', '--rom-id', type=int,
                            help='ROM image identifier')
        params.add_argument('-s', '--subst-perm', action='store_true',
                            help='Enable legacy mode with S&P mode')
        params.add_argument('-z', '--rom-size', metavar='SIZE',
                            type=HexInt.xparse,
                            help='ROM image size in bytes (accepts Ki suffix)')

        extra = argparser.add_argument_group(title='Extras')
        extra.add_argument('-v', '--verbose', action='count',
                           help='increase verbosity')
        extra.add_argument('-d', '--debug', action='store_true',
                           help='enable debug mode')

        args = argparser.parse_args()
        debug = args.debug

        configure_loggers(args.verbose, 'romimg', -1, name_width=12)

        rom_img = ROMImage(None, args.subst_perm)
        rom_img.load_config(args.config, args.rom_id)
        rom_img.load(args.rom[0], args.rom_size)

        if args.digest:
            print('Digest:', rom_img.hexdigest)

        with open(args.output, 'wb') if args.output else \
                sys.stdout.buffer as wfp:
            rom_img.save(wfp, args.output_format)

    except ArgError as exc:
        argparser.error(str(exc))
    except (IOError, ValueError, ImportError) as exc:
        print(f'\nError: {exc}', file=sys.stderr)
        if debug:
            print(''.join(format_exception(exc, chain=False)),
                  file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(2)


if __name__ == '__main__':
    main()
