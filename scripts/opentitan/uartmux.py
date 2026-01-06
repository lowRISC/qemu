#!/usr/bin/env python3

# Copyright (c) 2023-2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""QEMU UART muxer.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from argparse import ArgumentParser
from collections import deque
from logging import getLogger
from os.path import dirname, join as joinpath, normpath
from socketserver import StreamRequestHandler, ThreadingTCPServer
from threading import Event, Lock, Thread
from traceback import format_exc
from typing import Optional, TextIO
import sys

QEMU_PYPATH = joinpath(dirname(dirname(dirname(normpath(__file__)))),
                       'python', 'qemu')
sys.path.append(QEMU_PYPATH)

from ot.util.log import configure_loggers


class UartHandler(StreamRequestHandler):
    """Handle a single QEMU output stream.
    """

    def __init__(self, *args, **kwargs):
        self._buffer = bytearray()
        self._id = - 1
        self._log = None
        # init calls handle immediately, so local attributes need to be
        # initialized first
        super().__init__(*args, **kwargs)

    def setup(self):
        self._id = self.server.get_id(self)
        self._log = getLogger(f'mux.h[{self._id}]')
        self._log.debug('connected')
        self.request.settimeout(0.2)

    def finish(self):
        self._log.debug('disconnected')
        self.server.discard(self)

    def handle(self):
        self._log.debug('handle')
        try:
            while self.server.resume:
                try:
                    rdata = self.request.recv(1024)
                except TimeoutError:
                    continue
                except ConnectionResetError:
                    break
                if not rdata:
                    # blocking socket reading nothing: client is disconnected
                    break
                self._buffer.extend(rdata)
                while self._buffer:
                    self._log.debug('buf: %d', len(self._buffer))
                    pos = self._buffer.find(b'\n')
                    self._log.debug('pos %d', pos)
                    if pos < 0:
                        break
                    line = bytes(self._buffer[:pos+1])
                    self._buffer[:] = self._buffer[pos+1:]
                    self.server.push(self._id, line)
        except Exception as exc:
            if self.server.debug:
                print(format_exc(chain=False), file=sys.stderr)
            else:
                self._log.critical('Error: %s', str(exc))
            self.server.resume = False
            raise


class UartMuxer(ThreadingTCPServer):
    """A simple UART muxer for multiple QEMU output streams
    """

    # do not wait for thread completion
    daemon_threads = True
    allow_reuse_address = True
    timeout = None

    COLOR_PREFIX = '\x1b['
    COLOR_SUFFIX = ';1m'
    RESET = '\x1b[0m'

    def __init__(self, addr: tuple[str, int], out: TextIO, debug: bool = False,
                 separator: Optional[str] = None):
        super().__init__(addr, UartHandler)
        self._out = out
        self._debug = debug
        self._log = getLogger('mux.muxer')
        self._runner = Thread(target=self._pop, daemon=True)
        self._resume = False
        self._que: deque[tuple[int, bytes]] = deque()
        self._evt = Event()
        self._lock = Lock()
        self._handlers: list[UartHandler] = []
        self._discarded: set[UartHandler] = set()
        self._channel_count = 1
        self._channels: list[str] = []
        self._use_color = out.isatty()
        if separator:
            sep = separator if ' ' in separator else (separator * 80)[:80]
            self._sep = f'{sep}\n'
        else:
            self._sep = None

    @property
    def debug(self) -> bool:
        """Tell whether mode is enabled."""
        return self._debug

    @property
    def resume(self):
        """Tell whether muxer should continue listening on."""
        return self._resume

    @resume.setter
    def resume(self, value: bool):
        value = bool(value)
        if self._resume and not value:
            self._resume = value
            self._evt.set()
            self.shutdown()
        else:
            self._resume = value

    def run(self, channel_count: int, channels: list[str]) -> None:
        """Start listening on QEMU streams."""
        self._channel_count = min(channel_count, 8)
        self._channels = channels
        self._resume = True
        self._runner.start()
        try:
            self.serve_forever()
        finally:
            if self._use_color:
                self._out.write(self.RESET)

    def push(self, uart_id: int, line: bytes) -> None:
        """Push a new log message line from one of the QEMU stream listeners."""
        self._que.append((uart_id, line))
        self._evt.set()

    def get_id(self, uart_handler: UartHandler) -> None:
        """Get/assign a unique identifier to a listener."""
        with self._lock:
            if self._sep and not self._handlers:
                self._out.write(self._sep)
            if uart_handler not in self._handlers:
                self._handlers.append(uart_handler)
            return self._handlers.index(uart_handler)

    def discard(self, uart_handler: UartHandler) -> None:
        """Called when a listener terminates."""
        with self._lock:
            try:
                self._discarded.add(uart_handler)
            except ValueError:
                pass
            # track listeners; when all known ones are closed it is likely the
            # QEMU VM has quit, reset the all listeners so on next QEMU run,
            # same ids are assigned (which is likely to assign the same color
            # for the same QEMU output stream, despite it is somewhat a
            # heuristic)
            if self._discarded == set(self._handlers):
                self._discarded.clear()
                self._handlers.clear()
                self._log.debug('All clients flushed')

    def _pop(self) -> None:
        try:
            while self._resume:
                if not self._que:
                    if not self._evt.wait(0.1):
                        continue
                    self._evt.clear()
                if self._resume and self._que:
                    uid, byteline = self._que.popleft()
                    line = byteline.decode(errors='ignore')
                    if uid < len(self._channels):
                        name = f'{self._channels[uid]}: '
                    else:
                        name = ''
                    if self._use_color:
                        clr = uid % self._channel_count + 31
                        ansi = f'{self.COLOR_PREFIX}{clr:d}{self.COLOR_SUFFIX}'
                        # self._log.error('ANSI %s', ansi)
                        self._out.write(f'{name}{ansi}{line}{self.RESET}')
                        # raise ValueError()
                    else:
                        self._out.write(line)
        finally:
            self.resume = False


def main():
    """Main routine"""
    debug = True
    default_port = 9000
    default_iface = 'localhost'
    default_channel_count = 3
    mux = None
    try:
        desc = sys.modules[__name__].__doc__.split('.', 1)[0].strip()
        argparser = ArgumentParser(description=f'{desc}.')
        argparser.add_argument('name', nargs='*',
                               help='assign name to input connection')
        argparser.add_argument('-i', '--iface', default=default_iface,
                               help=f'specify TCP interface to listen to '
                                    f'(default: {default_iface})')
        argparser.add_argument('-p', '--port', default=default_port,
                               help=f'specify TCP port to listen to '
                                    f'(default: {default_port})')
        argparser.add_argument('-c', '--channel',
                               help=f'expected comm channel count'
                                    f'(default: {default_channel_count})')
        argparser.add_argument('-s', '--separator',
                               help='repeat separator between each session')
        argparser.add_argument('-v', '--verbose', action='count',
                               help='increase verbosity')
        argparser.add_argument('-d', '--debug', action='store_true',
                               help='enable debug mode')
        args = argparser.parse_args()
        debug = args.debug

        configure_loggers(args.verbose, 'mux')

        ThreadingTCPServer.allow_reuse_address = True
        mux = UartMuxer((args.iface, args.port), sys.stdout, debug,
                        args.separator)
        channel = args.channel
        if channel is None:
            channel = len(args.name) if args.name else default_channel_count
        mux.run(channel, args.name)

    except (IOError, ValueError, ImportError) as exc:
        print(f'\nError: {exc}', file=sys.stderr)
        if debug:
            print(format_exc(chain=False), file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        if mux:
            mux.resume = False
        sys.exit(2)


if __name__ == '__main__':
    main()
