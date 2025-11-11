#!/usr/bin/env python3

# Copyright (c) 2025 Rivos, Inc.
# Copyright (c) 2025 lowRISC contributors.
# SPDX-License-Identifier: Apache2

"""LifeCycle Controller tiny token tools

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from argparse import ArgumentParser, FileType
from binascii import hexlify, unhexlify
from os.path import dirname, join as joinpath, normpath
from traceback import format_exception
from typing import Optional

import sys

QEMU_PYPATH = joinpath(dirname(dirname(dirname(normpath(__file__)))),
                       'python', 'qemu')
sys.path.append(QEMU_PYPATH)

# ruff: noqa: E402
from ot.util.log import configure_loggers

_IEXC: Optional[Exception] = None  # noqa: F841
try:
    from ot.lc_ctrl.tools import LifeCycleTokenEngine
except ImportError as _IEXC:  # noqa: F841
    pass


def main():
    """Main routine"""
    debug = True
    try:
        desc = sys.modules[__name__].__doc__.split('.', 1)[0].strip()
        argparser = ArgumentParser(description=f'{desc}.')
        argparser.add_argument('-s', '--hash', metavar='TOKEN',
                               help='hash the submitted token')
        argparser.add_argument('-g', '--generate', metavar='TOKEN_NAME',
                               help='generate a new token pair')
        argparser.add_argument('-r', '--parse-rust', metavar='FILE',
                               type=FileType('rt'),
                               help='parse token from a Rust file')
        argparser.add_argument('-t', '--token', metavar='NAME',
                               help='only report named token')
        extra = argparser.add_argument_group(title='Extras')
        extra.add_argument('-v', '--verbose', action='count',
                           help='increase verbosity')
        extra.add_argument('-d', '--debug', action='store_true',
                           help='enable debug mode')

        args = argparser.parse_args()
        debug = args.debug

        configure_loggers(args.verbose, 'tkgen', -1, 'otp')

        if _IEXC is not None:
            if debug:
                print(''.join(format_exception(_IEXC, chain=False)),
                      file=sys.stderr)
            argparser.error(f'Missing PYTHONPATH: {_IEXC}')

        tkeng = LifeCycleTokenEngine()

        if args.hash:
            token = unhexlify(args.hash)
            hashed_str = hexlify(tkeng.hash(token)).decode()
            # try to follow the same case for the result as the input
            if args.hash == args.hash.upper():
                hashed_str = hashed_str.upper()
            print(hashed_str)

        if args.generate:
            tkpair = tkeng.generate()
            prefix = args.generate.upper()
            rust_code = tkeng.generate_code('rust', prefix, tkpair)
            print(rust_code)
            qemu_code = tkeng.generate_code('qemu', prefix, tkpair)
            print(qemu_code)

        if args.token and not args.parse_rust:
            argparser.error('Token name requires Rust file')

        if args.parse_rust:
            rust = args.parse_rust.read()
            args.parse_rust.close()
            tkpairs = tkeng.parse_rust(rust)
            if args.token:
                token_name = args.token.upper()
                if token_name not in tkpairs:
                    argparser.error('No such token')
                print(hexlify(tkpairs[token_name].text).decode().upper())
            else:
                for name, tkpair in tkpairs.items():
                    for kind in tkpair.__annotations__:
                        print(f'LC_{name}_TOKEN_{kind.upper()}='
                              f'{hexlify(getattr(tkpair, kind)).decode()}')
                    print()

    except (IOError, ValueError, ImportError) as exc:
        print(f'\nError: {exc}', file=sys.stderr)
        if debug:
            print(''.join(format_exception(exc, chain=False)), file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(2)


if __name__ == '__main__':
    main()
