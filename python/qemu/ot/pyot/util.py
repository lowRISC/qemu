# Copyright (c) 2023-2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Utilities for OpenTitan unit test sequencer.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from csv import reader as csv_reader
from textwrap import shorten
from typing import NamedTuple, Optional

import logging
import re


class ExecTime(float):
    """Float with hardcoded formatter.
    """

    def __repr__(self) -> str:
        return f'{self*1000:.0f} ms'


class TestCandidate(NamedTuple):
    """Test candidate.
    """
    name: str
    reason: Optional[str] = None


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
        """Load a CSV file (generated with an Executer) and parse it.

           :param csvpath: the path to the CSV file.
        """
        with open(csvpath, 'rt', encoding='utf-8') as cfp:
            csv = csv_reader(cfp)
            for row in csv:
                self._results.append(row)

    def show(self, spacing: bool = False, result: Optional[str] = None) -> None:
        """Print a simple formatted ASCII table with loaded CSV results.

           :param spacing: add an empty line before and after the table
           :param result: overall result, if any
        """
        results = [r[:-1] + [shorten(r[-1], width=100)] for r in self._results]
        if not results:
            return
        if spacing:
            print('')
        # third row is time, always defined as ms, see ExecTime
        total_time = sum(float(r[2].strip().split(' ')[0])
                               for r in self._results[1:])
        tt_str = f'{total_time / 1000:.1f} s'
        last_row = ['TEST SESSION', result or '?', tt_str]
        last_row.extend([''] * (len(self._results) - len(last_row)))
        results.append(last_row)
        widths = [max(len(x) for x in col) for col in zip(*results)]
        self._show_line(widths, '-')
        self._show_row(widths, results[0])
        self._show_line(widths, '=')
        last_rix = len(self._results) - 2
        for rix, row in enumerate(results[1:]):
            self._show_row(widths, row)
            self._show_line(widths, '-' if rix != last_rix else '=')
        if spacing:
            print('')

    def _show_line(self, widths: list[int], csep: str) -> None:
        print(f'+{"+".join([csep * (w+2) for w in widths])}+')

    def _show_row(self, widths: list[int], cols: list[str]) -> None:
        line = '|'.join([f' {c:{">" if p else "<"}{w}s} '
                         for p, (w, c) in enumerate(zip(widths, cols))])
        print(f'|{line}|')


class LogMessageClassifier:
    """Log level classifier for log messages.

       :param classifiers: a map of loglevel, list of RE-compatible strings
                           to match messages
       :param app_exec: the executable name, to filter out useless messages
    """

    def __init__(self, classifiers: Optional[dict[str, list[str]]] = None,
                 app_exec: Optional[str] = None):
        self._app_exec = app_exec
        if classifiers is None:
            classifiers = {}
        self._regexes: dict[int, re.Pattern] = {}
        for klv in 'error warning info debug'.split():
            uklv = klv.upper()
            cstrs = classifiers.get(klv, [])
            if not isinstance(cstrs, list):
                raise ValueError(f'Invalid log classifiers for {klv}')
            regexes = [f'{klv}: ', f'^{uklv} ', f' {uklv} ']
            for cstr in cstrs:
                try:
                    # only sanity-check pattern, do not use result
                    re.compile(cstr)
                except re.error as exc:
                    raise ValueError(f"Invalid log classifier '{cstr}' for "
                                     f"{klv}: {exc}") from exc
                regexes.append(cstr)
            if regexes:
                lvl = getattr(logging, uklv)
                self._regexes[lvl] = re.compile(f"({'|'.join(regexes)})")
            else:
                lvl = getattr(logging, 'NOTSET')
                # never match RE
                self._regexes[lvl] = re.compile(r'\A(?!x)x')

    def classify(self, line: str, default: int = logging.ERROR) -> int:
        """Classify log level of a line depending on its content.

           :param line: line to classify
           :param default: defaut log level in no classification is found
           :return: the logger log level to use
        """
        if self._app_exec and line.startswith(self._app_exec):
            # discard QEMU internal messages that cannot be disable from the VM
            if line.find("QEMU waiting") > 0:
                return logging.NOTSET
        for lvl, pattern in self._regexes.items():
            if pattern.search(line):
                return lvl
        return default
