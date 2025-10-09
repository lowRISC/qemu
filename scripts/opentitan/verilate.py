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

from ot.util.arg import ArgumentParser  # noqa: E402
from ot.util.log import configure_loggers  # noqa: E402
from ot.verilator import DEFAULT_TIMEOUT  # noqa: E402
from ot.verilator.filemgr import VtorFileManager  # noqa: E402
from ot.verilator.executer import VtorExecuter  # noqa: E402


def main():
    """Main routine."""
    debug = False
    try:
        desc = modules[__name__].__doc__.split('.', 1)[0].strip()
        argparser = ArgumentParser(description=f'{desc}.')
        files = argparser.add_argument_group(title='Files')
        files.add_argument('app', nargs='*', metavar='ELF',
                           help='application file, may be repeated')
        files.add_argument('-V', '--verilator',
                           help='Verilator executable')
        files.add_argument('-R', '--rom', metavar='FILE', action='append',
                           default=[],
                           help='ROM file, may be repeated')
        files.add_argument('-M', '--ram', metavar='FILE', action='append',
                           default=[],
                           help='RAM file, may be repeated')
        files.add_argument('-F', '--flash', metavar='FILE', action='append',
                           default=[],
                           help='eFlash file, may be repeated')
        files.add_argument('-O', '--otp', metavar='VMEM',
                           help='OTP file')
        files.add_argument('-K', '--keep-tmp', action='store_true',
                           help='do not automatically remove temporary files '
                                'and dirs on exit')
        files.add_argument('-D', '--tmp-dir',
                           help='store the temporary files in the specified '
                                'directory')
        files.add_argument('-c', '--config', metavar='CFG',
                           help='input QEMU OT config file '
                                '(required w/ non scrambled ROM images)')
        veri = argparser.add_argument_group(title='Verilator')
        veri.add_argument('-a', '--artifact-name', metavar='PREFIX',
                          help='set an alternative artifact name '
                               '(default is derived from the application name)')
        veri.add_argument('-C', '--cycles', type=int,
                          help='exit after the specified cycles')
        veri.add_argument('-I', '--show-init', action='store_true',
                          help='show initializable devices')
        veri.add_argument('-k', '--timeout', metavar='SECONDS', type=float,
                          help=f'exit after the specified seconds '
                               f'(default: {DEFAULT_TIMEOUT} secs)')
        veri.add_argument('-l', '--link-exec-log', action='store_true',
                          help='create a symlink to the execution log file')
        veri.add_argument('-P', '--profile', metavar='FILE',
                          help='enable/manage profile file')
        veri.add_argument('-w', '--wave-gen', action='store_true',
                          help='generate output wave file')
        veri.add_argument('-x', '--save-exec-log', action='store_true',
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

        log = configure_loggers(args.verbose, 'vtor', -1, 'elf', -1, 'romimg',
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

        if args.config:
            vtor.secret_file = args.config

        filenames = list(args.rom)  # need a copy of the list
        filenames.append(args.otp)
        filenames.extend(args.ram)
        filenames.extend(args.flash)
        if args.app:
            filenames.extend(args.app)
        for filename in filenames:
            if filename and not isfile(filename):
                argparser.error(f'No such file: {filename}')

        if args.artifact_name:
            if not isdir(realpath(dirname(args.artifact_name))):
                argparser.error('Invalid directory for execution log file')
            vtor.artifact_name = args.artifact_name
        vtor.enable_exec_log(args.save_exec_log, args.link_exec_log)
        vtor.generate_wave(args.wave_gen)
        ret = vtor.verilate(args.rom, args.ram, args.flash, args.app, args.otp,
                            timeout=args.timeout, cycles=args.cycles)

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
