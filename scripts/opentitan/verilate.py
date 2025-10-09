#!/usr/bin/env python3

"""Verilator wrapper."""

# Copyright (c) 2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

from os import getenv, sep, walk
from os.path import (dirname, isdir, isfile, join as joinpath, normpath,
                     realpath)
from sys import exit as sysexit, modules, stderr
from traceback import format_exc

import sys

QEMU_PYPATH = joinpath(dirname(dirname(dirname(normpath(__file__)))),
                       'python', 'qemu')
sys.path.append(QEMU_PYPATH)

# pylint: disable=wrong-import-position
# pylint: disable=wrong-import-order
# pylint: disable=import-error

from ot.util.arg import ArgumentParser, FileType  # noqa: E402
from ot.util.log import configure_loggers # noqa: E402
from ot.verilator import DEFAULT_TIMEOUT # noqa: E402
from ot.verilator.filemgr import VtorFileManager  # noqa: E402
from ot.verilator.executer import VtorExecuter  # noqa: E402


def main():
    """Main routine."""
    debug = False
    try:
        desc = modules[__name__].__doc__.split('.', 1)[0].strip()
        argparser = ArgumentParser(description=f'{desc}.')
        files = argparser.add_argument_group(title='Files')
        files.add_argument('flash', nargs='?', type=FileType('rb'),
                           metavar='ELF|VMEM', help='flash file')
        files.add_argument('-V', '--verilator',
                           help='Verilator executable')
        files.add_argument('-R', '--rom', type=FileType('rt'),
                           metavar='VMEM',
                           help='ROM file')
        files.add_argument('-O', '--otp', type=FileType('rt'),
                           metavar='VMEM',
                           help='OTP file')
        files.add_argument('-K', '--keep-tmp', action='store_true',
                           help='do not automatically remove temporary files '
                                'and dirs on exit')
        files.add_argument('-D', '--tmp-dir',
                           help='store the temporary files in the specified '
                                'directory')
        veri = argparser.add_argument_group(title='Verilator')
        veri.add_argument('-C', '--cycles', type=int,
                          help='exit after the specified cycles')
        veri.add_argument('-I', '--show-init', action='store_true',
                          help='show initializable devices')
        veri.add_argument('-k', '--timeout', metavar='SECONDS', type=float,
                          help=f'exit after the specified seconds '
                               f'(default: {DEFAULT_TIMEOUT} secs)')
        veri.add_argument('-l', '--link-log', metavar='LINK',
                          help='create a symbolic link to the log file')
        veri.add_argument('-P', '--profile', metavar='FILE',
                          help='enable/manage profile file')
        veri.add_argument('-w', '--wave', action='store_true',
                          help='generate output wave file')
        veri.add_argument('-x', '--execution-log', metavar='FILE',
                          help='save execution log')
        extra = argparser.add_argument_group(title='Extras')
        extra.add_argument('-v', '--verbose', action='count',
                           help='increase verbosity')
        extra.add_argument('-d', '--debug', action='store_true',
                           help='enable debug mode')
        extra.add_argument('--log-time', action='store_true',
                           help='show local time in log messages')
        args = argparser.parse_args()
        debug = args.debug

        log = configure_loggers(args.verbose, 'vtor', -1, 'elf',
                                name_width=12, ms=args.log_time)[0]

        if args.tmp_dir and not isdir(args.tmp_dir):
            argparser.error('Invalid directory for temporary files')

        verilator = args.verilator or getenv('VERILATOR')
        if not verilator:
            argparser.error('Verilator tool not specified')
        if not isfile(verilator) and isdir(verilator):
            top_dir = realpath(verilator)
            for dirpath, _, files in walk(top_dir):
                if 'Vchip_sim_tb' in files:
                    verilator = realpath(joinpath(dirpath, 'Vchip_sim_tb'))
                    if isfile(verilator):
                        short_veri = verilator.removeprefix(f'{top_dir}{sep}')
                        log.debug('Found Verilator tool as %s', short_veri)
                        break
        if not isfile(verilator):
            argparser.error('Verilator tool not found')

        vfm = VtorFileManager(args.keep_tmp, args.tmp_dir)
        vtor = VtorExecuter(vfm, verilator, args.profile, debug)

        if args.show_init:
            vtor.show_init_devices()
            sysexit(0)

        if not args.flash:
            argparser.error('ELF or VMEM file is required')
        if not args.rom:
            argparser.error('ROM is required')

        # let ArgumentParser validate the paths
        flash = realpath(args.flash[0].name)
        otp = realpath(args.otp.name) if args.otp else None
        rom = realpath(args.rom.name)
        args.flash[0].close()
        args.rom.close()
        if args.otp:
            args.otp.close()

        if args.execution_log:
            if not isdir(realpath(dirname(args.execution_log))):
                argparser.error('Invalid directory for execution log file')
            vtor.save_execution_log_file(args.execution_log)
        if args.link_log:
            vtor.link_log_file(args.link_log)
        ret = vtor.verilate(rom, flash, otp, args.wave, timeout=args.timeout,
                            cycles=args.cycles)

        sysexit(ret)
    # pylint: disable-msg=broad-except
    except Exception as exc:
        print(f'\nError: {exc}', file=stderr)
        if debug:
            print(format_exc(chain=False), file=stderr)
        sysexit(1)
    except KeyboardInterrupt:
        sysexit(2)


if __name__ == '__main__':
    main()
