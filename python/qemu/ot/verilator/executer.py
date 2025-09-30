"""Verilator wrapper."""

# Copyright (c) 2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

from collections import deque
from fcntl import fcntl, F_GETFL, F_SETFL
from io import BufferedRandom, TextIOWrapper
from os import O_NONBLOCK, getcwd, rename, symlink, unlink
from os.path import (basename, exists, isfile, islink, join as joinpath,
                     normpath, realpath, splitext)
from select import POLLIN, POLLERR, POLLHUP, poll as sel_poll
from shutil import copyfile
from subprocess import Popen, PIPE, TimeoutExpired
from sys import stderr
from tempfile import mkstemp
from threading import Thread
from time import sleep, time as now
from traceback import format_exc
from typing import Optional, Union

import logging
import re

from ot.util.file import guess_file_type, make_vmem_from_elf
from ot.util.log import ColorLogFormatter

from . import DEFAULT_TIMEOUT
from .filemgr import VtorFileManager


VeriVcpDescriptor = tuple[str, BufferedRandom, bytearray, logging.Logger]


class VtorExecuter:
    """Verilator wrapper."""

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
        self._log = logging.getLogger('vtor.exec')
        self._vlog = logging.getLogger('vtor.out')
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
        """Shorten the Verilator command line."""
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
