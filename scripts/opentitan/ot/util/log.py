# Copyright (c) 2023-2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Logging helpers.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from os import isatty
from sys import stderr
from typing import NamedTuple, Union

import logging


try:
    getLevelNamesMapping = logging.getLevelNamesMapping
except AttributeError:
    # getLevelNamesMapping not supported on old Python versions (< 3.11)
    def getLevelNamesMapping() -> dict[str, int]:
        # pylint: disable=invalid-name,missing-function-docstring
        return {lvl: getattr(logging, lvl) for lvl in
                'CRITICAL FATAL ERROR WARNING INFO DEBUG'.split()}

_LEVELS = {k: v for k, v in getLevelNamesMapping().items()
           if k not in ('NOTSET', 'WARN')}


class Color(NamedTuple):
    """Simple color wrapper."""
    color: str


class ColorLogFormatter(logging.Formatter):
    """Custom log formatter for ANSI terminals.
       Colorize log levels.

       Optional features:
         * 'time' (boolean): prefix log messsages with 24h HH:MM:SS time
         * 'ms' (boolean): prefix log messages with 24h HH:MM:SS.msec time
         * 'lineno'(boolean): show line numbers
         * 'name_width' (int): padding width for logger names
         * 'color' (boolean): enable/disable log level colorization
    """

    COLORS = {
        'GREY': "\x1b[38;20m",
        'GREEN': "\x1b[32;1m",
        'YELLOW': "\x1b[33;1m",
        'RED': "\x1b[31;1m",
        'BLUE': "\x1b[34;1m",
        'MAGENTA': "\x1b[35;1m",
        'CYAN': "\x1b[36;1m",
        'WHITE': "\x1b[37;1m",
    }

    RESET = "\x1b[0m"
    FMT_LEVEL = '%(levelname)8s'

    LOG_COLORS = {
        logging.DEBUG: COLORS['GREY'],
        logging.INFO: COLORS['WHITE'],
        logging.WARNING: COLORS['YELLOW'],
        logging.ERROR: COLORS['RED'],
        logging.CRITICAL: COLORS['MAGENTA'],
    }

    def __init__(self, *args, **kwargs):
        kwargs = dict(kwargs)
        name_width = kwargs.pop('name_width', 10)
        self._use_ansi = kwargs.pop('ansi', isatty(stderr.fileno()))
        use_func = kwargs.pop('funcname', False)
        use_ms = kwargs.pop('ms', False)
        use_time = kwargs.pop('time', use_ms)
        use_lineno = kwargs.pop('lineno', False)
        super().__init__(*args, **kwargs)
        self._logger_colors: dict[str, tuple[str, str]] = {}
        if use_time:
            tfmt = '%(asctime)s ' if not use_ms else '%(asctime)s.%(msecs)03d '
        else:
            tfmt = ''
        sep = ' ' if not use_lineno else ''
        fnc = f' %(funcName)s{sep}' if use_func else ' '
        sep = ' ' if not use_func else ''
        lno = f'{sep}[%(lineno)d] ' if use_lineno else ''
        fmt_trail = f' %(name)-{name_width}s{fnc}{lno}%(scr)s%(message)s%(ecr)s'
        self._plain_format = f'{tfmt}{self.FMT_LEVEL}{fmt_trail}'
        self._color_formats = {
            lvl: f'{tfmt}{clr}{self.FMT_LEVEL}{self.RESET}{fmt_trail}'
            for lvl, clr in self.LOG_COLORS.items()
        }
        self._formatter_args = ['%H:%M:%S'] if use_time else []

    def format(self, record):
        log_fmt = self._color_formats[record.levelno] if self._use_ansi \
                  else self._plain_format
        scr, ecr = ('', '')
        if self._use_ansi and record.name in self._logger_colors:
            scr, ecr = self._logger_colors[record.name]
        setattr(record, 'scr', scr)
        setattr(record, 'ecr', ecr)
        formatter = logging.Formatter(log_fmt, *self._formatter_args)
        return formatter.format(record)

    def add_logger_colors(self, logname: str, color: str) -> None:
        """Assign a color to the message of a specific logger."""
        if not self._use_ansi:
            return
        start_color = self.COLORS.get(color.upper(), '')
        if not start_color:
            return
        end_color = self.RESET
        self._logger_colors[logname] = (start_color, end_color)


def configure_loggers(level: int, *lognames: list[Union[str | int | Color]],
                      **kwargs) -> list[logging.Logger]:
    """Configure loggers.

       :param level: level (stepping: 1)
       :param lognames: one or more loggers to configure, or log modifiers
       :param kwargs: optional features
       :return: configured loggers or level change
    """
    loglevel = logging.ERROR - (10 * (level or 0))
    loglevel = min(logging.ERROR, loglevel)
    loglevels = {}
    for lvl in _LEVELS:
        lnames = kwargs.pop(lvl.lower(), None)
        if lnames:
            if isinstance(lnames, str):
                lnames = [lnames]
            loglevels[lvl] = tuple(lnames)
    formatter = ColorLogFormatter(**kwargs)
    logh = logging.StreamHandler(stderr)
    logh.setFormatter(formatter)
    loggers: list[logging.Logger] = []
    logdefs: list[tuple[list[str], logging.Logger]] = []
    color = None
    for logdef in lognames:
        if isinstance(logdef, int):
            loglevel += -10 * logdef
            continue
        if isinstance(logdef, Color):
            color = logdef.color
            continue
        if color:
            formatter.add_logger_colors(logdef, color)
        log = logging.getLogger(logdef)
        log.setLevel(max(logging.DEBUG, loglevel))
        loggers.append(log)
        logdefs.append((logdef.split('.'), log))
    for lvl, lnames in loglevels.items():
        for lname in lnames:
            log = logging.getLogger(lname)
        log.setLevel(_LEVELS[lvl])
    logdefs.sort(key=lambda p: len(p[0]))
    # ensure there is only one handler per logger subtree
    for _, log in logdefs:
        if not log.hasHandlers():
            log.addHandler(logh)
    return loggers
