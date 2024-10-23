# Copyright (c) 2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Debug Module tools.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""


from logging import getLogger
from sys import modules
from typing import Optional

from jtag.bits import BitSequence
from jtag.jtag import JtagEngine


class DMIError(RuntimeError):
    """DMI Error."""


class DMIBusyError(DMIError):
    """Last operation still in progress."""

    def __init__(self):
        super().__init__('DMI is busy')


class DMIFailedError(DMIError):
    """Last operation has failed."""

    def __init__(self):
        super().__init__('DMI execution failed')


class DTMRegister:
    """Abstract base class for DTM registers."""

    def __init__(self, dtm: 'DebugTransportModule'):
        name = self.__class__.__name__
        if (not hasattr(self.__class__, 'ADDRESS') or
                not isinstance(getattr(self, 'ADDRESS'), int)):
            raise NotImplementedError(f'{name} address not defined')
        self._dtm = dtm
        self._log = getLogger(f'dtm.{name.lower()}')

    @property
    def address(self) -> int:
        """Return the register address."""
        # pylint: disable=no-member
        return self.ADDRESS

    def _read(self, length: int) -> BitSequence:
        """Read the current register value."""
        self._log.debug('read %d bits', length)
        return self._dtm.read(self.address, length)

    def _write(self, bseq: BitSequence):
        """Read the current register value."""
        self._log.debug('write %s', bseq)
        return self._dtm.write(self.address, bseq)

    def _read_word(self) -> int:
        """Read the current register value as a 32-bit value."""
        self._log.debug('read_word')
        return self._dtm.read_word(self.address)

    def _write_word(self, value: int):
        """Read the current register value as a 32-bit value."""
        self._log.debug('write_word %x', value)
        return self._dtm.write_word(self.address, value)


class DTMCS(DTMRegister):
    """DTM Control and Status register."""

    ADDRESS = 0x10

    @property
    def idle(self) -> int:
        """Minimum number of cycles a debugger should spend in Run-Test/Idle
           after every DMI scan (hint only)
        """
        hint = (self._read_word() >> 12) & 0b111
        self._log.debug('idle: %d', hint)
        return hint

    @property
    def dmistat(self) -> int:
        """Return DMI status

           0: No error.
           1: Reserved. Interpret the same as 2 (see DMI register)
           2: An operation failed (resulted in DMI operation of type 2).
           3: An operation was attempted while a DMI access was still in
              progress (resulted in DMI operation of type 3).
        """
        dmistat = (self._read_word() >> 10) & 0b11
        self._log.debug('dmistat: %d', dmistat)
        return dmistat

    @property
    def abits(self) -> int:
        """The bit size of address in DMI."""
        abits = (self._read_word() >> 4) & 0b111111
        self._log.debug('abits: %d', abits)
        return abits

    @property
    def version(self) -> int:
        """Version of the supported DMI specification (raw code)."""
        version = self._read_word() & 0b1111
        self._log.debug('version: %d', version)
        return version

    @property
    def dmi_version(self) -> tuple[int, int]:
        """Version of the supported DMI specification."""
        try:
            tversion = {0: (0, 11), 1: (0, 13)}[self.version]
        except KeyError:
            tversion = (0, 0)
        self._log.debug('version: %d.%d', *tversion)
        return tversion

    def check(self) -> None:
        """Raise a DMI Error if any.
        """
        self._log.debug('check')
        dmistat = self.dmistat
        err = self._dtm.build_error(dmistat)
        if err:
            self._dtm.set_sticky(err)
            raise err

    def dmireset(self) -> None:
        """Clears the sticky error state and allows the DTM to retry or complete
           the previous transaction.
        """
        self._log.debug('dmi reset')
        self._write_word(1 << 16)
        self._dtm.clear_sticky()

    def dmihardreset(self) -> None:
        """Causes the DTM to forget about any outstanding DMI transactions.
        """
        self._log.debug('dmi hard reset')
        self._write_word(1 << 17)
        self._dtm.clear_sticky()


class DMI(DTMRegister):
    """Debug Module Interface Access."""

    ADDRESS = 0x11

    OPS = {'nop': 0, 'read': 1, 'write': 2}

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._abits = 0

    @property
    def length(self) -> int:
        """Return the register length in bits."""
        return self._abits + 32 + 2

    def write(self, address: int, value: int) -> None:
        """Write a 32-bit value to the specified address
        """
        dmi = self._build_dmi(address)
        value &= 0xffff_ffff
        dmi |= value << 2
        dmi |= self.OPS['write']
        wbseq = BitSequence(dmi, self.length)
        self._log.debug('write: 0x%08x', value)
        self._write(wbseq)
        rbseq = self._read(self.length)
        res = int(rbseq) & 0b11
        err = self._dtm.build_error(res)
        if err:
            self._log.error('op res %d -> %s', res, err)
            self._dtm.set_sticky(err)
            raise err

    def read(self, address: int) -> int:
        """Read a 32-bit value from the specified address
        """
        self._log.debug('read @ 0x%x', address)
        dmi = self._build_dmi(address)
        dmi |= self.OPS['read']
        wbseq = BitSequence(dmi, self.length)
        self._write(wbseq)
        rbseq = self._read(self.length)
        value = int(rbseq)
        res = value & 0b11
        err = self._dtm.build_error(res)
        if err:
            self._log.error('op res %d -> %s', res, err)
            self._dtm.set_sticky(err)
            raise err
        value >>= 2
        value &= 0xffff_ffff
        self._log.debug('read: 0x%08x', value)
        return value

    def _build_dmi(self, address: int) -> int:
        self._check_error()
        if not self._abits:
            self._abits = self._dtm.abits()
            if self._abits < 1:
                raise DMIError('Invalid reported address bits')
            self._log.info('DMI width: %d bits', self._abits)
        if address >= (1 << self._abits):
            raise ValueError(f'Address 0x{address:x} too large, '
                             f'max 0x{(1 << self._abits) -1:x}')
        return address << (32 + 2)

    def _check_error(self) -> None:
        self._dtm.check_sticky()


class DebugTransportModule:
    """Debug Transport Modules provide access to the DM over JTAG.

       :param engine: JTAG engine
       :param ir_length: the length in bits of the IR register
    """

    def __init__(self, engine: JtagEngine, ir_length: int):
        self._engine = engine
        self._ir_length = ir_length
        self._log = getLogger('dtm')
        self._regs = self._load_registers()
        self._sticky: Optional[DMIError] = None

    def __getitem__(self, name: str) -> DTMRegister:
        try:
            return self._regs[name]
        except KeyError as exc:
            raise ValueError(f"No such DTM reg '{name}'") from exc

    @property
    def engine(self) -> JtagEngine:
        """Provide the associated engine."""
        return self._engine

    @classmethod
    def build_error(cls, code: int) -> Optional[DMIError]:
        """Build a DMIError from an error code."""
        if code in (1, 2):
            return DMIFailedError()
        if code == 3:
            return DMIBusyError()
        if code == 0:
            return None
        raise ValueError(f'Invalid DTM error code: {code}')

    def read(self, address: int, length: int) -> BitSequence:
        """Read a bit sequence value."""
        self._log.debug("read addr: 0x%x len: %d", address, length)
        self._engine.write_ir(BitSequence(address, self._ir_length))
        return self._engine.read_dr(length)

    def write(self, address: int, bseq: BitSequence) -> None:
        """Write a bit sequence value."""
        self._log.debug("write addr: 0x%x len: %d", address, len(bseq))
        self._engine.write_ir(BitSequence(address, self._ir_length))
        self._engine.write_dr(bseq)
        self._engine.run()

    def read_word(self, address: int) -> int:
        """Read a 32-bit value."""
        return int(self.read(address, 32))

    def write_word(self, address: int, value: int) -> int:
        """Write a 32-bit value."""
        self.write(address, BitSequence(value, 32))

    def set_sticky(self, error: DMIError):
        """Set sticky error.
           @warning Internal API, should not be called but from the DMI
                    register.
        """
        self._sticky = error

    def clear_sticky(self) -> None:
        """Clear sticky error.
           @warning Internal API, should not be called but from the DMI
                    register.
        """
        self._sticky = None

    def check_sticky(self) -> None:
        """Raise a DMIError if a sticky error has been recorded.
        """
        if self._sticky:
            raise self._sticky

    def abits(self) -> int:
        """Report the bit size of address in DMI."""
        try:
            dtmcs = self['dtmcs']
        except KeyError as exc:
            raise DMIError('Missing DTMCS register') from exc
        return dtmcs.abits

    def _load_registers(self) -> dict[str, DTMRegister]:
        module = modules[__name__]
        regs = {}
        for name in dir(module):
            class_ = getattr(module, name)
            if not isinstance(class_, type):
                continue
            if not issubclass(class_, DTMRegister) or class_ is DTMRegister:
                continue
            address = getattr(class_, 'ADDRESS', None)
            if not address or not isinstance(address, int):
                continue
            self._log.debug('Instanciate %s', name)
            regs[name.lower()] = class_(self)
        return regs
