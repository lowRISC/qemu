# Copyright (c) 2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""SoC Debug Controller DMI.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from logging import getLogger
from typing import NamedTuple

from ot.dtm import DebugTransportModule


class SocDbgStatus(NamedTuple):
    """Execution status."""
    auth_debug_intent_set: bool
    auth_window_open: bool
    auth_window_closed: bool
    auth_unlock_success: bool
    auth_unlock_failed: bool
    current_policy: int
    requested_policy: int


class SocDbgBootStatus(NamedTuple):
    """Boot status."""
    main_clock: bool
    io_clock: bool
    usb_clock: bool
    otp_done: bool
    lc_done: bool
    rom_good: list[bool]
    rom_done: list[bool]
    cpu_fetch: bool


class SocDbgController:
    """SoC Debug Controller over DMI."""

    REGS = {'control': 0, 'jtag_status': 1, 'jtag_boot_status': 2}
    """Registers."""

    ROM_COUNT = 3
    """Count of ROMs, including non-ROM ones."""

    POLICY_WIDTH = 4
    """Num of bits in policy fields."""

    DEFAULT_DMI_ADDRESS = 0x8c0
    """Default DMI address of the controller."""

    def __init__(self, dtm: DebugTransportModule, address: int):
        self._log = getLogger('dtm.socdbg')
        self._dtm = dtm
        self._dmi = dtm['dmi']
        self._address = address

    def restart_system(self) -> None:
        """Restart the remote machine."""
        self._dtm.engine.controller.system_reset()

    def resume_boot(self, resume: bool = True) -> None:
        """Tell the SoCDbg controller Ibex execution may resume."""
        self._write_reg('control', int(bool(resume)))

    @property
    def status(self) -> SocDbgStatus:
        """Report the current status."""
        regval = self._read_reg('jtag_status')
        values = []
        for name, type_ in SocDbgStatus.__annotations__.items():
            length = 1 if type_ is bool else self.POLICY_WIDTH
            val = regval & ((1 << length) - 1)
            values.append(type_(val))
            regval >>= length if name != 'auth_debug_intent_set' else 4
        status = SocDbgStatus(*values)
        return status

    @property
    def boot_status(self) -> SocDbgBootStatus:
        """Report the boot status."""
        regval = self._read_reg('jtag_boot_status')
        values = []
        for type_ in SocDbgBootStatus.__annotations__.values():
            item = []
            if type_ is bool:
                values.append(bool(regval & 1))
                regval >>= 1
            else:
                item = []
                for _ in range(self.ROM_COUNT):
                    item.append(bool(regval & 1))
                    regval >>= 1
                values.append(item)
        status = SocDbgBootStatus(*values)
        return status

    def _write_reg(self, name: str, value: int) -> None:
        if value >= (1 << 32):
            raise ValueError('Invalid value')
        try:
            reg = self.REGS[name]
        except KeyError as exc:
            raise ValueError(f"No such SocDbg register: '{name}'") from exc
        self._log.info('write %s: 0x%08x', name, value)
        return self._dmi.write(self._address + reg, value)

    def _read_reg(self, name: str) -> int:
        try:
            reg = self.REGS[name]
        except KeyError as exc:
            raise ValueError(f"No such SocDbg register: '{name}'") from exc
        self._log.info('read %s', name)
        value = self._dmi.read(self._address + reg)
        self._log.info('read 0x%08x', value)
        return value
