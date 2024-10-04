#!/usr/bin/env python3

# Copyright (c) 2023-2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""QEMU GDB replay.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from argparse import ArgumentParser, FileType, Namespace
from binascii import hexlify
from logging import getLogger
from os import linesep
from os.path import dirname, isfile, join as joinpath, normpath
from socket import (SOL_SOCKET, SO_REUSEADDR, SHUT_RDWR, socket,
                    timeout as LegacyTimeoutError)
from string import ascii_uppercase
from traceback import format_exc
from typing import BinaryIO, Optional, TextIO
import re
import sys

QEMU_PYPATH = joinpath(dirname(dirname(dirname(normpath(__file__)))),
                       'python', 'qemu')
sys.path.append(QEMU_PYPATH)

from ot.util.elf import ElfBlob
from ot.util.log import configure_loggers


class QEMUMemoryController:
    """Memory controller.

       Store known memory content in banks.
    """

    def __init__(self):
        self._log = getLogger('gdbrp.mem')
        self._banks: dict[tuple(int, int), bytes] = {}

    def add_memory(self, addr: int, blob: bytes) -> None:
        """Add a new memory bank.

           :param addr: absolute address of the first memory cell
           :param blob: memory content
        """
        length = len(blob)
        rng = range(addr, addr+length)
        self._banks[rng] = blob

    def read(self, addr: int, length: int) -> bytes:
        """Read the content of a memory region.

           Note that the returned memory blob may be shorter than the requested
           length (to match GDB behaviour)

           :param addr: absolute address of the first cell to read out
           :param length: count of bytes to read
           :return: the memory blob as a byte sequence
           :raise IndexError: if the selected address is not mapped
        """
        for rng, data in self._banks.items():
            if addr not in rng:
                continue
            offset = addr-rng.start
            self._log.debug('Mem section [%08x..%08x]', rng.start, rng.stop)
            return bytes(data[offset:offset+length])
        raise IndexError('Invalid memory address')


class QEMUVCPU:
    """Virtual CPU storage.

       :param memctrl: memory controller
    """

    # pylint: disable=invalid-name

    PC_XPOS = 32
    """Index of the PC in the GPR sequence, starting from x0."""

    def __init__(self, memctrl: QEMUMemoryController):
        self._log = getLogger('gdbrp.vcpu')
        self._seq: list[tuple[int, str]] = []
        self._regs: list[Optional[int]] = [None] * 33
        self._xpos = 0
        self._memctrl = memctrl
        self._hwbreaks: list[range] = []

    def record(self, pc: int, func: Optional[str]) -> None:
        """Record execution of a single instruction.

           :param pc: the address of the instruction
           :param func: the name of the executed function, if any
        """
        self._seq.append((pc, func))

    def reset(self) -> None:
        """Restart the execution of the vCPU to the very first registered
           instruction.
        """
        self._xpos = 0

    def step(self, back: bool = False) -> None:
        """Advance a single vCPU instruction.

           :param back: whether to step back or next (default)
           :raise RuntimeError: if there are not more recorded instructions to
                                'execute'
        """
        if not back:
            self._xpos += 1
            if self._xpos >= len(self._seq):
                raise RuntimeError('Reached end of exec stream')
        else:
            if self._xpos > 0:
                self._xpos -= 1
            else:
                raise RuntimeError('Reached start of exec stream')

    def cont(self, back: bool = False, addr: Optional[int] = None) -> bool:
        """Continue execution of instruction stream till either a HW breakpoint
           or the end of the execution stream is reached.

           :param back: wether to execute backward or forward (default)
           :param addr: the address to resume from. A PC following the current
                        execution matching this address is only looked up in
                        the selected direction
           :return: True if a HW breakpoint has been reached, False otherwise
        """

        if addr is not None:
            try:
                self._move_to(addr, not back)
            except ValueError:
                self._log.warning('Cannot resume from 0x%08x', addr)
                self._xpos = len(self._seq)
        last_pc = None
        while True:
            try:
                self.step(back)
            except RuntimeError as exc:
                self._log.warning('%s', exc)
                break
            _ = self.instruction_length
            pc = self.pc
            if pc == last_pc:
                continue
            last_pc = pc
            for hwp, hwb in enumerate(self._hwbreaks, start=1):
                if pc in hwb:
                    self._log.info('Breakpoint #%d @ %08x', hwp, pc)
                    return True
        return False

    def add_hw_break(self, addr: int, length: int):
        """Add a HW breakpoint.

           :param addr: absolute address of the HW breakpoint
           :param length: count of bytes starting from the address
           :raise ValueError: if another breakpoint exists at this location
        """
        rng = range(addr, addr+length)
        if rng in self._hwbreaks:
            raise ValueError('Duplicate breakpoint')
        self._hwbreaks.append(rng)
        self._log.info('Add HW breakpoint on [%08x:%08x[', rng.start, rng.stop)

    def del_hw_break(self, addr: int, length: int):
        """Remove an existing HW breakpoint.

           :param addr: absolute address of the HW breakpoint
           :param length: count of bytes starting from the address
           :raise ValueError: if no breakpoint exists at this location
        """
        rng = range(addr, addr+length)
        try:
            self._hwbreaks.remove(rng)
            self._log.info('Remove HW breakpoint from [%08x:%08x[',
                           rng.start, rng.stop)
        except ValueError as exc:
            raise ValueError('Non-existent breakpoint') from exc

    @property
    def instruction_count(self) -> int:
        """Return the count of recorded instructions.

           :return: the count of recorded instructions.
        """
        return len(self._seq)

    @property
    def instruction_length(self) -> int:
        """Return the length of the current instruction.

           :return: the length of the instruction (either 2 or 4)
        """
        return self._get_instruction_length(self.pc)

    @property
    def pc(self) -> int:
        """Return the current instruction.

           Note that the instruction may not be valid if execution point has
           reached past the point of the latest recorded instruction.

           :return: the instruction
        """
        if self._xpos < len(self._seq):
            return self._seq[self._xpos][0]
        last_pc = self._seq[-1][0]
        return last_pc + self._get_instruction_length(last_pc)

    @property
    def regs(self) -> list[int]:
        """Return the values of the GPRs.

           This is a requirement of GDB.

           There are all set to 0 for now, except the PC.
        """
        self._regs[-1] = self.pc
        return self._regs

    def _move_to(self, pc: int, forward: bool) -> None:
        """Change execution point to the selected PC.

           Note that there are many execution points that may contain the
           selected PC in the execution stream, this function only reaches the
           closest one toward the selected execution direction.

           :param pc: the PC address to locate and execution point to
           :param foward: whether to look forward or backward the current
                          execution point
           :raise ValueError: if no such PC is found
        """
        pos = self._xpos
        if forward:
            while pos < len(self._seq):
                if self._seq[pos][0] == pc:
                    self._xpos = pos
                    self._log.info('Forward to PC %08x @ %d', pc, pos)
                    return
                pos += 1
        else:
            while pos >= 0:
                if self._seq[pos][0] == pc:
                    self._xpos = pos
                    self._log.info('Rewind to PC %08x @ %d', pc, pos)
                    return
                pos -= 1
        raise ValueError(f'No such address: 0x{pc:08x}')

    def _get_instruction_length(self, pc: int) -> int:
        """Return the length of the instruction at the specified address.

           Note that if the selected address is not found, the default
           instruction size (4) is returned.

           :return: the length of the instruction (either 2 or 4)
        """
        try:
            instr = int.from_bytes(self._memctrl.read(pc, 4), 'little')
        except IndexError:
            self._log.error('Invalid PC @ %d', self._xpos)
            return 4
        length = 4 if instr & 0x3 else 2
        opcode = f'{instr:08x}' if length == 4 else f'{instr & 0xffff:04x}'
        self._log.info('Instruction @ 0x%08x: %s', pc, opcode)
        return length


class QEMUGDBReplay:
    """Tiny GDB server that replays a logged QEMU guest execution stream.

       :param rv64: RV64 vs. RV32
    """

    DEFAULT_SERVICE = 'localhost:3333'
    """Default TCP host:port to serve GDB remote clients."""

    MAX_PACKET_LENGTH = 4096
    """Maximum packet size."""

    # Trace 0: 0x280003d00 [00000000/00008c9a/00101003/ff020000] _boot_start
    TCRE = re.compile(r'^Trace\s(\d+):\s0x[0-9a-f]+\s\[[0-9a-f]+/([0-9a-f]+)'
                      r'/[0-9a-f]+/[0-9a-f]+\](?:\s([&,<>\s\w:]+))?\s*$')
    """Regex to parse QEMU execution trace from a QEMU log file."""

    SIGNALS = {
        'HUP': 1,
        'INT': 2,
        'QUIT': 3,
        'ILL': 4,
        'TRAP': 5,
        'ABRT': 6
    }
    """GDB signals."""

    def __init__(self, rv64: Optional[bool] = None):
        self._log = getLogger('gdbrp')
        self._rv64 = rv64
        self._xlen = 8 if rv64 else 4
        self._thread_id: Optional[int] = None
        self._select_thread_id: Optional[int] = None
        self._conn: Optional[socket] = None
        self._no_ack = False
        self._vcpus: dict[int, QEMUVCPU] = {}
        self._memctrl = QEMUMemoryController()

    @property
    def xlen(self) -> int:
        """Return current XLEN, default to RV32.

           :return: xlen in bytes
        """
        return self._xlen or 4

    def load(self, qfp: TextIO, cpus: Optional[list[int]] = None) -> None:
        """Load a recorded execution stream from a QEMU log file.
           see QEMU `-d exec` option.

           :param qfp: text stream to parse
        """
        threshold = 10
        for lno, line in enumerate(qfp, start=1):
            tmo = self.TCRE.match(line)
            if not tmo:
                continue
            scpu, spc, func = tmo.groups()
            if lno > threshold:
                threshold *= 10
            if not lno % threshold:
                self._log.debug('Parsed %d lines', lno)
            xcpu = int(scpu)
            if cpus and xcpu not in cpus:
                continue
            xpc = int(spc, 16)
            if xcpu not in self._vcpus:
                self._vcpus[xcpu] = QEMUVCPU(self._memctrl)
            self._vcpus[xcpu].record(xpc, func)
        qfp.close()
        if not self._vcpus:
            raise RuntimeError('Unable to find QEMU execution traces')
        for vix, vcpu in self._vcpus.items():
            self._log.info('CPU %d: %d instructions',
                           vix, vcpu.instruction_count)
            vcpu.reset()

    def load_bin(self, address: int, bfp: BinaryIO) -> None:
        """Load an application from a binary stream.

           :param address: the address of the first byte in memory
           :param bfp: the binary stream
        """
        blob = bfp.read()
        # note: for now, overlapping memories are not managed
        self._memctrl.add_memory(address, blob)

    def load_elf(self, elf: BinaryIO) -> None:
        """Load an application from a ELF stream.

           :param elf: the ELF stream
        """
        blob = ElfBlob()
        blob.load(elf)
        if not self._xlen:
            self._xlen = blob.address_size // 8
        elif blob.address_size != self._xlen * 8:
            raise ValueError('Wrong ELF file type (XLEN)')
        # note: for now, overlapping memories are not managed
        self._memctrl.add_memory(blob.load_address, blob.blob)

    def serve(self, gdb: str) -> None:
        """Serve GDB client requests.

           :param gdb: TCP host:port description string.
        """
        devdesc = gdb.split(':')
        try:
            port = int(devdesc[1])
        except TypeError as exc:
            raise ValueError('Invalid TCP serial device') from exc
        if not 0 < port < 65536:
            raise ValueError('Invalid TCP port')
        tcpdev = (devdesc[0], port)
        gdbs = socket()
        gdbs.setsockopt(SOL_SOCKET, SO_REUSEADDR, 1)
        gdbs.bind(tcpdev)
        while True:
            gdbs.listen()
            self._conn, peer = gdbs.accept()
            self._log.info('Remote connection from %s:%d', *peer)
            with self._conn:
                self._conn.settimeout(0.1)
                try:
                    self._serve()
                except OSError:
                    break
            self._conn = None

    def _serve(self):
        """Serve GDB request from a single client/connection.
        """
        buf = bytearray()
        while self._conn:
            try:
                data = self._conn.recv(self.MAX_PACKET_LENGTH)
            except (TimeoutError, LegacyTimeoutError):
                continue
            if not data:
                continue
            buf.extend(data)
            start = buf.find(b'$')
            if start < 0:
                continue
            end = buf.find(b'#', start)
            if end < 0:
                continue
            if len(buf)-end < 2:
                continue
            req = buf[start+1:end]
            crc = int(buf[end+1:end+3], 16)
            buf = req[end+3:]
            self._log.info('Request %s', bytes(req))
            if not self._no_ack:
                if sum(req) & 0xff != crc:
                    self._log.error('Invalid CRC')
                    self._conn.send(b'-')
                else:
                    self._conn.send(b'+')
            self._handle_request(req)

    def _handle_request(self, req: bytearray):
        """Dispatch incoming GDB request.

           :param req: the byte request received from the remote client.
        """
        for clen in 1, 2:
            cmd = bytes(req[0:clen]).decode()
            cmd = {'?': 'interrogate'}.get(cmd, cmd)
            if cmd in ascii_uppercase:
                cmd = f'_{cmd.lower()}'
            handler = getattr(self, f'_do_{cmd}', None)
            if handler:
                break
        else:
            self._send('')
            return
        # pylint: disable=not-callable
        resp = handler(bytes(req[clen:]))
        if resp is not None:
            self._send(resp)

    def _send(self, payload: str):
        """Send a reply to the remote GDB client.

           :param payload: the string to send
        """
        self._log.info('Reply: "%s"', payload)
        self._send_bytes(payload.encode())

    def _send_bytes(self, payload: [bytes | bytearray]):
        """Send a reply to the remote GDB client.

           :param payload: the byte sequence to send
        """
        if not isinstance(payload, (bytes, bytearray)):
            raise TypeError('Invalid payload type')
        if self._conn:
            crc = sum(payload) & 0xff
            bcrc = f'{crc:02x}'.encode()
            resp = b''.join((b'$', payload, b'#', bcrc))
            self._log.debug('< %s', resp)
            self._conn.send(resp)

    def _do_interrogate(self, *_) -> str:
        """Query current status."""
        return 'S00'

    def _do_bc(self, *_: bytes):
        """Continue backward."""
        vcpu = self._vcpus[self._thread_id]
        hwbreak = vcpu.cont(True)
        if hwbreak:
            xpc = vcpu.pc
            sig = self.SIGNALS['TRAP']
            spc = hexlify(xpc.to_bytes(self.xlen, 'little')).decode()
            return f'T{sig:02x}{vcpu.PC_XPOS:02x}:{spc};hwbreak:;'
        # reached start of program
        return 'S00'  # not sure about the expected code

    def _do_c(self, payload: bytes):
        """Continue."""
        addr = int(payload) if payload else None
        vcpu = self._vcpus[self._thread_id]
        hwbreak = vcpu.cont(False, addr)
        if hwbreak:
            xpc = vcpu.pc
            sig = self.SIGNALS['TRAP']
            spc = hexlify(xpc.to_bytes(self.xlen, 'little')).decode()
            return f'T{sig:02x}{vcpu.PC_XPOS:02x}:{spc};hwbreak:;'
        # reached end of program
        sig = self.SIGNALS['QUIT']
        return f'S{sig:02x}'

    def _do_g(self, *_):
        """Get vCPU register values."""
        # "The bytes with the register are transmitted in target byte order"
        vcpu = self._vcpus[self._thread_id]
        return ''.join(hexlify(r.to_bytes(self.xlen, 'little')).decode()
                       if r is not None else 'xx' * self.xlen
                       for r in vcpu.regs)

    def _do_k(self, *_):
        """Kill."""
        for vcpu in self._vcpus.values():
            vcpu.reset()
        self._conn.shutdown(SHUT_RDWR)
        self._conn.close()
        self._conn = None

    def _do_q(self, payload: bytes):
        """Generic query decoder."""
        parts = payload.split(b':', 1)
        handler = getattr(self, f'_do_query_{parts[0].decode().lower()}', None)
        if not handler:
            return ''
        # pylint: disable=not-callable
        return handler(bytes(parts[1]) if len(parts) > 1 else b'')

    def _do_m(self, payload: bytes) -> str:
        """Read memory."""
        addr, length = (int(x, 16) for x in payload.decode().split(',', 1))
        self._log.info('Read mem [%08x..%08x]', addr, addr+length)
        try:
            data = self._memctrl.read(addr, length)
        except IndexError:
            return 'E01'
        return hexlify(data).decode()

    def _do_s(self, payload: bytes) -> str:
        """Step instruction."""
        vcpu = self._vcpus[self._thread_id]
        if not payload:
            vcpu.step()
            addr = vcpu.pc
            haddr = hexlify(addr.to_bytes(self.xlen, 'little')).decode()
            sig = self.SIGNALS['TRAP']
            return f'T{sig:02x}{vcpu.PC_XPOS:02x}:{haddr};'
        # for now there is no way to jump to another PC location
        return 'E01'

    def _do_bs(self, *_: bytes) -> str:
        """Step back instruction."""
        vcpu = self._vcpus[self._thread_id]
        vcpu.step(True)
        addr = vcpu.pc
        haddr = hexlify(addr.to_bytes(self.xlen, 'little')).decode()
        sig = self.SIGNALS['TRAP']
        return f'T{sig:02x}{vcpu.PC_XPOS:02x}:{haddr};'

    def _do__h(self, payload: bytes) -> str:
        """Select thread."""
        cmd = payload[0:1]
        if cmd in b'GgMmc':
            try:
                tid = int(payload[1:], 16)
            except (TypeError, ValueError):
                self._log.error('Unsupported thread id %s', payload[1:])
                return 'E01'
            if tid != -1 and tid not in self._vcpus:
                self._log.warning('Unknown thread id %d ignored', tid)
                return 'E02'
            self._select_thread_id = tid
            if tid <= 0:
                self._thread_id = sorted(self._vcpus)[0]
            return 'OK'
        return 'E03'

    def _do_z(self, payload: bytes) -> str:
        """Remove breakpoint."""
        return self._do__z(payload, True)

    def _do__z(self, payload: bytes, remove=False) -> str:
        """Add breakpoint."""
        parts = payload.split(b';')
        if len(parts) > 1:
            self._log.warning('Conditional breakpoint not supported')
            return ''
        try:
            kind, addr, length = (int(x, 16) for x in parts[0].split(b','))
        except ValueError:
            self._log.error('Invalid debug request')
            return 'E01'
        if kind != 1:
            self._log.warning('Only hw breakpoint are supported')
            return ''
        vcpu = self._vcpus[self._thread_id]
        try:
            if not remove:
                vcpu.add_hw_break(addr, length)
            else:
                vcpu.del_hw_break(addr, length)
            return 'OK'
        except ValueError:
            return 'E02'

    def _do_query_c(self, *_) -> str:
        return f'QC{self._thread_id}'

    def _do_query_supported(self, payload: bytes) -> str:
        """Query supported features."""
        req = payload.decode()
        resp = [f'PacketSize={self.MAX_PACKET_LENGTH-16:x}',
                'ReverseStep+', 'ReverseContinue+']
        for cap in req.split(';'):
            supp = cap in ('hwbreak+')
            self._log.info('Query support for %s: %s',
                           cap, 'Y' if supp else 'N')
            if supp:
                resp.append(cap)
        return ';'.join(resp)

    def _do_query_symbol(self, *_) -> str:
        """Target does not need to lookup symbol."""
        return 'OK'

    def _do_query_tstatus(self, *_) -> str:
        """Query stop reason."""
        return 'T0;tnotrun:0'

    def _do_query_fthreadinfo(self, *_) -> str:
        """Query thread id, i.e. HW vCPUs identifiers."""
        resp = ';'.join((f'{x:x}' for x in sorted(self._vcpus)))
        return f'm{resp}'

    def _do_query_sthreadinfo(self, *_) -> str:
        """End of thread query."""
        return 'l'

    def _do_query_attached(self, *_) -> str:
        """Query program status."""
        return '0'  # or '1' ?


def main():
    """Main routine"""
    debug = True
    qemu_path = normpath(joinpath(dirname(dirname(dirname(normpath(__file__)))),
                                  'build', 'qemu-system-riscv32'))
    if not isfile(qemu_path):
        qemu_path = None
    try:
        args: Optional[Namespace] = None
        desc = sys.modules[__name__].__doc__.split('.', 1)[0].strip()
        argparser = ArgumentParser(description=f'{desc}.')
        argparser.add_argument('-t', '--trace', metavar='LOG',
                               type=FileType('rt'),
                               help='QEMU execution trace log')
        argparser.add_argument('-e', '--elf', action='append',
                               type=FileType('rb'),
                               help='ELF application')
        argparser.add_argument('-c', '--cpu', action='append', type=int,
                               help='Only consider selected CPUs')
        argparser.add_argument('-a', '--address', action='append',
                               type=lambda x: int(x, 16 if x[1:2].lower() == 'x'
                                                  else 10),
                               help='Address to load each specified binary')
        argparser.add_argument('-b', '--bin', action='append',
                               type=FileType('rb'),
                               help='Binary application')
        argparser.add_argument('-g', '--gdb',
                               default=QEMUGDBReplay.DEFAULT_SERVICE,
                               help=f'GDB server '
                               f'(default to {QEMUGDBReplay.DEFAULT_SERVICE})')
        argparser.add_argument('-v', '--verbose', action='count',
                               help='increase verbosity')
        argparser.add_argument('-d', '--debug', action='store_true',
                               help='enable debug mode')
        args = argparser.parse_args()
        debug = args.debug

        configure_loggers(args.verbose, 'gdbrp', 'elf')

        acount = len(args.address or [])
        bcount = len(args.bin or [])
        if acount != bcount:
            argparser.error('Expecting same count of address and bin args')

        gdbr = QEMUGDBReplay()
        if args.elf:
            if ElfBlob.ELF_ERROR:
                argparser.error('Please install PyElfTools package')
            for elf in args.elf:
                gdbr.load_elf(elf)
        if args.bin:
            for addr, blob in zip(args.address, args.bin):
                gdbr.load_bin(addr, blob)
        if args.trace:
            gdbr.load(args.trace, args.cpu)

        gdbr.serve(args.gdb)

        sys.exit(0)
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
