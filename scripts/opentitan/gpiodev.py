#!/usr/bin/env python3

"""GPIO device tiny simulator.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

# Copyright (c) 2024 Rivos, Inc.
# All rights reserved.

from argparse import ArgumentParser, FileType
from logging import getLogger
from os.path import dirname, join as joinpath, normpath
from socket import create_server, socket, SHUT_RDWR
from traceback import format_exc
from time import sleep
from typing import Optional, TextIO
import re
import sys

QEMU_PYPATH = joinpath(dirname(dirname(dirname(normpath(__file__)))),
                       'python', 'qemu')
sys.path.append(QEMU_PYPATH)

from ot.util.log import configure_loggers
from ot.util.misc import HexInt

# pylint: disable-msg=missing-function-docstring,missing-class-docstring
# pylint: disable-msg=too-few-public-methods
# pylint: disable-msg=too-many-instance-attributes


class GpioChecker:

    CHK_RE = r'^\s*(\d+)?\s*([@]):([0-9a-fA-F]{8})$'
    """Handler either log file or `uniq -c` post-processed file."""

    def __init__(self):
        self._log = getLogger('gpio.check')
        self._seq = []

    def load(self, lfp: TextIO) -> None:
        commands = ''.join(GpioDevice.INPUT_CMD_MAP.keys())
        chk_re = self.CHK_RE.replace('@', commands)
        error = 0
        for lno, line in enumerate(lfp, start=1):
            line = line.strip()
            cmo = re.match(chk_re, line)
            if not cmo:
                self._log.error('Unknown check line @ %d: %s', lno, line)
                error += 1
                continue
            repeat, command, value = cmo.groups()
            irepeat = int(repeat) if repeat else 1
            ivalue = int(value, 16)
            while irepeat:
                self._seq.append((command, ivalue))
                irepeat -= 1
        if error:
            raise RuntimeError('Cannot use checker file {lfp.name}')

    class Iterator:

        def __init__(self, parent: 'GpioChecker'):
            self._log = getLogger('gpio.dev.it')
            self._iter = enumerate(parent._seq)
            self._len = len(parent._seq)
            self._pos = 0

        def __iter__(self):
            return self

        def __next__(self) -> tuple[str, int]:
            self._pos, value = next(self._iter)
            return value

        @property
        def last(self) -> bool:
            self._log.debug("pos: %d len: %d", self._pos, self._len)
            return self._pos + 1 >= self._len

    def __iter__(self) -> Iterator:
        return self.Iterator(self)


class GpioDevice:

    INPUT_CMD_MAP = {
        'C': 'clear',
        'D': 'direction',
        'O': 'output',
        'P': 'pull',
        'Q': 'query',
        'Y': 'ynput',
        'Z': 'hi_z',
    }

    OUTPUT_CMD_MAP = {
        'input': 'I',
        'mask': 'M',
        'repeat': 'R',
    }

    def __init__(self):
        self._log = getLogger('gpio.dev')
        self._socket = None
        self._checker: Optional[TextIO] = None
        self._resume = False
        self._oe = 0  # output enable (1: out, 0: in)
        self._out = 0  # output value (from peer)
        self._hiz = 0xffff_ffff  # high-Z
        self._wpud = 0  # weak pull (1: up 0: down)
        self._inact = 0xffff_ffff  # input activated
        self._in = 0  # input value (to peer)
        self._yn = 0  # mirror input value handled by QEMU
        self._error_count = 0
        self._record = []

    def load(self, lfp: TextIO) -> None:
        self._checker = GpioChecker()
        self._checker.load(lfp)

    def save(self, sfp: TextIO) -> None:
        for cmd, value in self._record:
            print(f'{cmd}:{value:08x}', end='\r\n', file=sfp)
        sfp.close()

    def run(self, port: int, single_run: bool, fatal: bool,
            end: Optional[int]) -> bool:
        self._socket = create_server(('localhost', port), reuse_port=True)
        fail = False
        self._socket.listen(0)
        while True:
            resume = True
            peer, addr = self._socket.accept()
            with peer:
                self._log.info('Connection from %s:%d', *addr)
                # rewind on each connection
                it_cmd = iter(self._checker) if self._checker else None
                self._error_count = 0
                buf = bytearray()
                while resume:
                    if end is not None:
                        if it_cmd and it_cmd.last:
                            self._terminate(peer, end)
                            resume = False
                            break
                    data = peer.recv(1024)
                    if not data:
                        break
                    buf.extend(data)
                    try:
                        buf = self._process(peer, it_cmd, buf)
                    except ValueError:
                        fail = True
                        resume = False
                        break
            try:
                self._log.info('Disconnect from %s:%d', *addr)
                peer.close()
                peer.shutdown(SHUT_RDWR)
            except OSError:
                pass
            if single_run:
                break
            if fail and fatal:
                break
        try:
            self._socket.close()
            self._socket.shutdown(SHUT_RDWR)
        except OSError:
            pass
        self._socket = None
        return not fail

    def _process(self, peer: socket, it_cmd: Optional[GpioChecker.Iterator],
                 buf: bytearray) -> bytearray:
        while True:
            eol = buf.find(b'\n')
            if eol < 0:
                return buf
            line, buf = buf[:eol], buf[eol+1:]
            line = line.strip()
            sline = line.decode('utf8')
            self._log.debug('in %s', sline)
            resp = self._inject(sline, it_cmd)
            if resp is not None:
                for oline in resp.split('\n'):
                    if oline:
                        self._log.info('send %s', oline.strip())
                out = resp.encode('utf8')
                peer.send(out)

    def _terminate(self, peer: socket, end: int) -> None:
        resp = self._build_reply(mask=~end, input=end)
        for oline in resp.split('\n'):
            if oline:
                self._log.info('send %s', oline.strip())
        out = resp.encode('utf8')
        peer.send(out)
        sleep(0.1)

    def _inject(self, line: str, it_cmd: Optional[GpioChecker.Iterator]) -> \
            Optional[str]:
        try:
            cmd, value = line.split(':', 1)
        except ValueError:
            self._log.error('Unsupported line: %s', line)
            return None
        try:
            word = int(value, 16)
        except ValueError:
            self._log.error('Unsupported value: %s', value)
            return None
        try:
            command = self.INPUT_CMD_MAP[cmd]
        except KeyError:
            self._log.error('Unsupported command: %s', cmd)
            return None
        handler = getattr(self, f'_inject_{command}', None)
        if handler is None:
            self._log.warning('Unimplemented handler for %s', command)
            return None
        self._log.info('recv %s: 0x%08x', command, word)
        self._record.append((cmd, word))
        # pylint: disable=not-callable
        out = handler(word)
        if it_cmd:
            try:
                refcmd, refword = next(it_cmd)
            except StopIteration as exc:
                self._log.warning('End of checker')
                raise ValueError('Unexpected command') from exc
            self._log.debug('ck %c:%08x', refcmd, refword)
            if cmd != refcmd:
                self._log.error('Received command %s differs from expected %s',
                                cmd, refcmd)
                raise ValueError('Command mismatch')
            if word != refword:
                self._log.error('Received word 0x%08x differs from expected '
                                '0x%08x', word, refword)
                raise ValueError('Value mismatch')
        return out

    def _inject_clear(self, _) -> None:
        self._oe = 0
        self._out = 0
        self._hiz = 0xfffffff
        self._wpud = 0
        self._inact = 0
        self._in = 0

    def _inject_output(self, value) -> None:
        self._out = value

    def _inject_direction(self, value) -> None:
        self._oe = value

    def _inject_pull(self, value) -> None:
        self._wpud = value

    def _inject_query(self, _) -> str:
        return self._build_reply(mask=self._inact, input=self._in)

    def _inject_hi_z(self, value) -> None:
        self._hiz = value

    def _inject_ynput(self, value) -> None:
        self._yn = value

    @classmethod
    def _build_reply(cls, **kwargs) -> str:
        lines = []
        for cmd, value in kwargs.items():
            value &= (1 << 32) - 1
            lines.append(f'{cls.OUTPUT_CMD_MAP[cmd]}:{value:08x}\r\n')
        return ''.join(lines)


def main():
    """Main routine.
    """
    debug = False
    try:
        desc = sys.modules[__name__].__doc__.split('.', 1)[0].strip()
        argparser = ArgumentParser(description=f'{desc}.')
        argparser.add_argument('-p', '--port', type=int, default=8007,
                               help='remote host TCP port (defaults to 8007)')
        argparser.add_argument('-c', '--check', type=FileType('rt'),
                               help='input file to check command sequence')
        argparser.add_argument('-r', '--record', type=FileType('wt'),
                               help='output file to record command sequence')
        argparser.add_argument('-e', '--end', type=HexInt.parse,
                               help='emit the specified value to trigger '
                                    'remote exit on last received command')
        argparser.add_argument('-q', '--quit-on-error', action='store_true',
                               default=False, help='exit on first error')
        argparser.add_argument('-s', '--single', action='store_true',
                               default=False,
                               help='run once: terminate once first remote '
                                    'disconnects')
        argparser.add_argument('-t', '--log-time', action='store_true',
                               default=False, help='emit time in log messages')
        argparser.add_argument('-v', '--verbose', action='count',
                               help='increase verbosity')
        argparser.add_argument('-d', '--debug', action='store_true',
                               help='enable debug mode')
        args = argparser.parse_args()
        debug = args.debug

        configure_loggers(args.verbose, 'gpio', ms=args.log_time)

        if args.end and not args.check:
            argparser.error('Auto-end cannot be enabled without a check file')

        gpio = GpioDevice()
        if args.check:
            gpio.load(args.check)
        try:
            exec_ok = gpio.run(args.port, args.single, args.quit_on_error,
                               args.end)
        finally:
            if args.record:
                gpio.save(args.record)
        sys.exit(int(not exec_ok))

    except (IOError, ValueError, ImportError) as exc:
        print(f'\nError: {exc}', file=sys.stderr)
        if debug:
            print(format_exc(chain=False), file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(2)


if __name__ == '__main__':
    main()
