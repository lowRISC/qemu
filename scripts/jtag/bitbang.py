# Copyright (c) 2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""JTAG controller for OpenOCD/QEMU bitbang protocol.

   :author: Emmanuel Blot <eblot@rivosinc.com>

   Protocol abstract:

                  TCLK TMS TDI
    b'0' - Write    0   0   0
    b'1' - Write    0   0   1
    b'2' - Write    0   1   0
    b'3' - Write    0   1   1
    b'4' - Write    1   0   0
    b'5' - Write    1   0   1
    b'6' - Write    1   1   0
    b'7' - Write    1   1   1

                  TRST SRST
    b'r' - Reset    0   0
    b's' - Reset    0   1
    b't' - Reset    1   0
    b'u' - Reset    1   1

    b'R' - Read request  -> b'0' or b'1'
    b'Q' - Quit request
"""

from logging import getLogger
from socket import socket
from time import time as now
from typing import Optional

from .bits import BitSequence
from .jtag import JtagController


class JtagBitbangController(JtagController):
    """JTAG master for Remote Bitbang connection.

       :param sock: communication socket with remote QEMU JTAG server
       :param link_log: whether to emit link log messages
    """

    DEFAULT_PORT = 3335
    """Default TCP port."""

    RECV_TIMEOUT = 0.25
    """Maximum allowed time in seconds to receive a response from the JTAG
       controller.
    """

    READ = b'R'
    """JTAG bitbang code to receive data from TDO."""

    QUIT = b'Q'
    """JTAG bitbang code to quit."""

    def __init__(self, sock: socket, link_log: bool = False):
        self._log = getLogger('jtag.rbb')
        self._sock = sock
        self._link_log = link_log
        self._last: Optional[bool] = None  # Last deferred TDO bit
        self._outbuf = bytearray()
        self._tck = False
        self._tms = False
        self._tdi = False
        self._trst = False
        self._srst = False

    def tap_reset(self, use_trst: bool = False) -> None:
        self._log.info('TAP reset (%s)', 'TRST' if use_trst else 'SW')
        if use_trst:
            self._trst = not self._trst
            self._write(self._reset_code(self._trst, self._srst))
            self._trst = not self._trst
            self._write(self._reset_code(self._trst, self._srst))
        else:
            self.write_tms(BitSequence('11111'))

    def system_reset(self) -> None:
        self._log.info('System reset')
        self._write(self._reset_code(self._trst, self._srst))
        self._srst = not self._srst
        self._write(self._reset_code(self._trst, self._srst))
        self._srst = not self._srst
        self._write(self._reset_code(self._trst, self._srst))

    def quit(self) -> None:
        self._log.info('Quit')
        self._sock.send(self.QUIT)

    def write_tms(self, modesel: BitSequence) -> None:
        if not isinstance(modesel, BitSequence):
            raise ValueError('Expect a BitSequence')
        # apply the last TDO bit
        if self._last is not None:
            self._tdi = self._last
            self._last = None
        if self._link_log:
            self._log.debug('write TMS [%d] %s', len(modesel), modesel)
        tck = self._tck
        tdi = self._tdi
        code = self._bus_code
        stream = bytearray()
        while modesel:
            tms = modesel.pop_left_bit()
            stream.append(code(tck, tms, tdi))
            tck = not tck
            stream.append(code(tck, tms, tdi))
            tck = not tck
        self._sock.send(stream)
        self._tms = tms
        self._tck = tck

    def write(self, out: BitSequence):
        if not isinstance(out, BitSequence):
            raise ValueError('out is not a BitSequence')
        if self._last is not None:
            # TODO: check if this case needs to be handled
            raise NotImplementedError('Last is lost')
        self._last = out.pop_left_bit()
        if self._link_log:
            self._log.debug('write TDI [%d] %s', len(out), out)
        tms = self._tms
        tck = self._tck
        code = self._bus_code
        stream = bytearray()
        while out:
            tdi = out.pop_right_bit()
            stream.append(code(tck, tms, tdi))
            tck = not tck
            stream.append(code(tck, tms, tdi))
            tck = not tck
        self._sock.send(stream)
        self._tdi = tdi
        self._tms = tms
        self._tck = tck

    def read(self, length: int) -> BitSequence:
        if length == 0:
            raise ValueError()
        bseq = BitSequence()
        rem = length
        timeout = now() + self.RECV_TIMEOUT
        if self._link_log:
            self._log.debug('read %d bits, TMS: %d', length, self._tms)
        tms = self._tms
        tck = self._tck
        tdi = self._tdi
        read = ord(self.READ)
        code = self._bus_code
        stream = bytearray()
        for _ in range(length):
            stream.append(code(tck, tms, tdi))
            tck = not tck
            stream.append(code(tck, tms, tdi))
            tck = not tck
            stream.append(read)
        self._sock.send(stream)
        while rem:
            try:
                data = self._sock.recv(length)
            except TimeoutError:
                if now() < timeout:
                    continue
                raise
            rem -= len(data)
            bseq.push_right(data)
            timeout = now() + self.RECV_TIMEOUT
        bseq.reverse()
        if self._link_log:
            self._log.debug('read TDI [%d] %s', len(bseq), bseq)
        return bseq

    @classmethod
    def _bus_code(cls, tclk: bool, tms: bool, tdi: bool) -> int:
        return 0x30 + ((int(tclk) << 2) | (int(tms) << 1) | tdi)

    @classmethod
    def _reset_code(cls, trst: bool, srst: bool) -> int:
        return ord('r') + ((int(trst) << 1) | srst)

    def _write(self, code: int):
        if self._link_log:
            self._log.debug('_write 0x%02x %s (%s)', code, f'{code-0x30:03b}',
                            chr(code))
        self._sock.send(bytes([code]))
