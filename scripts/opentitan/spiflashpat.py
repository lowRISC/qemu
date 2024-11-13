#!/usr/bin/env python3

"""SPI flash pattern generator tiny tool.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

# Copyright (c) 2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

from argparse import ArgumentParser
from logging import getLogger
from os import SEEK_SET, linesep, stat, truncate, unlink
from os.path import dirname, exists, join as joinpath, normpath
from traceback import format_exc
import sys

QEMU_PYPATH = joinpath(dirname(dirname(dirname(normpath(__file__)))),
                       'python', 'qemu')
sys.path.append(QEMU_PYPATH)

from ot.util.log import configure_loggers
from ot.util.misc import HexInt


class SpiFlashPatternGenerator:
    """Tiny pattern generator for SPI flash device image."""

    def __init__(self, size: int, path: str, big_endian: bool):
        self._log = getLogger('spiflash.gen')
        self._size = size
        self._path = path
        self._be = big_endian

    def resize(self, reset: bool) -> None:
        """Resize the flash image to the expected flash size.
           Either truncate or zero-extend the image to match the target size.

           :param reset: whether to clear out any existing flash image content.
        """
        if reset and exists(self._path):
            unlink(self._path)
        try:
            stat_res = stat(self._path)
        except FileNotFoundError:
            self._log.info('Creating file of %d bytes', self._size)
            with open(self._path, 'wb') as ffp:
                ffp.write(bytes(self._size))
            return
        size = stat_res.st_size
        if size > self._size:
            self._log.info('Truncating file from %d bytes down to %d bytes',
                           size, self._size)
            truncate(self._path, self._size)
        elif size < self._size:
            self._log.info('Extending file from %d bytes up to %d bytes',
                           size, self._size)
            with open(self._path, 'ab') as ffp:
                ffp.write(bytes(self._size - size))

    def generate(self, address: int, byte_count: int, inc: int, length: int,
                 width: int) -> None:
        """Generate binary patterns as flash content.

           :param address: start address of the first pattern
           :param byte_count: how many bytes to generate
           :param inc: increment value between each pattern
           :param length: pattern length in byte
           :param width: pattern width in bits, i.e. pattern repetition inside
                         a single length-long pattern
        """
        count = byte_count // length
        nlen = length * 2
        dbg_str = f'Old val: 0x%0{nlen}x, new val: 0x%0{nlen}x, length: %d'
        with open(self._path, 'r+b') as ffp:
            ffp.seek(address, SEEK_SET)
            rdata = ffp.read(length)
            value = int.from_bytes(rdata, 'big' if self._be else 'little')
            while count:
                count -= 1
                nvalue = self._gen_next_value(value, inc, length, width)
                wdata = nvalue.to_bytes(length, 'big' if self._be else 'little')
                self._log.debug(dbg_str, value, nvalue, len(wdata))
                ffp.write(wdata)
                value = nvalue

    def _gen_next_value(self, value: int, inc: int, length: int, width: int) \
            -> int:
        out = 0
        for pos in range(0, length * 8, width):
            mask = (1 << width) - 1
            val = (value >> pos) & mask
            val = (val + inc) & mask
            out |= val << pos
        return out


def main():
    """Main routine"""
    debug = True
    widths = {
        'nibble': 4,
        'byte': 8,
        'half': 16,
        'word': 32,
        'double': 64,
    }

    try:
        desc = sys.modules[__name__].__doc__.split('.', 1)[0].strip()
        argparser = ArgumentParser(description=f'{desc}.')
        argparser.add_argument('-f', '--file', required=True,
                               help='Flash image file')
        argparser.add_argument('-a', '--address', type=HexInt.xparse,
                               default='0',
                               help='Start address')
        argparser.add_argument('-b', '--big-endian', action='store_true',
                               help='Use big-endian encoding (default: little)')
        argparser.add_argument('-c', '--count', default='0', type=HexInt.xparse,
                               help='How many bytes to generate')
        argparser.add_argument('-i', '--inc', default=1, type=int,
                               help='Increment, may be negative (default: 1)')
        argparser.add_argument('-l', '--length', choices=list(widths)[1:],
                               default='byte',
                               help='Length of each slot (default: byte)')
        argparser.add_argument('-r', '--reset', action='store_true',
                               help='Reset the flash image content (to 0)')
        argparser.add_argument('-s', '--size', type=HexInt.xparse,
                               default='64MiB',
                               help='Flash image size (default: 64MiB)')
        argparser.add_argument('-w', '--width', choices=widths, default='byte',
                               help='Pattern width for each slot '
                                    '(default: byte)')
        argparser.add_argument('-v', '--verbose', action='count',
                               help='increase verbosity')
        argparser.add_argument('-d', '--debug', action='store_true',
                               help='enable debug mode')
        args = argparser.parse_args()
        debug = args.debug

        configure_loggers(args.verbose, 'spiflash')

        length = widths[args.length] // 8
        if args.address + length > args.size:
            argparser.error('Start address cannot extend after end of file')
        bitwidth = widths[args.width]
        if bitwidth > length * 8:
            argparser.error('Pattern width larger than slot size')

        flasher = SpiFlashPatternGenerator(args.size, args.file, args.big_endian)
        flasher.resize(args.reset)
        flasher.generate(args.address, args.count, args.inc, length,
                         bitwidth)

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
