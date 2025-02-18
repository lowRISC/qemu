# Copyright (c) 2023-2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""QEMU wrapper for QEMU unit test sequencer.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from collections import deque
from os.path import basename, dirname
from select import POLLIN, POLLERR, POLLHUP, poll as spoll
from socket import socket, timeout as LegacyTimeoutError
from subprocess import Popen, PIPE, TimeoutExpired
from threading import Event, Thread
from time import time as now
from traceback import format_exc
from typing import Any, Optional

import logging
import re
import sys

from ot.util.log import ColorLogFormatter
from ot.util.misc import EasyDict

from ot.pyot.util import ExecTime, LogMessageClassifier


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

    ANSI_CRE = re.compile(rb'(\x9B|\x1B\[)[0-?]*[ -\/]*[@-~]')
    """ANSI escape sequences."""

    GUEST_ERROR_OFFSET = 40
    """Offset for guest errors. Should be larger than the host max signal value.
    """

    NO_MATCH_RETURN_CODE = 100
    """Return code when no matching string is found in guest output."""

    def __init__(self, log_classifiers: dict[str, list[str]], debug: bool):
        self._log_classifiers = log_classifiers
        self._debug = debug
        self._log = logging.getLogger('pyot')
        self._qlog = logging.getLogger('pyot.qemu')

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
                - validate, a string to match on the QEMU virtual comm port
                           output to early exit. It may be defined as a regular
                           expression.
                - start_delay, the delay to wait before starting the execution
                           of the context once QEMU command has been started.
           :return: a 3-uple of exit code, execution time, and last guest error
        """
        # stdout and stderr belongs to QEMU VM
        # OT's UART0 is redirected to a TCP stream that can be accessed through
        # self._device. The VM pauses till the TCP socket is connected
        xre = re.compile(self.EXIT_ON)
        sync_event = None
        if tdef.trigger:
            sync_event = Event()
        match_pattern = tdef.trigger or tdef.validate
        if match_pattern:
            if match_pattern.startswith("r'") and match_pattern.endswith("'"):
                try:
                    tmo = re.compile(match_pattern[2:-1].encode())
                except re.error as exc:
                    raise ValueError('Invalid regex: {exc}') from exc

                def trig_match(bline):
                    return tmo.match(bline)
            else:
                btrigger = match_pattern.encode()

                def trig_match(bline):
                    return bline.find(btrigger) >= 0
        else:
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
                   args=(proc, log_q, True), daemon=True).start()
            Thread(target=self._qemu_logger, name='qemu_err_logger',
                   args=(proc, log_q, False), daemon=True).start()
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
                    minfo = ', '.join(f'{d} @ {r[0]}:{r[1]}'
                                      for d, r in connect_map.items())
                    raise TimeoutError(f'Cannot connect to QEMU VCPs: {minfo}')
                connected = []
                for vcpid, (host, port) in connect_map.items():
                    try:
                        # timeout for connecting to VCP
                        sock = socket()
                        sock.settimeout(1)
                        sock.connect((host, port))
                        connected.append(vcpid)
                        vcp_name = re.sub(r'^.*[-\.+]', '', vcpid)
                        vcp_lognames.append(vcp_name)
                        vcp_log = logging.getLogger(f'{vcplogname}.{vcp_name}')
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
                        print(format_exc(chain=False), file=sys.stderr)
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
            classifier = LogMessageClassifier(classifiers=self._log_classifiers,
                                              qemux=qemu_exec)
            abstimeout = float(tdef.timeout) + now()
            qemu_default_log = logging.ERROR
            vcp_default_log = logging.DEBUG
            while now() < abstimeout:
                while log_q:
                    err, qline = log_q.popleft()
                    if err:
                        level = classifier.classify(qline, qemu_default_log)
                        if level == logging.INFO and \
                           qline.find('QEMU waiting for connection') >= 0:
                            level = logging.DEBUG
                    else:
                        level = logging.INFO
                    self._qlog.log(level, qline)
                if tdef.context:
                    wret = tdef.context.check_error()
                    if wret:
                        ret = wret
                        last_error = tdef.context.first_error or \
                            'Fail to execute worker'
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
                            trig_match = None
                            if sync_event:
                                log.info('Trigger pattern detected, resuming '
                                         'for %.0f secs', tdef.timeout)
                                sync_event.set()
                            else:
                                log.info('Validation pattern detected, exiting')
                                ret = 0
                                break
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
                        level = classifier.classify(sline, vcp_default_log)
                        if level == logging.ERROR:
                            err = re.sub(r'^.*:\d+]', '', sline).lstrip()
                            # be sure not to preserve comma as this char is
                            # used as a CSV separator.
                            last_error = err.strip('"').replace(',', ';')
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
    def classify_log(cls, line: str, default: int = logging.ERROR,
                     qemux: Optional[str] = None) -> int:
        """Classify log level of a line depending on its content.

           :param line: line to classify
           :param default: defaut log level in no classification is found
           :return: the logger log level to use
        """
        if qemux and line.startswith(qemux):
            # discard QEMU internal messages that cannot be disable from the VM
            return logging.NOTSET
        if (line.find('info: ') >= 0 or
            line.startswith('INFO ') or
            line.find(' INFO ') >= 0):  # noqa
            return logging.INFO
        if (line.find('warning: ') >= 0 or
            line.startswith('WARNING ') or
            line.find(' WARNING ') >= 0):  # noqa
            return logging.WARNING
        if (line.find('debug: ') >= 0 or
            line.startswith('DEBUG ') or
            line.find(' DEBUG ') >= 0):  # noqa
            return logging.DEBUG
        return default

    def _colorize_vcp_log(self, vcplogname: str, lognames: list[str]) -> None:
        vlog = logging.getLogger(vcplogname)
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
            clr_fmt.add_logger_colors(f'{vcplogname}.{logname}', color)

    def _qemu_logger(self, proc: Popen, queue: deque, err: bool):
        # worker thread, blocking on VM stdout/stderr
        stream = proc.stderr if err else proc.stdout
        while proc.poll() is None:
            line = stream.readline().strip()
            if line:
                queue.append((err, line))

    def _get_exit_code(self, xmo: re.Match) -> int:
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
        alts = re.sub(rb'^.*\((.*?)\).*$', rb'\1', xmo.re.pattern).split(b'|')
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
