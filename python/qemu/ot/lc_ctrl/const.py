# Copyright (c) 2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Lifecycle helpers.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from copy import deepcopy
from logging import getLogger
from typing import TextIO

from ot.otp.lifecycle import OtpLifecycle


class LcCtrlConstants:
    """Life Cycle constant manager.
    """

    def __init__(self):
        self._log = getLogger('lc.const')
        self._states: dict[str, tuple[str, str]] = {}
        self._tokens: dict[str, str] = {}
        self._diversifiers: dict [str, str] = {}

    def load_sv(self, svp: TextIO) -> None:
        """Decode LC information from a System Verilog file.

           :param svp: System Verilog stream with OTP definitions.
        """
        lcext = OtpLifecycle()
        lcext.load(svp)
        states = lcext.get_configuration('LC_STATE')
        if not states:
            raise ValueError('Cannot obtain LifeCycle states')
        for raw in {s for s in states if int(s, 16) == 0}:
            del states[raw]
        ostates = list(states)
        self._states['lc_state'] = ostates[0], ostates[-1]
        self._log.info("States first: '%s', last '%s'",
                       states[ostates[0]], states[ostates[-1]])
        trans = lcext.get_configuration('LC_TRANSITION_CNT')
        if not trans:
            raise ValueError('Cannot obtain LifeCycle transitions')
        for raw in {s for s in trans if int(s, 16) == 0}:
            del trans[raw]
        otrans = list(trans)
        self._states['lc_trscnt'] = otrans[0], otrans[-1]
        self._log.info('Transitions first: %d, last %d',
                       int(trans[otrans[0]]),
                       int(trans[otrans[1]]))
        self._tokens.update(lcext.get_tokens(False, False))
        socdbg = lcext.get_configuration('SOCDBG')
        if socdbg:
            for raw in {s for s in socdbg if int(s, 16) == 0}:
                del socdbg[raw]
            osoc = list(socdbg)
            self._states['socdbg'] = osoc[0], osoc[-1]
            self._log.info("Socdbg first: '%s', last '%s'",
                           socdbg[osoc[0]], socdbg[osoc[-1]])
        ownership = lcext.get_configuration('OWNERSHIP')
        if ownership:
            for raw in {s for s in ownership if int(s, 16) == 0}:
                del ownership[raw]
            osoc = list(ownership)
            self._states['ownership'] = osoc[0], osoc[-1]
            self._log.info("Ownership first: '%s', last '%s'",
                           ownership[osoc[0]], ownership[osoc[1]])

    @property
    def states(self) -> dict[str, tuple[str, str]]:
        """Return initial and final values for all known states.

           :return: a state name indexed map with first and last states
                    hex string values.
        """
        return deepcopy(self._states)

    @property
    def tokens(self) -> dict[str, str]:
        """Return LC token values.
        """
        return deepcopy(self._tokens)
