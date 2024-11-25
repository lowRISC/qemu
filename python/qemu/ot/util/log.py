# Copyright (c) 2023-2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Logging helpers.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from os import getenv, isatty
from pickle import loads as ploads
from socketserver import DatagramRequestHandler, ThreadingUDPServer
from struct import calcsize as scalc, unpack as sunpack
from sys import stderr
from threading import Thread
from typing import NamedTuple, Optional, Sequence, Union

import logging
from logging.handlers import (DatagramHandler, MemoryHandler,
                              DEFAULT_UDP_LOGGING_PORT)


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
        'BLUE': 34,
        'CYAN': 36,
        'YELLOW': 33,
        'GREEN': 32,
        'MAGENTA': 35,
        'RED': 31,
        'GREY': 38,
        'WHITE': 37,
        'BLACK': 30,
    }

    XCOLORS = (
        202,
        214,
        228,
        154,
        48,
        39,
        111,
        135,
        165,
        197,
        208,
        226,
        82,
        51,
        105,
        99,
        171,
        201,
    )
    """Extended ANSI colors for 256-color terminals."""

    RESET = "\x1b[0m"
    FMT_LEVEL = '%(levelname)8s'

    # pylint: disable=no-self-argument
    def _make_fg_color(code: int, ext: bool = False,
                       bright: Optional[bool] = None) -> str:
        """Create the ANSI escape sequence for a foregound color.

           :param code: color code
           :param ext: whether to use limited (16) or extended (256) color
                       palette
           :param bright: when set, use bright mode
           :return: the ANSI escape sequence
        """
        if not ext:
            if not 30 <= code <= 39:
                raise ValueError(f'Invalid ANSI color: {code}')
            bri = 1 if bright else 20
            return f'\x1b[{code};{bri}m' if code != 38 else '\x1b[38;20m'
        if not 0 <= code < 255:
            raise ValueError(f'Invalid ANSI color: {code}')
        return f'\x1b[38;5;{code}m'

    # pylint: disable=no-staticmethod-decorator
    make_fg_color = staticmethod(_make_fg_color)

    LOG_COLORS = {
        logging.DEBUG: _make_fg_color(COLORS['GREY']),
        logging.INFO: _make_fg_color(COLORS['WHITE'], bright=True),
        logging.WARNING: _make_fg_color(COLORS['YELLOW'], bright=True),
        logging.ERROR: _make_fg_color(COLORS['RED'], bright=True),
        logging.CRITICAL: _make_fg_color(COLORS['MAGENTA'], bright=True),
    }

    def __init__(self, *args, **kwargs):
        kwargs = dict(kwargs)
        name_width = kwargs.pop('name_width', 10)
        self._use_ansi = kwargs.pop('ansi', isatty(stderr.fileno()))
        self._use_xansi = self._use_ansi and getenv('TERM', '').find('256') >= 0
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
        fnc = f' %(funcName)s(){sep}' if use_func else ' '
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
        if self._use_ansi:
            logname = record.name
            while logname:
                if logname in self._logger_colors:
                    scr, ecr = self._logger_colors[logname]
                    break
                if '.' not in logname:
                    break
                logname = logname.rsplit('.', 1)[0]
        setattr(record, 'scr', scr)
        setattr(record, 'ecr', ecr)
        formatter = logging.Formatter(log_fmt, *self._formatter_args)
        return formatter.format(record)

    def add_logger_colors(self, logname: str, color: Union[int | str]) -> None:
        """Assign a color to the message of a specific logger."""
        if not self._use_ansi:
            return
        if isinstance(color, int):
            if self._use_xansi:
                code = self.XCOLORS[color % len(self.XCOLORS)]
                start_color = self.make_fg_color(code, ext=True)
            else:
                code = list(self.COLORS.values())[color % len(self.COLORS)]
                start_color = self.make_fg_color(code)
        elif isinstance(color, str):
            scolor = self.COLORS.get(color.upper(), '')
            if not scolor:
                return
            start_color = self.make_fg_color(scolor)
        else:
            raise TypeError(f'Unknown logger color specifier: {color}')
        end_color = self.RESET
        self._logger_colors[logname] = (start_color, end_color)

    @classmethod
    def override_xcolors(cls, codes: Sequence[int]) -> None:
        """Override default extended color palette.

           :param codes: ANSI color codes
        """
        xcolors = list(codes)
        if any(c for c in xcolors
               if not (isinstance(c, int) and 0 <= c <= 255)):
            raise ValueError('Invalid color code(s)')
        cls.XCOLORS = xcolors


class RemoteLogHandler(DatagramRequestHandler):
    """Handle a remote log UDP request."""

    HEADER_FMT = '!L'
    """UDP prefix that specifies the payload length."""

    HEADER_SIZE = scalc(HEADER_FMT)
    """The length in bytes of the header."""

    def handle(self):
        buffer = bytearray()
        while True:
            data = self.rfile.read(4)
            if not data:
                break
            buffer.extend(data)
            if len(buffer) < self.HEADER_SIZE:
                continue
            header, buffer = (buffer[:self.HEADER_SIZE],
                buffer[self.HEADER_SIZE:])
            record_len, = sunpack(self.HEADER_FMT, header)
            while len(buffer) < record_len:
                data = self.rfile.read(4096)
                if not data:
                    break
                buffer.extend(data)
            obj_data = buffer[:record_len]
            obj = ploads(obj_data)
            record = logging.makeLogRecord(obj)
            logname = f'pyot.{record.name}'
            self.server.do_log(logname, record)


class RemoteLogServer(ThreadingUDPServer):
    """ThreadingUDPServer extension with logger cache manager."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._cache: dict[str, logging.Logger] = {}

    def do_log(self, logname: str, record: logging.LogRecord):
        """Do actual log.

           This function enables log level local filtering and caching.

           :param logname: name of the logger
           :param record: the log record
        """
        if logname not in self._cache:
            logger = logging.getLogger(logname)
            self._cache[logname] = logger
        lgr = self._cache[logname]
        lgr.handle(record)


class RemoteLogService:
    """Simple UDP receiver for handling remote log record."""

    allow_reuse_address = True
    daemon_threads = True

    def __init__(self,  host='127.0.0.1', port=0):
        RemoteLogServer.allow_reuse_address = True
        RemoteLogServer.daemon_threads = True
        self._port = port or DEFAULT_UDP_LOGGING_PORT
        self._server = RemoteLogServer((host, self._port), RemoteLogHandler)
        self._thread = Thread(target=self._serve, daemon=True)

    @property
    def port(self) -> int:
        """Return the selected UDP port."""
        return self._port

    def start(self):
        """Start the logging service thread."""
        self._thread.start()

    def stop(self):
        """Stop the logging service thread."""
        self._server.shutdown()

    def _serve(self):
        with self._server:
            self._server.serve_forever()


def configure_loggers(level: int, *lognames: list[Union[str,  int, Color]],
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
    quiet = kwargs.pop('quiet', False)
    udplog = kwargs.pop('udplog', None)
    formatter = ColorLogFormatter(**kwargs)
    if udplog is not None:
        udpport = udplog or DEFAULT_UDP_LOGGING_PORT
        assert isinstance(udpport, int)
        # use IP value rather than localhost host to avoid repetitive name
        # resolution
        shandler = DatagramHandler('127.0.0.1', udpport)
    else:
        shandler = logging.StreamHandler(stderr)
    shandler.setFormatter(formatter)
    if quiet:
        logh = MemoryHandler(100000, target=shandler, flushOnClose=False)
        shandler.setLevel(loglevel)
    else:
        logh = shandler
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


def flush_memory_loggers(loggers: Sequence[Union[logging.Logger, str]],
                         level: Optional[int] = None) -> None:
    """Discard all buffered records of any MemoryHandler logger handlers.

       :param loggers: the loggers to consider, or the root logger if none is
                       specified.
       :param level: if not None, flush all records whose level is higher or
                     equal to the specified level.
    """
    if not loggers:
        loggers = [logging.getLogger()]
    for log in loggers:
        if isinstance(log, str):
            log = logging.getLogger(log)
        for hdlr in log.handlers:
            if not isinstance(hdlr, MemoryHandler):
                continue
            if level is None:
                super(MemoryHandler, hdlr).flush()
                continue
            with hdlr.lock:
                if hdlr.target:
                    for record in hdlr.buffer:
                        if record.levelno >= level:
                            hdlr.target.handle(record)
                    hdlr.buffer.clear()
