#!/usr/bin/env python3

"""SPI device flasher tool.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

# Copyright (c) 2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

from argparse import ArgumentParser, FileType
from logging import getLogger
from os import linesep
from os.path import dirname, join as joinpath, normpath
from time import sleep, time as now
from traceback import format_exc
import sys

QEMU_PYPATH = joinpath(dirname(dirname(dirname(normpath(__file__)))),
                       'python', 'qemu')
sys.path.append(QEMU_PYPATH)

from ot.spi import SpiDevice
from ot.util.log import configure_loggers
from ot.util.misc import HexInt


class SpiDeviceFlasher:
    "Simple SPI device flasher, using OT protocol."

    DEFAULT_PORT = 8004
    """Default TCP port for SPI device."""

    def __init__(self):
        self._log = getLogger('spidev.flash')
        self._spidev = SpiDevice()

    def connect(self, host: str, port: int):
        """Connect to the remote SPI device and wait for sync."""
        self._spidev.connect(host, port)
        self._wait_for_remote()

    def disconnect(self):
        """Disconnect from the remote host."""
        self._spidev.power_down()

    def program(self, data: memoryview, offset: int = 0):
        """Programm a buffer into the remote flash device."""
        start = now()
        total = 0
        page_size = 256
        page_count = (len(data) + page_size - 1) // page_size
        log = getLogger('spidev')
        log.info('\nRead SFTP')
        self._spidev.read_sfdp()
        log.info('\nChip erase')
        self._spidev.enable_write()
        self._spidev.chip_erase()
        self._spidev.wait_idle()
        for pos in range(0, len(data), page_size):
            page = data[pos:pos+page_size]
            log.debug('Program page @ 0x%06x %d/%d, %d bytes',
                      pos + offset, pos//page_size, page_count, len(page))
            self._spidev.enable_write()
            self._spidev.page_program(pos + offset, page)
            sleep(0.003)
            self._spidev.wait_idle(pace=0.001)  # bootrom is slow :-)
            total += len(page)
        delta = now() - start
        msg = f'{delta:.1f}s to send {total/1024:.1f}KB: ' \
              f'{total/(1024*delta):.1f}KB/s'
        log.info('%s', msg)
        self._spidev.reset()

    def _wait_for_remote(self):
        # use JEDEC ID presence as a sycnhronisation token
        # remote SPI device firware should set JEDEC ID when it is full ready
        # to handle requests
        timeout = now() + 3.0
        while now() < timeout:
            jedec = set(self._spidev.read_jedec_id())
            if len(jedec) > 1 or jedec.pop() not in (0x00, 0xff):
                return
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
                               help='Address in the SPI flash (default to 0)')
        argparser.add_argument('-r', '--host',
                               default='127.0.0.1',
                               help='remote host name (default: localhost)')
        argparser.add_argument('-p', '--port', type=int,
                               default=SpiDeviceFlasher.DEFAULT_PORT,
                               help=f'remote host TCP port (defaults to '
                                    f'{SpiDeviceFlasher.DEFAULT_PORT})')
        argparser.add_argument('-v', '--verbose', action='count',
                               help='increase verbosity')
        argparser.add_argument('-d', '--debug', action='store_true',
                               help='enable debug mode')
        args = argparser.parse_args()
        debug = args.debug

        configure_loggers(args.verbose, 'spidev')

        flasher = SpiDeviceFlasher()
        flasher.connect(args.host, args.port)
        data = args.file.read()
        args.file.close()
        flasher.program(data, args.address)
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
