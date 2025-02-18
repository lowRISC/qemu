#!/usr/bin/env python3

# Copyright (c) 2023-2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""OpenTitan QEMU unit test sequencer.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from argparse import ArgumentParser, FileType, Namespace
try:
    _HJSON_ERROR = None
    from hjson import load as jload
except ImportError as hjson_exc:
    _HJSON_ERROR = str(hjson_exc)
    def hjload(*_, **__):  # noqa: E301
        """dummy func if HJSON module is not available"""
        return {}
from os import close, linesep, unlink
from os.path import (basename, dirname, isfile, join as joinpath, normpath,
                     relpath)
from tempfile import mkstemp
from time import sleep
from traceback import format_exc
from typing import Optional

import sys

QEMU_PYPATH = joinpath(dirname(dirname(dirname(normpath(__file__)))),
                       'python', 'qemu')
sys.path.append(QEMU_PYPATH)

# pylint: disable=wrong-import-position
# pylint: disable=wrong-import-order
# pylint: disable=import-error

from ot.pyot import DEFAULT_TIMEOUT, DEFAULT_TIMEOUT_FACTOR
from ot.pyot.executer import QEMUExecuter
from ot.pyot.filemgr import QEMUFileManager
from ot.pyot.util import ResultFormatter
from ot.util.log import ColorLogFormatter, RemoteLogService, configure_loggers

DEFAULT_MACHINE = 'ot-earlgrey'
DEFAULT_DEVICE = 'localhost:8000'


def main():
    """Main routine."""
    debug = True
    qemu_dir = normpath(joinpath(dirname(dirname(dirname(normpath(__file__))))))
    qemu_paths = [normpath(joinpath(qemu_dir, 'build', 'qemu-system-riscv32'))]
    if sys.platform == 'darwin':
        qemu_paths.append(f'{qemu_paths[0]}-unsigned')
    for qpath in qemu_paths:
        if isfile(qpath):
            qemu_path = qpath
            break
    else:
        qemu_path = None
    tmp_result: Optional[str] = None
    result_file: Optional[str] = None
    rlog: Optional[RemoteLogService] = None
    try:
        args: Optional[Namespace] = None
        desc = sys.modules[__name__].__doc__.split('.', 1)[0].strip()
        argparser = ArgumentParser(description=f'{desc}.')
        qvm = argparser.add_argument_group(title='Virtual machine')
        rel_qemu_path = relpath(qemu_path) if qemu_path else '?'
        qvm.add_argument('-D', '--start-delay', type=float, metavar='DELAY',
                         help='QEMU start up delay before initial comm')
        qvm.add_argument('-i', '--icount',
                         help='virtual instruction counter with 2^ICOUNT clock '
                              'ticks per inst. or \'auto\'')
        qvm.add_argument('-L', '--log_file',
                         help='log file for trace and log messages')
        qvm.add_argument('-M', '--variant',
                         help='machine variant (machine specific)')
        qvm.add_argument('-N', '--log', action='append',
                         help='log message types')
        qvm.add_argument('-m', '--machine',
                         help=f'virtual machine (default to {DEFAULT_MACHINE})')
        qvm.add_argument('-Q', '--opts', action='append',
                         help='QEMU verbatim option (can be repeated)')
        qvm.add_argument('-q', '--qemu',
                         help=f'path to qemu application '
                              f'(default: {rel_qemu_path})')
        qvm.add_argument('-P', '--vcp', action='append',
                         help='serial port devices (default: use serial0)')
        qvm.add_argument('-p', '--device',
                         help=f'serial port device name / template name '
                              f'(default to {DEFAULT_DEVICE})')
        qvm.add_argument('-t', '--trace', type=FileType('rt', encoding='utf-8'),
                         help='trace event definition file')
        qvm.add_argument('-S', '--first-soc', default=None,
                         help='Identifier of the first SoC, if any')
        qvm.add_argument('-s', '--singlestep', action='store_const',
                         const=True,
                         help='enable "single stepping" QEMU execution mode')
        qvm.add_argument('-U', '--muxserial', action='store_const',
                         const=True,
                         help='enable multiple virtual UARTs to be muxed into '
                              'same host output channel')
        files = argparser.add_argument_group(title='Files')
        files.add_argument('-b', '--boot',
                           metavar='file', help='bootloader 0 file')
        files.add_argument('-c', '--config', metavar='HJSON',
                           type=FileType('rt', encoding='utf-8'),
                           help='path to HJSON configuration file')
        files.add_argument('-e', '--embedded-flash', metavar='BUS', type=int,
                           help='generate an eflash image file for MTD bus')
        files.add_argument('-f', '--flash', metavar='RAW',
                           help='SPI flash image file')
        files.add_argument('-g', '--otcfg', metavar='file',
                           help='configuration options for OpenTitan devices')
        files.add_argument('-K', '--keep-tmp', action='store_true',
                           help='Do not automatically remove temporary files '
                                'and dirs on exit')
        files.add_argument('-l', '--loader', metavar='file',
                           help='ROM trampoline to execute, if any')
        files.add_argument('-O', '--otp-raw', metavar='RAW',
                           help='OTP image file')
        files.add_argument('-o', '--otp', metavar='VMEM', help='OTP VMEM file')
        files.add_argument('-r', '--rom', metavar='ELF', action='append',
                           help='ROM file (can be repeated, in load order)')
        files.add_argument('-w', '--result', metavar='CSV',
                           help='path to output result file')
        files.add_argument('-x', '--exec', metavar='file',
                           help='application to load')
        files.add_argument('-X', '--rom-exec', action='store_const', const=True,
                           help='load application as ROM image '
                                '(default: as kernel)')
        exe = argparser.add_argument_group(title='Execution')
        exe.add_argument('-F', '--filter', metavar='TEST', action='append',
                         help='run tests with matching filter, prefix with "!" '
                              'to exclude matching tests')
        exe.add_argument('-k', '--timeout', metavar='SECONDS', type=float,
                         help=f'exit after the specified seconds '
                              f'(default: {DEFAULT_TIMEOUT} secs)')
        exe.add_argument('-z', '--list', action='store_true',
                         help='show a list of tests to execute and exit')
        exe.add_argument('-R', '--summary', action='store_true',
                         help='show a result summary')
        exe.add_argument('-T', '--timeout-factor', type=float, metavar='FACTOR',
                         default=DEFAULT_TIMEOUT_FACTOR,
                         help='timeout factor')
        exe.add_argument('-Z', '--zero', action='store_true',
                         help='do not error if no test can be executed')
        extra = argparser.add_argument_group(title='Extras')
        extra.add_argument('-v', '--verbose', action='count',
                           help='increase verbosity')
        extra.add_argument('-V', '--vcp-verbose', action='count',
                           help='increase verbosity of QEMU virtual comm ports')
        extra.add_argument('-d', dest='dbg', action='store_true',
                           help='enable debug mode')
        extra.add_argument('--quiet', action='store_true',
                           help='quiet logging: only be verbose on errors')
        extra.add_argument('--log-time', action='store_true',
                           help='show local time in log messages')
        extra.add_argument('--log-udp', type=int, metavar='UDP_PORT',
                           help='Change UDP port for log messages, '
                                'use 0 to disable')
        extra.add_argument('--debug', action='append', metavar='LOGGER',
                           help='assign debug level to logger(s)')
        extra.add_argument('--info', action='append', metavar='LOGGER',
                           help='assign info level to logger(s)')
        extra.add_argument('--warn', action='append', metavar='LOGGER',
                           help='assign warning level to logger(s)')

        try:
            # all arguments after `--` are forwarded to QEMU
            pos = sys.argv.index('--')
            sargv = sys.argv[1:pos]
            opts = sys.argv[pos+1:]
        except ValueError:
            sargv = sys.argv[1:]
            opts = []
        cli_opts = list(opts)
        args = argparser.parse_args(sargv)
        if args.dbg is not None:
            debug = args.dbg

        if _HJSON_ERROR:
            argparser.error('Missing HJSON module: {_HJSON_ERROR}')

        if args.summary:
            if not args.result:
                tmpfd, tmp_result = mkstemp(suffix='.csv')
                close(tmpfd)
                args.result = tmp_result
            result_file = args.result

        log = configure_loggers(args.verbose, 'pyot',
                                -1, 'flashgen', 'elf', 'otp', 1,
                                args.vcp_verbose or 0,
                                'pyot.vcp', name_width=30,
                                ms=args.log_time, quiet=args.quiet,
                                debug=args.debug, info=args.info,
                                warning=args.warn)[0]

        qfm = QEMUFileManager(args.keep_tmp)
        qfm.set_qemu_src_dir(qemu_dir)

        if args.log_udp != 0:
            rlog = RemoteLogService(port=args.log_udp)
            rlog.start()
            qfm.set_udp_log_port(rlog.port)

        # this is a bit circomvulted, as we need to parse the config filename
        # if any, and load the default values out of the configuration file,
        # without overriding any command line argument that should take
        # precedence. set_defaults() does not check values for validity, so it
        # cannot be used as JSON configuration may also contain invalid values
        json = {}
        if args.config:
            qfm.set_config_dir(dirname(args.config.name))
            json = jload(args.config)
            if 'aliases' in json:
                aliases = json['aliases']
                if not isinstance(aliases, dict):
                    argparser.error('Invalid aliases definitions')
                qfm.define(aliases)
            jdefaults = json.get('default', {})
            xcolors = jdefaults.get('vcp-color', [])
            if xcolors:
                # config file -only option, not exposed to the argparser
                del jdefaults['vcp-color']
                ColorLogFormatter.override_xcolors(xcolors)
            jargs = []
            for arg, val in jdefaults.items():
                is_bool = isinstance(val, bool)
                if is_bool:
                    if not val:
                        continue
                optname = f'--{arg}' if len(arg) > 1 else f'-{arg}'
                if isinstance(val, list):
                    val = QEMUExecuter.flatten(v.split() for v in val)
                    for valit in val:
                        jargs.append(f'{optname}={qfm.interpolate(valit)}')
                else:
                    jargs.append(optname)
                    if is_bool:
                        continue
                    # arg parser expects only string args, and substitute shell
                    # env.
                    val = qfm.interpolate(val)
                    jargs.append(val)
            if jargs:
                jwargs = argparser.parse_args(jargs)
                # pylint: disable=protected-access
                for name, val in jwargs._get_kwargs():
                    if not hasattr(args, name):
                        argparser.error(f'Unknown config file default: {name}')
                    if getattr(args, name) is None:
                        setattr(args, name, val)
            opts = json.get('opts', [])
            if not isinstance(opts, list):
                argparser.error("'opts' should be defined as a list")
            opts = [opt for optline in json.get('opts', [])
                    for opt in optline.split() if opt]
            opts.extend(getattr(args, 'opts', None) or [])
            setattr(args, 'opts', opts)
        elif args.filter:
            argparser.error('Filter option only valid with a config file')
        if cli_opts:
            qopts = getattr(args, 'opts') or []
            qopts.extend(cli_opts)
            setattr(args, 'opts', qopts)
        if args.otcfg and not isfile(args.otcfg):
            argparser.error(f'Invalid OpenTitan configuration file '
                            f'{basename(args.otcfg)}')
        # as the JSON configuration file may contain default value, the
        # argparser default method cannot be used to define default values, or
        # they would take precedence over the JSON defined ones
        defaults = {
            'qemu': qemu_path,
            'timeout': DEFAULT_TIMEOUT,
            'device': DEFAULT_DEVICE,
            'machine': DEFAULT_MACHINE,
        }
        for name, val in defaults.items():
            if getattr(args, name) is None:
                setattr(args, name, val)
        if args.qemu:
            qfm.set_qemu_bin_dir(dirname(args.qemu))
        qexc = QEMUExecuter(qfm, json, args)
        if args.list:
            for tst in qexc.enumerate_tests():
                print(tst)
            sys.exit(0)
        try:
            qexc.build()
        except ValueError as exc:
            if debug:
                print(format_exc(chain=False), file=sys.stderr)
            argparser.error(str(exc))
        ret = qexc.run(debug, args.zero)
        log.debug('End of execution with code %d', ret or 0)
        if rlog:
            # leave extra time for last logging packets to be received
            sleep(0.1)
        sys.exit(ret)
    # pylint: disable=broad-except
    except Exception as exc:
        print(f'{linesep}Error: {exc}', file=sys.stderr)
        if debug:
            print(format_exc(chain=False), file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(2)
    finally:
        if rlog:
            rlog.stop()
        if result_file:
            rfmt = ResultFormatter()
            try:
                rfmt.load(result_file)
                rfmt.show(True)
            # pylint: disable=broad-except
            except Exception as exc:
                print(f'Cannot generate result file: {exc}', file=sys.stderr)
        if tmp_result and isfile(tmp_result):
            unlink(tmp_result)


if __name__ == '__main__':
    main()
