# Copyright (c) 2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""RISC-V Debug Module tools.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from enum import IntEnum
from io import SEEK_END
from logging import getLogger
from time import sleep, time as now
from typing import Any, BinaryIO, Optional

from .regs import CSRS, GPRS
from ..bitfield import BitField
from ..dtm import DebugTransportModule


class DebugModule:
    """RISC-V Debug Module.

       Only support a single hart for now
    """

    # pylint: disable=attribute-defined-outside-init

    DM_VERSION = (0, 13, 2)
    """Supported version."""

    REGISTERS = {
        'data0': 0x04,
        'data1': 0x05,
        'data2': 0x06,
        'data3': 0x07,
        'data4': 0x08,
        'data5': 0x09,
        'data6': 0x0a,
        'data7': 0x0b,
        'data8': 0x0c,
        'data9': 0x0d,
        'data10': 0x0e,
        'data11': 0x0f,
        'dmcontrol': 0x10,
        'dmstatus': 0x11,
        'hartinfo': 0x12,
        'abstractcs': 0x16,
        'command': 0x17,
        'abstractauto': 0x18,
        'nextdm': 0x1d,
        'progbuf0': 0x20,
        'progbuf1': 0x21,
        'progbuf2': 0x22,
        'progbuf3': 0x23,
        'progbuf4': 0x24,
        'progbuf5': 0x25,
        'progbuf6': 0x26,
        'progbuf7': 0x27,
        'progbuf8': 0x28,
        'progbuf9': 0x29,
        'progbuf10': 0x2a,
        'progbuf11': 0x2b,
        'progbuf12': 0x2c,
        'progbuf13': 0x2d,
        'progbuf14': 0x2e,
        'progbuf15': 0x2f,
        'sbcs': 0x38,
        'sbaddress0': 0x39,
        'sbaddress1': 0x3a,
        'sbdata0': 0x3c,
        'sbdata1': 0x3d,
        'haltsum0': 0x40,
    }
    """Supported registers"""

    CMDERR = IntEnum('cmderr',
                     ['none', 'busy', 'notsup', 'exc', 'halt', 'bus', 'rsv',
                      'other'], start=0)
    """Command error."""

    VERSION = IntEnum('version', ['nodebug', 'v0.11', 'v0.13', 'v1.0'], start=0)
    """DMSTATUS version."""

    SBERROR = IntEnum('sberror', ['none', 'timeout', 'badaddr', 'badalign',
                                  'badsize', 'rsv5', 'rsv6', 'other'], start=0)
    """SBCS sberror."""

    SBVERSION = IntEnum('sbversion', ['legacy', 'v1.0'], start=0)
    """SBCS sbversion."""

    BITFIELDS = {
        'DMCONTROL': BitField({
            'dmactive': (0, 1),
            'ndmreset': (1, 1),
            'clrresethaltreq': (2, 1),
            'setresethaltreq': (3, 1),
            'hartselhi': (6, 10),
            'hartsello': (16, 10),
            'hasel': (26, 1),
            'ackhavereset': (28, 1),
            'hartreset': (29, 1),
            'resumereq': (30, 1),
            'haltreq': (31, 1)}),
        'DMSTATUS': BitField({
            'version': (0, 4, VERSION),
            'confstrptrvalid': (4, 1),
            'hasresethaltreq': (5, 1),
            'authbusy': (6, 1),
            'authenticated': (7, 1),
            'anyhalted': (8, 1),
            'allhalted': (9, 1),
            'anyrunning': (10, 1),
            'allrunning': (11, 1),
            'anyunavail': (12, 1),
            'allunavail': (13, 1),
            'anynonexistent': (14, 1),
            'allnonexistent': (15, 1),
            'anyresumeack': (16, 1),
            'allresumeack': (17, 1),
            'anyhavereset': (18, 1),
            'allhavereset': (19, 1),
            'impebreak': (22, 1)}),
        'HARTINFO': BitField({
            'dataaddr': (0, 12),
            'datasize': (12, 4),
            'dataaccess': (16, 1),
            'nscratch': (20, 4)}),
        'ABSTRACTCS': BitField({
            'datacount': (0, 4),
            'cmderr': (8, 3, CMDERR),
            'busy': (12, 1),
            'progbufsize': (24, 5)}),
        'COMMAND': BitField({
            'control': (0, 24),
            'cmdtype': (24, 8, IntEnum('cmdtype', ['reg', 'quick', 'mem'],
                                       start=0), True),
            'write': (16, 1),
            }, reg={
            'regno': (0, 16),
            'transfer': (17, 1),
            'postexec': (18, 1),
            'aarpostincrement': (19, 1),
            'aarsize': (20, 3, IntEnum('aarsize',
                                       ['b8', 'b16', 'b32', 'b64', 'b128'],
                                       start=0)),
            }, mem={
            'aampostincrement': (19, 1),
            'aamsize': (20, 3),
            'aamvirtual': (23, 1)}),
        'ABSTRACTAUTO': BitField({
            'autoexecdata': (0, 12),
            'autoexecprogbuf': (16, 16)}),
        'SBCS': BitField({
            'sbaccess8': (0, 1),
            'sbaccess16': (1, 1),
            'sbaccess32': (2, 1),
            'sbaccess64': (3, 1),
            'sbaccess128': (4, 1),
            'sbasize': (5, 7),
            'sberror': (12, 3, SBERROR),
            'sbreadondata': (15, 1),
            'sbautoincrement': (16, 1),
            'sbaccess': (17, 3),
            'sbreadonaddr': (20, 1),
            'sbbusy': (21, 1),
            'sbbusyerror': (22, 1),
            'sbversion': (29, 3, SBVERSION)
        })
    }

    def __init__(self, dtm: DebugTransportModule, address: int):
        self._log = getLogger('dtm.rvdm')
        self._dtm = dtm
        self._dmi = dtm['dmi']
        self._address = address
        self._hart: int = 0  # currently selected hart
        self._cache: dict[str, int] = {}

    def restart_system(self) -> None:
        """Restart the remote machine."""
        self._dtm.engine.controller.system_reset()

    @classmethod
    def decode(cls, name: str, value: int) -> dict[str, Any]:
        """Decode a bitfield register."""
        bitfield = cls.BITFIELDS.get(f'{name.upper()}')
        if not bitfield:
            raise ValueError('Cannot decode {name} register')
        return bitfield.decode(value)

    def initialize(self) -> None:
        """Initialize the debug module."""
        btf = self.BITFIELDS['DMCONTROL']
        self.dmcontrol = 0
        enable = btf.encode(dmactive=True)
        self.dmcontrol = enable
        allharts = btf.encode(dmactive=True, hartsello=-1, hartselhi=-1,
                              hasel=True)
        self.dmcontrol = allharts
        self._hart = 0
        select = btf.encode(dmactive=True, hasel=False, hartsello=self._hart)
        self.dmcontrol = select
        dmcontrol = btf.decode(self.dmcontrol)
        assert dmcontrol['dmactive']
        btf = self.BITFIELDS['DMSTATUS']
        version = btf.decode(self.dmstatus)['version']
        if version == self.VERSION['v0.11']:
            raise RuntimeError(f'Detected incompatible DM version {version!r}')
        if version != self.VERSION['v0.13']:
            self._log.warning('Detected incompatible DM version %r', version)

    @property
    def status(self) -> dict[str, int]:
        """Report debug module status."""
        btf = self.BITFIELDS['DMSTATUS']
        # TODO would need to check if another hart needs to be selected first
        return btf.decode(self.dmstatus)

    @property
    def hart_info(self) -> dict[str, int]:
        """Report current hart information."""
        btf = self.BITFIELDS['HARTINFO']
        # TODO would need to check if another hart needs to be selected first
        return btf.decode(self.hartinfo)

    @property
    def system_bus_info(self) -> dict[str, int]:
        """Report system bus capabilities."""
        btf = self.BITFIELDS['SBCS']
        return btf.decode(self.sbcs)

    @property
    def is_halted(self) -> bool:
        """Report whether the currently selected hart is halted."""
        btf = self.BITFIELDS['DMSTATUS']
        val = self.dmstatus
        status = btf.decode(val)
        return status['allhalted']

    def halt(self, hart: int = 0) -> None:
        """Halt execution the selected hart."""
        btf = self.BITFIELDS['DMCONTROL']
        halt = btf.encode(dmactive=True, haltreq=True,
                          hasel=False, hartsello=hart)
        self.dmcontrol = halt
        self._hart = hart
        timeout = now() + 1.0
        btf = self.BITFIELDS['DMSTATUS']
        while now() < timeout:
            val = self.dmstatus
            status = btf.decode(val)
            if status['allhalted']:
                self._log.info('Hart %d halted', hart)
                break
            sleep(0.001)
        else:
            self._log.error('Status %s', status)
            raise TimeoutError(f'Cannot halt hart {self._hart}')

    def resume(self, hart: int = 0) -> None:
        """Resume execution of the selected hart."""
        btf = self.BITFIELDS['DMCONTROL']
        resume = btf.encode(dmactive=True, resumereq=True,
                            hasel=False, hartsello=hart)
        self.dmcontrol = resume
        self._hart = hart
        timeout = now() + 1.0
        btf = self.BITFIELDS['DMSTATUS']
        while now() < timeout:
            val = self.dmstatus
            status = btf.decode(val)
            if status['anyresumeack']:
                self._log.info('Hart %d resumed', hart)
                break
            sleep(0.001)
        else:
            self._log.error('Status %s', status)
            raise TimeoutError(f'Cannot resume hart {self._hart}')

    def read_csr(self, reg: [str | int]) -> int:
        """Read the value of a CSR."""
        ireg = self._get_register_index(reg)
        btf = self.BITFIELDS['COMMAND']
        command = btf.encode(cmdtype='reg', regno=ireg, aarsize='b32',
                             write=False, transfer=True)
        self.command = command
        try:
            self._wait_abtract_command()
        except (RuntimeError, TimeoutError, ValueError) as exc:
            raise exc.__class__(f'{exc} while reading register {reg}') from exc
        value = self.data0
        self._log.info('read %s = %08x', reg, value)
        return value

    def write_csr(self, reg: [str | int], value: int) -> None:
        """Write a value to a CSR."""
        ireg = self._get_register_index(reg)
        btf = self.BITFIELDS['COMMAND']
        self.data0 = value
        command = btf.encode(cmdtype='reg', regno=ireg, aarsize='b32',
                             write=True, transfer=True)
        self.command = command
        try:
            self._wait_abtract_command()
        except (RuntimeError, TimeoutError, ValueError) as exc:
            raise exc.__class__(f'{exc} while writing register {reg}') from exc
        self._log.info('write %s = %08x', reg, value)
        return value

    def memory_copy(self, mfp: BinaryIO, mop: str, addr: int,
                    size: Optional[int], no_check: bool = False) -> None:
        """Handle memory operations.

           Only support 32-bit transfers (address and size should be aligned)
           for now.

           :param mfp: I/O stream to read data from or write data to, depending
                       on the selected operation
           :param mop: the operation to perform (read, write)
           :param addr: start address
           :param size: count of bytes to write
           :param no_check: assume remote peer always accepts incoming data:
                            SBCS status is not checked during transfer if this
                            option is set.
        """
        read = mop == 'read'
        write = mop == 'write'
        if not (read or write):
            raise ValueError(f'Unsupported memcopy operation {mop}')
        if addr & 0x3 != 0:
            raise ValueError('Invalid address')
        if (size and size & 0x3 != 0) or (read and not size):
            raise ValueError('Invalid size')
        start = now()
        btf = self.BITFIELDS['SBCS']
        val = self._wait_sb_idle(check=True)
        val = btf.encode(val,
                         sbreadonaddr=read,
                         sbreadondata=read,
                         sbautoincrement=True,
                         sbaccess=2)  # 32-bit access
        self.sbcs = val
        # trigger first read (sbreadonaddr) in read mode
        self.sbaddress0 = addr
        if read:
            to_go = size
            # pylint: disable=access-member-before-definition
            while to_go > 0:
                self._log.debug('reading mem from 0x%08x', addr)
                if not no_check:
                    self._wait_sb_idle()
                # trigger next read (sbreadondata), inc addr (sbautoincrement)
                data = self.sbdata0
                mfp.write(data.to_bytes(4, 'little'))
                to_go -= 4
                addr += 4
        elif write:
            if not size:
                # mfp needs to be seekable
                pos = mfp.tell()
                mfp.seek(0, SEEK_END)
                end = mfp.tell()
                size = (end - pos) & ~0x3
                mfp.seek(pos)
            to_go = size
            while to_go > 0:
                buf = mfp.read(4)
                self._log.debug('writing mem to 0x%08x %d', addr, len(buf))
                assert len(buf) == 4
                data = int.from_bytes(buf, 'little')
                # inc addr (sbautoincrement)
                self.sbdata0 = data
                if not no_check:
                    self._wait_sb_idle()
                to_go -= 4
                addr += 4
        if no_check:
            self._wait_sb_idle(check=True)
        lap = now() - start
        rate = size / (lap * 1024)
        if size > 1024:
            self._log.info('copied %d KB @ %.1f KB/s', size//1024, rate)
        else:
            self._log.info('copied %d bytes @ %.1f KB/s', size, rate)

    def read32(self, addr: int) -> int:
        """Read a single word from memory."""
        if addr & 0x3 != 0:
            raise ValueError('Invalid address')
        btf = self.BITFIELDS['SBCS']
        val = self._wait_sb_idle(check=True)
        val = btf.encode(val,
                         sbreadonaddr=True,
                         sbreadondata=False,
                         sbautoincrement=False,
                         sbaccess=2)  # 32-bit access
        self.sbcs = val
        # trigger first read (sbreadonaddr) in read mode
        self._log.debug('reading mem from 0x%08x', addr)
        self.sbaddress0 = addr
        self._wait_sb_idle()
        value = self.sbdata0
        return value

    def write32(self, addr: int, value: int) -> None:
        """Write a single word to memory."""
        if addr & 0x3 != 0:
            raise ValueError('Invalid address')
        btf = self.BITFIELDS['SBCS']
        val = self._wait_sb_idle(check=True)
        val = btf.encode(val,
                         sbreadonaddr=False,
                         sbreadondata=False,
                         sbautoincrement=False,
                         sbaccess=2)  # 32-bit access
        self.sbcs = val
        self._log.debug('writing mem to 0x%08x', addr)
        self.sbaddress0 = addr
        self.sbdata0 = value
        self._wait_sb_idle()

    def read64(self, addr: int) -> int:
        """Read two words from memory."""
        if addr & 0x3 != 0:
            raise ValueError('Invalid address')
        btf = self.BITFIELDS['SBCS']
        val = self._wait_sb_idle(check=True)
        val = btf.encode(val,
                         sbreadonaddr=True,
                         sbreadondata=False,
                         sbautoincrement=False,
                         sbaccess=2)  # 32-bit access
        self.sbcs = val
        self._log.debug('reading mem from 0x%08x', addr)
        self.sbaddress0 = addr
        self._wait_sb_idle()
        value = self.sbdata0
        self.sbaddress0 = addr + 4
        self._wait_sb_idle()
        value |= self.sbdata0 << 32
        return value

    def write64(self, addr: int, value: int) -> None:
        """Write two words to memory."""
        if addr & 0x3 != 0:
            raise ValueError('Invalid address')
        btf = self.BITFIELDS['SBCS']
        val = self._wait_sb_idle(check=True)
        val = btf.encode(val,
                         sbreadonaddr=False,
                         sbreadondata=False,
                         sbautoincrement=True,
                         sbaccess=2)  # 32-bit access
        self.sbcs = val
        self._log.debug('writing mem to 0x%08x', addr)
        self.sbaddress0 = addr
        self.sbdata0 = value & 0xffff_ffff
        self._wait_sb_idle()
        value >>= 32
        self.sbdata0 = value & 0xffff_ffff
        self._wait_sb_idle()

    def set_pc(self, addr: int) -> None:
        """Set the next Program Counter address."""
        if not self.is_halted:
            raise RuntimeError('Cannot update PC while running')
        self.write_csr('dpc', addr)

    def __getattr__(self, name) -> int:
        name = name.lower()
        regaddr = self.REGISTERS.get(name, None)
        if regaddr is None:
            raise AttributeError('No such attribute {name}')
        return self._read_reg(regaddr)

    def __setattr__(self, name, value):
        name = name.lower()
        regaddr = self.REGISTERS.get(name, None)
        if regaddr is not None:
            self._write_reg(regaddr, value)
        else:
            super().__setattr__(name, value)

    def _write_reg(self, reg: int, value: int) -> None:
        if not isinstance(value, int):
            raise TypeError(f'Invalid type {type(value)}')
        if value >= (1 << 32):
            raise ValueError('Invalid value')
        self._log.debug('write %02x: 0x%08x', reg, value)
        self._dmi.write(self._address + reg, value)
        self._cache[reg] = value

    def _read_reg(self, reg: str) -> int:
        self._log.debug('read %02x', reg)
        value = self._dmi.read(self._address + reg)
        self._cache[reg] = value
        self._log.debug('read 0x%08x', value)
        return value

    def _get_register_index(self, reg: [str | int]) -> int:
        if isinstance(reg, str):
            # Not supported: FPR, Vector, etc.
            ireg = CSRS.get(reg.lower())
            if ireg is None:
                ireg = GPRS.get(reg.lower())
                if ireg is None:
                    raise ValueError(f"No such CSR '{reg}'")
                ireg += 0x1000
            return ireg
        return reg

    def _wait_abtract_command(self) -> None:
        """Wait for the completion of an abstract command."""
        timeout = now() + 1.0
        btf = self.BITFIELDS['ABSTRACTCS']
        exc = None
        error = 0
        while now() < timeout:
            # pylint: disable=access-member-before-definition
            val = self.abstractcs
            cmd = btf.decode(val)
            error = cmd['cmderr']
            if error != self.CMDERR.none:
                exc = RuntimeError(f'DM in error: {error!r}')
                break
            if not cmd['busy']:
                break
            sleep(0.001)
        else:
            # need a recovery feature, see hung command handling in spec.
            raise TimeoutError()
        if exc:
            clear_err = btf.encode(cmderr=error)
            self.abstractcs = clear_err
            raise exc

    def _wait_sb_idle(self, check: bool = False) -> int:
        """Wait for the completion of a system bus access.

           :param check: whether to check the access is supported
        """
        btf = self.BITFIELDS['SBCS']
        timeout = now() + 1.0
        while now() < timeout:
            # pylint: disable=access-member-before-definition
            val = self.sbcs
            sbcs = btf.decode(val)
            if check:
                # check supported version
                assert sbcs['sbversion'] == self.SBVERSION['v1.0']
                # check System Bus access is supported
                assert sbcs['sbasize'] != 0
                # for now, only use 32-bit access
                assert sbcs['sbaccess32']
                check = False
            error = sbcs['sberror']
            if sbcs['sberror'] != self.SBERROR['none']:
                # clear the error
                val = btf.encode(val, sberror=True)
                self.sbcs = val
                # then raise the error
                self._log.error('sbcs 0x%08x %s', val, btf.decode(val))
                raise RuntimeError(f'SBCS in error {error!r}')
            if sbcs['sbbusyerror']:
                # clear the error
                val = btf.encode(val, sbbusyerror=True)
                self.sbcs = val
                # then raise the error
                self._log.error('sbcs 0x%08x %s', val, btf.decode(val))
                raise RuntimeError('SBCS in busy error')
            if not sbcs['sbbusy']:
                return val
            sleep(0.001)
        # need a recovery feature, see hung command handling in spec.
        raise TimeoutError('System Bus stalled')
