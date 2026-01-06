# Copyright (c) 2023-2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Test executer for OpenTitan unit test sequencer.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from argparse import Namespace
from collections import defaultdict
from csv import writer as csv_writer
from fnmatch import fnmatchcase
from glob import glob
from logging import DEBUG as LOG_DEBUG, INFO as LOG_INFO, getLogger, FileHandler
from os import curdir, environ, getcwd, sep
from os.path import (abspath, basename, dirname, isabs, isfile,
                     join as joinpath, normpath)
from traceback import format_exc
from typing import Any, Iterator, Optional

import re
import sys

from ot.util.file import guess_file_type
from ot.util.log import flush_memory_loggers
from ot.util.misc import EasyDict, alphanum_key, flatten

from . import DEFAULT_TIMEOUT, DEFAULT_TIMEOUT_FACTOR
from .context import ExecContext
from .filemgr import FileManager
from .util import ExecTime, TestCandidate, TestResult
from .wrapper import Wrapper


class Executer:
    """Test execution sequencer.

       :param tfm: file manager that tracks temporary files
       :param config: configuration dictionary
       :param args: parsed arguments
    """

    CONTEXT_ERROR = 99

    RESULT_MAP = {
        -1: 'SKIPPED',
        0: 'PASS',
        1: 'ERROR',
        6: 'ABORT',
        11: 'CRASH',
        Wrapper.GUEST_ERROR_OFFSET - 1: 'GUEST_ESC',
        Wrapper.GUEST_ERROR_OFFSET + 1: 'FAIL',
        98: 'UNEXP_SUCCESS',
        CONTEXT_ERROR: 'CONTEXT',
        # convention: exit code in [100..115] range report uncaught exceptions
        100: 'INST_ADDR_MISALIGN',
        101: 'INST_ACCESS_FAULT',
        102: 'INST_ILLEGAL',
        103: 'BREAKPOINT',
        104: 'LOAD_ADDR_ALIGN',
        105: 'LOAD_ACCESS_FAULT',
        106: 'STORE_ADDR_ALIGN',
        107: 'STORE_ACCESS_FAULT',
        108: 'ECALL_U_MODE',
        109: 'ECALL_S_MODE',
        110: 'ECALL_H_MODE',
        111: 'ECALL_M_MODE',
        112: 'INST_PAGE_FAULT',
        113: 'LOAD_PAGE_FAULT',
        115: 'STORE_PAGE_FAULT',
        # end of RISC-V exception converted to exit code
        124: 'TIMEOUT',
        125: 'DEADLOCK',
        126: 'CONTEXT',
        Wrapper.NO_MATCH_RETURN_CODE: 'UNKNOWN',
    }

    DEFAULT_START_DELAY = 1.0
    """Default start up delay to let QEMU initialize before connecting the
       virtual UART port.
    """

    DEFAULT_SERIAL_PORT = 'serial0'
    """Default VCP name."""

    WRAPPER = Wrapper
    """Default wrapper."""

    def __init__(self, tfm: FileManager, config: dict[str, any],
                 args: Namespace):
        self._log = getLogger('pyot.exec')
        self._tfm = tfm
        self._config = config
        self._args = args
        self._argdict: dict[str, Any] = {}
        self._qemu_cmd: list[str] = []
        self._suffixes = []
        self._virtual_tests: dict[str, str] = {}
        if hasattr(self._args, 'opts'):
            setattr(self._args, 'global_opts', getattr(self._args, 'opts'))
            setattr(self._args, 'opts', [])
        else:
            setattr(self._args, 'global_opts', [])

    def build(self) -> None:
        """Build initial QEMU arguments.

           :raise ValueError: if some argument is invalid
        """
        exec_info = self._build_command(self._args)
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
        for candidate in sorted(self._build_test_list()):
            tst, reason = candidate.name, candidate.reason
            tpath = self._virtual_tests.get(tst)
            ttype = guess_file_type(tpath or tst)
            rpath = f' [{basename(tpath)}]' if tpath else ''
            discarded = f' {{discarded: {reason}}}' if reason else ''
            yield f'{basename(tst)} ({ttype}){rpath}{discarded}'

    def run(self, debug: bool, allow_no_test: bool) -> int:
        """Execute all requested tests.

           :return: success or the code of the first encountered error
        """
        log_classifiers = self._config.get('logclass', {})
        qot = self.WRAPPER(log_classifiers, debug)
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
                timeout = int(float(self._argdict.get('timeout')) *
                              float(self._argdict.get('timeout_factor',
                                                      DEFAULT_TIMEOUT_FACTOR)))
                self._log.debug('Execute %s', basename(self._argdict['exec']))
                adef = EasyDict(command=self._qemu_cmd, timeout=timeout,
                                start_delay=self.DEFAULT_START_DELAY,
                                asan=self._argdict.get('asan', False))
                ret, xtime, err = qot.run(adef)
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
            for tpos, tcand in enumerate(tests, start=1):
                test, reason = tcand.name, tcand.reason
                test_name = self.get_test_radix(test)
                vcplogfile = None
                if reason:
                    tret = -1
                    sret = self.RESULT_MAP[tret]
                    if csv:
                        self._log.info('[TEST %s] (%d/%d) skipped',
                                       test_name, tpos, tcount)
                        csv.writerow(TestResult(test_name, sret, '',
                                                '', reason.title()))
                        cfp.flush()
                        results[tret] += 1
                    continue
                try:
                    self._tfm.define_transient({
                        'UTPATH': test,
                        'UTDIR': normpath(dirname(test)),
                        'UTFILE': basename(test),
                    })
                    self._log.info('[TEST %s] (%d/%d)', test_name, tpos, tcount)
                    context = 'pre'
                    exec_info = self._build_test_command(test)
                    exec_info.test_name = test_name
                    vcplogfile = self._log_vcp_streams(exec_info)
                    exec_info.context.execute(context)
                    context = 'with'
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
                        tret = self.CONTEXT_ERROR
                    if tret and not err:
                        err = exec_info.context.first_error
                    if not tret:
                        context = 'post'
                    exec_info.context.execute(context, tret)
                # pylint: disable=broad-except
                except Exception as exc:
                    self._log.critical('%s', str(exc))
                    if debug:
                        print(format_exc(chain=False), file=sys.stderr)
                    tret = self.CONTEXT_ERROR
                    xtime = ExecTime(0.0)
                    err = str(exc)
                finally:
                    self._discard_vcp_log(vcplogfile)
                    self._tfm.cleanup_transient()
                    if not self._tfm.keep_temporary:
                        self._tfm.delete_default_dir(test_name)
                    flush_memory_loggers(['pyot', 'pyot.vcp', 'pyot.ctx',
                                          'pyot.file'], LOG_INFO)
                results[tret] += 1
                sret = self.RESULT_MAP.get(tret) or str(tret)
                if tret == self.CONTEXT_ERROR:
                    sret = f'{sret} ({context.upper()})'
                try:
                    targs = exec_info.args
                    icount = self.get_namespace_arg(targs, 'icount')
                except (AttributeError, KeyError, UnboundLocalError):
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
        # sort by the largest occurrence, discarding success
        errors = sorted((x for x in results.items() if x[0] > 0),
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
    def abspath(path: str) -> str:
        """Build absolute path"""
        if isabs(path):
            return normpath(path)
        return normpath(joinpath(getcwd(), path))

    def _cleanup_temp_files(self, storage: dict[str, set[str]]) -> None:
        if self._tfm.keep_temporary:
            return
        for kind, files in storage.items():
            delete_file = getattr(self._tfm, f'delete_{kind}_image')
            for filename in files:
                delete_file(filename)

    def _build_command(self, args: Namespace,
                       opts: Optional[list[str]] = None) -> EasyDict[str, Any]:
        raise NotImplementedError('Abstract command')

    def _build_test_command(self, filename: str) -> EasyDict[str, Any]:
        test_name = self.get_test_radix(filename)
        args, opts, timeout, texp = self._build_test_args(test_name)
        setattr(args, 'exec', filename)
        exec_info = self._build_command(args, opts)
        exec_info.pop('connection', None)
        exec_info.args = args
        exec_info.context = self._build_test_context(test_name)
        exec_info.timeout = timeout
        exec_info.expect_result = texp
        return exec_info

    def _build_test_list(self) -> list[TestCandidate]:
        pathnames = set()
        testdir = normpath(self._tfm.interpolate(self._config.get('testdir',
                                                                  curdir)))
        self._tfm.define({'testdir': testdir})
        cfilters = self._args.filter or []
        pfilters = [f for f in cfilters if not f.startswith('!')]
        if not pfilters:
            cfilters = ['*'] + cfilters
            tfilters = ['*'] + pfilters
        else:
            tfilters = list(pfilters)
        virttests = self._config.get('virtual', {})
        if not isinstance(virttests, dict):
            raise ValueError('Invalid virtual tests definition')
        vtests = {}
        for vname, vpath in virttests.items():
            if not isinstance(vname, str):
                raise ValueError(f"Invalid virtual test definition '{vname}'")
            if sep in vname:
                raise ValueError(f"Virtual test name cannot contain directory "
                                 f"specifier: '{vname}'")
            rpath = normpath(self._tfm.interpolate(vpath))
            if not isfile(rpath):
                raise ValueError(f"Invalid virtual test '{vname}': "
                                 f"missing file '{rpath}'")
            vtests[vname] = rpath
        self._virtual_tests.update(vtests)
        inc_filters = self._build_config_list('include')
        if inc_filters:
            self._log.debug('Searching for tests from %s dir', testdir)
            for candidate in inc_filters:
                path_filter = candidate.name
                if not path_filter:
                    continue
                if testdir:
                    path_filter = joinpath(testdir, path_filter)
                paths = set(glob(path_filter, recursive=True))
                for path in paths:
                    if isfile(path):
                        for tfilter in tfilters:
                            if fnmatchcase(self.get_test_radix(path), tfilter):
                                pathnames.add(path)
                                break
                for vpath in vtests:
                    for tfilter in tfilters:
                        if fnmatchcase(self.get_test_radix(vpath), tfilter):
                            pathnames.add(vpath)
                            break
        for testfile in self._enumerate_from('include_from'):
            if not isfile(testfile):
                raise ValueError(f'Unable to locate test file '
                                 f'"{testfile}"')
            for tfilter in tfilters:
                if fnmatchcase(self.get_test_radix(testfile), tfilter):
                    pathnames.add(testfile)
        if not pathnames:
            return []
        roms = self._argdict.get('rom', [])
        pathnames -= {normpath(rom) for rom in roms}

        xtfilters = [TestCandidate(f[1:].strip(), 'CMDLINE')
                     for f in cfilters if f.startswith('!')]
        exc_filters = self._build_config_list('exclude')
        xtfilters.extend(exc_filters)
        discarded: set[TestCandidate] = set()
        if xtfilters:
            for candidate in xtfilters:
                path_filter, reason = candidate.name, candidate.reason
                if not path_filter:
                    continue
                if testdir:
                    path_filter = joinpath(testdir, path_filter)
                paths = set(glob(path_filter, recursive=True))
                if reason:
                    discarded.update(TestCandidate(t, reason)
                                     for t in pathnames & paths)
                pathnames -= paths
                vdiscards: set[str] = set()
                for vpath in vtests:
                    if fnmatchcase(vpath, basename(path_filter)):
                        vdiscards.add(vpath)
                if reason:
                    discarded.update(TestCandidate(t, reason)
                                     for t in pathnames & vdiscards)
                pathnames -= vdiscards
        ftfilters = set(self._enumerate_from('exclude_from'))
        pathnames -= ftfilters
        candidates = set(TestCandidate(t) for t in pathnames)
        candidates.update(discarded)
        return sorted(candidates,
                      key=lambda tc: alphanum_key(basename(tc.name)))

    def _enumerate_from(self, config_entry: str) -> Iterator[str]:
        incf_filters = self._build_config_list(config_entry)
        if incf_filters:
            for candidate in incf_filters:
                incf = candidate.name
                incf = normpath(self._tfm.interpolate(incf))
                if not isfile(incf):
                    raise ValueError(f'Invalid test file: "{incf}"')
                self._log.debug('Loading test list from %s', incf)
                incf_dir = dirname(incf)
                with open(incf, 'rt', encoding='utf-8') as ifp:
                    for testfile in ifp:
                        testfile = re.sub('#.*$', '', testfile).strip()
                        if not testfile:
                            continue
                        testfile = self._tfm.interpolate(testfile)
                        if not testfile.startswith(sep):
                            testfile = joinpath(incf_dir, testfile)
                        yield normpath(testfile)

    def _build_config_list(self, config_entry: str) -> list[TestCandidate]:
        cfglist: list[str] = []
        items = self._config.get(config_entry)
        if not items:
            return cfglist
        if not isinstance(items, list):
            raise ValueError(f'Invalid configuration file: '
                             f'"{config_entry}" is not a list')
        for item in items:
            if isinstance(item, str):
                cfglist.append(TestCandidate(item))
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
                                cfglist.append(TestCandidate(sitem, dname))
        return cfglist

    def _build_test_args(self, test_name: str) \
            -> tuple[Namespace, list[str], int, int]:
        tests_cfg = self._config.get('tests', {})
        if not isinstance(tests_cfg, dict):
            raise ValueError('Invalid tests sub-section')
        kwargs = dict(self._args.__dict__)
        test_cfg = tests_cfg.get(test_name, {})
        if test_cfg is None:
            # does not default to an empty dict to differentiate empty from
            # inexistent test configuration
            self._log.debug('No configuration for test %s', test_name)
            opts = None
        else:
            # use same arg parser dash-underscore replacement for option name
            test_cfg = {k.replace('-', '_'): v for k, v in test_cfg.items()
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
            opts = flatten([opt.split(' ') for opt in opts])
            opts = [self._tfm.interpolate(opt) for opt in opts]
            opts = flatten([opt.split(' ') for opt in opts])
            opts = [self._tfm.interpolate_dirs(opt, test_name) for opt in opts]
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

    def _build_test_context(self, test_name: str) -> ExecContext:
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
                    cmd = re.sub(r'[\n\r]', ' ', cmd.strip())
                    cmd = re.sub(r'\s{2,}', ' ', cmd)
                    cmd = self._tfm.interpolate(cmd)
                    cmd = self._tfm.interpolate_dirs(cmd, test_name)
                    context[ctx_name].append(cmd)
            env = test_cfg.get('env')
            if env:
                if not isinstance(env, dict):
                    raise ValueError('Invalid context environment')
                test_env = {k: self._tfm.interpolate(v) for k, v in env.items()}
        return ExecContext(test_name, self._qemu_cmd, dict(context), test_env)

    def _log_vcp_streams(self, exec_info: EasyDict[str, Any]) -> str:
        logfile = exec_info.get('logfile', None)
        if not logfile:
            return None
        assert exec_info.test_name
        logfile = self._tfm.interpolate_dirs(logfile, exec_info.test_name)
        vcplog = getLogger('pyot.vcp')
        logfh = FileHandler(logfile, 'w')
        # log everything
        logfh.setLevel(LOG_DEBUG)
        # force the logger to emit all messages as well
        # (side effect on all handlers)
        vcplog.setLevel(LOG_DEBUG)
        vcplog.handlers.append(logfh)
        return logfile

    def _discard_vcp_log(self, vcplogfile: Optional[str]) -> None:
        if not vcplogfile:
            return
        vcplogfile = abspath(vcplogfile)
        vcplog = getLogger('pyot.vcp')
        for handler in vcplog.handlers:
            if isinstance(handler, FileHandler):
                if handler.baseFilename == vcplogfile:
                    handler.close()
                    vcplog.removeHandler(handler)
