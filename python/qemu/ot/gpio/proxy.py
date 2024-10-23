# Copyright (c) 2024 Rivos, Inc.
# All rights reserved.

"""GPIO proxy.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from enum import IntEnum
from logging import getLogger
from select import POLLIN, poll as spoll
from socket import (IPPROTO_TCP, TCP_NODELAY, SHUT_RDWR, socket)
from time import time as now
from typing import Optional


class GpioProxy:
    """Simple GPIO proxy using the QEMU OT GPIO protocol."""

    DEFAULT_PORT = 8007
    """Default GPIO TCP port."""

    INPUT_CMD_MAP = {
        'C': 'clear',
        'D': 'direction',
        'O': 'output',
        'P': 'pull',
        'Q': 'query',
        'Y': 'ynput',
        'Z': 'hi_z',
    }

    OUTPUT_CMD_MAP = {
        'input': 'I',
        'mask': 'M',
        'repeat': 'R',
    }

    TIMEOUT = 1.0
    """Default allowed timeout to complete an exchange with the remote target.
    """

    POLL_TIMEOUT = 0.05
    """Maximum time to wait on a blocking operation.
    """

    def __init__(self, sock: socket):
        self._log = getLogger('gpio.proxy')
        self._socket = sock
        self._socket.settimeout(None)
        self._socket.setsockopt(IPPROTO_TCP, TCP_NODELAY, 1)
        self._out: Optional[int] = None
        self._last_out: Optional[int] = None
        self._yn: Optional[int] = None
        self._last_yn: Optional[int] = None
        self._in = 0
        self._dir = 0
        self._pull = 0
        self._hiz = 0
        self._buf: bytearray()
        self._timestamps: dict[str, float] = {}

    def quit(self) -> None:
        """Close the communication socket."""
        if self._socket:
            self._socket.shutdown(SHUT_RDWR)
            self._socket.close()
            self._socket = None

    def sync(self) -> None:
        """Request current status from QEMU."""
        self._receive()
        self._write('repeat', 0)

    def gpio_out(self, force: bool = False) -> int:
        """Retrieve the output value of the 32-bit GPIO lines."""
        self._receive()
        if self._out is None or force:
            self._write('R', 0)
            start = now()
            timeout = start + self.TIMEOUT
            while now() < timeout:
                update = self._timestamps.get('output', 0.0)
                if update > start:
                    break
            else:
                raise TimeoutError('No answer from remote')
        return self._out

    def get_updated_out(self, gpio_def: type[IntEnum]) -> dict[IntEnum, bool]:
        """Get GPIO changes since last call."""
        out = self.gpio_out()
        if self._last_out is None:
            changes = ~0
        else:
            changes = self._last_out ^ out
        if not changes:
            return {}
        gpio_changes = {}
        for gpio in gpio_def:
            bit = gpio.value
            if changes & (1 << bit):
                gpio_changes[gpio.name] = bool((out >> bit) & 1)
        self._last_out = out
        return gpio_changes

    def gpio_mirrored_in(self, force: bool = False) -> int:
        """Retrieve the mirrored values of the 32-bit GPIO lines."""
        self._receive()
        if self._yn is None or force:
            self._write('R', 0)
            start = now()
            timeout = start + self.TIMEOUT
            while now() < timeout:
                update = self._timestamps.get('ynput', 0.0)
                if update > start:
                    break
            else:
                raise TimeoutError('No answer from remote')
        return self._yn

    def get_updated_mirrored_in(self, gpio_def: type[IntEnum]) -> \
            dict[IntEnum, bool]:
        """Get GPIO input changes since last call."""
        m_in = self.gpio_mirrored_in()
        if self._last_yn is None:
            changes = ~0
        else:
            changes = self._last_yn ^ m_in
        if not changes:
            return {}
        gpio_changes = {}
        for gpio in gpio_def:
            bit = gpio.value
            if changes & (1 << bit):
                gpio_changes[gpio.name] = bool((m_in >> bit) & 1)
        self._last_yn = m_in
        return gpio_changes

    def _write(self, command: str, value: int) -> None:
        if not isinstance(command, str):
            raise ValueError("Invalid command {command}'")
        if len(command) > 1:
            command = self.OUTPUT_CMD_MAP.get(command.lower())
        if command not in self.OUTPUT_CMD_MAP.values():
            raise ValueError("Invalid command {command}'")
        if not 0 <= value < (1 << 32):
            raise ValueError("Invalid value: 0x{value:08x}")
        req = f'{command}:{value:08x}\r\n'.encode()
        self._log.debug('TX %s', req.decode().rstrip())
        self._socket.send(req)

    def _receive(self) -> None:
        buf = bytearray()
        poller = spoll()
        poller.register(self._socket, POLLIN)
        for _ in poller.poll(self.POLL_TIMEOUT):
            data = self._socket.recv(1024)
            buf.extend(data)
        while buf:
            pos = buf.find(b'\n')
            if pos < 0:
                break
            chunk, buf = buf[:pos], buf[pos+1:]
            if pos != 11:
                self._log.error("Find truncated GPIO packet: '%s'", chunk)
                continue
            tchunk = chunk.strip().decode()
            cmd, sep, tval = tchunk[0], tchunk[1], tchunk[2:]
            if cmd not in self.INPUT_CMD_MAP:
                self._log.error("Unknown received command: {%s}", cmd)
                continue
            if sep != ':':
                self._log.error("Unknown received separator: {%s}", sep)
                continue
            command = self.INPUT_CMD_MAP[cmd]
            rcvfn = getattr(self, f'_receive_{command}', None)
            if not rcvfn or not callable(rcvfn):
                raise RuntimeError(f"Invalid/missing handler for '{command}'")
            value = int(tval, 16)
            if not 0 <= value < (1 << 32):
                raise ValueError(f"Invalid value: 0x{value:08x}")
            self._log.debug('RX %s %08x', command.rstrip(), value)
            # pylint: disable=not-callable
            rcvfn(value)
            self._timestamps[command] = now()

    def _receive_clear(self, _: int) -> None:
        self._in = 0
        self._yn = None
        self._last_yn = None
        self._dir = 0
        self._pull = 0
        self._hiz = 0

    def _receive_direction(self, value: int) -> None:
        self._dir = value

    def _receive_ynput(self, value: int) -> None:
        self._yn = value

    def _receive_output(self, value: int) -> None:
        self._out = value

    def _receive_pull(self, value: int) -> None:
        self._pull = value

    def _receive_query(self, _: int) -> None:
        self._write('input', self._in)

    def _receive_hi_z(self, value: int) -> None:
        self._hiz = value
