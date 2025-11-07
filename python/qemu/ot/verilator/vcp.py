# Copyright (c) 2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Verilator virtual comm port management."""

from fcntl import fcntl, F_GETFL, F_SETFL
from os import O_NONBLOCK
from io import BufferedRandom
from select import POLLIN, POLLERR, POLLHUP, poll as sel_poll
from sys import stderr
from time import time as now
from traceback import format_exc
from typing import NamedTuple, Optional

import logging
import re

from ot.util.log import ColorLogFormatter


class VtorVcpDescriptor(NamedTuple):
    """ Virtual communication port."""

    vcpid: str
    """VCP identifier."""
    pty: BufferedRandom
    """Attached pseudo terminal."""
    buffer: bytearray
    """Data buffer."""
    logger: logging.Logger
    """Associated logger."""


class VtorVcpManager:
    """Virtual COM port manager.

       Handles messages emitted on serial port.
    """

    ANSI_CRE = re.compile(rb'(\x9B|\x1B\[)[0-?]*[ -\/]*[@-~]')
    """ANSI escape sequences."""

    def __init__(self):
        self._log = logging.getLogger('vtor.vcp')
        self._vcps: dict[int, VtorVcpDescriptor] = {}
        self._poller = sel_poll()

    def connect(self, vcp_map: dict[str, str], delay: float) -> None:
        """Connect Verilator pseudo-terminals.

           :param vcp_map: a map of serial port, pseudo terminal device
           :param delay: how long to wait for a successful connection,
                         in seconds
        """
        vcp_map = dict(vcp_map)
        timeout = now() + delay
        # ensure that QEMU starts and give some time for it to set up
        # when multiple VCPs are set to 'wait', one VCP can be connected at
        # a time, i.e. QEMU does not open all connections at once.
        vcp_lognames = []
        vcplogname = 'vtor'
        vcplognames = []
        while vcp_map:
            if now() > timeout:
                minfo = ', '.join(f'{d} @ {r}'
                                  for d, r in vcp_map.items())
                raise TimeoutError(f'Cannot connect to Verilator VCPs: {minfo}')
            connected = []
            for vcpid, ptyname in vcp_map.items():
                try:
                    # pylint: disable=consider-using-with
                    vcp = open(ptyname, 'rb+', buffering=0)
                    flags = fcntl(vcp, F_GETFL)
                    fcntl(vcp, F_SETFL, flags | O_NONBLOCK)
                    connected.append(vcpid)
                    vcp_lognames.append(vcpid)
                    vcp_log = logging.getLogger(f'{vcplogname}.{vcpid}')
                    vcplognames.append(vcpid)
                    vcp_fno = vcp.fileno()
                    assert vcp_fno not in self._vcps
                    self._vcps[vcp_fno] = VtorVcpDescriptor(vcpid, vcp,
                                                            bytearray(),
                                                            vcp_log)
                    self._log.debug('VCP %s connected to pty %s',
                                    vcpid, ptyname)
                    self._poller.register(vcp, POLLIN | POLLERR | POLLHUP)
                except ConnectionRefusedError:
                    continue
                except OSError as exc:
                    self._log.error('Cannot setup Verilator VCP connection %s: '
                                    '%s', vcpid, exc)
                    print(format_exc(chain=False), file=stderr)
                    raise
            # removal from dictionary cannot be done while iterating it
            for vcpid in connected:
                del vcp_map[vcpid]
        self._colorize_vcp_log(vcplogname, vcplognames)

    def disconnect(self) -> None:
        """Disconnect all managed VCPs."""
        for _, vcp, _, _ in self._vcps.values():
            self._poller.unregister(vcp)
            vcp.close()
        self._vcps.clear()

    def process(self) -> Optional[int]:
        """Handle any received message on VCPs.

           :return: an optional integer (exit code) to early abort execution.
                    not yet implemented, always return None.
        """
        ret = None
        for vfd, event in self._poller.poll(0.01):
            if event in (POLLERR, POLLHUP):
                self._poller.modify(vfd, 0)
                continue
            _, vcp, vcp_buf, vcp_log = self._vcps[vfd]
            try:
                data = vcp.read(256)
            except TimeoutError:
                self._log.error('Unexpected timeout w/ poll on %s', vcp)
                continue
            if not data:
                continue
            vcp_buf += data
            lines = vcp_buf.split(b'\n')
            vcp_buf[:] = bytearray(lines[-1])
            for line in lines[:-1]:
                line = self.ANSI_CRE.sub(b'', line)
                sline = line.decode('utf-8', errors='ignore').rstrip()
                level = logging.INFO
                vcp_log.log(level, sline)
            if ret is not None:
                # match for exit sequence on current VCP
                break
        return ret

    def _colorize_vcp_log(self, logbase: str, lognames: list[str]) -> None:
        vlog = logging.getLogger(logbase)
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
            clr_fmt.add_logger_colors(f'{logbase}.{logname}', color)
