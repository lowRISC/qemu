# Copyright (c) 2023-2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Test context for OpenTitan unit test sequencer.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from logging import getLogger, DEBUG, ERROR, INFO
from os import environ, pardir, sep
from os.path import basename, dirname, normpath, relpath
from subprocess import Popen, PIPE, TimeoutExpired
from threading import Event
from typing import Optional

import re

from .util import LogMessageClassifier
from .worker import ContextWorker


class ExecContext:
    """Execution context for test executer session.

       Execute commands before, while and after test executer runs.

       :param test_name: the name of the test executer should execute
       :param ex_cmd: the host command and argument to execute the test
       :param context: the contex configuration for the current test
    """

    def __init__(self, test_name: str, ex_cmd: list[str],
                 context: dict[str, list[str]],
                 env: Optional[dict[str, str]] = None):
        # pylint: disable=too-many-arguments
        self._clog = getLogger('pyot.ctx')
        self._test_name = test_name
        self._ex_cmd = ex_cmd
        self._context = context
        self._env = env or {}
        self._workers: list[Popen] = []
        self._first_error: str = ''
        self._classifier = LogMessageClassifier()

    @property
    def test_name(self) -> str:
        """Provide the test name."""
        return self._test_name

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
        if self._ex_cmd:
            env['PATH'] = ':'.join((env['PATH'], dirname(self._ex_cmd[0])))
        if ctx:
            for cmd in ctx:
                bkgnd = ctx_name == 'with'
                timeout = 5
                optmo = re.match(r'^(.*)(?:(&)|(!)(?:@(\d+))?)$', cmd)
                if optmo:
                    cmd = optmo.group(1)
                    if optmo.group(2):
                        bkgnd = True
                    elif optmo.group(3):
                        bkgnd = False
                        if optmo.group(4):
                            timeout = int(optmo.group(4))
                cmd = normpath(cmd.rstrip())
                if bkgnd:
                    if ctx_name == 'post':
                        raise ValueError(f"Cannot execute background command "
                                         f"in [{ctx_name}] context for "
                                         f"'{self._test_name}'")
                    rcmd = relpath(cmd)
                    if rcmd.startswith(pardir):
                        rcmd = cmd
                    rcmd = ' '.join(p if not p.startswith(sep) else basename(p)
                                    for p in rcmd.split(' '))
                    self._clog.info('Execute "%s" in background for [%s] '
                                    'context', rcmd, ctx_name)
                    worker = ContextWorker(cmd, env, sync)
                    worker.run()
                    self._workers.append(worker)
                else:
                    if sync:
                        self._clog.debug('Synchronization ignored')
                    cmd = normpath(cmd.rstrip())
                    rcmd = relpath(cmd)
                    if rcmd.startswith(pardir):
                        rcmd = cmd
                    rcmd = ' '.join(p if not p.startswith(sep) else basename(p)
                                    for p in rcmd.split(' '))
                    self._clog.info('Execute "%s" in sync for [%s] context',
                                    rcmd, ctx_name)
                    # pylint: disable=consider-using-with
                    proc = Popen(cmd, bufsize=1, stdout=PIPE, stderr=PIPE,
                                 shell=True, env=env, encoding='utf-8',
                                 errors='ignore', text=True)
                    ret = 0
                    try:
                        outs, errs = proc.communicate(timeout=timeout)
                        ret = proc.returncode
                    except TimeoutExpired:
                        proc.kill()
                        outs, errs = proc.communicate()
                        ret = proc.returncode
                    for sfp, deflevel in zip((outs, errs),
                                             (DEBUG, ERROR if ret else INFO)):
                        for line in sfp.split('\n'):
                            line = line.strip()
                            if line:
                                loglevel = self._classifier.classify(line,
                                                                     deflevel)
                                self._clog.log(loglevel, line)
                                if loglevel >= ERROR and not self._first_error:
                                    self._first_error = line
                    if not self._first_error:
                        self._first_error = errs.split('\n', 1)[0]
                    if ret:
                        self._clog.error("Fail to execute '%s' command for "
                                         "'%s'", cmd, self._test_name)
                        errmsg = self._first_error or \
                            f'Cannot execute [{ctx_name}] command'
                        raise OSError(ret, errmsg)

    def check_error(self) -> int:
        """Check if any background worker exited in error.

           :return: a non-zero value on error
        """
        for worker in self._workers:
            ret = worker.exit_code()
            if not ret:
                continue
            if not self._first_error:
                self._first_error = worker.first_error
            self._clog.error("%s exited with %d", worker.command, ret)
            return ret
        return 0

    @property
    def first_error(self):
        """Return the message of the first error, if any."""
        return self._first_error

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
                self._clog.warning("Command '%s' has failed for '%s': %d",
                                   worker.command, self._test_name, ret)
                if not self._first_error:
                    self._first_error = worker.first_error
        return max(rets)
