#!/usr/bin/env python3

# Copyright (c) 2023-2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""OpenTitan QEMU unit test sequencer.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from argparse import ArgumentParser, FileType, Namespace
from atexit import register
from collections import defaultdict, deque
from csv import reader as csv_reader, writer as csv_writer
from fnmatch import fnmatchcase
from glob import glob
try:
    # try to use HJSON if available
    from hjson import load as jload
except ImportError:
    # fallback on legacy JSON syntax otherwise
    from json import load as jload
from logging import CRITICAL, DEBUG, INFO, ERROR, NOTSET, WARNING, getLogger
from os import close, curdir, environ, getcwd, linesep, pardir, sep, unlink
from os.path import (abspath, basename, dirname, exists, isabs, isdir, isfile,
                     join as joinpath, normpath, relpath)
from re import Match, compile as re_compile, error as re_error, sub as re_sub
from select import POLLIN, POLLERR, POLLHUP, poll as spoll
from shutil import rmtree
from socket import socket, timeout as LegacyTimeoutError
from subprocess import Popen, PIPE, TimeoutExpired
from sys import argv, exit as sysexit, modules, stderr
from threading import Event, Thread
from tempfile import mkdtemp, mkstemp
from time import time as now
from traceback import format_exc
from typing import Any, Iterator, NamedTuple, Optional

from ot.util.log import ColorLogFormatter, configure_loggers
from ot.util.misc import EasyDict


DEFAULT_MACHINE = 'ot-earlgrey'
DEFAULT_DEVICE = 'localhost:8000'
DEFAULT_TIMEOUT = 60  # seconds
DEFAULT_TIMEOUT_FACTOR = 1.0


class ExecTime(float):
    """Float with hardcoded formatter.
    """

    def __repr__(self) -> str:
        return f'{self*1000:.0f} ms'


class TestResult(NamedTuple):
    """Test result.
    """
    name: str
    result: str
    time: ExecTime
    icount: Optional[str]
    error: str


class ResultFormatter:
    """Format a result CSV file as a simple result table."""

    def __init__(self):
        self._results = []

    def load(self, csvpath: str) -> None:
        """Load a CSV file (generated with QEMUExecuter) and parse it.

           :param csvpath: the path to the CSV file.
        """
        with open(csvpath, 'rt', encoding='utf-8') as cfp:
            csv = csv_reader(cfp)
            for row in csv:
                self._results.append(row)

    def show(self, spacing: bool = False) -> None:
        """Print a simple formatted ASCII table with loaded CSV results.

           :param spacing: add an empty line before and after the table
        """
        if spacing:
            print('')
        widths = [max(len(x) for x in col) for col in zip(*self._results)]
        self._show_line(widths, '-')
        self._show_row(widths, self._results[0])
        self._show_line(widths, '=')
        for row in self._results[1:]:
            self._show_row(widths, row)
            self._show_line(widths, '-')
        if spacing:
            print('')

    def _show_line(self, widths: list[int], csep: str) -> None:
        print(f'+{"+".join([csep * (w+2) for w in widths])}+')

    def _show_row(self, widths: list[int], cols: list[str]) -> None:
        line = '|'.join([f' {c:{">" if p else "<"}{w}s} '
                         for p, (w, c) in enumerate(zip(widths, cols))])
        print(f'|{line}|')


class QEMUWrapper:
    """A small engine to run tests with QEMU.

       :param tcpdev: a host, port pair that defines how to access the TCP
                      Virtual Com Port of QEMU first UART
       :param debug: whether running in debug mode
    """
    # pylint: disable=too-few-public-methods

    EXIT_ON = rb'(PASS|FAIL)!\r'
    """Matching strings to search for in guest output.

       The return code of the script is the position plus the GUEST_ERROR_OFFSET
       in the above RE group when matched, except first item which is always 0.
       This offset is used to differentiate from QEMU own return codes. QEMU may
       return negative values, which are the negative value of POSIX signals,
       such as SIGABRT.
    """

    ANSI_CRE = re_compile(rb'(\x9B|\x1B\[)[0-?]*[ -\/]*[@-~]')
    """ANSI escape sequences."""

    GUEST_ERROR_OFFSET = 40
    """Offset for guest errors. Should be larger than the host max signal value.
    """

    NO_MATCH_RETURN_CODE = 100
    """Return code when no matching string is found in guest output."""

    LOG_LEVELS = {'D': DEBUG, 'I': INFO, 'E': ERROR}
    """OpenTitan log levels."""

    def __init__(self, debug: bool):
        self._debug = debug
        self._log = getLogger('pyot')
        self._qlog = getLogger('pyot.qemu')

    def run(self, tdef: EasyDict[str, Any]) -> tuple[int, ExecTime, str]:
        """Execute the specified QEMU command, aborting execution if QEMU does
           not exit after the specified timeout.

           :param tdef: test definition and parameters
                - command, a list of strings defining the QEMU command to
                           execute with all its options
                - vcp_map: how to connect to QEMU virtual communication ports
                - timeout, the allowed time for the command to execute,
                           specified as a real number
                - expect_result, the expected outcome of QEMU (exit code). Some
                           tests may expect that QEMU terminates with a non-zero
                           exit code
                - context, an option QEMUContextWorker instance, to execute
                           concurrently with the QEMU process. Many tests
                           expect to communicate with the QEMU process.
                - trigger, a string to match on the QEMU virtual comm port
                           output to trigger the context execution. It may be
                           defined as a regular expression.
                - start_delay, the delay to wait before starting the execution
                           of the context once QEMU command has been started.
           :return: a 3-uple of exit code, execution time, and last guest error
        """
        # stdout and stderr belongs to QEMU VM
        # OT's UART0 is redirected to a TCP stream that can be accessed through
        # self._device. The VM pauses till the TCP socket is connected
        xre = re_compile(self.EXIT_ON)
        otre = r'^([' + ''.join(self.LOG_LEVELS.keys()) + r'])\d{5}\s'
        lre = re_compile(otre)
        if tdef.trigger:
            sync_event = Event()
            if tdef.trigger.startswith("r'") and tdef.trigger.endswith("'"):
                try:
                    tmo = re_compile(tdef.trigger[2:-1].encode())
                except re_error as exc:
                    raise ValueError('Invalid trigger regex: {exc}') from exc

                def trig_match(bline):
                    return tmo.match(bline)
            else:
                btrigger = tdef.trigger.encode()

                def trig_match(bline):
                    return bline.find(btrigger) >= 0
        else:
            sync_event = None
            trig_match = None
        ret = None
        proc = None
        xstart = None
        xend = None
        log = self._log
        last_error = ''
        vcp_map = tdef.vcp_map
        vcp_ctxs: dict[int, tuple[str, socket, bytearray]] = {}
        try:
            workdir = dirname(tdef.command[0])
            log.debug('Executing QEMU as %s', ' '.join(tdef.command))
            # pylint: disable=consider-using-with
            proc = Popen(tdef.command, bufsize=1, cwd=workdir, stdout=PIPE,
                         stderr=PIPE, encoding='utf-8', errors='ignore',
                         text=True)
            try:
                proc.wait(0.1)
            except TimeoutExpired:
                pass
            else:
                ret = proc.returncode
                log.error('QEMU bailed out: %d for "%s"', ret, tdef.test_name)
                raise OSError()
            log.debug('Execute QEMU for %.0f secs', tdef.timeout)
            # unfortunately, subprocess's stdout calls are blocking, so the
            # only way to get near real-time output from QEMU is to use a
            # dedicated thread that may block whenever no output is available
            # from the VM. This thread reads and pushes back lines to a local
            # queue, which is popped and logged to the local logger on each
            # loop. Note that Popen's communicate() also relies on threads to
            # perform stdout/stderr read out.
            log_q = deque()
            Thread(target=self._qemu_logger, name='qemu_out_logger',
                   args=(proc, log_q, True)).start()
            Thread(target=self._qemu_logger, name='qemu_err_logger',
                   args=(proc, log_q, False)).start()
            poller = spoll()
            connect_map = vcp_map.copy()
            timeout = now() + tdef.start_delay
            # ensure that QEMU starts and give some time for it to set up
            # when multiple VCPs are set to 'wait', one VCP can be connected at
            # a time, i.e. QEMU does not open all connections at once.
            vcp_lognames = []
            vcplogname = 'pyot.vcp'
            while connect_map:
                if now() > timeout:
                    raise TimeoutError(f'Cannot connect to QEMU VCPs '
                                       f'{", ".join(connect_map)}')
                connected = []
                for vcpid, (host, port) in connect_map.items():
                    try:
                        # timeout for connecting to VCP
                        sock = socket()
                        sock.settimeout(1)
                        sock.connect((host, port))
                        connected.append(vcpid)
                        vcp_name = re_sub(r'^.*[-\.+]', '', vcpid)
                        vcp_lognames.append(vcp_name)
                        vcp_log = getLogger(f'{vcplogname}.{vcp_name}')
                        vcp_ctxs[sock.fileno()] = [vcpid, sock, bytearray(),
                                                   vcp_log]
                        # remove timeout for VCP comm, as poll is used
                        sock.settimeout(None)
                        poller.register(sock, POLLIN | POLLERR | POLLHUP)
                    except ConnectionRefusedError:
                        continue
                    except OSError as exc:
                        log.error('Cannot setup QEMU VCP connection %s: %s',
                                  vcpid, exc)
                        print(format_exc(chain=False), file=stderr)
                        raise
                # removal from dictionary cannot be done while iterating it
                for vcpid in connected:
                    del connect_map[vcpid]
            self._colorize_vcp_log(vcplogname, vcp_lognames)
            xstart = now()
            if tdef.context:
                try:
                    tdef.context.execute('with', sync=sync_event)
                except OSError as exc:
                    ret = exc.errno
                    last_error = exc.strerror
                    raise
                # pylint: disable=broad-except
                except Exception as exc:
                    ret = 126
                    last_error = str(exc)
                    raise
            qemu_exec = f'{basename(tdef.command[0])}: '
            abstimeout = float(tdef.timeout) + now()
            while now() < abstimeout:
                while log_q:
                    err, qline = log_q.popleft()
                    if err:
                        level = self.classify_log(qline, qemux=qemu_exec)
                        if level == INFO and \
                           qline.find('QEMU waiting for connection') >= 0:
                            level = DEBUG
                    else:
                        level = INFO
                    self._qlog.log(level, qline)
                if tdef.context:
                    wret = tdef.context.check_error()
                    if wret:
                        ret = wret
                        last_error = 'Fail to execute worker'
                        raise OSError(wret, last_error)
                xret = proc.poll()
                if xret is not None:
                    if xend is None:
                        xend = now()
                    ret = xret
                    if ret != 0:
                        if ret != tdef.expect_result:
                            logfn = getattr(log, 'critical')
                        else:
                            logfn = getattr(log, 'warning')
                        logfn('Abnormal QEMU termination: %d for "%s"',
                              ret, tdef.test_name)
                    break
                for vfd, event in poller.poll(0.01):
                    if event in (POLLERR, POLLHUP):
                        poller.modify(vfd, 0)
                        continue
                    vcpid, vcp, vcp_buf, vcp_log = vcp_ctxs[vfd]
                    try:
                        data = vcp.recv(4096)
                    except (TimeoutError, LegacyTimeoutError):
                        log.error('Unexpected timeout w/ poll on %s', vcp)
                        continue
                    vcp_buf += data
                    if not vcp_buf:
                        continue
                    lines = vcp_buf.split(b'\n')
                    vcp_buf[:] = bytearray(lines[-1])
                    for line in lines[:-1]:
                        line = self.ANSI_CRE.sub(b'', line)
                        if trig_match and trig_match(line):
                            # reset timeout from this event
                            abstimeout = float(tdef.timeout) + now()
                            log.info('Trigger pattern detected, resuming for '
                                     '%.0f secs', tdef.timeout)
                            sync_event.set()
                            trig_match = None
                        xmo = xre.search(line)
                        if xmo:
                            xend = now()
                            exit_word = xmo.group(1).decode('utf-8',
                                                            errors='ignore')
                            ret = self._get_exit_code(xmo)
                            log.info("Exit sequence detected: '%s' -> %d",
                                     exit_word, ret)
                            if ret == 0:
                                last_error = ''
                            break
                        sline = line.decode('utf-8', errors='ignore').rstrip()
                        lmo = lre.search(sline)
                        if lmo:
                            level = self.LOG_LEVELS.get(lmo.group(1))
                            if level == ERROR:
                                err = re_sub(r'^.*:\d+]', '', sline).lstrip()
                                # be sure not to preserve comma as this char is
                                # used as a CSV separator.
                                last_error = err.strip('"').replace(',', ';')
                        else:
                            level = DEBUG  # fall back when no prefix is found
                        vcp_log.log(level, sline)
                    else:
                        # no match for exit sequence on current VCP
                        continue
                    if ret is not None:
                        # match for exit sequence on current VCP
                        break
                if ret is not None:
                    # match for exit sequence on last VCP
                    break
            if ret is None:
                log.warning('Execution timed out for "%s"', tdef.test_name)
                ret = 124  # timeout
        except (OSError, ValueError) as exc:
            if ret is None:
                log.error('Unable to execute QEMU: %s', exc)
                ret = proc.returncode if proc.poll() is not None else 125
        finally:
            if xend is None:
                xend = now()
            for _, sock, _, _ in vcp_ctxs.values():
                sock.close()
            vcp_ctxs.clear()
            if proc:
                if xend is None:
                    xend = now()
                proc.terminate()
                try:
                    # leave 1 second for QEMU to cleanly complete...
                    proc.wait(1.0)
                except TimeoutExpired:
                    # otherwise kill it
                    log.error('Force-killing QEMU')
                    proc.kill()
                if ret is None:
                    ret = proc.returncode
                # retrieve the remaining log messages
                stdlog = self._qlog.info if ret else self._qlog.debug
                for msg, logger in zip(proc.communicate(timeout=0.1),
                                       (stdlog, self._qlog.error)):
                    for line in msg.split('\n'):
                        line = line.strip()
                        if line:
                            logger(line)
        xtime = ExecTime(xend-xstart) if xstart and xend else 0.0
        return abs(ret) or 0, xtime, last_error

    @classmethod
    def classify_log(cls, line: str, default: int = ERROR,
                     qemux: Optional[str] = None) -> int:
        """Classify log level of a line depending on its content.

           :param line: line to classify
           :param default: defaut log level in no classification is found
           :return: the logger log level to use
        """
        if qemux and line.startswith(qemux):
            # discard QEMU internal messages that cannot be disable from the VM
            return NOTSET
        if (line.find('info: ') >= 0 or
            line.startswith('INFO ') or
            line.find(' INFO ') >= 0):  # noqa
            return INFO
        if (line.find('warning: ') >= 0 or
            line.startswith('WARNING ') or
            line.find(' WARNING ') >= 0):  # noqa
            return WARNING
        if (line.find('debug: ') >= 0 or
            line.startswith('DEBUG ') or
            line.find(' DEBUG ') >= 0):  # noqa
            return DEBUG
        return default

    def _colorize_vcp_log(self, vcplogname: str, lognames: list[str]) -> None:
        vlog = getLogger(vcplogname)
        clr_fmt = None
        while vlog:
            for hdlr in vlog.handlers:
                if isinstance(hdlr.formatter, ColorLogFormatter):
                    clr_fmt = hdlr.formatter
                    break
            vlog = vlog.parent
        if not clr_fmt:
            return
        colors = ('blue', 'yellow', 'red', 'magenta', 'cyan', 'green')
        for logname, color in zip(lognames, colors):
            clr_fmt.add_logger_colors(f'{vcplogname}.{logname}', color)

    def _qemu_logger(self, proc: Popen, queue: deque, err: bool):
        # worker thread, blocking on VM stdout/stderr
        stream = proc.stderr if err else proc.stdout
        while proc.poll() is None:
            line = stream.readline().strip()
            if line:
                queue.append((err, line))

    def _get_exit_code(self, xmo: Match) -> int:
        groups = xmo.groups()
        if not groups:
            self._log.debug('No matching group, using defaut code')
            return self.NO_MATCH_RETURN_CODE
        match = groups[0]
        try:
            # try to match an integer value
            return int(match)
        except ValueError:
            pass
        # try to find in the regular expression whether the match is one of
        # the alternative in the first group
        alts = re_sub(rb'^.*\((.*?)\).*$', r'\1', xmo.re.pattern).split(b'|')
        try:
            pos = alts.index(match)
            if pos:
                pos += self.GUEST_ERROR_OFFSET
            return pos
        except ValueError as exc:
            self._log.error('Invalid match: %s with %s', exc, alts)
            return len(alts)
        # any other case
        self._log.debug('No match, using defaut code')
        return self.NO_MATCH_RETURN_CODE


class QEMUFileManager:
    """Simple file manager to generate and track temporary files.

       :param keep_temp: do not automatically discard generated files on exit
    """

    DEFAULT_OTP_ECC_BITS = 6

    def __init__(self, keep_temp: bool = False):
        self._log = getLogger('pyot.file')
        self._keep_temp = keep_temp
        self._in_fly: set[str] = set()
        self._otp_files: dict[str, tuple[str, int]] = {}
        self._env: dict[str, str] = {}
        self._transient_vars: set[str] = set()
        self._dirs: dict[str, str] = {}
        register(self._cleanup)

    @property
    def keep_temporary(self) -> bool:
        """Tell whether temporary files and directories should be preserved or
           not.

           :return: True if temporary items should not be suppressed
        """
        return self._keep_temp

    def set_qemu_src_dir(self, path: str) -> None:
        """set the QEMU "source" directory.

           :param path: the path to the QEMU source directory
        """
        self._env['QEMU_SRC_DIR'] = abspath(path)

    def set_qemu_bin_dir(self, path: str) -> None:
        """set the QEMU executable directory.

           :param path: the path to the QEMU binary directory
        """
        self._env['QEMU_BIN_DIR'] = abspath(path)

    def set_config_dir(self, path: str) -> None:
        """Assign the configuration directory.

           :param path: the directory that contains the input configuration
                        file
        """
        self._env['CONFIG'] = abspath(path)

    def interpolate(self, value: Any) -> str:
        """Interpolate a ${...} marker with shell substitutions or local
           substitution.

           :param value: input value
           :return: interpolated value as a string
        """
        def replace(smo: Match) -> str:
            name = smo.group(1)
            val = self._env[name] if name in self._env \
                else environ.get(name, '')
            return val
        svalue = str(value)
        nvalue = re_sub(r'\$\{(\w+)\}', replace, svalue)
        if nvalue != svalue:
            self._log.debug('Interpolate %s with %s', value, nvalue)
        return nvalue

    def define(self, aliases: dict[str, Any]) -> None:
        """Store interpolation variables into a local dictionary.

            Variable values are interpolated before being stored.

           :param aliases: an alias JSON (sub-)tree
        """
        def replace(smo: Match) -> str:
            name = smo.group(1)
            val = self._env[name] if name in self._env \
                else environ.get(name, '')
            return val
        for name in aliases:
            value = str(aliases[name])
            value = re_sub(r'\$\{(\w+)\}', replace, value)
            if exists(value):
                value = normpath(value)
            aliases[name] = value
            self._env[name.upper()] = value
            self._log.debug('Store %s as %s', name.upper(), value)

    def define_transient(self, aliases: dict[str, Any]) -> None:
        """Add short-lived aliases that are all discarded when cleanup_transient
           is called.

           :param aliases: a dict of aliases
        """
        for name in aliases:
            name = name.upper()
            # be sure not to make an existing non-transient variable transient
            if name not in self._env:
                self._transient_vars.add(name)
        self.define(aliases)

    def cleanup_transient(self) -> None:
        """Remove all transient variables."""
        for name in self._transient_vars:
            if name in self._env:
                del self._env[name]
        self._transient_vars.clear()

    def interpolate_dirs(self, value: str, default: str) -> str:
        """Resolve temporary directories, creating ones whenever required.

           :param value: the string with optional directory placeholders
           :param default: the default name to use if the placeholder contains
                           none
           :return: the interpolated string
        """
        def replace(smo: Match) -> str:
            name = smo.group(1)
            if name == '':
                name = default
            if name not in self._dirs:
                tmp_dir = mkdtemp(prefix='qemu_ot_dir_')
                self._dirs[name] = tmp_dir
            else:
                tmp_dir = self._dirs[name]
            if not tmp_dir.endswith(sep):
                tmp_dir = f'{tmp_dir}{sep}'
            return tmp_dir
        nvalue = re_sub(r'\@\{(\w*)\}/', replace, value)
        if nvalue != value:
            self._log.debug('Interpolate %s with %s', value, nvalue)
        return nvalue

    def delete_default_dir(self, name: str) -> None:
        """Delete a temporary directory, if has been referenced.

           :param name: the name of the directory reference
        """
        if name not in self._dirs:
            return
        if not isdir(self._dirs[name]):
            return
        try:
            self._log.debug('Removing tree %s for %s', self._dirs[name], name)
            rmtree(self._dirs[name])
            del self._dirs[name]
        except OSError:
            self._log.error('Cannot be removed dir %s for %s', self._dirs[name],
                            name)

    def create_eflash_image(self, app: Optional[str] = None,
                            bootloader: Optional[str] = None) -> str:
        """Generate a temporary flash image file.

           :param app: optional path to the application or the rom extension
           :param bootloader: optional path to a bootloader
           :return: the full path to the temporary flash file
        """
        # pylint: disable=import-outside-toplevel
        from flashgen import FlashGen
        gen = FlashGen(FlashGen.CHIP_ROM_EXT_SIZE_MAX if bool(bootloader)
                       else 0, True)
        self._configure_logger(gen)
        flash_fd, flash_file = mkstemp(suffix='.raw', prefix='qemu_ot_flash_')
        self._in_fly.add(flash_file)
        close(flash_fd)
        self._log.debug('Create %s', basename(flash_file))
        try:
            gen.open(flash_file)
            if app:
                with open(app, 'rb') as afp:
                    gen.store_rom_ext(0, afp)
            if bootloader:
                with open(bootloader, 'rb') as bfp:
                    gen.store_bootloader(0, bfp)
        finally:
            gen.close()
        return flash_file

    def create_otp_image(self, vmem: str) -> str:
        """Generate a temporary OTP image file.

           If a temporary file has already been generated for the input VMEM
           file, use it instead.

           :param vmem: path to the VMEM source file
           :return: the full path to the temporary OTP file
        """
        # pylint: disable=import-outside-toplevel
        if vmem in self._otp_files:
            otp_file, ref_count = self._otp_files[vmem]
            self._log.debug('Use existing %s', basename(otp_file))
            self._otp_files[vmem] = (otp_file, ref_count + 1)
            return otp_file
        from otptool import OtpImage
        otp = OtpImage()
        self._configure_logger(otp)
        with open(vmem, 'rt', encoding='utf-8') as vfp:
            otp.load_vmem(vfp, 'otp')
        otp_fd, otp_file = mkstemp(suffix='.raw', prefix='qemu_ot_otp_')
        self._log.debug('Create %s', basename(otp_file))
        self._in_fly.add(otp_file)
        close(otp_fd)
        with open(otp_file, 'wb') as rfp:
            otp.save_raw(rfp)
        self._otp_files[vmem] = (otp_file, 1)
        return otp_file

    def delete_flash_image(self, filename: str) -> None:
        """Delete a previously generated flash image file.

           :param filename: full path to the file to delete
        """
        if not isfile(filename):
            self._log.warning('No such flash image file %s', basename(filename))
            return
        self._log.debug('Delete flash image file %s', basename(filename))
        unlink(filename)
        self._in_fly.discard(filename)

    def delete_otp_image(self, filename: str) -> None:
        """Delete a previously generated OTP image file.

           The file may be used by other tests, it is only deleted if it not
           useful anymore.

           :param filename: full path to the file to delete
        """
        if not isfile(filename):
            self._log.warning('No such OTP image file %s', basename(filename))
            return
        for vmem, (raw, count) in self._otp_files.items():
            if raw != filename:
                continue
            count -= 1
            if not count:
                self._log.debug('Delete OTP image file %s', basename(filename))
                unlink(filename)
                self._in_fly.discard(filename)
                del self._otp_files[vmem]
            else:
                self._log.debug('Keep OTP image file %s', basename(filename))
                self._otp_files[vmem] = (raw, count)
            break

    def _configure_logger(self, tool) -> None:
        log = getLogger('pyot')
        flog = tool.logger
        # sub-tool get one logging level down to reduce log messages
        floglevel = min(CRITICAL, log.getEffectiveLevel() + 10)
        flog.setLevel(floglevel)
        for hdlr in log.handlers:
            flog.addHandler(hdlr)

    def _cleanup(self) -> None:
        """Remove a generated, temporary flash image file.
        """
        removed: set[str] = set()
        for tmpfile in self._in_fly:
            if not isfile(tmpfile):
                removed.add(tmpfile)
                continue
            if not self._keep_temp:
                self._log.debug('Delete %s', basename(tmpfile))
                try:
                    unlink(tmpfile)
                    removed.add(tmpfile)
                except OSError:
                    self._log.error('Cannot delete %s', basename(tmpfile))
        self._in_fly -= removed
        if self._in_fly:
            if not self._keep_temp:
                raise OSError(f'{len(self._in_fly)} temp. files cannot be '
                              f'removed')
            for tmpfile in self._in_fly:
                self._log.warning('Temporary file %s not suppressed', tmpfile)
        removed: set[str] = set()
        if not self._keep_temp:
            for tmpname, tmpdir in self._dirs.items():
                if not isdir(tmpdir):
                    removed.add(tmpname)
                    continue
                self._log.debug('Delete dir %s', tmpdir)
                try:
                    rmtree(tmpdir)
                    removed.add(tmpname)
                except OSError as exc:
                    self._log.error('Cannot delete %s: %s', tmpdir, exc)
            for tmpname in removed:
                del self._dirs[tmpname]
        if self._dirs:
            if not self._keep_temp:
                raise OSError(f'{len(self._dirs)} temp. dirs cannot be removed')
            for tmpdir in self._dirs.values():
                self._log.warning('Temporary dir %s not suppressed', tmpdir)


class QEMUContextWorker:

    """Background task for QEMU context.
    """

    def __init__(self, cmd: str, env: dict[str, str],
                 sync: Optional[Event] = None):
        self._log = getLogger('pyot.cmd')
        self._cmd = cmd
        self._env = env
        self._sync = sync
        self._log_q = deque()
        self._resume = False
        self._thread: Optional[Thread] = None
        self._ret = None

    def run(self):
        """Start the worker.
        """
        self._thread = Thread(target=self._run)
        self._thread.start()

    def stop(self) -> int:
        """Stop the worker.
        """
        if self._thread is None:
            raise ValueError('Cannot stop idle worker')
        self._resume = False
        self._thread.join()
        return self._ret

    def exit_code(self) -> Optional[int]:
        """Return the exit code of the worker.

           :return: the exit code or None if the worked has not yet completed.
        """
        return self._ret

    @property
    def command(self) -> str:
        """Return the executed command name.
        """
        return normpath(self._cmd.split(' ', 1)[0])

    def _run(self):
        self._resume = True
        if self._sync and not self._sync.is_set():
            self._log.info('Waiting for sync')
            while self._resume:
                if self._sync.wait(0.1):
                    self._log.debug('Synchronized')
                    break
            self._sync.clear()
        # pylint: disable=consider-using-with
        proc = Popen(self._cmd,  bufsize=1, stdout=PIPE, stderr=PIPE,
                     shell=True, env=self._env, encoding='utf-8',
                     errors='ignore', text=True)
        Thread(target=self._logger, args=(proc, True)).start()
        Thread(target=self._logger, args=(proc, False)).start()
        qemu_exec = f'{basename(self._cmd[0])}: '
        while self._resume:
            while self._log_q:
                err, qline = self._log_q.popleft()
                if err:
                    loglevel = QEMUWrapper.classify_log(qline, qemux=qemu_exec)
                    self._log.log(loglevel, qline)
                else:
                    self._log.debug(qline)
            if proc.poll() is not None:
                # worker has exited on its own
                self._resume = False
                break
        try:
            # give some time for the process to complete on its own
            proc.wait(0.2)
            self._ret = proc.returncode
            self._log.debug('"%s" completed with %d', self.command, self._ret)
        except TimeoutExpired:
            # still executing
            proc.terminate()
            try:
                # leave 1 second for QEMU to cleanly complete...
                proc.wait(1.0)
                self._ret = 0
            except TimeoutExpired:
                # otherwise kill it
                self._log.error('Force-killing command "%s"', self.command)
                proc.kill()
                self._ret = proc.returncode
        # retrieve the remaining log messages
        stdlog = self._log.info if self._ret else self._log.debug
        for sfp, logger in zip(proc.communicate(timeout=0.1),
                               (stdlog, self._log.error)):
            for line in sfp.split('\n'):
                line = line.strip()
                if line:
                    logger(line)

    def _logger(self, proc: Popen, err: bool):
        # worker thread, blocking on VM stdout/stderr
        stream = proc.stderr if err else proc.stdout
        while proc.poll() is None:
            line = stream.readline().strip()
            if line:
                self._log_q.append((err, line))


class QEMUContext:
    """Execution context for QEMU session.

       Execute commands before, while and after QEMU executes.

       :param test_name: the name of the test QEMU should execute
       :param qfm: the file manager
       :param qemu_cmd: the command and argument to execute QEMU
       :param context: the contex configuration for the current test
    """

    def __init__(self, test_name: str, qfm: QEMUFileManager,
                 qemu_cmd: list[str], context: dict[str, list[str]],
                 env: Optional[dict[str, str]] = None):
        # pylint: disable=too-many-arguments
        self._clog = getLogger('pyot.ctx')
        self._test_name = test_name
        self._qfm = qfm
        self._qemu_cmd = qemu_cmd
        self._context = context
        self._env = env or {}
        self._workers: list[Popen] = []

    def execute(self, ctx_name: str, code: int = 0,
                sync: Optional[Event] = None) -> None:
        """Execute all commands, in order, for the selected context.

           Synchronous commands are executed in order. If one command fails,
           subsequent commands are not executed.

           Background commands are started in order, but a failure does not
           stop other commands.

           :param ctx_name: the name of the execution context
           :param code: a previous error completion code, if any
           :param sync: an optional synchronisation event to start up the
                        execution
        """
        ctx = self._context.get(ctx_name, None)
        if ctx_name == 'post' and code:
            self._clog.info("Discard execution of '%s' commands after failure "
                            "of '%s'", ctx_name, self._test_name)
            return
        env = dict(environ)
        env.update(self._env)
        if self._qemu_cmd:
            env['PATH'] = ':'.join((env['PATH'], dirname(self._qemu_cmd[0])))
        if ctx:
            for cmd in ctx:
                if cmd.endswith('&'):
                    if ctx_name == 'post':
                        raise ValueError(f"Cannot execute background command "
                                         f"in [{ctx_name}] context for "
                                         f"'{self._test_name}'")
                    cmd = normpath(cmd[:-1].rstrip())
                    rcmd = relpath(cmd)
                    if rcmd.startswith(pardir):
                        rcmd = cmd
                    self._clog.debug('Execute "%s" in background for [%s] '
                                     'context', rcmd, ctx_name)
                    worker = QEMUContextWorker(cmd, env, sync)
                    worker.run()
                    self._workers.append(worker)
                else:
                    if sync:
                        self._clog.debug('Synchronization ignored')
                    cmd = normpath(cmd.rstrip())
                    rcmd = relpath(cmd)
                    if rcmd.startswith(pardir):
                        rcmd = cmd
                    self._clog.debug('Execute "%s" in sync for [%s] context',
                                     rcmd, ctx_name)
                    # pylint: disable=consider-using-with
                    proc = Popen(cmd, bufsize=1, stdout=PIPE, stderr=PIPE,
                                 shell=True, env=env, encoding='utf-8',
                                 errors='ignore', text=True)
                    ret = 0
                    try:
                        outs, errs = proc.communicate(timeout=5)
                        ret = proc.returncode
                    except TimeoutExpired:
                        proc.kill()
                        outs, errs = proc.communicate()
                        ret = proc.returncode
                    for sfp, logger in zip(
                            (outs, errs),
                            (self._clog.debug,
                             self._clog.error if ret else self._clog.info)):
                        for line in sfp.split('\n'):
                            line = line.strip()
                            if line:
                                logger(line)
                    if ret:
                        self._clog.error("Fail to execute '%s' command for "
                                         "'%s'", cmd, self._test_name)
                        raise OSError(ret,
                                      f'Cannot execute [{ctx_name}] command')
        if ctx_name == 'post':
            if not self._qfm.keep_temporary:
                self._qfm.delete_default_dir(self._test_name)

    def check_error(self) -> int:
        """Check if any background worker exited in error.

           :return: a non-zero value on error
        """
        for worker in self._workers:
            ret = worker.exit_code()
            if not ret:
                continue
            self._clog.error("%s exited with %d", worker.command, ret)
            return ret
        return 0

    def finalize(self) -> int:
        """Terminate any running background command, in reverse order.

           :return: a non-zero value if one or more workers have reported an
                    error
        """
        rets = {0}
        while self._workers:
            worker = self._workers.pop()
            ret = worker.stop()
            rets.add(ret)
            if ret:
                self._clog.warning('Command "%s" has failed for "%s": %d',
                                   worker.command, self._test_name, ret)
        return max(rets)


class QEMUExecuter:
    """Test execution sequencer.

       :param qfm: file manager that tracks temporary files
       :param config: configuration dictionary
       :param args: parsed arguments
    """

    RESULT_MAP = {
        0: 'PASS',
        1: 'ERROR',
        6: 'ABORT',
        11: 'CRASH',
        QEMUWrapper.GUEST_ERROR_OFFSET - 1: 'GUEST_ESC',
        QEMUWrapper.GUEST_ERROR_OFFSET + 1: 'FAIL',
        98: 'UNEXP_SUCCESS',
        99: 'CONTEXT',
        124: 'TIMEOUT',
        125: 'DEADLOCK',
        126: 'CONTEXT',
        QEMUWrapper.NO_MATCH_RETURN_CODE: 'UNKNOWN',
    }

    DEFAULT_START_DELAY = 1.0
    """Default start up delay to let QEMU initialize before connecting the
       virtual UART port.
    """

    DEFAULT_SERIAL_PORT = 'serial0'
    """Default VCP name."""

    def __init__(self, qfm: QEMUFileManager, config: dict[str, any],
                 args: Namespace):
        self._log = getLogger('pyot.exec')
        self._qfm = qfm
        self._config = config
        self._args = args
        self._argdict: dict[str, Any] = {}
        self._qemu_cmd: list[str] = []
        self._suffixes = []
        if hasattr(self._args, 'opts'):
            setattr(self._args, 'global_opts', getattr(self._args, 'opts'))
            setattr(self._args, 'opts', [])
        else:
            setattr(self._args, 'global_opts', [])

    def build(self) -> None:
        """Build initial QEMU arguments.

           :raise ValueError: if some argument is invalid
        """
        exec_info = self._build_qemu_command(self._args)
        self._qemu_cmd = exec_info.command
        self._argdict = dict(self._args.__dict__)
        self._suffixes = []
        suffixes = self._config.get('suffixes', [])
        if not isinstance(suffixes, list):
            raise ValueError('Invalid suffixes sub-section')
        self._suffixes.extend(suffixes)

    def enumerate_tests(self) -> Iterator[str]:
        """Enumerate tests to execute.
        """
        self._argdict = dict(self._args.__dict__)
        for tst in sorted(self._build_test_list()):
            ttype = self.guess_test_type(tst)
            yield f'{basename(tst)} ({ttype})'

    def run(self, debug: bool, allow_no_test: bool) -> int:
        """Execute all requested tests.

           :return: success or the code of the first encountered error
        """
        qot = QEMUWrapper(debug)
        ret = 0
        results = defaultdict(int)
        result_file = self._argdict.get('result')
        # pylint: disable=consider-using-with
        cfp = open(result_file, 'wt', encoding='utf-8') if result_file else None
        try:
            csv = csv_writer(cfp) if cfp else None
            if csv:
                csv.writerow((x.title() for x in TestResult._fields))
            app = self._argdict.get('exec')
            if app:
                assert 'timeout' in self._argdict
                timeout = int(float(self._argdict.get('timeout') *
                              float(self._argdict.get('timeout_factor',
                                                      DEFAULT_TIMEOUT_FACTOR))))
                self._log.debug('Execute %s', basename(self._argdict['exec']))
                ret, xtime, err = qot.run(self._qemu_cmd, timeout,
                                          self.get_test_radix(app), None,
                                          self.DEFAULT_START_DELAY)
                results[ret] += 1
                sret = self.RESULT_MAP.get(ret, ret)
                icount = self._argdict.get('icount')
                if csv:
                    csv.writerow(TestResult(self.get_test_radix(app), sret,
                                            xtime, icount, err))
                    cfp.flush()
            tests = self._build_test_list()
            tcount = len(tests)
            self._log.info('Found %d tests to execute', tcount)
            if not tcount and not allow_no_test:
                self._log.error('No test can be run')
                return 1
            targs = None
            temp_files = {}
            for tpos, test in enumerate(tests, start=1):
                self._log.info('[TEST %s] (%d/%d)', self.get_test_radix(test),
                               tpos, tcount)
                try:
                    self._qfm.define_transient({
                        'UTPATH': test,
                        'UTDIR': normpath(dirname(test)),
                        'UTFILE': basename(test),
                    })
                    test_name = self.get_test_radix(test)
                    exec_info = self._build_qemu_test_command(test)
                    exec_info.test_name = test_name
                    exec_info.context.execute('pre')
                    tret, xtime, err = qot.run(exec_info)
                    cret = exec_info.context.finalize()
                    if exec_info.expect_result != 0:
                        if tret == exec_info.expect_result:
                            self._log.info('QEMU failed with expected error, '
                                           'assume success')
                            tret = 0
                        elif tret == 0:
                            self._log.warning('QEMU success while expected '
                                              'error %d, assume error', tret)
                            tret = 98
                    if tret == 0 and cret != 0:
                        tret = 99
                    exec_info.context.execute('post', tret)
                # pylint: disable=broad-except
                except Exception as exc:
                    self._log.critical('%s', str(exc))
                    if debug:
                        print(format_exc(chain=False), file=stderr)
                    tret = 99
                    xtime = 0.0
                    err = str(exc)
                finally:
                    self._qfm.cleanup_transient()
                results[tret] += 1
                sret = self.RESULT_MAP.get(tret, tret)
                if targs:
                    icount = self.get_namespace_arg(targs, 'icount')
                else:
                    icount = None
                if csv:
                    csv.writerow(TestResult(test_name, sret, xtime, icount,
                                            err))
                    # want to commit result as soon as possible if some client
                    # is live-tracking progress on long test runs
                    cfp.flush()
                else:
                    self._log.info('"%s" executed in %s (%s)',
                                   test_name, xtime, sret)
                self._cleanup_temp_files(temp_files)
        finally:
            if cfp:
                cfp.close()
        for kind in sorted(results):
            self._log.info('%s count: %d',
                           self.RESULT_MAP.get(kind, kind),
                           results[kind])
        # sort by the largest occurence, discarding success
        errors = sorted((x for x in results.items() if x[0]),
                        key=lambda x: -x[1])
        # overall return code is the most common error, or success otherwise
        ret = errors[0][0] if errors else 0
        self._log.info('Total count: %d, overall result: %s',
                       sum(results.values()),
                       self.RESULT_MAP.get(ret, ret))
        return ret

    def get_test_radix(self, filename: str) -> str:
        """Extract the radix name from a test pathname.

           :param filename: the path to the test executable
           :return: the test name
        """
        test_name = basename(filename).split('.')[0]
        for suffix in self._suffixes:
            if not test_name.endswith(suffix):
                continue
            return test_name[:-len(suffix)]
        return test_name

    @classmethod
    def get_namespace_arg(cls, args: Namespace, name: str) -> Optional[str]:
        """Extract a value from a namespace.

           :param args: the namespace
           :param name: the value's key
           :return: the value if any
        """
        return args.__dict__.get(name)

    @staticmethod
    def flatten(lst: list) -> list:
        """Flatten a list.
        """
        return [item for sublist in lst for item in sublist]

    @staticmethod
    def abspath(path: str) -> str:
        """Build absolute path"""
        if isabs(path):
            return normpath(path)
        return normpath(joinpath(getcwd(), path))

    @staticmethod
    def guess_test_type(filepath: str) -> str:
        """Guess a test file type from its contents.

           :return: identified content
        """
        with open(filepath, 'rb') as bfp:
            header = bfp.read(4)
        if header == b'\x7fELF':
            return 'elf'
        if header == b'OTPT':
            return 'spiflash'
        return 'bin'

    def _cleanup_temp_files(self, storage: dict[str, set[str]]) -> None:
        if self._qfm.keep_temporary:
            return
        for kind, files in storage.items():
            delete_file = getattr(self._qfm, f'delete_{kind}_image')
            for filename in files:
                delete_file(filename)

    def _build_qemu_fw_args(self, args: Namespace) \
            -> tuple[str, str, list[str]]:
        rom_exec = bool(args.rom_exec)
        roms = args.rom or []
        multi_rom = (len(roms) + int(rom_exec)) > 1
        # generate pre-application ROM option
        fw_args: list[str] = []
        machine = args.machine
        variant = args.variant
        chiplet_count = 1
        if variant:
            machine = f'{machine},variant={variant}'
            try:
                chiplet_count = sum(int(x)
                                    for x in re.split(r'[A-Za-z]', variant)
                                    if x)
            except ValueError:
                self._log.warning('Unknown variant syntax %s', variant)
        for chip_id in range(chiplet_count):
            rom_count = 0
            for rom in roms:
                rom_path = self._qfm.interpolate(rom)
                if not isfile(rom_path):
                    raise ValueError(f'Unable to find ROM file {rom_path}')
                rom_ids = []
                if args.first_soc:
                    if chiplet_count == 1:
                        rom_ids.append(f'{args.first_soc}.')
                    else:
                        rom_ids.append(f'{args.first_soc}{chip_id}.')
                rom_ids.append('rom')
                if multi_rom:
                    rom_ids.append(f'{rom_count}')
                rom_id = ''.join(rom_ids)
                rom_opt = f'ot-rom-img,id={rom_id},file={rom_path},digest=fake'
                fw_args.extend(('-object', rom_opt))
                rom_count += 1
        xtype = None
        if args.exec:
            exec_path = self.abspath(args.exec)
            xtype = self.guess_test_type(exec_path)
            if xtype == 'spiflash':
                fw_args.extend(('-drive',
                                f'if=mtd,bus=0,format=raw,file={exec_path}'))
            else:
                if xtype != 'elf':
                    raise ValueError(f'No support for test type: {xtype} '
                                     f'({basename(exec_path)})')
                if rom_exec:
                    # generate ROM option for the application itself
                    rom_ids = []
                    if args.first_soc:
                        if chiplet_count == 1:
                            rom_ids.append(f'{args.first_soc}.')
                        else:
                            rom_ids.append(f'{args.first_soc}0.')
                    rom_ids.append('rom')
                    if multi_rom:
                        rom_ids.append(f'{rom_count}')
                    rom_id = ''.join(rom_ids)
                    rom_opt = (f'ot-rom-img,id={rom_id},file={exec_path},'
                               f'digest=fake')
                    fw_args.extend(('-object', rom_opt))
                    rom_count += 1
                else:
                    fw_args.extend(('-kernel', exec_path))
        return machine, xtype, fw_args

    def _build_qemu_vcp_args(self, args: Namespace):
        device = args.device
        devdesc = device.split(':')
        host = devdesc[0]
        try:
            port = int(devdesc[1])
            if not 0 < port < 65536:
                raise ValueError(f'Invalid serial TCP port: {port}')
        except IndexError as exc:
            raise ValueError(f'TCP port not specified: {device}') from exc
        except TypeError as exc:
            raise ValueError(f'Invalid TCP serial device: {device}') from exc
        mux = f'mux={"on" if args.muxserial else "off"}'
        vcps = args.vcp or [self.DEFAULT_SERIAL_PORT]
        vcp_args = ['-display', 'none']
        vcp_map = {}
        for vix, vcp in enumerate(vcps):
            vcp_map[vcp] = (host, port+vix)
            vcp_args.extend(('-chardev',
                             f'socket,id={vcp},host={host},port={port+vix},'
                             f'{mux},server=on,wait=on'))
            if vcp == self.DEFAULT_SERIAL_PORT:
                vcp_args.extend(('-serial', 'chardev:serial0'))
        return vcp_args, vcp_map

    def _build_qemu_command(self, args: Namespace,
                            opts: Optional[list[str]] = None) \
            -> EasyDict[str, Any]:
        """Build QEMU command line from argparser values.

           :param args: the parsed arguments
           :param opts: any QEMU-specific additional options
           :return: a dictionary defining how to execute the command
        """
        if args.qemu is None:
            raise ValueError('QEMU path is not defined')
        machine, xtype, fw_args = self._build_qemu_fw_args(args)
        qemu_args = [args.qemu, '-M', machine]
        qemu_args.extend(fw_args)
        temp_files = defaultdict(set)
        if all((args.otp, args.otp_raw)):
            raise ValueError('OTP VMEM and RAW options are mutually exclusive')
        if args.otp:
            if not isfile(args.otp):
                raise ValueError(f'No such OTP file: {args.otp}')
            otp_file = self._qfm.create_otp_image(args.otp)
            temp_files['otp'].add(otp_file)
            qemu_args.extend(('-drive',
                              f'if=pflash,file={otp_file},format=raw'))
        elif args.otp_raw:
            otp_raw_path = self.abspath(args.otp_raw)
            qemu_args.extend(('-drive',
                              f'if=pflash,file={otp_raw_path},format=raw'))
        if args.flash:
            if xtype == 'spiflash':
                raise ValueError('Cannot use a flash file with a flash test')
            if not isfile(args.flash):
                raise ValueError(f'No such flash file: {args.flash}')
            if any((args.exec, args.boot)):
                raise ValueError('Flash file argument is mutually exclusive '
                                 'with bootloader or rom extension')
            flash_path = self.abspath(args.flash)
            qemu_args.extend(('-drive', f'if=mtd,bus=1,file={flash_path},'
                                        f'format=raw'))
        elif any((args.exec, args.boot)):
            if args.exec and not isfile(args.exec):
                raise ValueError(f'No such exec file: {args.exec}')
            if args.boot and not isfile(args.boot):
                raise ValueError(f'No such bootloader file: {args.boot}')
            if args.embedded_flash:
                flash_file = self._qfm.create_eflash_image(args.exec, args.boot)
                temp_files['flash'].add(flash_file)
                qemu_args.extend(('-drive', f'if=mtd,bus=1,file={flash_file},'
                                 f'format=raw'))
        if args.log_file:
            qemu_args.extend(('-D', self.abspath(args.log_file)))
        if args.trace:
            # use a FileType to let argparser validate presence and type
            args.trace.close()
            qemu_args.extend(('-trace',
                              f'events={self.abspath(args.trace.name)}'))
        if args.log:
            qemu_args.append('-d')
            qemu_args.append(','.join(args.log))
        if args.singlestep:
            qemu_args.append('-singlestep')
        if 'icount' in args:
            if args.icount is not None:
                qemu_args.extend(('-icount', f'shift={args.icount}'))
        try:
            start_delay = float(getattr(args, 'start_delay') or
                                self.DEFAULT_START_DELAY)
        except ValueError as exc:
            raise ValueError(f'Invalid start up delay {args.start_delay}') \
                from exc
        start_delay *= args.timeout_factor
        trigger = getattr(args, 'trigger', '')
        vcp_args, vcp_map = self._build_qemu_vcp_args(args)
        qemu_args.extend(vcp_args)
        qemu_args.extend(args.global_opts or [])
        if opts:
            qemu_args.extend((str(o) for o in opts))
        return EasyDict(command=qemu_args, vcp_map=vcp_map,
                        tmpfiles=temp_files, start_delay=start_delay,
                        trigger=trigger)

    def _build_qemu_test_command(self, filename: str) -> EasyDict[str, Any]:
        test_name = self.get_test_radix(filename)
        args, opts, timeout, texp = self._build_test_args(test_name)
        setattr(args, 'exec', filename)
        exec_info = self._build_qemu_command(args, opts)
        exec_info.pop('connection', None)
        exec_info.args = args
        exec_info.context = self._build_test_context(test_name)
        exec_info.timeout = timeout
        exec_info.expect_result = texp
        return exec_info

    def _build_test_list(self, alphasort: bool = True) -> list[str]:
        pathnames = set()
        testdir = normpath(self._qfm.interpolate(self._config.get('testdir',
                                                                  curdir)))
        self._qfm.define({'testdir': testdir})
        cfilters = self._args.filter or []
        pfilters = [f for f in cfilters if not f.startswith('!')]
        if not pfilters:
            cfilters = ['*'] + cfilters
            tfilters = ['*'] + pfilters
        else:
            tfilters = list(pfilters)
        inc_filters = self._build_config_list('include')
        if inc_filters:
            self._log.debug('Searching for tests from %s dir', testdir)
            for path_filter in filter(None, inc_filters):
                if testdir:
                    path_filter = joinpath(testdir, path_filter)
                paths = set(glob(path_filter, recursive=True))
                for path in paths:
                    if isfile(path):
                        for tfilter in tfilters:
                            if fnmatchcase(self.get_test_radix(path), tfilter):
                                pathnames.add(path)
                                break
        for testfile in self._enumerate_from('include_from'):
            if not isfile(testfile):
                raise ValueError(f'Unable to locate test file '
                                 f'"{testfile}"')
            for tfilter in tfilters:
                if fnmatchcase(self.get_test_radix(testfile),
                               tfilter):
                    pathnames.add(testfile)
        if not pathnames:
            return []
        roms = self._argdict.get('rom')
        pathnames -= {normpath(rom) for rom in roms}
        xtfilters = [f[1:].strip() for f in cfilters if f.startswith('!')]
        exc_filters = self._build_config_list('exclude')
        xtfilters.extend(exc_filters)
        if xtfilters:
            for path_filter in filter(None, xtfilters):
                if testdir:
                    path_filter = joinpath(testdir, path_filter)
                paths = set(glob(path_filter, recursive=True))
                pathnames -= paths
        pathnames -= set(self._enumerate_from('exclude_from'))
        if alphasort:
            return sorted(pathnames, key=basename)
        return list(pathnames)

    def _enumerate_from(self, config_entry: str) -> Iterator[str]:
        incf_filters = self._build_config_list(config_entry)
        if incf_filters:
            for incf in incf_filters:
                incf = normpath(self._qfm.interpolate(incf))
                if not isfile(incf):
                    raise ValueError(f'Invalid test file: "{incf}"')
                self._log.debug('Loading test list from %s', incf)
                incf_dir = dirname(incf)
                with open(incf, 'rt', encoding='utf-8') as ifp:
                    for testfile in ifp:
                        testfile = re_sub('#.*$', '', testfile).strip()
                        if not testfile:
                            continue
                        testfile = self._qfm.interpolate(testfile)
                        if not testfile.startswith(sep):
                            testfile = joinpath(incf_dir, testfile)
                        yield normpath(testfile)

    def _build_config_list(self, config_entry: str) -> list:
        cfglist = []
        items = self._config.get(config_entry)
        if not items:
            return cfglist
        if not isinstance(items, list):
            raise ValueError(f'Invalid configuration file: '
                             f'"{config_entry}" is not a list')
        for item in items:
            if isinstance(item, str):
                cfglist.append(item)
                continue
            if isinstance(item, dict):
                for dname, dval in item.items():
                    try:
                        cond = bool(int(environ.get(dname, '0')))
                    except (ValueError, TypeError):
                        cond = False
                    if not cond:
                        continue
                    if isinstance(dval, str):
                        dval = [dval]
                    if isinstance(dval, list):
                        for sitem in dval:
                            if isinstance(sitem, str):
                                cfglist.append(sitem)
        return cfglist

    def _build_test_args(self, test_name: str) \
            -> tuple[Namespace, list[str], int]:
        tests_cfg = self._config.get('tests', {})
        if not isinstance(tests_cfg, dict):
            raise ValueError('Invalid tests sub-section')
        kwargs = dict(self._args.__dict__)
        test_cfg = tests_cfg.get(test_name, {})
        if test_cfg is None:
            # does not default to an empty dict to differenciate empty from
            # inexistent test configuration
            self._log.debug('No configuration for test %s', test_name)
            opts = None
        else:
            test_cfg = {k: v for k, v in test_cfg.items()
                        if k not in ('pre', 'post', 'with')}
            self._log.debug('Using custom test config for %s', test_name)
            discards = {k for k, v in test_cfg.items() if v == ''}
            if discards:
                test_cfg = dict(test_cfg)
                for discard in discards:
                    del test_cfg[discard]
                    if discard in kwargs:
                        del kwargs[discard]
            kwargs.update(test_cfg)
            opts = kwargs.get('opts')
            if opts and not isinstance(opts, list):
                raise ValueError('fInvalid QEMU options for {test_name}')
            opts = self.flatten([opt.split(' ') for opt in opts])
            opts = [self._qfm.interpolate(opt) for opt in opts]
            opts = self.flatten([opt.split(' ') for opt in opts])
            opts = [self._qfm.interpolate_dirs(opt, test_name) for opt in opts]
        timeout = float(kwargs.get('timeout', DEFAULT_TIMEOUT))
        tmfactor = float(kwargs.get('timeout_factor', DEFAULT_TIMEOUT_FACTOR))
        itimeout = int(timeout * tmfactor)
        texpect = kwargs.get('expect', 0)
        try:
            texp = int(texpect)
        except ValueError:
            result_map = {v: k for k, v in self.RESULT_MAP.items()}
            try:
                texp = result_map[texpect.upper()]
            except KeyError as exc:
                raise ValueError(f'Unsupported expect: {texpect}') from exc
        return Namespace(**kwargs), opts or [], itimeout, texp

    def _build_test_context(self, test_name: str) -> QEMUContext:
        context = defaultdict(list)
        tests_cfg = self._config.get('tests', {})
        test_cfg = tests_cfg.get(test_name, {})
        test_env = None
        if test_cfg:
            for ctx_name in ('pre', 'with', 'post'):
                if ctx_name not in test_cfg:
                    continue
                ctx = test_cfg[ctx_name]
                if not isinstance(ctx, list):
                    raise ValueError(f'Invalid context "{ctx_name}" '
                                     f'for test {test_name}')
                for pos, cmd in enumerate(ctx, start=1):
                    if not isinstance(cmd, str):
                        raise ValueError(f'Invalid command #{pos} in '
                                         f'"{ctx_name}" for test {test_name}')
                    cmd = re_sub(r'[\n\r]', ' ', cmd.strip())
                    cmd = re_sub(r'\s{2,}', ' ', cmd)
                    cmd = self._qfm.interpolate(cmd)
                    cmd = self._qfm.interpolate_dirs(cmd, test_name)
                    context[ctx_name].append(cmd)
            env = test_cfg.get('env')
            if env:
                if not isinstance(env, dict):
                    raise ValueError('Invalid context environment')
                test_env = {k: self._qfm.interpolate(v) for k, v in env.items()}
        return QEMUContext(test_name, self._qfm, self._qemu_cmd, dict(context),
                           test_env)


def main():
    """Main routine"""
    debug = True
    qemu_dir = normpath(joinpath(dirname(dirname(dirname(normpath(__file__))))))
    qemu_path = normpath(joinpath(qemu_dir, 'build', 'qemu-system-riscv32'))
    if not isfile(qemu_path):
        qemu_path = None
    tmp_result: Optional[str] = None
    try:
        args: Optional[Namespace] = None
        desc = modules[__name__].__doc__.split('.', 1)[0].strip()
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
        qvm.add_argument('-M', '--log', action='append',
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
        qvm.add_argument('-V', '--variant',
                         help='machine variant (machine specific)')
        files = argparser.add_argument_group(title='Files')
        files.add_argument('-b', '--boot',
                           metavar='file', help='bootloader 0 file')
        files.add_argument('-c', '--config', metavar='JSON',
                           type=FileType('rt', encoding='utf-8'),
                           help='path to configuration file')
        files.add_argument('-e', '--embedded-flash', action='store_true',
                           help='generate an embedded flash image file')
        files.add_argument('-f', '--flash', metavar='RAW',
                           help='SPI flash image file')
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
        exe.add_argument('-k', '--timeout', metavar='SECONDS', type=int,
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
        extra.add_argument('-d', action='store_true',
                           help='enable debug mode')
        extra.add_argument('--log-time', action='store_true',
                           help='show local time in log messages')
        extra.add_argument('--debug', action='append', metavar='LOGGER',
                           help='assign debug level to logger(s)')
        extra.add_argument('--info', action='append', metavar='LOGGER',
                           help='assign info level to logger(s)')
        extra.add_argument('--warn', action='append', metavar='LOGGER',
                           help='assign warning level to logger(s)')

        try:
            # all arguments after `--` are forwarded to QEMU
            pos = argv.index('--')
            sargv = argv[1:pos]
            opts = argv[pos+1:]
        except ValueError:
            sargv = argv[1:]
            opts = []
        cli_opts = list(opts)
        args = argparser.parse_args(sargv)
        if args.debug is not None:
            debug = args.debug
        if args.summary and not args.result:
            tmpfd, tmp_result = mkstemp(suffix='.csv')
            close(tmpfd)
            args.result = tmp_result

        log = configure_loggers(args.verbose, 'pyot',
                                name_width=16,
                                ms=args.log_time, debug=args.debug,
                                info=args.info, warning=args.warn)[0]

        qfm = QEMUFileManager(args.keep_tmp)

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
            jargs = []
            for arg, val in jdefaults.items():
                is_bool = isinstance(val, bool)
                if is_bool:
                    if not val:
                        continue
                optname = f'--{arg}' if len(arg) > 1 else f'-{arg}'
                if isinstance(val, list):
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
        elif args.filter:
            argparser.error('Filter option only valid with a config file')
        if cli_opts:
            qopts = getattr(args, 'opts') or []
            qopts.extend(cli_opts)
            setattr(args, 'opts', qopts)
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
        qfm.set_qemu_src_dir(qemu_dir)
        qfm.set_qemu_bin_dir(dirname(args.qemu))
        qexc = QEMUExecuter(qfm, json, args)
        if args.list:
            for tst in qexc.enumerate_tests():
                print(tst)
            sysexit(0)
        try:
            qexc.build()
        except ValueError as exc:
            if debug:
                print(format_exc(chain=False), file=stderr)
            argparser.error(str(exc))
        ret = qexc.run(debug, args.zero)
        if args.summary:
            rfmt = ResultFormatter()
            rfmt.load(args.result)
            rfmt.show(True)
        log.debug('End of execution with code %d', ret or 0)
        sysexit(ret)
    # pylint: disable=broad-except
    except Exception as exc:
        print(f'{linesep}Error: {exc}', file=stderr)
        if debug:
            print(format_exc(chain=False), file=stderr)
        sysexit(1)
    except KeyboardInterrupt:
        sysexit(2)
    finally:
        if tmp_result and isfile(tmp_result):
            unlink(tmp_result)


if __name__ == '__main__':
    main()
