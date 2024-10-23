#!/usr/bin/env python3

"""GPIO device tiny simulator.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

# Copyright (c) 2024 Rivos, Inc.
# All rights reserved.

from argparse import ArgumentParser, FileType
from os.path import dirname, join as joinpath, normpath
from traceback import format_exc
import sys

QEMU_PYPATH = joinpath(dirname(dirname(dirname(normpath(__file__)))),
                       'python', 'qemu')
sys.path.append(QEMU_PYPATH)

from ot.gpio.device import GpioDevice
from ot.util.log import configure_loggers
from ot.util.misc import HexInt


def main():
    """Main routine.
    """
    debug = False
    try:
        desc = sys.modules[__name__].__doc__.split('.', 1)[0].strip()
        argparser = ArgumentParser(description=f'{desc}.')
        argparser.add_argument('-p', '--port', type=int, default=8007,
                               help='remote host TCP port (defaults to 8007)')
        argparser.add_argument('-c', '--check', type=FileType('rt'),
                               help='input file to check command sequence')
        argparser.add_argument('-r', '--record', type=FileType('wt'),
                               help='output file to record command sequence')
        argparser.add_argument('-e', '--end', type=HexInt.parse,
                               help='emit the specified value to trigger '
                                    'remote exit on last received command')
        argparser.add_argument('-q', '--quit-on-error', action='store_true',
                               default=False, help='exit on first error')
        argparser.add_argument('-s', '--single', action='store_true',
                               default=False,
                               help='run once: terminate once first remote '
                                    'disconnects')
        argparser.add_argument('-t', '--log-time', action='store_true',
                               default=False, help='emit time in log messages')
        argparser.add_argument('-v', '--verbose', action='count',
                               help='increase verbosity')
        argparser.add_argument('-d', '--debug', action='store_true',
                               help='enable debug mode')
        args = argparser.parse_args()
        debug = args.debug

        configure_loggers(args.verbose, 'gpio', ms=args.log_time)

        if args.end and not args.check:
            argparser.error('Auto-end cannot be enabled without a check file')

        gpio = GpioDevice()
        if args.check:
            gpio.load(args.check)
        try:
            exec_ok = gpio.run(args.port, args.single, args.quit_on_error,
                               args.end)
        finally:
            if args.record:
                gpio.save(args.record)
        sys.exit(int(not exec_ok))

    except (IOError, ValueError, ImportError) as exc:
        print(f'\nError: {exc}', file=sys.stderr)
        if debug:
            print(format_exc(chain=False), file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(2)


if __name__ == '__main__':
    main()
