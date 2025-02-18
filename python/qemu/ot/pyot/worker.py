# Copyright (c) 2023-2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Test executer for QEMU unit test sequencer.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""


from collections import deque
from logging import getLogger
from os.path import basename, normpath
from subprocess import Popen, PIPE, TimeoutExpired
from threading import Event, Thread
from typing import Optional

from .util import LogMessageClassifier


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
        self._first_error = ''

    def run(self):
        """Start the worker.
        """
        self._thread = Thread(target=self._run, daemon=True)
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

    @property
    def first_error(self):
        """Return the message of the first error, if any."""
        return self._first_error

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
        Thread(target=self._logger, args=(proc, True), daemon=True).start()
        Thread(target=self._logger, args=(proc, False), daemon=True).start()
        qemu_exec = f'{basename(self._cmd[0])}: '
        classifier = LogMessageClassifier(qemux=qemu_exec)
        while self._resume:
            while self._log_q:
                err, qline = self._log_q.popleft()
                if err:
                    if not self._first_error:
                        self._first_error = qline
                    loglevel = classifier.classify(qline)
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
            self._log.debug("'%s' completed with '%d'", self.command, self._ret)
        except TimeoutExpired:
            # still executing
            proc.terminate()
            try:
                # leave 1 second for QEMU to cleanly complete...
                proc.wait(1.0)
                self._ret = 0
            except TimeoutExpired:
                # otherwise kill it
                self._log.error("Force-killing command '%s'", self.command)
                proc.kill()
                self._ret = proc.returncode
        # retrieve the remaining log messages
        stdlog = self._log.info if self._ret else self._log.debug
        try:
            outs, errs = proc.communicate(timeout=0.1)
            if not self._first_error:
                self._first_error = errs.split('\n', 1)[0]
            for sfp, logger in zip((outs, errs), (stdlog, self._log.error)):
                for line in sfp.split('\n'):
                    line = line.strip()
                    if line:
                        logger(line)
        except TimeoutExpired:
            proc.kill()
            if self._ret is None:
                self._ret = proc.returncode

    def _logger(self, proc: Popen, err: bool):
        # worker thread, blocking on VM stdout/stderr
        stream = proc.stderr if err else proc.stdout
        while proc.poll() is None:
            line = stream.readline().strip()
            if line:
                self._log_q.append((err, line))
