#!/usr/bin/env python3

"""Verilator wrapper."""

# Copyright (c) 2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

from atexit import register
from collections import deque
from fcntl import fcntl, F_GETFL, F_SETFL
from io import BufferedRandom, TextIOWrapper
from os import O_NONBLOCK, getcwd, getenv, rename, symlink, unlink, walk
from os.path import (basename, dirname, exists, isdir, isfile, islink,
                     join as joinpath, normpath, realpath, splitext)
from select import POLLIN, POLLERR, POLLHUP, poll as sel_poll
from shutil import copyfile, rmtree
from subprocess import Popen, PIPE, TimeoutExpired
from sys import exit as sysexit, modules, stderr
from tempfile import mkdtemp, mkstemp
from threading import Thread
from time import sleep, time as now
from traceback import format_exc
from typing import Optional, Union

import logging
import re
import sys

QEMU_PYPATH = joinpath(dirname(dirname(dirname(normpath(__file__)))),
                       'python', 'qemu')
sys.path.append(QEMU_PYPATH)

# pylint: disable=wrong-import-position
# pylint: disable=wrong-import-order
# pylint: disable=import-error

getLogger = logging.getLogger

from ot.util.arg import ArgumentParser, FileType  # noqa: E402
from ot.util.file import guess_file_type, make_vmem_from_elf
from ot.util.log import ColorLogFormatter, configure_loggers

VeriVcpDescriptor = tuple[str, BufferedRandom, bytearray, logging.Logger]

DEFAULT_TIMEOUT = 10.0 * 60
"""Default execution timeout."""


class VtorFileManager:
    """File manager.

       Handle temporary directories and files.

       :param keep_temp: do not automatically discard generated files on exit
       :param tmp_dir: store the temporary files in the specified directory
    """

    def __init__(self, keep_temp: bool = False, tmp_dir: Optional[str] = None):
        self._log = getLogger('vtor.file')
        self._keep_temp = keep_temp
        self._base_tmp_dir = tmp_dir
        self._tmp_dir: Optional[str] = None
        register(self._cleanup)

    @property
    def tmp_dir(self) -> str:
        """Provides the main temporary directory for executing Verilator."""
        if not self._tmp_dir:
            self._tmp_dir = mkdtemp(prefix='verilator_ot_dir_',
                                    dir=self._base_tmp_dir)
        return self._tmp_dir

    def _cleanup(self):
        if self._tmp_dir and isdir(self._tmp_dir):
            if self._keep_temp:
                self._log.warning('Preserving temp dir %s', self._tmp_dir)
                return
            self._log.debug('Removing temp dir %s', self._tmp_dir)
            rmtree(self._tmp_dir)
            self._tmp_dir = None


class VtorExecuter:
    """Verilator wrapper.

        -c|--term-after-cycles=N
    """

    VMEM_OFFSET = 0
    """Offset when converting BM test to VMEM file."""

    ANSI_CRE = re.compile(rb'(\x9B|\x1B\[)[0-?]*[ -\/]*[@-~]')
    """ANSI escape sequences."""

    DV_CRE = re.compile(r'^(\d+):\s\(../.*?\)\s(.*)$')
    """DV macro messages."""

    DEADLOCK = 125
    """Default error code when Verilator is stuck."""

    def __init__(self, vfm: VtorFileManager, verilator: str,
                 profile: Optional[str], debug: bool = False):
        self._log = getLogger('vtor.exec')
        self._vlog = getLogger('vtor.out')
        self._profile = profile
        self._debug = debug
        self._verilator = verilator
        self._fm = vfm
        # parsed communication ports from Verilator
        self._ports: dict[str, Union[int, str]] = {}
        # where Verilator stores the execution log file
        self._xlog_path: Optional[str] = None
        # where to copy the execution log file, if requested
        self._dest_log_path: Optional[str] = None
        # where to create a symbolic list to the execution log file
        self._link_log_path: Optional[str] = None
        self._vcps: dict[int, VeriVcpDescriptor] = {}
        self._poller = sel_poll()
        self._resume = False
        self._threads: list[Thread] = []

    @classmethod
    def _simplifly_cli(cls, args: list[str]) -> str:
        """Shorten the Verilator commmand line."""
        arggen = (','.join((basename(path) for path in arg.split(','))) for arg in args)
        return ' '.join(arggen)

    def link_log_file(self, logpath: str) -> None:
        """Request to create a link to the live log file."""
        self._link_log_path = logpath

    def save_execution_log_file(self, logpath: str) -> None:
        """Request to store the execution log file."""
        self._dest_log_path = logpath

    def verilate(self, rom: str, flash: str, otp: Optional[str],
                 gen_wave: bool = False, timeout: float = None,
                 cycles: Optional[int] = None) -> int:
        """Execute a Verilator simulation."""
        workdir = self._fm.tmp_dir
        self._log.debug('Work dir: %s', workdir)
        if not timeout:
            timeout = DEFAULT_TIMEOUT
        ret = None
        xend = None
        proc = None
        simulate = False
        log_q: Optional[deque] = None
        flash_file = self._convert_app_file(flash)
        profile_file = None
        try:
            args = [self._verilator]
            args.append(f'--meminit=rom,{rom}')
            args.append(f'--meminit=flash0,{flash_file}')
            if self._profile:
                args.append('--prof-pgo')
                profile_file = normpath(self._profile)
                if isfile(profile_file):
                    self._log.info('Using profile file %s', profile_file)
                    args.append(profile_file)
            if otp:
                args.append(f'--meminit=otp,{otp}')
            if cycles:
                args.append(f'--term-after-cycles={cycles}')
            if gen_wave:
                wave_name = f'{splitext(basename(flash))[0]}.fst'
                wave_name = realpath(joinpath(getcwd(), wave_name))
                args.append(f'--trace={wave_name}')
            self._log.debug('Executing Verilator as %s',
                            self._simplifly_cli(args))
            # pylint: disable=consider-using-with
            proc = Popen(args,
                         bufsize=1, cwd=workdir,
                         stdout=PIPE, stderr=PIPE,
                         encoding='utf-8', errors='ignore', text=True)
            try:
                proc.wait(0.1)
            except TimeoutExpired:
                pass
            else:
                ret = proc.returncode
                self._log.error('Verilator bailed out: %d', ret)
                raise OSError()
            # if execution starts and the execution log should be generated
            # discards any previous file to avoid leaving a previous version of
            # this file that would not match the current session
            self._discard_exec_log()
            log_q = deque()
            self._resume = True
            for pos, stream in enumerate(('out', 'err')):
                thread = Thread(target=self._vtor_logger,
                                name=f'veri_{stream}_logger',
                                args=(getattr(proc, f'std{stream}'), log_q,
                                      bool(pos)),
                                daemon=True)
                self._threads.append(thread)
                thread.start()
            abstimeout = float(timeout) + now()
            while now() < abstimeout:
                while log_q:
                    err, qline = log_q.popleft()
                    level = logging.ERROR if err else logging.INFO
                    if not err and not simulate:
                        if qline.startswith('Simulation running, '):
                            simulate = True
                            self._connect_vcps(2.0)
                            self._log.info('Simulation begins')
                        else:
                            self._parse_verilator_info(qline)
                    else:
                        qline = self._parse_verilator_output(qline, err)
                        if qline:
                            self._vlog.log(level, qline)
                    break
                else:
                    sleep(0.005)
                xret = proc.poll()
                if xret is None:
                    xret = self._process_vcps()
                if xret is not None:
                    if xend is None:
                        xend = now()
                    ret = xret
                    break
        except (OSError, ValueError) as exc:
            if self._debug:
                print(format_exc(chain=False), file=stderr)
            if ret is None:
                self._log.error('Unable to execute Verilator: %s', exc)
                if proc and proc.poll() is not None:
                    ret = proc.returncode
                else:
                    ret = self.DEADLOCK
        finally:
            self._disconnect_vcps()
            if proc:
                # leave some for Verilator to cleanly complete and flush its
                # streams
                wait = 0.5
                if xend is None:
                    xend = now()
                waited_time = now()
                proc.terminate()
                try:
                    # go through if Verilator has exited on its own
                    proc.wait(wait)
                except TimeoutExpired:
                    # otherwise kill it
                    self._log.error('Force-killing Verilator')
                    proc.kill()
                # ensure completion delay is always satisfied whatever the
                # termination reason
                waited_time = now() - waited_time
                rem = wait - waited_time
                if rem > 0.0:
                    sleep(rem)
                if ret is None:
                    ret = proc.returncode
                self._resume = False
                timeout_count = 10
                while self._threads:
                    timeout_count -= 1
                    if not timeout_count:
                        self._log.error('Cannot terminate %s',
                                        ', '.join((t.name
                                                   for t in self._threads)))
                        break
                    thread = self._threads.pop(0)
                    thread.join(timeout=0.2)
                    if thread.is_alive():
                        self._threads.append(thread)
                # retrieve the remaining log messages
                while log_q:
                    err, qline = log_q.popleft()
                    level = logging.ERROR if err else logging.INFO
                    self._vlog.log(level, qline)
                for msg, logger in zip(proc.communicate(timeout=0.1),
                                       (self._vlog.info, self._vlog.error)):
                    # should have been captured by the logger threads
                    for line in msg.splitlines():
                        line = line.rstrip()
                        if line:
                            logger(line)
                if log_q:
                    # should never happen
                    self._vlog.error('Lost traces')
                self._save_exec_log()
            tmp_profile = joinpath(workdir, 'profile.vlt')
            if isfile(tmp_profile) and profile_file:
                self._log.info('Saving profile file as %s', profile_file)
                copyfile(tmp_profile, profile_file)
        return abs(ret or 0)

    def _vtor_logger(self, stream: TextIOWrapper, queue: deque, err: bool) \
            -> None:
        # worker thread, blocking on VM stdout/stderr
        if not stream:
            self._log.error('No Verilator std%s stream',
                            'err' if err else 'out')
            return
        while self._resume:
            try:
                line = stream.readline().rstrip()
            # pylint: disable=broad-except
            except Exception as exc:
                # do not raise an error if stream disappears
                self._log.error('%s', exc)
                if self._debug:
                    print(format_exc(chain=False), file=stderr)
                break
            if line:
                queue.append((err, line))
            else:
                sleep(0.005)
        self._log.debug('End of std%s logger thread', 'err' if err else 'out')

    def _parse_verilator_info(self, line: str) -> None:
        """Parse initial verilator output which contains communication port
           descriptors as strings.
        """
        if line.startswith('remote_bitbang_port '):
            self._ports['jtag'] = int(line.split(' ')[1])
            return
        if line.startswith('GPIO: FIFO pipes'):
            parts = line.split(' ')
            for gdir in ('read', 'write'):
                pos = parts.index(f'({gdir})')
                if pos > 0:
                    self._ports[f'gpio_{gdir[0]}'] = parts[pos-1]
            return
        if any((line.startswith(f'{dev}: Created ')
                for dev in ('UART', 'SPI'))):
            parts = line.split('.')[0].split(' ')
            self._ports[parts[-1]] = parts[-3]
            return

    def _parse_verilator_output(self, line: str, err: bool) -> str:
        if err:
            return line
        dvmo = self.DV_CRE.match(line)
        if dvmo:
            return ' '.join(dvmo.groups())
        if not line.startswith('TOP.chip_sim_tb'):
            return line
        if not self._xlog_path:
            trace_msg = 'Writing execution trace to '
            pos = line.find(trace_msg)
            if pos >= 0:
                self._xlog_path = realpath(joinpath(self._fm.tmp_dir,
                                                    line[pos+len(trace_msg):]))
                self._log.info('Execution log: %s', self._xlog_path)
                if self._link_log_path:
                    tmpslnk = f'{self._link_log_path}.tmp'
                    if islink(tmpslnk):
                        unlink(tmpslnk)
                    elif exists(tmpslnk):
                        raise FileExistsError(f'File {tmpslnk} already exists')
                    symlink(self._xlog_path, tmpslnk)
                    rename(tmpslnk, self._link_log_path)
                return ''
        return line

    def _connect_vcps(self, delay: float):
        connect_map = {k: v for k, v in self._ports.items()
                       if k.startswith('uart')}
        timeout = now() + delay
        # ensure that QEMU starts and give some time for it to set up
        # when multiple VCPs are set to 'wait', one VCP can be connected at
        # a time, i.e. QEMU does not open all connections at once.
        vcp_lognames = []
        vcplogname = 'vtor'
        vcplognames = []
        while connect_map:
            if now() > timeout:
                minfo = ', '.join(f'{d} @ {r}'
                                  for d, r in connect_map.items())
                raise TimeoutError(f'Cannot connect to Verilator VCPs: {minfo}')
            connected = []
            for vcpid, ptyname in connect_map.items():
                try:
                    # pylint: disable=pylint: consider-using-with
                    vcp = open(ptyname, 'rb+', buffering=0)
                    flags = fcntl(vcp, F_GETFL)
                    fcntl(vcp, F_SETFL, flags | O_NONBLOCK)
                    connected.append(vcpid)
                    vcp_lognames.append(vcpid)
                    vcp_log = logging.getLogger(f'{vcplogname}.{vcpid}')
                    vcplognames.append(vcpid)
                    vcp_fno = vcp.fileno()
                    assert vcp_fno not in self._vcps
                    self._vcps[vcp_fno] = (vcpid, vcp, bytearray(), vcp_log)
                    self._log.debug('VCP %s connected to pty %s',
                                    vcpid, ptyname)
                    self._poller.register(vcp, POLLIN | POLLERR | POLLHUP)
                except ConnectionRefusedError:
                    continue
                except OSError as exc:
                    self._log.error('Cannot setup Verilator VCP connection %s: '
                                    '%s', vcpid, exc)
                    print(format_exc(chain=False), file=stderr)
                    raise
            # removal from dictionary cannot be done while iterating it
            for vcpid in connected:
                del connect_map[vcpid]
        self._colorize_vcp_log(vcplogname, vcplognames)

    def _disconnect_vcps(self):
        for _, vcp, _, _ in self._vcps.values():
            self._poller.unregister(vcp)
            vcp.close()
        self._vcps.clear()

    def _process_vcps(self) -> Optional[int]:
        ret = None
        for vfd, event in self._poller.poll(0.01):
            if event in (POLLERR, POLLHUP):
                self._poller.modify(vfd, 0)
                continue
            _, vcp, vcp_buf, vcp_log = self._vcps[vfd]
            try:
                data = vcp.read(256)
            except TimeoutError:
                self._log.error('Unexpected timeout w/ poll on %s', vcp)
                continue
            if not data:
                continue
            vcp_buf += data
            lines = vcp_buf.split(b'\n')
            vcp_buf[:] = bytearray(lines[-1])
            for line in lines[:-1]:
                line = self.ANSI_CRE.sub(b'', line)
                sline = line.decode('utf-8', errors='ignore').rstrip()
                level = logging.INFO
                vcp_log.log(level, sline)
            if ret is not None:
                # match for exit sequence on current VCP
                break
        return ret

    def _colorize_vcp_log(self, logbase: str, lognames: list[str]) -> None:
        vlog = logging.getLogger(logbase)
        clr_fmt = None
        while vlog:
            for hdlr in vlog.handlers:
                if isinstance(hdlr.formatter, ColorLogFormatter):
                    clr_fmt = hdlr.formatter
                    break
            vlog = vlog.parent
        if not clr_fmt:
            return
        for color, logname in enumerate(sorted(lognames)):
            clr_fmt.add_logger_colors(f'{logbase}.{logname}', color)

    def _discard_exec_log(self) -> None:
        if not self._dest_log_path or not isfile(self._dest_log_path):
            return
        try:
            unlink(self._dest_log_path)
            self._log.debug('Old execution log file discarded')
        except OSError as exc:
            self._log.error('Cannot remove previous execution log file: %s',
                            exc)

    def _save_exec_log(self) -> None:
        if not self._dest_log_path:
            return
        if not self._xlog_path:
            self._log.error('No execution log file found')
            return
        if not isfile(self._xlog_path):
            self._log.error('Missing execution log file')
            return
        copyfile(self._xlog_path, self._dest_log_path)

    def _convert_app_file(self, filepath: str) -> str:
        kind = guess_file_type(filepath)
        if kind == 'vmem':
            # no conversion required
            return filepath
        if kind == 'elf':
            prefix = f'{splitext(basename(filepath))[0]}.'
            vmem_no, vmem_path = mkstemp(suffix='.vmem', prefix=prefix,
                                         dir=self._fm.tmp_dir, text=True)
            with open(vmem_no, 'wt') as vfp:
                make_vmem_from_elf(filepath, vfp, offset=self.VMEM_OFFSET,
                                   chunksize=8, offsetsize=8)
            self._log.debug('Using temp application file %s',
                            basename(vmem_path))
            return vmem_path
        raise RuntimeError(f'Unsupported application file type: {kind}')


def main():
    """Main routine."""
    debug = False
    try:
        desc = modules[__name__].__doc__.split('.', 1)[0].strip()
        argparser = ArgumentParser(description=f'{desc}.')
        files = argparser.add_argument_group(title='Files')
        files.add_argument('flash', nargs=1, type=FileType('rb'),
                           metavar='ELF|VMEM', help='flash file')
        files.add_argument('-V', '--verilator',
                           help='Verilator executable')
        files.add_argument('-R', '--rom', type=FileType('rt'),
                           metavar='VMEM', required=True,
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

        # let ArgumentParser validate the paths
        flash = realpath(args.flash[0].name)
        otp = realpath(args.otp.name) if args.otp else None
        rom = realpath(args.rom.name)
        args.flash[0].close()
        args.rom.close()
        if args.otp:
            args.otp.close()

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
                        short_veri = verilator[len(top_dir)+1:]
                        log.debug('Found Verilator tool as %s', short_veri)
                        break
        if not isfile(verilator):
            argparser.error('Verilator tool not found')

        vfm = VtorFileManager(args.keep_tmp, args.tmp_dir)
        vtor = VtorExecuter(vfm, verilator, args.profile, debug)
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
