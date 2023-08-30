#!/usr/bin/env python3

"""OpenTitan QEMU test sequencer.
"""

# Copyright (c) 2023 Rivos, Inc.
# SPDX-License-Identifier: Apache2

from argparse import ArgumentParser, FileType, Namespace
from atexit import register
from collections import defaultdict
from csv import writer as csv_writer
from fnmatch import fnmatchcase
from glob import glob
try:
    # try to use HJSON if available
    from hjson import load as jload
except ImportError:
    # fallback on legacy JSON syntax otherwise
    from json import load as jload
from logging import (Formatter, StreamHandler, CRITICAL, DEBUG, INFO, ERROR,
                     WARNING, getLogger)
from os import close, curdir, environ, isatty, linesep, pardir, sep, unlink
from os.path import (basename, dirname, isdir, isfile, join as joinpath,
                     normpath, relpath)
from re import Match, compile as re_compile, sub as re_sub
from shutil import rmtree
from socket import socket, timeout as LegacyTimeoutError
from subprocess import Popen, PIPE, TimeoutExpired
from sys import argv, exit as sysexit, modules, stderr, stdout
from threading import Thread
from tempfile import mkdtemp, mkstemp
from time import time as now
from traceback import format_exc
from typing import (Any, Deque, Dict, Iterator, List, NamedTuple, Optional, Set,
                    Tuple)


#pylint: disable=too-many-lines

DEFAULT_MACHINE ='ot-earlgrey'
DEFAULT_DEVICE = 'localhost:8000'
DEFAULT_TIMEOUT = 60  # seconds


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
    icount: int
    error: str


class CustomFormatter(Formatter):
    """Custom log formatter for ANSI terminals. Colorize log levels.
    """

    GREY = "\x1b[38;20m"
    YELLOW = "\x1b[33;1m"
    RED = "\x1b[31;1m"
    MAGENTA = "\x1b[35;1m"
    WHITE = "\x1b[37;1m"
    RESET = "\x1b[0m"
    FORMAT_LEVEL = '%(levelname)8s'
    FORMAT_TRAIL = ' %(name)-10s %(message)s'

    COLOR_FORMATS = {
        DEBUG: f'{GREY}{FORMAT_LEVEL}{RESET}{FORMAT_TRAIL}',
        INFO: f'{WHITE}{FORMAT_LEVEL}{RESET}{FORMAT_TRAIL}',
        WARNING: f'{YELLOW}{FORMAT_LEVEL}{RESET}{FORMAT_TRAIL}',
        ERROR: f'{RED}{FORMAT_LEVEL}{RESET}{FORMAT_TRAIL}',
        CRITICAL: f'{MAGENTA}{FORMAT_LEVEL}{RESET}{FORMAT_TRAIL}',
    }

    PLAIN_FORMAT = f'{FORMAT_LEVEL}{FORMAT_TRAIL}'

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._istty = isatty(stdout.fileno())

    def format(self, record):
        log_fmt = self.COLOR_FORMATS[record.levelno] if self._istty \
                  else self.PLAIN_FORMAT
        formatter = Formatter(log_fmt)
        return formatter.format(record)


class QEMUWrapper:
    """A small engine to run tests with QEMU.

       :param tcpdev: a host, port pair that defines how to access the TCP
                      Virtual Com Port of QEMU first UART
       :param debug: whether running in debug mode
    """
    #pylint: disable=too-few-public-methods

    EXIT_ON = rb'(PASS|FAIL)!\r'
    """Matching strings to search for in guest output.

       The return code of the script is the position plus the GUEST_ERROR_OFFSET
       in the above RE group when matched, except first item which is always 0.
       This offset is used to differentiate from QEMU own return codes. QEMU may
       return negative values, which are the negative value of POSIX signals,
       such as SIGABRT.
    """

    GUEST_ERROR_OFFSET = 30
    """Offset for guest errors. Should be larger than the host max signal value.
    """

    NO_MATCH_RETURN_CODE = 100
    """Return code when no matching string is found in guest output."""

    LOG_LEVELS = { 'D': DEBUG, 'I': INFO, 'E': ERROR }
    """OpenTitan log levels."""

    def __init__(self, tcpdev: Tuple[str, int], debug: bool):
        # self._mterm: Optional[MiniTerm] = None
        self._device = tcpdev
        self._debug = debug
        self._log = getLogger('pyot')
        self._qlog = getLogger('pyot.qemu')
        self._otlog = getLogger('pyot.ot')

    def run(self, qemu_args: List[str], timeout: int, name: str,
            ctx: Optional['QEMUContext']) -> Tuple[int, ExecTime, str]:
        """Execute the specified QEMU command, aborting execution if QEMU does
           not exit after the specified timeout.

           :param qemu_args: QEMU argument list (first arg is the path to QEMU)
           :param timeout: the delay in seconds after which the QEMU session
                            is aborted
           :param name: the tested application name
           :param ctx: execution context, if any
           :return: a 3-uple of exit code, execution time, and last guest error
        """
        #pylint: disable=too-many-locals
        #pylint: disable=too-many-branches
        #pylint: disable=too-many-statements

        # stdout and stderr belongs to QEMU VM
        # OT's UART0 is redirected to a TCP stream that can be accessed through
        # self._device. The VM pauses till the TCP socket is connected
        xre = re_compile(self.EXIT_ON)
        otre = r'^([' + ''.join(self.LOG_LEVELS.keys()) + r'])\d{5}\s'
        lre = re_compile(otre)
        ret = None
        proc = None
        sock = None
        xstart = None
        xend = None
        log = self._log
        last_guest_error = ''
        #pylint: disable=too-many-nested-blocks
        try:
            log.info('Executing QEMU as %s', ' '.join(qemu_args))
            #pylint: disable=consider-using-with
            proc = Popen(qemu_args, bufsize=1, stdout=PIPE, stderr=PIPE,
                         encoding='utf-8', errors='ignore', text=True)
            try:
                # ensure that QEMU starts and give some time for it to set up
                # its VCP before attempting to connect to it
                proc.wait(0.9)
            except TimeoutExpired:
                pass
            else:
                ret = proc.returncode
                log.error('QEMU bailed out: %d for "%s"', ret, name)
                raise OSError()
            sock = socket()
            log.debug('Connecting QEMU VCP as %s:%d', *self._device)
            try:
                # timeout for connecting to VCP
                sock.settimeout(1)
                sock.connect(self._device)
            except OSError as exc:
                log.error('Cannot connect to QEMU VCP port: %s', exc)
                raise
            # timeout for communicating over VCP
            sock.settimeout(0.05)
            log.debug('Execute QEMU for %.0f secs', timeout)
            vcp_buf = bytearray()
            # unfortunately, subprocess's stdout calls are blocking, so the
            # only way to get near real-time output from QEMU is to use a
            # dedicated thread that may block whenever no output is available
            # from the VM. This thread reads and pushes back lines to a local
            # queue, which is popped and logged to the local logger on each
            # loop. Note that Popen's communicate() also relies on threads to
            # perform stdout/stderr read out.
            log_q = Deque()
            Thread(target=self._qemu_logger, name='qemu_out_logger',
                   args=(proc, log_q, True)).start()
            Thread(target=self._qemu_logger, name='qemu_err_logger',
                   args=(proc, log_q, False)).start()
            if ctx:
                ctx.execute('with')
            xstart = now()
            abstimeout = float(timeout) + xstart
            while now() < abstimeout:
                while log_q:
                    err, qline = log_q.popleft()
                    if err:
                        if qline.find('info: ') > 0:
                            self._qlog.info(qline)
                        elif qline.find('warning: ') > 0:
                            self._qlog.warning(qline)
                        else:
                            self._qlog.error(qline)
                    else:
                        self._qlog.info(qline)
                if ctx.check_error():
                    ret = 126
                    raise OSError()
                xret = proc.poll()
                if xret is not None:
                    if xend is None:
                        xend = now()
                    ret = xret
                    if ret != 0:
                        self._log.critical('Abnormal QEMU termination: %d '
                                           'for "%s"', ret, name)
                    break
                try:
                    data = sock.recv(4096)
                except (TimeoutError, LegacyTimeoutError):
                    continue
                vcp_buf += data
                if not vcp_buf:
                    continue
                lines = vcp_buf.split(b'\n')
                vcp_buf = bytearray(lines[-1])
                for line in lines[:-1]:
                    xmo = xre.search(line)
                    if xmo:
                        xend = now()
                        exit_word = xmo.group(1).decode('utf-8',
                                                        errors='ignore')
                        ret = self._get_exit_code(xmo)
                        log.info("Exit sequence detected: '%s' -> %d",
                                 exit_word, ret)
                        if ret == 0:
                            last_guest_error = ''
                        break
                    sline = line.decode('utf-8', errors='ignore').rstrip()
                    lmo = lre.search(sline)
                    if lmo:
                        level = self.LOG_LEVELS.get(lmo.group(1))
                        if level == ERROR:
                            err = re_sub(r'^.*:\d+]', '', sline).lstrip()
                            # be sure not to preserve comma as this char is
                            # used as a CSV separator.
                            last_guest_error = err.strip('"').replace(',', ';')
                    else:
                        level = DEBUG  # fall back when no prefix is found
                    self._otlog.log(level, sline)
                else:
                    # no match
                    continue
                # match
                break
            if ret is None:
                log.warning('Execution timed out for "%s"', name)
                ret = 124  # timeout
        except (OSError, ValueError) as exc:
            if ret is None:
                log.error('Unable to execute QEMU: %s', exc)
                ret = proc.resultcode if proc.poll() is not None else 125
        finally:
            if xend is None:
                xend = now()
            if sock:
                sock.close()
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
        return abs(ret) or 0, xtime, last_guest_error

    def _qemu_logger(self, proc: Popen, queue: Deque, err: bool):
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
    #pylint: disable=too-few-public-methods

    DEFAULT_OTP_ECC_BITS = 6

    def __init__(self, keep_temp: bool = False):
        self._log = getLogger('pyot.file')
        self._keep_temp = keep_temp
        self._in_fly: Set[str] = set()
        self._otp_files: Dict[str, Tuple[str, int]] = {}
        self._env: Dict[str, str] = {}
        self._dirs: Dict[str, str] = {}
        register(self._cleanup)

    @property
    def keep_temporary(self) -> bool:
        """Tell whether temporary files and directories should be preserved or
           not.

           :return: True if temporary items should not be suppressed
        """
        return self._keep_temp

    def set_config_dir(self, path: str) -> None:
        """Assign the configuration directory.

           :param path: the directory that contains the input configuration
                        file
        """
        self._env['CONFIG'] = normpath(path)

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

    def define(self, aliases: Dict[str, Any]) -> None:
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
            aliases[name] = value
            self._env[name.upper()] = value
            self._log.debug('Store %s as %s', name.upper(), value)

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

    def create_flash_image(self, app: Optional[str] = None,
                           bootloader: Optional[str] = None) -> str:
        """Generate a temporary flash image file.

           :param app: optional path to the application or the rom extension
           :param bootloader: optional path to a bootloader
           :return: the full path to the temporary flash file
        """
        #pylint: disable=import-outside-toplevel
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
        #pylint: disable=import-outside-toplevel
        if vmem in self._otp_files:
            otp_file, ref_count = self._otp_files[vmem]
            self._log.debug('Use existing %s', basename(otp_file))
            self._otp_files[vmem] = (otp_file, ref_count + 1)
            return otp_file
        from otpconv import OtpConverter
        otp = OtpConverter(self.DEFAULT_OTP_ECC_BITS)
        self._configure_logger(otp)
        with open(vmem, 'rt', encoding='utf-8') as vfp:
            otp.parse(vfp)
        otp_fd, otp_file = mkstemp(suffix='.raw', prefix='qemu_ot_otp_')
        self._log.debug('Create %s', basename(otp_file))
        self._in_fly.add(otp_file)
        close(otp_fd)
        otp.save('raw', otp_file)
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
        #pylint: disable=too-many-branches
        removed: Set[str] = set()
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
        removed: Set[str] = set()
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

    def __init__(self, cmd: str, env: Dict[str, str]):
        self._log = getLogger('pyot.cmd')
        self._cmd = cmd
        self._env = env
        self._log_q = Deque()
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
        #pylint: disable=too-many-branches
        self._resume = True
        #pylint: disable=consider-using-with
        proc = Popen(self._cmd,  bufsize=1, stdout=PIPE, stderr=PIPE,
                     shell=True, env=self._env, encoding='utf-8',
                     errors='ignore', text=True)
        Thread(target=self._logger, args=(proc, True)).start()
        Thread(target=self._logger, args=(proc, False)).start()
        while self._resume:
            while self._log_q:
                err, qline = self._log_q.popleft()
                if err:
                    if qline.find('info: ') > 0:
                        self._log.info(qline)
                    elif qline.find('warning: ') > 0:
                        self._log.warning(qline)
                    else:
                        self._log.error(qline)
                else:
                    self._log.debug(qline)
            xret = proc.poll()
            if xret is not None:
                self._resume = False
                self._ret = xret
                self._log.debug('"%s" completed with %d', self.command, xret)
                break
        if self._ret is None:
            proc.terminate()
            try:
                # leave 1 second for QEMU to cleanly complete...
                proc.wait(1.0)
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
                 qemu_cmd: List[str], context: Dict[str, List[str]]):
        self._clog = getLogger('pyot.ctx')
        self._test_name = test_name
        self._qfm = qfm
        self._qemu_cmd = qemu_cmd
        self._context = context
        self._workers: List[Popen] = []

    def execute(self, ctx_name: str, code: int = 0) -> None:
        """Execute all commands, in order, for the selected context.

           Synchronous commands are executed in order. If one command fails,
           subsequent commands are not executed.

           Background commands are started in order, but a failure does not
           stop other commands.

           :param code: a previous error completion code, if any
        """
        #pylint: disable=too-many-branches
        #pylint: disable=too-many-locals
        #pylint: disable=too-many-nested-blocks
        ctx = self._context.get(ctx_name, None)
        if ctx_name == 'post' and code:
            self._clog.info('Discard execution of "%s" commands after failure '
                            'of "%s"', ctx_name, self._test_name)
            return
        env = dict(environ)
        if self._qemu_cmd:
            env['PATH'] = ':'.join((env['PATH'], dirname(self._qemu_cmd[0])))
        if ctx:
            for cmd in ctx:
                if cmd.endswith('&'):
                    if ctx_name == 'post':
                        raise ValueError(f'Cannot execute background command '
                                         f'in {ctx_name} context for '
                                         f'"{self._test_name}"')
                    cmd = cmd[:-1].rstrip()
                    self._clog.debug('Execute "%s" in backgrorund for [%s] '
                                     'context', cmd, ctx_name)
                    worker = QEMUContextWorker(cmd, env)
                    worker.run()
                    self._workers.append(worker)
                else:
                    self._clog.debug('Execute "%s" in sync for [%s] context',
                                     cmd, ctx_name)
                    #pylint: disable=consider-using-with
                    proc = Popen(cmd,  bufsize=1, stdout=PIPE, stderr=PIPE,
                                 shell=True, env=env, encoding='utf-8',
                                 errors='ignore', text=True)
                    try:
                        outs, errs = proc.communicate(timeout=5)
                        fail = bool(proc.returncode)
                    except TimeoutExpired:
                        proc.kill()
                        outs, errs = proc.communicate()
                        fail = True
                    for sfp, logger in zip((outs, errs),
                            (self._clog.debug,
                             self._clog.error if fail else self._clog.info)):
                        for line in sfp.split('\n'):
                            line = line.strip()
                            if line:
                                logger(line)
                    if fail:
                        self._log.error('Fail to execute "%s" command for "%s"',
                                        cmd, self._test_name)
                        raise ValueError(f'Cannot execute {ctx_name} command')
        if ctx_name == 'post':
            self._qfm.delete_default_dir(self._test_name)

    def check_error(self) -> bool:
        """Check if any background worker exited in error.

           :return: True if any worker has failed
        """
        for w in self._workers:
            ret = w.exit_code()
            if not ret:
                continue
            self._clog.error("%s exited with %d", w.command, ret)
            return True
        return False

    def finalize(self) -> None:
        """Terminate any running background command, in reverse order.
        """
        while self._workers:
            worker = self._workers.pop()
            ret = worker.stop()
            if ret:
                self._clog.warning('Fail to finalize "%s" command for "%s": %d',
                                   worker.command, self._test_name, ret)


class QEMUExecuter:
    """Test execution sequencer.

       :param qfm: file manager that tracks temporary files
       :param config: configuration dictionary
       :param args: parsed arguments
    """
    #pylint: disable=too-many-instance-attributes

    RESULT_MAP = {
        0: 'PASS',
        1: 'ERROR',
        6: 'ABORT',
        QEMUWrapper.GUEST_ERROR_OFFSET + 1: 'FAIL',
        124: 'TIMEOUT',
        125: 'DEADLOCK',
        126: 'CONTEXT',
        QEMUWrapper.NO_MATCH_RETURN_CODE: 'UNKNOWN',
    }

    def __init__(self, qfm: QEMUFileManager, config: Dict[str, any],
                 args: Namespace):
        self._log = getLogger('pyot.exec')
        self._qfm = qfm
        self._config = config
        self._args = args
        self._argdict: Dict[str, Any] = {}
        self._qemu_cmd: List[str] = []
        self._vcp: Optional[Tuple[str, int]] = None
        self._suffixes = []

    def build(self) -> None:
        """Build initial QEMU arguments.

           :raise ValueError: if some argument is invalid
        """
        self._qemu_cmd, self._vcp, _ = self._build_qemu_command(self._args)
        self._argdict = dict(self._args.__dict__)
        self._suffixes = []
        suffixes = self._config.get('suffixes', [])
        if not isinstance(suffixes, list):
            raise ValueError('Invalid suffixes sub-section')
        self._suffixes.extend(suffixes)

    def run(self, debug: bool) -> int:
        """Execute all requested tests.

           :return: success or the code of the first encountered error
        """
        #pylint: disable=too-many-locals
        qot = QEMUWrapper(self._vcp, debug)
        ret = 0
        results = defaultdict(int)
        result_file = self._argdict.get('result')
        #pylint: disable=consider-using-with
        cfp = open(result_file, 'wt',encoding='utf-8') if result_file else None
        try:
            csv = csv_writer(cfp) if cfp else None
            if csv:
                csv.writerow((x.title() for x in TestResult._fields))
            app = self._argdict.get('exec')
            if app:
                assert 'timeout' in self._argdict
                self._log.info('Execute %s', basename(self._argdict['exec']))
                ret, xtime, err = qot.run(self._qemu_cmd,
                                          self._argdict['timeout'],
                                          self.get_test_radix(app), None)
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
            for tpos, test in enumerate(tests, start=1):
                self._log.info('[TEST %s] (%d/%d)', self.get_test_radix(test),
                               tpos, tcount)
                qemu_cmd, targs, timeout, temp_files, ctx = \
                    self._build_qemu_test_command(test)
                test_name = self.get_test_radix(test)
                ctx.execute('pre')
                tret, xtime, err = qot.run(qemu_cmd, timeout, test_name, ctx)
                ctx.finalize()
                ctx.execute('post', tret)
                results[tret] += 1
                sret = self.RESULT_MAP.get(tret, tret)
                icount = self.get_namespace_arg(targs, 'icount')
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
    def flatten(lst: List) -> List:
        """Flatten a list.
        """
        return [item for sublist in lst for item in sublist]

    def _cleanup_temp_files(self, storage: Dict[str, Set[str]]) -> None:
        if self._qfm.keep_temporary:
            return
        for kind, files in storage.items():
            delete_file = getattr(self._qfm, f'delete_{kind}_image')
            for filename in files:
                delete_file(filename)

    def _build_qemu_command(self, args: Namespace,
                            opts: Optional[List[str]] = None) \
            ->  Tuple[List[str], Tuple[str, int], Dict[str, Set[str]]]:
        """Build QEMU command line from argparser values.

           :param args: the parsed arguments
           :param opts: any QEMU-specific additional options
           :return: a tuple of a list of QEMU command line,
                    the TCP device descriptor to connect to the QEMU VCP, and
                    a dictionary of generated temporary files
        """
        #pylint: disable=too-many-branches
        #pylint: disable=too-many-statements
        if args.qemu is None:
            raise ValueError('QEMU path is not defined')
        qemu_args = [
            args.qemu,
            '-M',
            args.machine,
            '-display',
            'none'
        ]
        if any((args.rom, args.exec, args.boot)):
            qemu_args.append('-kernel')
        if args.rom:
            qemu_args.append(args.rom)
        else:
            if all((args.exec, args.boot)):
                raise ValueError('Cannot use both a ROM ext/app and a '
                                 'bootloader without a ROM file')
            if args.exec:
                qemu_args.append(normpath(args.exec))
            if args.boot:
                qemu_args.append(normpath(args.boot))
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
            qemu_args.extend(('-drive',
                              f'if=pflash,file={args.otp_raw},format=raw'))
        if args.flash:
            if not isfile(args.flash):
                raise ValueError(f'No such flash file: {args.flash}')
            if any((args.exec, args.boot)):
                raise ValueError('Flash file argument is mutually exclusive with'
                                ' bootloader or rom extension')
            qemu_args.extend(('-drive', f'if=mtd,bus=1,file={args.flash},'
                                        f'format=raw'))
        elif any((args.exec, args.boot)):
            if args.exec and not isfile(args.exec):
                raise ValueError(f'No such exec file: {args.exec}')
            if args.boot and not isfile(args.boot):
                raise ValueError(f'No such bootloader file: {args.boot}')
            if args.rom:
                flash_file = self._qfm.create_flash_image(args.exec, args.boot)
                temp_files['flash'].add(flash_file)
                qemu_args.extend(('-drive', f'if=mtd,bus=1,file={flash_file},'
                                 f'format=raw'))
        if args.log_file:
            qemu_args.extend(('-D', args.log_file))
        if args.trace:
            # use a FileType to let argparser validate presence and type
            args.trace.close()
            qemu_args.extend(('-trace', f'events={args.trace.name}'))
        if args.log:
            qemu_args.append('-d')
            qemu_args.append(','.join(args.log))
        if args.singlestep:
            qemu_args.append('-singlestep')
        if 'icount' in args:
            if args.icount is not None:
                qemu_args.extend(('-icount', f'{args.icount}'))
        device = args.device
        devdesc = device.split(':')
        try:
            port = int(devdesc[1])
            if not 0 < port < 65536:
                raise ValueError('Invalid serial TCP port')
            tcpdev = (devdesc[0], port)
            qemu_args.extend(('-chardev',
                              f'socket,id=serial0,host={devdesc[0]},'
                              f'port={port},server=on,wait=on'))
            qemu_args.extend(('-serial', 'chardev:serial0'))
        except TypeError as exc:
            raise ValueError('Invalid TCP serial device') from exc
        if opts:
            qemu_args.extend((str(o) for o in opts))
        return qemu_args, tcpdev, temp_files

    def _build_qemu_test_command(self, filename: str) -> \
            Tuple[List[str], Namespace, int, Dict[str, Set[str]], \
            QEMUContext]:
        test_name = self.get_test_radix(filename)
        args, opts, timeout = self._build_test_args(test_name)
        setattr(args, 'exec', filename)
        qemu_cmd, _, temp_files = self._build_qemu_command(args, opts)
        ctx = self._build_test_context(test_name)
        return qemu_cmd, args, timeout, temp_files, ctx

    def _build_test_list(self, alphasort: bool = True) -> List[str]:
        #pylint: disable=too-many-branches
        #pylint: disable=too-many-locals
        #pylint: disable=too-many-nested-blocks
        pathnames = set()
        testdir = normpath(self._qfm.interpolate(self._config.get('testdir',
                                                                  curdir)))
        self._qfm.define({'testdir': testdir})
        tfilters = self._args.filter or ['*']
        inc_filters = self._config.get('include')
        if inc_filters:
            self._log.debug('Searching for tests from %s dir', testdir)
            if not isinstance(inc_filters, list):
                raise ValueError('Invalid configuration file: '
                                 '"include" is not a list')
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
        exc_filters = self._config.get('exclude')
        if exc_filters:
            if not isinstance(exc_filters, list):
                raise ValueError('Invalid configuration file: '
                                 '"exclude" is not a list')
            for path_filter in filter(None, exc_filters):
                if testdir:
                    path_filter = joinpath(testdir, path_filter)
                paths = set(glob(path_filter, recursive=True))
                pathnames -= paths
        pathnames -= set(self._enumerate_from('exclude_from'))
        if alphasort:
            return sorted(pathnames, key=basename)
        return list(pathnames)

    def _enumerate_from(self, config_entry: str) -> Iterator[str]:
        incf_filters = self._config.get(config_entry)
        if incf_filters:
            if not isinstance(incf_filters, list):
                raise ValueError(f'Invalid configuration file: '
                                 f'"{config_entry}" is not a list')
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

    def _build_test_args(self, test_name: str) \
            -> Tuple[Namespace, List[str], int]:
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
            opts = [self._qfm.interpolate_dirs(opt, test_name) for opt in opts]
        timeout = int(kwargs.get('timeout', DEFAULT_TIMEOUT))
        return Namespace(**kwargs), opts or [], timeout

    def _build_test_context(self, test_name: str) -> QEMUContext:
        context = defaultdict(list)
        tests_cfg = self._config.get('tests', {})
        test_cfg = tests_cfg.get(test_name, {})
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
                    cmd = self._qfm.interpolate(cmd.strip())
                    cmd = self._qfm.interpolate_dirs(cmd, test_name)
                    context[ctx_name].append(cmd)
        return QEMUContext(test_name, self._qfm, self._qemu_cmd, dict(context))


def main():
    """Main routine"""
    #pylint: disable=too-many-branches
    #pylint: disable=too-many-locals
    #pylint: disable=too-many-statements
    #pylint: disable=too-many-nested-blocks
    debug = True
    qemu_path = normpath(joinpath(dirname(dirname(dirname(__file__))),
                                  'build', 'qemu-system-riscv32'))
    if not isfile(qemu_path):
        qemu_path = None
    try:
        args: Optional[Namespace] = None
        argparser = ArgumentParser(description=modules[__name__].__doc__)
        argparser.add_argument('-c', '--config', metavar='JSON',
                               type=FileType('rt', encoding='utf-8'),
                               help='path to configuration file')
        argparser.add_argument('-w', '--result', metavar='CSV',
                               help='path to output result file')
        argparser.add_argument('-k', '--timeout', metavar='SECONDS', type=int,
                               help=f'exit after the specified seconds '
                                    f'(default: {DEFAULT_TIMEOUT} secs)')
        argparser.add_argument('-F', '--filter', metavar='TEST',
                               action='append',
                               help='Only run tests whose filename matches '
                                    'any defined filter (may be repeated)')
        argparser.add_argument('-K', '--keep-tmp', action='store_true',
                               default=False,
                               help='Do not automatically remove temporary '
                                    'files and dirs on exit')
        argparser.add_argument('-v', '--verbose', action='count',
                               help='increase verbosity')
        argparser.add_argument('-d', '--debug', action='store_true',
                               help='enable debug mode')
        qvm = argparser.add_argument_group(title='Virtual machine')
        rel_qemu_path = relpath(qemu_path) if qemu_path else '?'
        qvm.add_argument('-q', '--qemu',
                         help=f'path to qemu application '
                              f'(default: {rel_qemu_path})')
        qvm.add_argument('-Q', '--opts', action='append', default=[],
                         help='QEMU verbatim option (can be repeated)')
        qvm.add_argument('-m', '--machine',
                         help=f'virtual machine (default to {DEFAULT_MACHINE})')
        qvm.add_argument('-p', '--device',
                         help=f'serial port device name '
                              f'(default to {DEFAULT_DEVICE})')
        qvm.add_argument('-L', '--log_file',
                         help='log file for trace and log messages')
        qvm.add_argument('-M', '--log', action='append',
                         help='log message types')
        qvm.add_argument('-t', '--trace', type=FileType('rt', encoding='utf-8'),
                         help='trace event definition file')
        qvm.add_argument('-i', '--icount', metavar='N', type=int,
                         help='virtual instruction counter with 2^N clock ticks'
                              ' per inst.')
        qvm.add_argument('-s', '--singlestep', action='store_true',
                         default=False,
                         help='enable "single stepping" QEMU execution mode')
        files = argparser.add_argument_group(title='Files')
        files.add_argument('-r', '--rom', metavar='ELF', help='ROM file')
        files.add_argument('-O', '--otp-raw', metavar='RAW',
                           help='OTP image file')
        files.add_argument('-o', '--otp', metavar='VMEM', help='OTP VMEM file')
        files.add_argument('-f', '--flash', metavar='RAW',
                           help='embedded Flash image file')
        files.add_argument('-x', '--exec',
                           metavar='file', help='rom extension or application')
        files.add_argument('-b', '--boot',
                           metavar='file', help='bootloader 0 file')

        try:
            # all arguments after `--` are forwarded to QEMU
            pos = argv.index('--')
            sargv = argv[1:pos]
            opts = argv[pos+1:]
        except ValueError:
            sargv = argv[1:]
            opts = []
        args = argparser.parse_args(sargv)
        debug = args.debug
        if opts:
            qopts = getattr(args, 'opts')
            qopts.extend(opts)
            setattr(args, 'opts', qopts)

        loglevel = max(DEBUG, ERROR - (10 * (args.verbose or 0)))
        loglevel = min(ERROR, loglevel)
        formatter = CustomFormatter()
        log = getLogger('pyot')
        logh = StreamHandler(stderr)
        logh.setFormatter(formatter)
        log.setLevel(loglevel)
        log.addHandler(logh)

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
            defaults = json.get('default', {})
            jargs = []
            for arg, val in defaults.items():
                jargs.append(f'--{arg}' if len(arg) > 1 else f'-{arg}')
                # arg parser expects only string args, and substitute shell env.
                val = qfm.interpolate(val)
                jargs.append(val)
            if jargs:
                jwargs = argparser.parse_args(jargs)
                #pylint: disable=protected-access
                for name, val in jwargs._get_kwargs():
                    if not hasattr(args, name):
                        argparser.error(f'Unknown config file default: {name}')
                    if getattr(args, name) is None:
                        setattr(args, name, val)
        elif args.filter:
            argparser.error('Filter option only valid with a config file')
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
        qexc = QEMUExecuter(qfm, json, args)
        try:
            qexc.build()
        except ValueError as exc:
            if debug:
                print(format_exc(chain=False), file=stderr)
            argparser.error(str(exc))
        ret = qexc.run(args.debug)
        log.debug('End of execution with code %d', ret or 0)
        sysexit(ret)
    #pylint: disable=broad-except
    except Exception as exc:
        print(f'{linesep}Error: {exc}', file=stderr)
        if debug:
            print(format_exc(chain=False), file=stderr)
        sysexit(1)
    except KeyboardInterrupt:
        sysexit(2)


if __name__ == '__main__':
    main()
