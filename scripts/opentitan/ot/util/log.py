# Copyright (c) 2023-2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Logging helpers.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from logging import (Formatter, Logger, StreamHandler, CRITICAL, DEBUG, INFO,
                     ERROR, WARNING, getLogger)
from os import isatty
from sys import stderr
from typing import List, Tuple


class ColorLogFormatter(Formatter):
    """Custom log formatter for ANSI terminals.
       Colorize log levels.

       Optional features:
         * 'time' (boolean): prefix log messsages with 24h HH:MM:SS time
         * 'ms' (boolean): prefix log messages with 24h HH:MM:SS.msec time
         * 'lineno'(boolean): show line numbers
         * 'name_width' (int): padding width for logger names
         * 'color' (boolean): enable/disable log level colorization
    """

    GREY = "\x1b[38;20m"
    YELLOW = "\x1b[33;1m"
    RED = "\x1b[31;1m"
    MAGENTA = "\x1b[35;1m"
    WHITE = "\x1b[37;1m"
    RESET = "\x1b[0m"
    FMT_LEVEL = '%(levelname)8s'

    COLORS = {
        DEBUG: GREY,
        INFO: WHITE,
        WARNING: YELLOW,
        ERROR: RED,
        CRITICAL: MAGENTA,
    }

    def __init__(self, *args, **kwargs):
        kwargs = dict(kwargs)
        name_width = kwargs.pop('name_width', 10)
        self._use_ansi = kwargs.pop('color', isatty(stderr.fileno()))
        use_ms = kwargs.pop('ms', False)
        use_time = kwargs.pop('time', use_ms)
        use_lineno = kwargs.pop('lineno', False)
        super().__init__(*args, **kwargs)
        format_trail = f' %(name)-{name_width}s %(message)s'
        if use_time:
            tfmt = '%(asctime)s ' if not use_ms else '%(asctime)s.%(msecs)03d '
        else:
            tfmt = ''
        lno = ' [%(lineno)d] ' if use_lineno else ''
        self._plain_format = f'{tfmt}{self.FMT_LEVEL}{lno}{format_trail}'
        self._color_formats = {
            lvl: f'{tfmt}{clr}{self.FMT_LEVEL}{self.RESET}{lno}{format_trail}'
            for lvl, clr in self.COLORS.items()
        }
        self._formatter_args = ['%H:%M:%S'] if use_time else []

    def format(self, record):
        log_fmt = self._color_formats[record.levelno] if self._use_ansi \
                  else self._plain_format
        formatter = Formatter(log_fmt, *self._formatter_args)
        return formatter.format(record)


def configure_loggers(level: int, *lognames: List[str], **kwargs) \
        -> List[Logger]:
    """Configure loggers.

       :param level: level (stepping: 1)
       :param lognames: one or more loggers to configure
       :param kwargs: optional features
       :return: configured loggers or level change
    """
    loglevel = ERROR - (10 * (level or 0))
    loglevel = min(ERROR, loglevel)
    formatter = ColorLogFormatter(**kwargs)
    logh = StreamHandler(stderr)
    logh.setFormatter(formatter)
    loggers: List[Logger] = []
    logdefs: List[Tuple[List[str], Logger]] = []
    for logdef in lognames:
        if isinstance(logdef, int):
            loglevel += -10 * logdef
            continue
        log = getLogger(logdef)
        log.setLevel(max(DEBUG, loglevel))
        loggers.append(log)
        logdefs.append((logdef.split('.'), log))
    logdefs.sort(key=lambda p: len(p[0]))
    # ensure there is only one handler per logger subtree
    for _, log in logdefs:
        if not log.hasHandlers():
            log.addHandler(logh)
    return loggers
