# Copyright (c) 2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Verilator SPI protocol bridge.

   Accept incoming requests using QEMU OT SPI device protocol (TCP socket)
   Converts requests/responses to/from Verilator SPI DPI simplified protocol
   (PTY socket).

   See https://github.com/lowRISC/opentitan/pull/28803 for SPI DPI protocol.
"""

from binascii import hexlify
from collections import deque
from enum import IntFlag
from fcntl import fcntl, F_GETFL, F_SETFL
from os import O_NONBLOCK
from io import BufferedRandom
from socket import create_server, socket, SHUT_RDWR
from struct import unpack as sunpack
from threading import Event, Thread
from time import sleep, time as now
from traceback import format_exc
from typing import Optional, Union

import logging
import sys

from ot.spi.spi_device import SpiDevice


class VtorSpiInput(IntFlag):
    """Bit assignment on Verilator SPI device input."""
    SCK = 0
    CSB = 1
    SDI = 2


class VtorSpiOutput(IntFlag):
    """Bit assignment on Verilator SPI device output."""
    SDO_EN = 0
    SDO = 1


class VtorSpiBridge:
    """SPI bridge.

       Expose a QEMU OT SPI device-compatible socket.
       Relay SPI bus requests to Verilator SPI device port.
    """

    CONN_TIMETOUT = 0.25
    """Maximum time waiting for a connection before yielding
    """

    POLL_TIMEOUT = 0.05
    """Maximum time to wait on a blocking operation.
    """

    PTY_SYNC = 0.1
    """Time to poll for PTY connection.
    """

    SPI_BUS_TIMEOUT = 0.5
    """Maximum time to receive a response from Verilator PTY.
    """

    SCK_LOW = 0
    SCK_HIGH = 1 << VtorSpiInput.SCK
    CSB_LOW = 0
    CSB_HIGH = 1 << VtorSpiInput.CSB
    SDI_LOW = 0
    SDI_HIGH = 1 << VtorSpiInput.SDI
    SDO_EN_HIGH = 1 << VtorSpiOutput.SDO_EN
    SDO_HIGH = 1 << VtorSpiOutput.SDO

    # Each byte requires 16 PTY chars (two clock edges per bit, i.e. two chars).
    PTY_CHAR_PER_BYTE = 16
    DEFAULT_PTY_BUF_SIZE = 512  # 1024 should be a safe value

    ASCII_ZERO = ord('0')

    def __init__(self, debug: bool = False):
        self._log = logging.getLogger('vtor.spi')
        self._slog = logging.getLogger('vtor.spi.server')
        self._clog = logging.getLogger('vtor.spi.client')
        self._debug = debug
        self._server: Optional[Thread] = None
        self._pty: Optional[BufferedRandom] = None
        self._resume = False
        self._cs = False  # note: False means inactive (i.e. /CS high)
        self._exception = Optional[Exception]
        self._chunk_size = 0
        # initialize self._chunk_size with the property setter
        self.pty_buffer_size = self.DEFAULT_PTY_BUF_SIZE

    def create(self, tcp_port: int, end_event: Event) -> None:
        """Create fake QEMU SPI device server.
           :param tcp_port: TCP port on local host to listen for incoming
                            connections.
           :param end_event: event to signal on SPI bridge termination.
        """
        if not self._server:
            self._server = Thread(target=self._serve, name='spibridge',
                                  daemon=True, args=(tcp_port, end_event))

    @property
    def pty_buffer_size(self) -> int:
        """Get current PTY usable buffer size."""
        return self._chunk_size * self.PTY_CHAR_PER_BYTE

    @pty_buffer_size.setter
    def pty_buffer_size(self, pty_buffer_size: int) -> None:
        """Set PTY usable buffer size."""
        self._chunk_size = pty_buffer_size // self.PTY_CHAR_PER_BYTE

    def connect_pty(self, pty_name: str) -> None:
        """Connect Verilator SPI device pseudo-terminal.

           :param pty_name: pseudo terminal device to use for the SPI device
        """
        if self._pty:
            raise RuntimeError('Cannot reconnect to PTY')
        try:
            # pylint: disable=consider-using-with
            pty = open(pty_name, 'rb+', buffering=0)
            flags = fcntl(pty, F_GETFL)
            fcntl(pty, F_SETFL, flags | O_NONBLOCK)
            self._log.debug('SPI device PTY %s connected', pty_name)
            self._pty = pty
        except (ConnectionError, OSError) as exc:
            self._log.error('Cannot connect Verilator SPI device PTY %s: '
                            '%s', pty_name, exc)
            raise
        if not self._server:
            return
        self._resume = True
        self._server.start()

    def disconnect(self) -> None:
        """Disconnect all managed VCPs."""
        self._resume = False
        if self._server:
            if self._server.is_alive():
                self._server.join()
            self._server = None
        if self._pty:
            self._pty.close()
            self._pty = None

    @property
    def exception(self) -> Optional[Exception]:
        """Return last exception, if any has been raised."""
        return self._exception

    def _serve(self, tcp_port: int, event: Event):
        """Worker thread that bridge SPI device communication between a QEMU
           SPI device client and Verilator bitbang SPI device PTY.
        """
        try:
            sock = create_server(('localhost', tcp_port),
                                 backlog=1, reuse_port=True)
        except OSError:
            self._log.fatal('Cannot connect to :%d', tcp_port)
            raise
        sock.settimeout(self.CONN_TIMETOUT)
        with sock:
            try:
                while self._resume:
                    try:
                        conn, addr = sock.accept()
                    except TimeoutError:
                        # check whether server should resume every CONN_TIMEOUT
                        # to avoid deadlocking when no client wants to connect
                        continue
                    self._log.debug('New SPI device connection %s:%s', *addr)
                    with conn:
                        self._serve_connection(conn)
            except Exception as exc:
                self._exception = exc
                event.set()
                raise
            finally:
                try:
                    sock.shutdown(SHUT_RDWR)
                    sock.close()
                except OSError:
                    pass

    def _serve_connection(self, sock: socket) -> None:
        buffer = bytearray()
        length = 0
        pty_en = False
        cfg = 0
        while self._resume:
            if not self._pty:
                # there is no point handling incoming SPI host requests while
                # downstream PTY is not available
                sleep(self.PTY_SYNC)
                continue
            if not pty_en:
                # set IDLE state on SPI device connection: /CS high, SCK low
                self._log.debug('SPI idle')
                idle = bytes([self.CSB_HIGH | self.SCK_LOW])
                sync = 10
                while sync:
                    self._pty.write(idle)
                    data = self._pty.read(len(idle))
                    if data:
                        break
                    sleep(0.1)
                    sync -= 1
                else:
                    raise TimeoutError('No reply from SPI device')
                pty_en = True
            try:
                # arbitrary length to receive all buffered data at once.
                data = sock.recv(1024)
                if not data:
                    self._log.warning('SPI host disconnected')
                    return
                buffer.extend(data)
                if not buffer:
                    # wait for data
                    continue
                if not length:
                    # wait for header
                    if len(buffer) < SpiDevice.CS_HEADER_SIZE:
                        continue
                    magic, version, cfg, length = \
                        sunpack(SpiDevice.CS_HEADER_FMT,
                                buffer[:SpiDevice.CS_HEADER_SIZE])
                    if magic != b'/CS':
                        self._slog.error('Invalid SPI magic: %s',
                                         hexlify(magic).decode())
                        if self._debug:
                            raise RuntimeError('SPI protocol error')
                        return
                    if version != 0:
                        self._slog.error('Invalid SPI protocol version: %d',
                                         version)
                        if self._debug:
                            raise RuntimeError('SPI version error')
                        return
                    buffer = buffer[SpiDevice.CS_HEADER_SIZE:]
                if len(buffer) < length:
                    # wait for SPI packet/payload
                    self._slog.debug('Expect %d bytes, got %d; %d to go',
                                     length, len(buffer), length-len(buffer))
                    continue
                request, buffer = bytes(buffer[:length]), buffer[length:]
                release = not bool(cfg >> 7)
                cfg &= 0b1111
                if cfg:
                    # only support SPI mode 0, default order
                    self._slog.error('SPI mode/config not supported: %s',
                                     f'0b{cfg:04b}')
                    sock.send(bytes([0xff] * length))
                    length = 0
                    continue
                if len(request) <= 32:
                    self._slog.debug('SPI request  > %s (%d)',
                                     hexlify(request).decode(), len(request))
                else:
                    self._slog.debug('SPI request  > %s (%d) ...',
                                     hexlify(request[:32]).decode(),
                                     len(request))
                response = self._handle_spi_request(request, release)
                if len(request) <= 32:
                    self._slog.debug('SPI response < %s, %s /CS',
                                     hexlify(response).decode(),
                                     'release' if release else 'hold')
                else:
                    self._slog.debug('SPI response < %s, %s ... /CS',
                                     hexlify(response[:32]).decode(),
                                     'release' if release else 'hold')
                assert len(response) == length
                length = 0
                if not sock.send(response):
                    self._log.warning('SPI host disconnected')
                    return
            except (BrokenPipeError, ConnectionResetError):
                self._log.warning('SPI host disconnected')
                return
            # pylint: disable=broad-except
            except Exception as exc:
                # connection shutdown may have been requested
                if self._resume:
                    self._resume = False
                    self._log.fatal('Exception: %s', exc)
                    if self._debug:
                        print(format_exc(chain=False), file=sys.stderr)
                    raise

    def _exchange_pty(self, bit_seq: Union[bytes, bytearray]) -> bytearray:
        self._pty.write(bit_seq)
        return self._read_from_pty(len(bit_seq))

    def _read_from_pty(self, size: int) -> bytearray:
        mi_bit_seq = bytearray()
        # increase a bit the timeout for large packets
        timeout = now() + self.SPI_BUS_TIMEOUT * (1 + size / 20)
        while now() < timeout:
            data = self._pty.read(size-len(mi_bit_seq))
            if not data:
                continue
            mi_bit_seq.extend(data)
            if len(mi_bit_seq) == size:
                break
        else:
            raise TimeoutError(f'Not enough data received from SPI device: '
                               f'{len(mi_bit_seq)} < {size}')
        return mi_bit_seq

    def _handle_spi_request(self, request: bytes, release: bool) -> bytes:
        # ensure CSB is low (if not already)
        prolog = bytes([self.CSB_LOW | self.SCK_LOW])
        self._exchange_pty(prolog)
        mo_bit_seq = bytearray()
        mi_bit_que = deque()
        # add ASCII '0' so it's easier to debug the comm channel:
        # no need to hexlify, upper nibble is never printed out (0x01 -> '1')
        # the remote peer (Verilator SPI DPI) only considers the 3 LSBs anyway.
        pos = 0
        # PTY buffer size is hardcoded (OS setting, not configurable)
        # we do not want to overshoot its capability, so split the request
        # into manageable chunks.
        for _ in range(0, len(request), self._chunk_size):
            for _ in range(self._chunk_size):
                byte = request[pos]
                for _ in range(8):
                    bit = byte & 0x80
                    byte <<= 1
                    sdi = self.SDI_HIGH if bit else self.SDI_LOW
                    mo_bit_seq.append(self.ASCII_ZERO | sdi |
                                      self.CSB_LOW | self.SCK_LOW)
                    mo_bit_seq.append(self.ASCII_ZERO | sdi |
                                      self.CSB_LOW | self.SCK_HIGH)
                pos += 1
                if pos == len(request):
                    break
            self._clog.debug('MOSI %s', mo_bit_seq.decode())
            mi_bit_seq = self._exchange_pty(mo_bit_seq)
            self._clog.debug('MISO %s', bytes(mi_bit_seq).decode())
            assert len(mo_bit_seq) == len(mi_bit_seq)
            mo_bit_seq.clear()
            mi_bit_que.extend(mi_bit_seq)
        if release:
            self._clog.debug('Release')
            epilog = bytes([self.ASCII_ZERO |
                            self.SDI_LOW | self.CSB_HIGH | self.SCK_LOW])
            self._exchange_pty(epilog)
        self._cs = not release
        response = bytearray()
        req_byte_count = len(request)
        while len(response) < req_byte_count:
            byte = 0
            for _ in range(8):
                if len(mi_bit_que) < 2:
                    raise RuntimeError('Incoherent response length')
                mi_bit_que.popleft()  # sample (clock low)
                bval = mi_bit_que.popleft()  # sample (clock high / raise)
                # ignore ASCII MSBs if any
                # pylint: disable=superfluous-parens
                if not (bval & self.SDO_EN_HIGH):
                    bit = 0
                else:
                    bit = int(bool(bval & self.SDO_HIGH))
                byte <<= 1
                byte |= bit
            response.append(byte)
        if mi_bit_que:
            raise RuntimeError(f'Incoherent response length {len(mi_bit_que)} '
                               f'/ {int(release)}')
        return response
