# Copyright (c) 2024 Rivos Inc. Emmanuel Blot <eblot@rivosinc.com>
# Copyright (c) 2010-2024, Emmanuel Blot <emmanuel.blot@free.fr>
# Copyright (c) 2016, Emmanuel Bouaziz <ebouaziz@free.fr>
# All rights reserved.
#
# SPDX-License-Identifier: Apache2

"""JTAG controller API."""

from ..bits import BitSequence


class JtagController:
    """JTAG master API."""

    INSTRUCTIONS = {'bypass': 0x0, 'idcode': 0x1}
    """Common instruction register codes."""

    def tap_reset(self, use_trst: bool = False) -> None:
        """Reset the TAP controller.

           :param use_trst: use TRST HW wire if available
        """
        raise NotImplementedError('ABC')

    def system_reset(self) -> None:
        """Reset the device."""

    def quit(self) -> None:
        """Terminate session."""

    def scan(self) -> BitSequence:
        """Flush output buffer and read back the requested BitSequence.
        """
        raise NotImplementedError('ABC')

    def write_tms(self, modesel: BitSequence, read_tdo: bool = False) -> None:
        """Change the TAP controller state.

           :note: modesel content may be consumed, i.e. emptied
           :note: last TMS bit should be stored and clocked on next write
                  request

           :param modesel: the bit sequence of TMS bits to clock in
           :param read_tdo: whether to read back the TDO bit on the first CLK
                            cycle of the TMS sequence
        """
        raise NotImplementedError('ABC')

    def write(self, out: BitSequence) -> None:
        """Write a sequence of bits to TDI.

           :note: out content may be consumed, i.e. emptied
           :param out: the bit sequence of TDI bits to clock in
        """
        raise NotImplementedError('ABC')

    def read(self, length: int) -> None:
        """Read out a sequence of bits from TDO.

           :param length: the number of bits to clock out from the remote
                          device
        """
        raise NotImplementedError('ABC')

    def exchange(self, out: BitSequence) -> BitSequence:
        """Write a sequence to TDI and read out a sequence of the same length
           from TDO

           :param out: the bit sequence of TDI bits to clock in
           :return: the bit sequence received from TDO
        """
        raise NotImplementedError('ABC')
