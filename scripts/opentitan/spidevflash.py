#!/usr/bin/env python3

"""SPI device flasher tool.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

# Copyright (c) 2024-2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

from argparse import ArgumentParser, FileType
from logging import getLogger
from os import linesep
from os.path import dirname, join as joinpath, normpath
from time import sleep, time as now
from traceback import format_exc
from typing import Optional, Union
import sys

QEMU_PYPATH = joinpath(dirname(dirname(dirname(normpath(__file__)))),
                       'python', 'qemu')
sys.path.append(QEMU_PYPATH)

from ot.spi import JedecId, SpiDevice
from ot.util.log import configure_loggers
from ot.util.misc import HexInt


class SpiDeviceFlasher:
    """Simple SPI device flasher, using OT protocol.
    """

    DEFAULT_SYNC_TIME = 5.0
    """Default synchronization max time."""

    DEFAULT_PORT = 8004
    """Default TCP port for SPI device."""

    JEDEC_MANUFACTURER = (12, 0xef)
    """Expected JEDEC manufacturer."""

    def __init__(self):
        self._log = getLogger('spidev.flash')
        self._spidev = SpiDevice()

    def connect(self, host: str, port: Optional[int], retry_count: int = 1,
                sync_time: Optional[float] = None):
        """Connect to the remote SPI device and wait for sync.

           :param host: a hostname or a connection string
           :param port: a TCP port, should be None if a connection string is
                        defined
           :param retry_count: how many attempts should be made at most to
                               coonnect to the remote peer (once per second)
           :param sync_time: max allowed synchronization time once a connection
                             is established to receive a valid JEDEC ID.

           Supported connection string format:
            - tcp:<host>:<port>
            - unix:<path>
        """
        while True:
            try:
                self._spidev.connect(host, port)
                break
            except TimeoutError:
                retry_count -= 1
                if not retry_count:
                    raise
        duration = now()
        self._log.info('Synchronizing')
        jedec_id = self._synchronize(sync_time or self.DEFAULT_SYNC_TIME)
        if (jedec_id.bank, jedec_id.jedec[0]) != self.JEDEC_MANUFACTURER:
            raise ValueError(f'Unexpected manufacurer '
                             f'{jedec_id.bank}:{jedec_id.jedec[0]}')
        duration = now() - duration
        self._log.info('Synchronization completed in %.0f ms', duration * 1000)

    def disconnect(self):
        """Disconnect from the remote host."""
        self._spidev.power_down()

    def program(self, data: Union[bytes, bytearray], offset: int = 0):
        """Programm a buffer into the remote flash device."""
        start = now()
        total = 0
        page_size = 256
        page_count = (len(data) + page_size - 1) // page_size
        self._log.info('Chip erase')
        self._spidev.enable_write()
        self._spidev.chip_erase()
        self._spidev.wait_idle(timeout=self.DEFAULT_SYNC_TIME, pace=0.1)
        self._log.info('Upload file')
        for pos in range(0, len(data), page_size):
            page = data[pos:pos+page_size]
            self._log.debug('Program page @ 0x%06x %d/%d, %d bytes',
                            pos + offset, pos//page_size, page_count, len(page))
            self._spidev.enable_write()
            self._spidev.page_program(pos + offset, page)
            self._spidev.wait_idle(timeout=self.DEFAULT_SYNC_TIME,
                                   pace=0.01)
            total += len(page)
        delta = now() - start
        msg = f'{delta:.1f}s to send {total/1024:.1f}KB: ' \
              f'{total/(1024*delta):.1f}KB/s'
        self._log.info('%s', msg)
        self._spidev.reset()

    def _synchronize(self, timeout: float) -> JedecId:
        """Wait for remote peer to be ready."""
        # use JEDEC ID presence as a sycnhronisation token
        # remote SPI device firware should set JEDEC ID when it is full ready
        # to handle requests
        expire = now() + timeout
        while now() < expire:
            jedec_id = self._spidev.read_jedec_id()
            jedec = set(jedec_id.jedec)
            if len(jedec) > 1 or jedec.pop() not in (0x00, 0xff):
                return jedec_id
            sleep(0.1)
        raise RuntimeError('Remote SPI device not ready')


def main():
    """Main routine"""
    debug = True
    try:
        desc = sys.modules[__name__].__doc__.split('.', 1)[0].strip()
        argparser = ArgumentParser(description=f'{desc}.')
        argparser.add_argument('-f', '--file', type=FileType('rb'),
                               required=True,
                               help='Binary file to flash')
        argparser.add_argument('-a', '--address', type=HexInt.parse,
                               default='0',
                               help='Address in the SPI flash (default: 0)')
        argparser.add_argument('-S', '--socket',
                               help='connection string')
        argparser.add_argument('-R', '--retry-count', type=int, default=1,
                               help='connection retry count (default: 1)')
        argparser.add_argument('-T', '--sync-time', type=float,
                               default=SpiDeviceFlasher.DEFAULT_SYNC_TIME,
                               help=f'synchronization max time (default: '
                                    f'{SpiDeviceFlasher.DEFAULT_SYNC_TIME:.1f}'
                                    f's)')
        argparser.add_argument('-t', '--terminate', action='store_true',
                               help='terminate QEMU VM on completion')
        argparser.add_argument('-r', '--host',
                               help='remote host name (default: localhost)')
        argparser.add_argument('-p', '--port', type=int,
                               help=f'remote host TCP port (default: '
                                    f'{SpiDeviceFlasher.DEFAULT_PORT})')
        argparser.add_argument('--log-udp', type=int, metavar='UDP_PORT',
                               help='Log to a local UDP logger')
        argparser.add_argument('-v', '--verbose', action='count',
                               help='increase verbosity')
        argparser.add_argument('-d', '--debug', action='store_true',
                               help='enable debug mode')
        args = argparser.parse_args()
        debug = args.debug

        configure_loggers(args.verbose, 'spidev', -1, 'spidev.dev',
                          udplog=args.log_udp)

        flasher = SpiDeviceFlasher()
        if args.socket:
            if any((args.host, args.port)):
                argparser.error('Connection string is mutually exclusive '
                                'with host and port')
            flasher.connect(args.socket, None, retry_count=args.retry_count,
                            sync_time=args.sync_time)
        else:
            flasher.connect(args.host or 'localhost',
                            args.port or SpiDeviceFlasher.DEFAULT_PORT,
                            retry_count=args.retry_count,
                            sync_time=args.sync_time)
        data = args.file.read()
        args.file.close()
        flasher.program(data, args.address)
        if args.terminate:
            flasher.disconnect()

        sys.exit(0)
    # pylint: disable=broad-except
    except Exception as exc:
        print(f'{linesep}Error: {exc}', file=sys.stderr)
        if debug:
            print(format_exc(chain=False), file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(2)


if __name__ == '__main__':
    main()
