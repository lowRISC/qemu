#!/usr/bin/env python3

# Copyright (c) 2024, Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""OTP controller access through the RISC-V Debug Module.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from argparse import ArgumentParser, Namespace, FileType
from binascii import hexlify
from io import StringIO
from os import linesep
from os.path import dirname, join as joinpath, normpath
from socket import create_connection
from traceback import format_exc
from typing import Optional
import sys

QEMU_PYPATH = joinpath(dirname(dirname(dirname(normpath(__file__)))),
                       'python', 'qemu')
sys.path.append(QEMU_PYPATH)

from jtag.bitbang import JtagBitbangController
from jtag.jtag import JtagEngine

from ot.dm import DebugModule
from ot.dm.otp import OTPController
from ot.dtm import DebugTransportModule
from ot.otp import OtpMap
from ot.util.log import configure_loggers
from ot.util.misc import HexInt, dump_buffer


DEFAULT_IR_LENGTH = 5
"""Default TAP Instruction Register length."""

DEFAULT_DMI_ADDRESS = 0x0
"""Default DMI address of the DM."""

DEFAULT_OTP_BASE_ADDRESS = 0x30130000
"""Default base address of the OTP controller on the OT local crossbar."""


def main():
    """Entry point."""
    debug = True
    try:
        args: Optional[Namespace] = None
        argparser = ArgumentParser(
            description=sys.modules[__name__].__doc__.split('.')[0])
        qvm = argparser.add_argument_group(title='Virtual machine')
        qvm.add_argument('-H', '--host', default='127.0.0.1',
                         help='JTAG host (default: localhost)')
        qvm.add_argument('-P', '--port', type=int,
                         default=JtagBitbangController.DEFAULT_PORT,
                         help=f'JTAG port, '
                              f'default: {JtagBitbangController.DEFAULT_PORT}')
        qvm.add_argument('-Q', '--no-quit', action='store_true', default=False,
                         help='do not ask the QEMU to quit on exit')
        dmi = argparser.add_argument_group(title='DMI')
        dmi.add_argument('-l', '--ir-length', type=int,
                         default=DEFAULT_IR_LENGTH,
                         help=f'bit length of the IR register '
                              f'(default: {DEFAULT_IR_LENGTH})')
        dmi.add_argument('-b', '--base', type=HexInt.parse,
                         default=DEFAULT_DMI_ADDRESS,
                         help=f'define DMI base address '
                              f'(default: 0x{DEFAULT_DMI_ADDRESS:x})')
        otp = argparser.add_argument_group(title='OTP')
        otp.add_argument('-j', '--otp-map', type=FileType('rt'),
                         metavar='HJSON',
                         help='input OTP controller memory map file')
        otp.add_argument('-a', '--address', type=HexInt.parse,
                         default=DEFAULT_OTP_BASE_ADDRESS,
                         help=f'base address the OTP controller, default: '
                              f'0x{DEFAULT_OTP_BASE_ADDRESS:08x}')
        otp.add_argument('-p', '--partition',
                         help='select a partition')
        otp.add_argument('-i', '--item',
                         help='select a partition item')
        otp.add_argument('-L', '--list', action='store_true',
                         help='list the partitions and/or the items')
        otp.add_argument('-r', '--read', action='store_true',
                         help='read the value of the selected item')
        otp.add_argument('-w', '--write',
                         help='write the value to the selected item')
        otp.add_argument('-D', '--digest', action='store_true',
                         help='show the OTP HW partition digests')
        extra = argparser.add_argument_group(title='Extras')
        extra.add_argument('-v', '--verbose', action='count',
                           help='increase verbosity')
        extra.add_argument('-d', '--debug', action='store_true',
                           help='enable debug mode')

        args = argparser.parse_args()
        debug = args.debug

        configure_loggers(args.verbose, 'dtm.rvdm', 'dtm.otp', -1, 'dtm',
                          'jtag')

        sock = create_connection((args.host, args.port), timeout=0.5)
        sock.settimeout(0.1)

        if args.otp_map:
            otpmap = OtpMap()
            otpmap.load(args.otp_map)
            args.otp_map.close()
        else:
            if args.digest:
                argparser.error('Digest feature requires an OTP map')
            otpmap = None
        ctrl = JtagBitbangController(sock)
        eng = JtagEngine(ctrl)
        ctrl.tap_reset(True)
        ir_length = args.ir_length
        dtm = DebugTransportModule(eng, ir_length)
        rvdm = DebugModule(dtm, args.base)
        try:
            rvdm.initialize()
            rvdm.halt()
            otp = OTPController(rvdm, base=0x30130000)
            partition = None
            item = None
            if otpmap:
                otp.set_map(otpmap)
            elif any((getattr(args, a) for a in
                     ('partition', 'item', 'list', 'read', 'write', 'digest'))):
                argparser.error('OTP map is required for this operation')
            if args.partition:
                partname = args.partition.upper()
                try:
                    partition = otpmap.partitions[partname]
                except KeyError:
                    argparser.error(f"Unknown partition '{args.partition}'")
            else:
                partition = None
                partname = None
            if args.item:
                if not partition:
                    argparser.error('Missing partition for selecting item')
                item_name = args.item.upper()
                item = None
                for entry in partition['items']:
                    if entry.get('name', '') == item_name:
                        item = entry
                        break
                else:
                    argparser.error(f"Unknown item '{args.item}")
            else:
                item = None
            if args.digest:
                if partition is None:
                    print('HW digests:')
                    for name, digest in otp.get_hw_partition_digests().items():
                        print(f' * {name:10} 0x{digest:016x}')
                else:
                    digest = otp.get_hw_partition_digest(partname)
                    print(f'0x{digest:016x}')
            if args.list:
                if partition:
                    if not item:
                        print('Items:')
                        for item in partition.get('items', []):
                            name = item['name']
                            print(f' * {name}')
                    else:
                        print(item['name'])
                        for name, value in item.items():
                            if name == 'name':
                                continue
                            print(f' * {name}: {value}')
                else:
                    print('Partitions:')
                    for part in otpmap.enumerate_partitions():
                        print(f' * {part.name}')
            if args.read or args.write:
                if not partition or not partname:
                    argparser.error('Partition is required for this operation')
                if not item:
                    argparser.error('Item is required for this operation')
            if args.read:
                val = otp.read_partition_item(partname, item['name'])
                if isinstance(val, int):
                    pad = ' * value: ' if args.list else ''
                    print(f'{pad}0x{val:x}')
                else:
                    if args.list:
                        out = StringIO()
                        dump_buffer(val, file=out)
                        print(' * value: |')
                        for line in out.getvalue().split('\n'):
                            print(f'   {line}')
                    else:
                        print(hexlify(val).decode())
            if args.write:
                otp.write_partition_item(partname, item['name'], args.write)
        finally:
            if not args.no_quit:
                ctrl.quit()

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
