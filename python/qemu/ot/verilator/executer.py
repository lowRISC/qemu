"""Verilator wrapper."""

# Copyright (c) 2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

from collections import deque
from fcntl import fcntl, F_GETFL, F_SETFL
from io import BufferedRandom, TextIOWrapper
from os import O_NONBLOCK, close, getcwd, rename, symlink, unlink
from os.path import (abspath, basename, exists, isfile, islink,
                     join as joinpath, realpath, splitext)
from select import POLLIN, POLLERR, POLLHUP, poll as sel_poll
from shutil import copyfile
from subprocess import Popen, PIPE, TimeoutExpired
from sys import stderr
from tempfile import mkstemp
from threading import Thread
from time import sleep, time as now
from traceback import format_exc
from typing import NamedTuple, Optional, Union

import logging
import re

from ot.rom.image import ROMImage
from ot.util.elf import ElfBlob
from ot.util.file import guess_file_type, make_vmem_from_elf
from ot.util.log import ColorLogFormatter
from ot.util.misc import HexInt, split_map_join

from . import DEFAULT_TIMEOUT
from .filemgr import VtorFileManager


class VtorVcpDescriptor(NamedTuple):
    """ Virtual communication port."""

    vcpid: str
    """VCP identifier."""
    pty: BufferedRandom
    """Attached pseudo terminal."""
    buffer: bytearray
    """Data buffer."""
    logger: logging.Logger
    """Associated logger."""


class VtorMemRange(NamedTuple):
    """Memory range (for an initializable memory device)."""

    address: HexInt
    """Base address."""
    size: HexInt
    """Size (in bytes)."""


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

    START_TIMEOUT = 2.0
    """Initial timeout to load and query Verilator."""

    def __init__(self, vfm: VtorFileManager, verilator: str,
                 profile: Optional[str], debug: bool = False):
        self._log = logging.getLogger('vtor.exec')
        self._vlog = logging.getLogger('vtor.out')
        self._profile = profile
        self._debug = debug
        self._verilator = verilator
        self._fm = vfm
        self._artifact_name: Optional[str] = None
        self._save_xlog = False
        self._link_xlog = False
        self._gen_wave = False
        # parsed communication ports from Verilator
        self._ports: dict[str, Union[int, str]] = {}
        # where Verilator stores the execution log file
        self._xlog_path: Optional[str] = None
        self._vcps: dict[int, VtorVcpDescriptor] = {}
        self._poller = sel_poll()
        self._resume = False
        self._threads: list[Thread] = []
        self._devices: dict[str, VtorMemRange[int, int]] = {}
        self._secret_file: Optional[str] = None
        self._device_aliases: dict[str, str] = {}

    @classmethod
    def _simplifly_cli(cls, args: list[str]) -> str:
        """Shorten the Verilator command line."""
        return ' '.join(args)
        return ' '.join(split_map_join(',', arg,
                                       lambda part: split_map_join('=', part,
                                                                   basename))
                        for arg in args)

    def enable_exec_log(self, save_log: bool, link_log: bool = False) -> None:
        """Configure management of the execution log file."""
        self._save_xlog = save_log
        self._link_xlog = link_log

    def generate_wave(self, enable: bool) -> None:
        """Enable generation of an FST wave file."""
        self._gen_wave = enable

    def show_init_devices(self):
        """Show initializable devices"""
        if not self._devices:
            self._load_devices()
        width = max(len(d) for d in self._devices) + 1
        for name, mrg in self._devices.items():
            print(f'{name:{width}s} 0x{mrg.address:08x} '
                  f'{mrg.size // 1024:5d} KiB')

    @property
    def artifact_name(self) -> Optional[str]:
        """Return the selected name for artifacts, if any."""
        return self._artifact_name

    @artifact_name.setter
    def artifact_name(self, basepath: str):
        """Set the base path for all artifact files."""
        # store the absolute path, as Verilator runs from a temporary directory
        self._artifact_name = abspath(basepath)

    @property
    def rom_devices(self) -> list[str]:
        """Return a list of supported ROM devices."""
        if not self._devices:
            self._load_devices()
        return [d for d in self._devices if d.startswith('rom')]

    @property
    def flash_devices(self) -> list[str]:
        """Return a list of supported embedded flash devices."""
        if not self._devices:
            self._load_devices()
        return [d for d in self._devices if d.startswith('flash')]

    @property
    def ram_devices(self) -> list[str]:
        """Return a list of supported RAM devices."""
        if not self._devices:
            self._load_devices()
        return [d for d in self._devices if d.startswith('ram')]

    @property
    def otp_device(self) -> Optional[str]:
        """Return the name of the OTP device, if any."""
        if not self._devices:
            self._load_devices()
        return 'otp' if 'otp' in self._devices else None

    @property
    def secret_file(self) -> Optional[str]:
        """Secret file observer."""
        return self._secret_file

    @secret_file.setter
    def secret_file(self, file_path: str) -> None:
        """Secret file modifier."""
        if not isinstance(file_path, str) or not isfile(file_path):
            raise FileNotFoundError(f'No such secret file {file_path}')
        self._secret_file = file_path

    def verilate(self, rom_files: list[str], ram_files: list[str],
                 flash_files: list[str], app_files: list[str],
                 otp: Optional[str], timeout: float = None,
                 cycles: Optional[int] = None) -> int:
        """Execute a Verilator simulation.

           :param rom_files: optional list of files to load in ROMs
           :param ram_files: optional list of files to load in RAM
           :param flash_files: optional list of files to load in eFlash
           :param app_files: optional list of application ELF files to execute
           :param otp: optional file to load as OTP image
           :param timeout: optional max execution delay in seconds
           :paran cycles: optional max execution cycles
        """
        workdir = self._fm.tmp_dir
        self._log.debug('Work dir: %s', workdir)
        if not timeout:
            timeout = DEFAULT_TIMEOUT
        ret = None
        xend = None
        proc = None
        simulate = False
        log_q: Optional[deque] = None
        profile_file = None
        if not self._devices:
            self._load_devices()
        try:
            args = [self._verilator]
            mem_init: dict[str, str] = {}
            self._assign_init(mem_init, 'rom', rom_files)
            self._assign_init(mem_init, 'flash', flash_files)
            self._assign_init(mem_init, 'ram', ram_files)
            self._assign_apps(mem_init, app_files)
            args.extend(self._make_init(mem_init))
            app_name = self._get_app_name(app_files, flash_files, ram_files,
                                          rom_files)
            if not app_name:
                raise RuntimeError('Unable to find an application to run')
            if not self._artifact_name:
                basepath = splitext(basename(app_name))[0]
                self._artifact_name = realpath(joinpath(getcwd(), basepath))
            if self._profile:
                args.append('--prof-pgo')
                profile_file = abspath(self._profile)
                if isfile(profile_file):
                    self._log.info('Using profile file %s', profile_file)
                    args.append(profile_file)
            if otp:
                if not self.otp_device:
                    raise ValueError('Verilator does not support OTP device')
                otp = abspath(otp)
                if not isfile(otp):
                    raise ValueError(f'No such OTP file: {otp}')
                args.append(f'--meminit=otp,{otp}')
            if cycles:
                args.append(f'--term-after-cycles={cycles}')
            if self._gen_wave:
                wave_name = f'{self._artifact_name}.fst'
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
                    qline = self._parse_verilator_output(qline, err)
                    if qline:
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

    def _load_devices(self) -> None:
        args = [self._verilator, '--meminit=list']
        out = ''
        with Popen(args, bufsize=1, cwd=self._fm.tmp_dir, encoding='utf-8',
                   errors='ignore', text=True, stdout=PIPE) as proc:
            proc.wait(self.START_TIMEOUT)
            out = proc.stdout.read()
        for mro in re.finditer(r"'(?P<name>\w+)'.*"
                               r"\[(?P<start>0x[0-9a-f]+),\s*"
                               r"(?P<end>0x[0-9a-f]+)\]", out):
            self._devices[mro.group('name')] = VtorMemRange(
                start := HexInt(mro.group('start'), 16),
                HexInt(mro.group('end'), 16) - start + 1)
        # handle the special DJ case where RAM is call CTN_RAM
        if 'ctn_ram' in self._devices:
            # rename any existing ram devices
            ram_devices = sorted(d for d in self._devices
                                 if d.startswith('ram'))
            renamed_devices: dict[str, str] = {}
            for rpos, ram_dev in enumerate(ram_devices, 1):
                ram_name = f'ram{rpos}'
                renamed_devices[ram_name] = self._devices.pop(ram_dev)
                self._device_aliases[ram_name] = ram_dev
            # insert CTM_RAM as the first RAM device, since this is likely the
            # one to be used for loading application
            self._devices['ram0'] = self._devices.pop('ctn_ram')
            self._device_aliases['ram0'] = 'ctn_ram'
            self._devices.update(renamed_devices)

    def _assign_init(self, mem_init: dict[str, str], dev_kind: str,
                     dev_files: list[str]) -> list[str]:
        known_devs = getattr(self, f'{dev_kind}_devices', [])
        if len(dev_files) > len(known_devs):
            raise ValueError(f'Verilator does not support {len(dev_files)} '
                             f'{dev_kind.upper()} images')
        for dpos, (kdev, dev_file) in enumerate(zip(known_devs, dev_files)):
            if not dev_file:
                continue
            file_kind = guess_file_type(dev_file)
            if dev_kind == 'rom':
                size = self._devices[kdev].size
                real_file = self._convert_rom_file(file_kind, dev_file, size,
                                                   dpos)
            else:
                real_file = self._convert_app_file(file_kind, dev_file)
            mem_init[kdev] = real_file

    def _assign_apps(self, mem_init: dict[str, str], app_files: list[str]):
        for app_file in app_files:
            file_kind = guess_file_type(app_file)
            base_file = basename(app_file)
            # only ELF file contains meta information to know the load address
            # if any other type of file is used (bin/vmem/svmem/hex), the
            # load destination device needs to be specified, assigning the app
            # to a known device
            if file_kind != 'elf':
                raise ValueError(f'No known address to load {base_file}')
            # load the ELF to retrieve load address and size
            elf = ElfBlob()
            with open(app_file, 'rb') as efp:
                elf.load(efp)
            app_addr = elf.load_address
            app_size = elf.size
            app_end = app_addr + app_size
            # find the destination device based on the extracted meta info
            for dev_name, mrg in self._devices.items():
                dev_end = mrg.address + mrg.size
                if mrg.address <= app_addr < dev_end:
                    real_name = self._device_aliases.get(dev_name, dev_name)
                    if app_end > dev_end:
                        raise ValueError(f'{base_file} cannot fit in '
                                         f'{real_name}')
                    if dev_name in mem_init:
                        raise ValueError(f'{base_file} overrides another file '
                                         f'in {real_name}')
                    self._log.info('Locating %s in %s', base_file, real_name)
                    # if the destination device is a ROM device, then the file
                    # to load need to be scrambled
                    if dev_name.startswith('rom'):
                        dev_idx = list(self.rom_devices).index(dev_name)
                        mem_init[dev_name] = \
                            self._convert_rom_file(file_kind, app_file,
                                                   mrg.size, dev_idx)
                    # otherwise, a regular VMEM should be enough
                    else:
                        mem_init[dev_name] = self._convert_app_file(file_kind,
                                                                    app_file)
                    break
            else:
                raise ValueError(f'No matching device to fit {base_file}')

    def _make_init(self, mem_init: dict[str, str]) -> list[str]:
        """Build the initialization argument list from the memory device map."""
        args = []
        for dev_name, real_file in mem_init.items():
            real_dev = self._device_aliases.get(dev_name, dev_name)
            args.append(f'--meminit={real_dev},{real_file}')
        return args

    def _get_app_name(self, *args) -> Optional[str]:
        """Try to find which application is tested from the list of initialized
           devices.
        """
        for apps in args:
            if not isinstance(apps, list):
                apps = [apps]
            for app in reversed(apps):
                if app:
                    return app
        return None

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
                if self._link_xlog:
                    assert self._artifact_name is not None
                    log_path = f'{self._artifact_name}.log'
                    tmpslnk = f'{log_path}.tmp'
                    if islink(tmpslnk):
                        unlink(tmpslnk)
                    elif exists(tmpslnk):
                        raise FileExistsError(f'File {tmpslnk} already exists')
                    self._log.debug('Symlinking execution log as %s', log_path)
                    symlink(self._xlog_path, tmpslnk)
                    rename(tmpslnk, log_path)
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
                    # pylint: disable=consider-using-with
                    vcp = open(ptyname, 'rb+', buffering=0)
                    flags = fcntl(vcp, F_GETFL)
                    fcntl(vcp, F_SETFL, flags | O_NONBLOCK)
                    connected.append(vcpid)
                    vcp_lognames.append(vcpid)
                    vcp_log = logging.getLogger(f'{vcplogname}.{vcpid}')
                    vcplognames.append(vcpid)
                    vcp_fno = vcp.fileno()
                    assert vcp_fno not in self._vcps
                    self._vcps[vcp_fno] = VtorVcpDescriptor(vcpid, vcp,
                                                            bytearray(),
                                                            vcp_log)
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
        if not self._artifact_name:
            return
        log_path = f'{self._artifact_name}.log'
        if log_path or not isfile(log_path):
            return
        try:
            unlink(log_path)
            self._log.debug('Old execution log file discarded')
        except OSError as exc:
            self._log.error('Cannot remove previous execution log file: %s',
                            exc)

    def _save_exec_log(self) -> None:
        if not self._save_xlog:
            return
        if not self._xlog_path:
            self._log.error('No execution log file found')
            return
        if not isfile(self._xlog_path):
            self._log.error('Missing execution log file')
            return
        assert self._artifact_name is not None
        log_path = f'{self._artifact_name}.log'
        # discard existing log_path if it has been created as a symlink
        if self._link_xlog and islink(log_path):
            unlink(log_path)
        self._log.debug('Saving execution log as %s', log_path)
        copyfile(self._xlog_path, log_path)

    def _convert_rom_file(self, file_kind: str, file_path: str, size: int,
                          rom_idx: int) -> str:
        if file_kind in ('hex', 'svmem'):
            # no conversion required
            return file_path
        # need to create a scrambled version of the file for the ROM
        if not self._secret_file:
            raise RuntimeError('Cannot create a scrambled ROM image w/o '
                               'ROM secrets')
        rom = ROMImage()
        with open(self._secret_file, 'rt') as cfp:
            rom.load_config(cfp, rom_idx)
        with open(file_path, 'rb') as rfp:
            rom.load(rfp, size)
        prefix = f'{splitext(basename(file_path))[0]}.'
        hex_no, hex_path = mkstemp(suffix='.39.vmem', prefix=prefix,
                                   dir=self._fm.tmp_dir, text=True)
        close(hex_no)
        with open(hex_path, 'wb') as hfp:
            rom.save(hfp, 'svmem')
        self._log.debug('ROM#%d: using temp scrambled as ROM file %s, %d bytes',
                         rom_idx, basename(hex_path), size)
        return hex_path

    def _convert_app_file(self, file_kind: str, file_path: str) -> str:
        if file_kind == 'vmem':
            # no conversion required
            return abspath(file_path)
        if file_kind == 'elf':
            prefix = f'{splitext(basename(file_path))[0]}.'
            vmem_no, vmem_path = mkstemp(suffix='.vmem', prefix=prefix,
                                         dir=self._fm.tmp_dir, text=True)
            with open(vmem_no, 'wt') as vfp:
                make_vmem_from_elf(file_path, vfp, offset=self.VMEM_OFFSET,
                                   chunksize=8, offsetsize=8)
            self._log.debug('Using temp application file %s',
                            basename(vmem_path))
            return vmem_path
        raise RuntimeError(f'Unsupported application file type: {file_kind}')
