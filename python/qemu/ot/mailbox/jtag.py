# Copyright (c) 2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Mailbox over JTAG access.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from logging import getLogger

from .sysmbox import SysMbox
from ..dtm import DebugTransportModule


class JtagMbox(SysMbox):
    """JTAG Mailbox requester API
    """

    SYSREGS = {'control': 2, 'status': 3, 'wdata': 4, 'rdata': 5}

    def __init__(self, dtm: 'DebugTransportModule', address: int):
        super().__init__()
        self._log = getLogger('dtm.jtagmbx')
        self._dmi = dtm['dmi']
        self._address = address

    @property
    def busy(self) -> bool:
        return bool(self._read_reg('status') & 0b001)

    @property
    def on_error(self) -> bool:
        return bool(self._read_reg('status') & 0b100)

    @property
    def object_ready(self) -> bool:
        return bool(self._read_reg('status') >> 31)

    def go(self) -> None:
        self._write_reg('control', 1 << 31)

    def abort(self) -> None:
        self._write_reg('control', 0b1)

    def write_word(self, word: int) -> None:
        self._write_reg('wdata', word)

    def read_word(self, ack: bool = True) -> int:
        word = self._read_reg('rdata')
        if ack:
            self.acknowledge_read()
        return word

    def acknowledge_read(self) -> None:
        self._write_reg('rdata', 0)  # value is meaningless

    def _write_reg(self, name: str, value: int) -> None:
        if value >= (1 << 32):
            raise ValueError('Invalid value')
        try:
            reg = self.SYSREGS[name]
        except KeyError as exc:
            raise ValueError(f"No such mailbox register: '{name}'") from exc
        self._log.info('write %s: 0x%08x', name, value)
        return self._dmi.write(self._address + reg, value)

    def _read_reg(self, name: str) -> int:
        try:
            reg = self.SYSREGS[name]
        except KeyError as exc:
            raise ValueError(f"No such mailbox register: '{name}'") from exc
        self._log.info('read %s', name)
        value = self._dmi.read(self._address + reg)
        self._log.info('read 0x%08x', value)
        return value
