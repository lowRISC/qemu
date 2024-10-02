#!/usr/bin/env python3

# Copyright (c) 2024, Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Debug Transport Module tiny demo.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from argparse import ArgumentParser, Namespace, FileType
from io import BytesIO
from os import linesep
from os.path import dirname, join as joinpath, normpath
from socket import create_connection, socket, AF_UNIX, SOCK_STREAM
from traceback import format_exc
from typing import Optional
import sys

QEMU_PYPATH = joinpath(dirname(dirname(dirname(normpath(__file__)))),
                       'python', 'qemu')
sys.path.append(QEMU_PYPATH)

from jtag.bitbang import JtagBitbangController
from jtag.bits import BitSequence
from jtag.jtag import JtagEngine

from ot.dm import DebugModule
from ot.dtm import DebugTransportModule
from ot.util.elf import ElfBlob
from ot.util.log import configure_loggers
from ot.util.misc import HexInt, dump_buffer

DEFAULT_IR_LENGTH = 5
"""Default TAP Instruction Register length."""

DEFAULT_DMI_ADDRESS = 0x0
"""Default DMI address of the DM."""


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
    default_host = 'localhost'
    default_port = JtagBitbangController.DEFAULT_PORT
    try:
        args: Optional[Namespace] = None
        argparser = ArgumentParser(
            description=sys.modules[__name__].__doc__.split('.')[0])
        qvm = argparser.add_argument_group(title='Virtual machine')

        qvm.add_argument('-S', '--socket',
                         help=f'unix:path/to/socket or tcp:host:port '
                              f'(default tcp:{default_host}:{default_port})')
        qvm.add_argument('-t', '--terminate', action='store_true',
                         help='terminate QEMU when done')
        dmi = argparser.add_argument_group(title='DMI')
        dmi.add_argument('-l', '--ir-length', type=int,
                         default=DEFAULT_IR_LENGTH,
                         help=f'bit length of the IR register '
                              f'(default: {DEFAULT_IR_LENGTH})')
        dmi.add_argument('-b', '--base', type=HexInt.parse,
                         default=DEFAULT_DMI_ADDRESS,
                         help=f'define DMI base address '
                              f'(default: 0x{DEFAULT_DMI_ADDRESS:x})')
        info = argparser.add_argument_group(title='Info')
        info.add_argument('-I', '--info', action='store_true',
                          help='report JTAG ID code and DTM configuration')
        info.add_argument('-c', '--csr',
                          help='read CSR value from hart')
        info.add_argument('-C', '--csr-check', type=HexInt.parse, default=None,
                          help='check CSR value matches')
        act = argparser.add_argument_group(title='Actions')
        act.add_argument('-x', '--execute', action='store_true',
                         help='update the PC from a loaded ELF file')
        act.add_argument('-X', '--no-exec', action='store_true', default=False,
                         help='does not resume hart execution')
        mem = argparser.add_argument_group(title='Memory')
        mem.add_argument('-a', '--address', type=HexInt.parse,
                         help='address of the first byte to access')
        mem.add_argument('-m', '--mem', choices=('read', 'write'),
                         help='access memory using System Bus')
        mem.add_argument('-s', '--size', type=HexInt.parse,
                         help='size in bytes of memory to access')
        mem.add_argument('-f', '--file',
                         help='file to read/write data for memory access')
        mem.add_argument('-e', '--elf', type=FileType('rb'),
                         help='load ELF file into memory')
        mem.add_argument('-F', '--fast-mode', default=False,
                         action='store_true',
                         help='do not check system bus status while '
                              'transfering')
        extra = argparser.add_argument_group(title='Extras')
        extra.add_argument('-v', '--verbose', action='count',
                           help='increase verbosity')
        extra.add_argument('-d', '--debug', action='store_true',
                           help='enable debug mode')

        args = argparser.parse_args()
        debug = args.debug

        configure_loggers(args.verbose, 'dtm.rvdm', -1, 'dtm', 'jtag')

        try:
            if args.socket:
                socket_type, socket_args = args.socket.split(":", 1)
                if socket_type == "tcp":
                    host, port = socket_args.split(":")
                    sock = create_connection((host, int(port)), timeout=0.5)
                elif socket_type == "unix":
                    sock = socket(AF_UNIX, SOCK_STREAM)
                    sock.connect(socket_args)
                else:
                    raise ValueError(f"Invalid socket type {socket_type}")
            else:
                sock = create_connection((default_host, default_port),
                                         timeout=0.5)
        except Exception as exc:
            raise RuntimeError(f'Cannot connect to {args.socket}: '
                               f'{exc}') from exc
        sock.settimeout(0.1)
        ctrl = JtagBitbangController(sock)
        eng = JtagEngine(ctrl)
        ctrl.tap_reset(True)
        ir_length = args.ir_length
        dtm = DebugTransportModule(eng, ir_length)
        rvdm = None
        try:
            if args.info:
                code = idcode(eng, ir_length)
                print(f'IDCODE:    0x{code:x}')
                version = dtm['dtmcs'].dmi_version
                abits = dtm['dtmcs'].abits
                print(f'DTM:       v{version[0]}.{version[1]}, {abits} bits')
                dtm['dtmcs'].check()
            dtm['dtmcs'].dmireset()
            if args.csr_check is not None and not args.csr:
                argparser.error('CSR check requires CSR option')
            if args.csr:
                if not rvdm:
                    rvdm = DebugModule(dtm, args.base)
                    rvdm.initialize()
                rvdm.halt()
                dmver = rvdm.status['version']
                if args.info:
                    print(f'DM:        {dmver.name}')
                sbver = rvdm.system_bus_info['sbversion']
                if args.info:
                    print(f'SYSBUS:    {sbver.name}')
                if not args.csr:
                    csr = 'misa'
                else:
                    try:
                        csr = HexInt.parse(args.csr)
                    except ValueError:
                        csr = args.csr
                csr_val = rvdm.read_csr(csr)
                if not args.no_exec:
                    rvdm.resume()
                if args.csr_check is not None:
                    if csr_val != args.csr_check:
                        raise RuntimeError(f'CSR {args.csr} check failed: '
                                           f'0x{csr_val:08x} != '
                                           f'0x{args.csr_check:08x}')
                else:
                    pad = ' ' * (10 - len(args.csr))
                    print(f'{args.csr}:{pad}0x{csr_val:08x}')
            if args.mem:
                if args.address is None:
                    argparser.error('no address specified for memory operation')
                if args.mem == 'write' and not args.file:
                    argparser.error('no file specified for mem write operation')
                if args.mem == 'read' and not args.size:
                    argparser.error('no size specified for mem read operation')
                if not rvdm:
                    rvdm = DebugModule(dtm, args.base)
                    rvdm.initialize()
                if args.file:
                    mode = 'rb' if args.mem == 'write' else 'wb'
                    with open(args.file, mode) as mfp:
                        rvdm.memory_copy(mfp, args.mem, args.address,
                                         args.size, no_check=args.fast_mode)
                else:
                    mfp = BytesIO()
                    rvdm.memory_copy(mfp, args.mem, args.address, args.size,
                                     no_check=args.fast_mode)
                    dump_buffer(mfp, args.address)
            if args.elf:
                if ElfBlob.ELF_ERROR:
                    argparser.error('pyelftools module not available')
                elf = ElfBlob()
                elf.load(args.elf)
                args.elf.close()
                if elf.address_size != 32:
                    argparser.error('Only ELF32 files are supported')
                if not rvdm:
                    rvdm = DebugModule(dtm, args.base)
                    rvdm.initialize()
                try:
                    rvdm.halt()
                    mfp = BytesIO(elf.blob)
                    rvdm.memory_copy(mfp, 'write', elf.load_address, args.size,
                                     no_check=args.fast_mode)
                    if args.execute:
                        rvdm.set_pc(elf.entry_point)
                finally:
                    if args.execute or not args.no_exec:
                        rvdm.resume()
            else:
                if args.execute:
                    argparser.error('Cannot execute without loaded an ELF file')
        finally:
            if args.terminate:
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
