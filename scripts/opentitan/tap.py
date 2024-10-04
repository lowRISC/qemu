#!/usr/bin/env python3

"""JTAG TAP controller tiny simulator.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

# Copyright (c) 2022-2024 Rivos, Inc.
# All rights reserved.

from argparse import ArgumentParser
from enum import IntEnum
from logging import getLogger
from os.path import dirname, join as joinpath, normpath
from socket import create_server, socket, SHUT_RDWR
from traceback import format_exc
from typing import Optional
import sys

QEMU_PYPATH = joinpath(dirname(dirname(dirname(normpath(__file__)))),
                       'python', 'qemu')
sys.path.append(QEMU_PYPATH)

from ot.util.log import configure_loggers

# pylint: disable-msg=missing-function-docstring,empty-docstring
# pylint: disable-msg=too-few-public-methods
# pylint: disable-msg=too-many-instance-attributes


class TAPState(IntEnum):
    """
    """
    TEST_LOGIC_RESET = 0
    RUN_TEST_IDLE = 1
    SELECT_DR_SCAN = 2
    CAPTURE_DR = 3
    SHIFT_DR = 4
    EXIT1_DR = 5
    PAUSE_DR = 6
    EXIT2_DR = 7
    UPDATE_DR = 8
    SELECT_IR_SCAN = 9
    CAPTURE_IR = 10
    SHIFT_IR = 11
    EXIT1_IR = 12
    PAUSE_IR = 13
    EXIT2_IR = 14
    UPDATE_IR = 15


class TAPRegister:
    """
    """

    def __init__(self, name: str, length: int, reset: int):
        self._log = getLogger(f'tap.{name}')
        self._length = length
        self._reset = reset
        self._reg = self._reset

    @property
    def value(self) -> int:
        return self._reg

    def binstr(self, val: int) -> str:
        return f'[{val:0{self._length}b}]'

    def reset(self):
        self._reg = self._reset

    def capture(self):
        self._reg = self._reset
        self._log.info('capture %s', self.binstr(self._reg))

    def shift(self, tdi: bool):
        self._reg >>= 1
        self._reg |= int(tdi) << (self._length-1)
        self._log.debug('tdi:%u -> %s', tdi, self.binstr(self._reg))

    def update(self):
        self._log.info('update %s', self.binstr(self._reg))


class TAP_BYPASS(TAPRegister):
    """
    """

    # pylint: disable-msg=invalid-name

    def __init__(self):
        super().__init__('bypass', 1, 0)


class TAP_IDCODE(TAPRegister):
    """
    """

    # pylint: disable-msg=invalid-name

    def __init__(self, idcode: int):
        super().__init__('idcode', 32, idcode)


class TAP_DTMCS(TAPRegister):
    """
    """

    # pylint: disable-msg=invalid-name

    IDLE_CYCLE = 0
    VERSION = 1  # RISC-V Debug 0.13

    # 31..18      17         16    15  14..12  11..10   9..4   3..0
    #   0    dmihardreset dmireset  0   idle  dmistat  abits  version
    #  14         W:1        W:1    1   R:3     R:2     R:6    R:4

    def __init__(self, abits: int, dmi: 'TAP_DMI'):
        super().__init__('dtmcs', 32, (self.VERSION << 0) | (abits << 4))
        self._log = getLogger('tap.dtmcs')
        self._dmi = dmi
        self._dmistat = 0

    def set_error_failed(self):
        self._log.warning('Failure')
        self._dmistat = 0b10

    def set_error_busy(self):
        self._log.warning('Busy')
        self._dmistat = 0b11

    def capture(self):
        self._reg = self._reset | (self._dmistat << 10)

    def update(self):
        self._log.debug('%s', self.binstr(self._reg))
        dmireset = bool((self._reg >> 16) & 0xb1)
        dmihardreset = bool((self._reg >> 17) & 0xb1)
        if dmireset:
            self._log.warning('DMI reset')
            self._dmistat = 0
        if dmihardreset:
            self._log.warning('DMI hard reset')
            self._dmi.hardreset()


class TAPController:
    """
    """

    STATES = {
        TAPState.TEST_LOGIC_RESET:
            (TAPState.RUN_TEST_IDLE, TAPState.TEST_LOGIC_RESET),
        TAPState.RUN_TEST_IDLE:
            (TAPState.RUN_TEST_IDLE, TAPState.SELECT_DR_SCAN),
        TAPState.SELECT_DR_SCAN:
            (TAPState.CAPTURE_DR, TAPState.SELECT_IR_SCAN),
        TAPState.CAPTURE_DR:
            (TAPState.SHIFT_DR, TAPState.EXIT1_DR),
        TAPState.SHIFT_DR:
            (TAPState.SHIFT_DR, TAPState.EXIT1_DR),
        TAPState.EXIT1_DR:
            (TAPState.PAUSE_DR, TAPState.UPDATE_DR),
        TAPState.PAUSE_DR:
            (TAPState.PAUSE_DR, TAPState.EXIT2_DR),
        TAPState.EXIT2_DR:
            (TAPState.SHIFT_DR, TAPState.UPDATE_DR),
        TAPState.UPDATE_DR:
            (TAPState.RUN_TEST_IDLE, TAPState.SELECT_DR_SCAN),
        TAPState.SELECT_IR_SCAN:
            (TAPState.CAPTURE_IR, TAPState.TEST_LOGIC_RESET),
        TAPState.CAPTURE_IR:
            (TAPState.SHIFT_IR, TAPState.EXIT1_IR),
        TAPState.SHIFT_IR:
            (TAPState.SHIFT_IR, TAPState.EXIT1_IR),
        TAPState.EXIT1_IR:
            (TAPState.PAUSE_IR, TAPState.UPDATE_IR),
        TAPState.PAUSE_IR:
            (TAPState.PAUSE_IR, TAPState.EXIT2_IR),
        TAPState.EXIT2_IR:
            (TAPState.SHIFT_IR, TAPState.UPDATE_IR),
        TAPState.UPDATE_IR:
            (TAPState.RUN_TEST_IDLE, TAPState.SELECT_DR_SCAN)
    }

    def __init__(self, irlength: int, ext: Optional['TAPExtension'] = None):
        self._log = getLogger('tap.ctrl')
        self._trst = False
        self._srst = False
        self._tck = False
        self._tms = False
        self._tdi = False
        self._tdo = False
        self._ext = ext
        self._ir = TAPRegister('ir', irlength, 0b01)
        self._dr: Optional[TAPRegister] = None
        self._state = TAPState.RUN_TEST_IDLE
        self._bypass = TAP_BYPASS()
        self._idcode = TAP_IDCODE(0x04f5484d)
        self._reset()

    def _next(self, tms: bool) -> TAPState:
        self._state = self.STATES[self._state][int(tms)]
        return self._state

    def _reset(self):
        self._ir.reset()
        if self._dr:
            self._dr.reset()

    def _step(self, tck: bool, tms: bool, tdi: bool):
        if not self._tck and tck:
            # Clock rising edge
            if self._state == TAPState.SHIFT_IR:
                self._ir.shift(self._tdi)
            elif self._state == TAPState.SHIFT_DR:
                self._dr.shift(self._tdi)
            old = self._state
            new = self._next(self._tms)
            self._log.debug('State %s -> %s', old, new)
        else:
            # Clock falling edge
            if self._state == TAPState.RUN_TEST_IDLE:
                pass
            elif self._state == TAPState.TEST_LOGIC_RESET:
                self._reset()
            elif self._state == TAPState.CAPTURE_DR:
                self._capture_dr(self._ir.value)
            elif self._state == TAPState.SHIFT_DR:
                self._tdo = self._dr.value & 0b1
            elif self._state == TAPState.UPDATE_DR:
                self._dr.update()
            elif self._state == TAPState.SHIFT_IR:
                self._tdo = self._ir.value & 0b1
            elif self._state == TAPState.UPDATE_IR:
                self._ir.update()
        self._tck = tck
        self._tdi = tdi
        self._tms = tms

    def _capture_dr(self, value: int):
        old_dr = self._dr
        if value in (0x00, 0x1f):
            self._dr = self._bypass
        elif value == 0x01:
            self._dr = self._idcode
        else:
            if self._ext:
                dreg = self._ext.get_by_value(value)
                if not dreg:
                    raise ValueError('Unknown ID code')
                self._dr = dreg
        self._dr.capture()
        if self._dr != old_dr:
            self._log.info('Select DR %s', self._dr.__class__.__name__)


class BitBangController(TAPController):
    """
    """

    BB_MAP = {
        b'B': ('blink', (True,)),
        b'b': ('blink', (False,)),
        b'R': ('read', (),),
        b'Q': ('quit', (),),
        b'0': ('write', (False, False, False)),
        b'1': ('write', (False, False, True)),
        b'2': ('write', (False, True, False)),
        b'3': ('write', (False, True, True)),
        b'4': ('write', (True, False, False)),
        b'5': ('write', (True, False, True)),
        b'6': ('write', (True, True, False)),
        b'7': ('write', (True, True, True)),
        b'r': ('reset', (False, False)),
        b's': ('reset', (False, True)),
        b't': ('reset', (True, False)),
        b'u': ('reset', (True, True)),
    }

    IR_LENGTH = 0x5

    def __init__(self, *args, **kwargs):
        super().__init__(self.IR_LENGTH, *args, **kwargs)
        self._socket: Optional[socket] = None
        self._resume = False
        self._reset()

    def run(self, port: int):
        self._socket = create_server(('localhost', port), reuse_port=True)
        while True:
            self._socket.listen(0)
            self._resume = True
            while self._resume:
                peer, addr = self._socket.accept()
                with peer:
                    self._log.info('Connection from %s:%d', *addr)
                    while True:
                        req = peer.recv(1)
                        if not req:
                            break
                        resp = self._inject(req)
                        if resp is not None:
                            out = bytes((0x30 + int(resp),))
                            self._log.debug('Out %s', out)
                            peer.send(out)
        self._socket.shutdown(SHUT_RDWR)
        self._socket = None

    def _inject(self, req: bytes) -> Optional[bool]:
        self._log.debug('[%s]', req.decode())
        try:
            command, args = self.BB_MAP[req]
        except KeyError:
            self._log.error('Unknown input [%s]', req)
            return None
        handler = getattr(self, f'_inject_{command}', None)
        if handler is None:
            self._log.warning('Unimplemented handler for %s', command)
            return None
        # pylint: disable=not-callable
        return handler(*args)

    def _inject_quit(self):
        self._log.info('Quit')
        self._resume = False

    def _inject_blink(self, enable: bool):
        self._log.debug('Blink %u', enable)

    def _inject_reset(self, trst: bool, srst: bool):
        if trst != self._trst:
            self._log.info('TAP reset %u', trst)
            self._reset()
        if trst != self._trst:
            self._log.info('System reset %u', srst)
        self._trst, self._srst = trst, srst

    def _inject_read(self) -> bool:
        self._log.debug('TDO %u', self._tdo)
        return self._tdo

    def _inject_write(self, tck: bool, tms: bool, tdi: bool):
        self._log.debug('TCK %u TMS %u TDI %u', tck, tms, tdi)
        self._step(tck, tms, tdi)


class GPR(IntEnum):
    """
    """

    # pylint: disable-msg=invalid-name

    zero = 0
    ra = 1
    sp = 2
    gp = 3
    tp = 4
    t0 = 5
    t1 = 6
    t2 = 7
    fp = 8
    s1 = 9
    a0 = 10
    a1 = 11
    a2 = 12
    a3 = 13
    a4 = 14
    a5 = 15
    a6 = 16
    a7 = 17
    s2 = 18
    s3 = 19
    s4 = 20
    s5 = 21
    s6 = 22
    s7 = 23
    s8 = 24
    s9 = 25
    s10 = 26
    s11 = 27
    t3 = 28
    t4 = 29
    t5 = 30
    t6 = 31


class CSR(IntEnum):
    """
    """

    MISA = 0x301


def isa(ext: str):
    """Create MISA word's bit from a letter
    """
    return 1 << (ord(ext[0].upper())-ord('A'))


class TAP_DMI(TAPRegister):
    """
    See https://github.com/lowRISC/opentitan/blob/master/hw/vendor/
               pulp_riscv_dbg/doc/debug-system.md
    """

    # pylint: disable-msg=invalid-name

    class CmdErr(IntEnum):
        """
        """
        NONE = 0
        BUSY = 1
        NOT_SUPPORTED = 2
        EXCEPTION = 3
        HALT_RESUME = 4
        BUS = 5
        OTHER = 7

    DM_REGISTERS = {
        0x10: 'dmcontrol',
        0x11: 'dmstatus',
        0x12: 'hartinfo',
        0x16: 'abstractcs',
        0x17: 'command',
        0x38: 'sbcs'
    }

    DM_NAMES = {v: k for k, v in DM_REGISTERS.items()}

    DATABASE = 0x4
    PROGBUFBASE = 0x20

    DATACOUNT = 10
    PROGBUFSIZE = 8

    # RV32IMC
    RV32 = 0b01 << 30
    MISA = RV32 | isa('I') | isa('M') | isa('C')

    def __init__(self, abits: int):
        super().__init__('dmi', 32 + 2 + abits, 0)
        self._abits = abits
        self._addr = 0
        self._registers = {reg: 0 for reg in self.DM_REGISTERS}
        self._is_halted = False
        self._is_resumed = False
        self._sb_error = 0
        self._data = [0] * self.DATACOUNT
        self._progbuf = [0] * self.PROGBUFSIZE
        self._cmderr = TAP_DMI.CmdErr.NONE

    def hardreset(self):
        self._log.warning('HARD RESET')

    def capture(self):
        addr = self._addr
        regname = self.DM_REGISTERS.get(addr)
        if 0 <= addr-self.DATABASE < self.DATACOUNT:
            data = self._data[addr-self.DATABASE]
        elif 0 <= addr-self.PROGBUFBASE < self.PROGBUFSIZE:
            data = self._progbuf[addr-self.PROGBUFBASE]
        else:
            data = self._registers.get(addr, 0)
        self._reg = addr << (32 + 2) | (data << 2)
        self._log.info('%s @ 0x%02x = 0x%08x', regname, addr, data)

    def update(self):
        opname = {
            0: 'nop', 1: 'read', 2: 'write', 3: 'rsv'
        }.get(self.value & 0b11)
        if opname == 'nop':
            return
        addr = self.value >> (32 + 2) & ((1 << self._abits) - 1)
        data = (self.value >> 2) & ((1 << 32) - 1)
        write = opname == 'write'
        self._addr = addr
        if 0 <= addr-self.DATABASE < self.DATACOUNT:
            if write:
                self._data[addr-self.DATABASE] = data
            return
        if 0 <= addr-self.PROGBUFBASE < self.PROGBUFSIZE:
            if write:
                self._data[addr-self.DATABASE] = data
            return
        regname = self.DM_REGISTERS.get(addr)
        if regname is None:
            self._log.warning('Unsupported DM register 0x%02x', addr)
        else:
            self._log.info('%s @ %s [%02x]%s',
                           opname, regname, addr,
                           f' {data:08x}' if write else '')
        handler = getattr(self, f'_{regname}_{opname}', None)
        if handler:
            # pylint: disable=not-callable
            handler(data)

    def _dmcontrol_write(self, value: int):
        # harteq | hartsellen (== 0) | dmactive
        mask = (1 << 31) | (0 << 16) | (1 << 0)
        self._registers[self.DM_NAMES['dmcontrol']] = value & mask
        if value & (1 << 31):
            self._is_halted = True
            self._is_resumed = False
        elif value & (1 << 30):
            self._is_halted = False
            self._is_resumed = True

    def _command_write(self, value: int):
        cmdtype = value >> 24
        if cmdtype == 0:
            self._cmderr = self._access_register(value)
        elif cmdtype == 1:
            self._cmderr = self._quick_access(value)
        elif cmdtype == 2:
            self._cmderr = self._access_memory(value)

    def _dmstatus_read(self, *_):
        version = (TAP_DTMCS.VERSION + 1) << 0
        authenticated = 1 << 7
        allhalted = int(self._is_halted) << 9
        anyhalted = int(self._is_halted) << 8
        allresumed = int(self._is_resumed) << 17
        anyresumed = int(self._is_resumed) << 16
        regno = self.DM_NAMES['dmstatus']
        self._registers[regno] = (version | authenticated |
                                  allhalted | anyhalted |
                                  allresumed | anyresumed)

    def _hartinfo_read(self, *_):
        data_addr = 0 << 0
        data_size = 0 << 12
        data_access = 0 << 16
        nscratch = 0 << 20
        regno = self.DM_NAMES['hartinfo']
        self._registers[regno] = nscratch | data_access | data_size | data_addr

    def _sbcs_read(self, *_):
        sbaccess8 = 1 << 0
        sbaccess16 = 1 << 1
        sbaccess32 = 1 << 2
        sbasize = 32 << 5
        sberror = self._sb_error << 12
        sbversion = 1 << 29
        regno = self.DM_NAMES['sbcs']
        self._registers[regno] = (sbaccess8 | sbaccess16 | sbaccess32 |
                                  sbasize | sberror | sbversion)

    def _abstractcs_read(self, *_):
        progbufsize = self.PROGBUFSIZE << 24
        busy = 0 << 12
        cmderr = self._cmderr << 8
        datacount = self.DATACOUNT << 0
        regno = self.DM_NAMES['abstractcs']
        self._registers[regno] = progbufsize | busy | cmderr | datacount

    def _access_register(self, value: int) -> 'TAP_DMI.CmdErr':
        regno = value & ((1 << 16) - 1)
        write = bool(value & (1 << 16))
        transfer = bool(value & (1 << 17))
        postexec = bool(value & (1 << 18))
        aarpostincrement = bool(value & (1 << 19))
        aarsize = (value >> 20) & ((1 << 2) - 1)
        aarbits = 1 << (3 + aarsize)
        if transfer:
            if aarbits > 32:
                self._log.warning('Aarsize not supported %u for regno %03x',
                                  aarsize, regno)
                return TAP_DMI.CmdErr.NOT_SUPPORTED
            if aarpostincrement:
                self._log.warning('Post-increment not supported')
            if postexec:
                self._log.warning('Post-exec not supported')
        if write:
            self._log.warning('Write to reg 0x%04x not supported', regno)
            return TAP_DMI.CmdErr.NOT_SUPPORTED
        if 0 <= regno <= 0x0fff:
            try:
                csr = CSR(regno)
            except ValueError:
                csr = f'CSR 0x{regno:03x}'
            self._log.warning('Want to read %s', csr)
            if csr == CSR.MISA:
                self._data[0] = TAP_DMI.MISA
                return TAP_DMI.CmdErr.NONE
            self._log.warning('MISA: %08x', self.MISA)
        elif 0x1000 <= regno <= 0x101f:
            gpr = regno-0x1000
            self._log.warning('Want to read %s (x%d)', GPR(gpr), gpr)
        elif 0x1020 <= regno <= 0x103f:
            self._log.warning('Want to read FPR f%d', regno-0x1020)
            return TAP_DMI.CmdErr.NOT_SUPPORTED
        elif 0xc000 <= regno <= 0xffff:
            self._log.warning('Want to read custom 0x%02x', regno-0xc000)
        else:
            self._log.warning('Invalid register 0x%04x', regno)
            return TAP_DMI.CmdErr.NOT_SUPPORTED
        return TAP_DMI.CmdErr.NONE

    def _quick_access(self, _value: int) -> 'TAP_DMI.CmdErr':
        return TAP_DMI.CmdErr.NOT_SUPPORTED

    def _access_memory(self, _value: int) -> 'TAP_DMI.CmdErr':
        return TAP_DMI.CmdErr.NOT_SUPPORTED


class TAPExtension:
    """Support basic RISC-V DTM register
    """

    def __init__(self, abits: int):
        self._log = getLogger('tap.ext')
        dmi = TAP_DMI(abits)
        dtmcs = TAP_DTMCS(abits, dmi)
        self._irs = {0x10: dtmcs, 0x11: dmi}

    def get_by_value(self, value: int) -> Optional[TAPRegister]:
        return self._irs.get(value, None)


def main():
    """Main routine.
    """
    debug = False
    try:
        desc = sys.modules[__name__].__doc__.split('.', 1)[0].strip()
        argparser = ArgumentParser(description=f'{desc}.')
        argparser.add_argument('-p', '--port', type=int, default=3335,
                               help='remote host TCP port (defaults to 3335)')
        argparser.add_argument('-a', '--abits', type=int, default=7,
                               help='address width in bits (defaults to 7)')
        argparser.add_argument('-v', '--verbose', action='count',
                               help='increase verbosity')
        argparser.add_argument('-d', '--debug', action='store_true',
                               help='enable debug mode')
        args = argparser.parse_args()
        debug = args.debug

        configure_loggers(args.verbose, 'tap')

        ext = TAPExtension(args.abits)
        tap = BitBangController(ext)
        tap.run(args.port)
        sys.exit(0)

    except (IOError, ValueError, ImportError) as exc:
        print(f'\nError: {exc}', file=sys.stderr)
        if debug:
            print(format_exc(chain=False), file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(2)


if __name__ == '__main__':
    main()
