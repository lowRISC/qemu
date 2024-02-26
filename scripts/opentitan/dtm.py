#!/usr/bin/env python3

# Copyright (c) 2024, Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Debug Transport Module tiny demo.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from argparse import ArgumentParser, Namespace
from os import linesep
from os.path import dirname, join as joinpath, normpath
from socket import create_connection
from traceback import format_exc
from typing import Optional
import sys

# pylint: disable=wrong-import-position
# pylint: disable=wrong-import-order
# pylint: disable=import-error

# JTAG module is available from the scripts/ directory
sys.path.append(joinpath(normpath(dirname(dirname(sys.argv[0])))))

from ot.util.log import configure_loggers  # noqa: E402
from ot.dtm import DebugTransportModule  # noqa: E402
from jtag.bits import BitSequence  # noqa: E402
from jtag.bitbang import JtagBitbangController  # noqa: E402
from jtag.jtag import JtagEngine  # noqa: E402


def idcode(engine: JtagEngine, ir_length: int) -> None:
    """Retrieve ID code."""
    code = JtagBitbangController.INSTRUCTIONS['idcode']
    engine.write_ir(BitSequence(code, ir_length))
    value = engine.read_dr(32)
    engine.go_idle()
    return int(value)


def main():
    """Entry point."""
    debug = True
    try:
        args: Optional[Namespace] = None
        argparser = ArgumentParser(description=sys.modules[__name__].__doc__)
        qvm = argparser.add_argument_group(title='Virtual machine')

        qvm.add_argument('-H', '--host', default='127.0.0.1',
                         help='JTAG host (default: localhost)')
        qvm.add_argument('-P', '--port', type=int,
                         default=JtagBitbangController.DEFAULT_PORT,
                         help=f'JTAG port, '
                              f'default: {JtagBitbangController.DEFAULT_PORT}')
        qvm.add_argument('-I', '--info', action='store_true',
                         help='Report JTAG ID code and DTM configuration')
        qvm.add_argument('-l', '--ir-length', type=int, default=5,
                         help='bit length of the IR register')
        extra = argparser.add_argument_group(title='Extras')
        extra.add_argument('-v', '--verbose', action='count',
                           help='increase verbosity')
        extra.add_argument('-d', '--debug', action='store_true',
                           help='enable debug mode')

        args = argparser.parse_args()
        debug = args.debug

        configure_loggers(args.verbose, 'dtm', 'jtag')

        sock = create_connection((args.host, args.port), timeout=0.5)
        sock.settimeout(0.1)
        ctrl = JtagBitbangController(sock)
        eng = JtagEngine(ctrl)
        ctrl.tap_reset(True)
        ir_length = args.ir_length
        dtm = DebugTransportModule(eng, ir_length)
        if args.info:
            code = idcode(eng, ir_length)
            print(f'IDCODE:    0x{code:x}')
            sys.exit(0)
            version = dtm['dtmcs'].dmi_version
            abits = dtm['dtmcs'].abits
            print(f'DTM:       v{version[0]}.{version[1]}, {abits} bits')
            dtm['dtmcs'].check()
        dtm['dtmcs'].dmireset()

    # pylint: disable=broad-except
    except Exception as exc:
        print(f'{linesep}Error: {exc}', file=sys.stderr)
        if debug:
            print(format_exc(chain=False), file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(2)


if __name__ == '__main__':
    main()
