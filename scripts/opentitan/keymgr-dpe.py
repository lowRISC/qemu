#!/usr/bin/env python3

# Copyright (c) 2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""QEMU OT tool to generate Key Manager DPE keys.

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
from ot.km.dpe import KeyManagerDpe
from ot.km.engine import KeyManagerDpeEngine
from ot.otp.image import OtpImage
from ot.util.arg import ArgError
from ot.util.log import configure_loggers
from ot.util.misc import HexInt


def main():
    """Main routine"""
    debug = True
    desc = sys.modules[__name__].__doc__.split('.', 1)[0].strip()
    argparser = ArgumentParser(description=f'{desc}.')
    try:

        files = argparser.add_argument_group(title='Files')
        files.add_argument('-c', '--config', type=FileType('rt'),
                           metavar='CFG', required=True,
                           help='input QEMU OT config file')
        files.add_argument('-j', '--otp-map', type=FileType('rt'),
                           metavar='HJSON', required=True,
                           help='input OTP controller memory map file')
        files.add_argument('-l', '--lifecycle', type=FileType('rt'),
                           metavar='SV',
                           help='input lifecycle system verilog file')
        files.add_argument('-m', '--vmem', type=FileType('rt'),
                           help='input VMEM file')
        files.add_argument('-R', '--raw', type=FileType('rb'),
                           help='input QEMU OTP raw image file')
        files.add_argument('-r', '--rom', type=FileType('rb'), action='append',
                           help='input ROM image file, may be repeated')

        params = argparser.add_argument_group(title='Parameters')
        params.add_argument('-e', '--ecc', type=int,
                            default=OtpImage.DEFAULT_ECC_BITS,
                            metavar='BITS', help=f'ECC bit count (default: '
                                                 f'{OtpImage.DEFAULT_ECC_BITS})')
        params.add_argument('-z', '--rom-size', metavar='SIZE',
                            type=HexInt.xparse,
                            action='append', default=[],
                            help='ROM image size in bytes, may be repeated')

        extra = argparser.add_argument_group(title='Extras')
        extra.add_argument('-v', '--verbose', action='count',
                           help='increase verbosity')
        extra.add_argument('-d', '--debug', action='store_true',
                           help='enable debug mode')

        subparsers = argparser.add_subparsers(help='Execution mode',
                                              dest='exec')

        genparser = subparsers.add_parser('generate', help='generate a key')
        genparser.add_argument('-b', '--swbindings', action='append',
                            default=[], metavar='HEXSTR',
                            help='SW bindings, may be repeated')
        genparser.add_argument('-g', '--gen-out', choices=KeyManagerDpe.OUTPUTS,
                            help='generation output (default: auto)')
        genparser.add_argument('-k', '--key-version', metavar='HEXSTR',
                            type=HexInt, required=True,
                            help='Key version')
        genparser.add_argument('-o', '--output',
                           help='output file with generated key')
        genparser.add_argument('-R', '--rust-const', metavar='NAME',
                           help='Rust constant name for the generated key')
        genparser.add_argument('-s', '--salt', default=[], metavar='HEXSTR',
                            required=True,
                            help='Salt')
        genparser.add_argument('-t', '--target',
                            choices=KeyManagerDpe.TARGETS, required=True,
                            help='destination device')

        exeparser = subparsers.add_parser('execute', help='execute sqeuence')
        exeparser.add_argument('-s', '--sequence', metavar='INI', required=True,
                               type=FileType('rt'),
                               help='execution sequence definition')

        vrfparser = subparsers.add_parser('verify', help='verify execution log')
        vrfparser.add_argument('-l', '--exec-log', metavar='LOG', required=True,
                               type=FileType('rb'),
                               help='execution log to verify')

        args = argparser.parse_args()
        debug = args.debug

        configure_loggers(args.verbose, 'keymgr', 'romimg', -1, 'otp',
                          name_width=12)

        if not (args.vmem or args.raw):
            argparser.error('Either VMEM or RAW image file must be specified')
        if args.vmem and args.raw:
            argparser.error('Only one of VMEM or RAW image file can be specified')

        if args.exec == 'generate':
            KeyManagerDpe.from_args(args)
        else:
            kmd = KeyManagerDpe.create(
                args.otp_map, args.ecc, args.vmem, args.raw, args.lifecycle,
                args.config, args.rom, args.rom_size)
            kme = KeyManagerDpeEngine(kmd)
            if args.exec == 'execute':
                kme.execute(args.sequence)
            elif args.exec == 'verify':
                kme.verify(args.exec_log)

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
