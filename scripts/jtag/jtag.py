# Copyright (c) 2010-2024, Emmanuel Blot <emmanuel.blot@free.fr>
# Copyright (c) 2016, Emmanuel Bouaziz <ebouaziz@free.fr>
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

"""JTAG tools.

   Based on JTAG support for FTDI from PyFtdi module
"""

# pylint: enable=missing-function-docstring

from logging import getLogger
from typing import List, Optional, Tuple, Union

from .bits import BitSequence


class JtagError(Exception):
    """Generic JTAG error."""


class JtagState:
    """Test Access Port controller state.

       :param name: the name of the state
       :param modes: categories to which the state belongs
    """

    def __init__(self, name: str, modes: Tuple[str, str]):
        self.name = name
        self.modes = modes
        self.exits = [self, self]  # dummy value before initial configuration

    def __str__(self):
        return self.name

    def __repr__(self):
        return self.name

    def setx(self, fstate: 'JtagState', tstate: 'JtagState'):
        """Define the two exit state of a state."""
        self.exits = [fstate, tstate]

    def getx(self, event) -> 'JtagState':
        """Retrieve the exit state of the state.

           :param event: evaluated as a boolean value
           :return: next state
        """
        return self.exits[int(bool(event))]

    def is_of(self, mode: str) -> bool:
        """Report if the state is a member of the specified mode."""
        return mode in self.modes


class JtagStateMachine:
    """Test Access Port controller state machine."""

    def __init__(self):
        self._log = getLogger('jtag.fsm')
        self.states = {}
        for state, modes in [('test_logic_reset', ('reset', ' idle')),
                             ('run_test_idle', ('idle',)),
                             ('select_dr_scan', ('dr',)),
                             ('capture_dr', ('dr', 'shift', 'capture')),
                             ('shift_dr', ('dr', 'shift')),
                             ('exit_1_dr', ('dr', 'update', 'pause')),
                             ('pause_dr', ('dr', 'pause')),
                             ('exit_2_dr', ('dr', 'shift', 'udpate')),
                             ('update_dr', ('dr', 'idle')),
                             ('select_ir_scan', ('ir',)),
                             ('capture_ir', ('ir', 'shift', 'capture')),
                             ('shift_ir', ('ir', 'shift')),
                             ('exit_1_ir', ('ir', 'udpate', 'pause')),
                             ('pause_ir', ('ir', 'pause')),
                             ('exit_2_ir', ('ir', 'shift', 'update')),
                             ('update_ir', ('ir', 'idle'))]:
            self.states[state] = JtagState(state, modes)
        self['test_logic_reset'].setx(self['run_test_idle'],
                                      self['test_logic_reset'])
        self['run_test_idle'].setx(self['run_test_idle'],
                                   self['select_dr_scan'])
        self['select_dr_scan'].setx(self['capture_dr'],
                                    self['select_ir_scan'])
        self['capture_dr'].setx(self['shift_dr'], self['exit_1_dr'])
        self['shift_dr'].setx(self['shift_dr'], self['exit_1_dr'])
        self['exit_1_dr'].setx(self['pause_dr'], self['update_dr'])
        self['pause_dr'].setx(self['pause_dr'], self['exit_2_dr'])
        self['exit_2_dr'].setx(self['shift_dr'], self['update_dr'])
        self['update_dr'].setx(self['run_test_idle'],
                               self['select_dr_scan'])
        self['select_ir_scan'].setx(self['capture_ir'],
                                    self['test_logic_reset'])
        self['capture_ir'].setx(self['shift_ir'], self['exit_1_ir'])
        self['shift_ir'].setx(self['shift_ir'], self['exit_1_ir'])
        self['exit_1_ir'].setx(self['pause_ir'], self['update_ir'])
        self['pause_ir'].setx(self['pause_ir'], self['exit_2_ir'])
        self['exit_2_ir'].setx(self['shift_ir'], self['update_ir'])
        self['update_ir'].setx(self['run_test_idle'], self['select_dr_scan'])
        self._current = self['test_logic_reset']

    def __getitem__(self, name: str) -> JtagState:
        return self.states[name]

    @property
    def state(self) -> JtagState:
        """Return the current state."""
        return self._current

    def state_of(self, mode: str) -> bool:
        """Report if the current state is of the specified mode."""
        return self._current.is_of(mode)

    def reset(self):
        """Reset the state machine."""
        self._current = self['test_logic_reset']

    def find_path(self, target: Union[JtagState, str],
                  source: Union[JtagState, str, None] = None) \
            -> List[JtagState]:
        """Find the shortest event sequence to move from source state to
           target state. If source state is not specified, used the current
           state.

           :return: the list of states, including source and target states.
        """
        if source is None:
            source = self.state
        if isinstance(source, str):
            source = self[source]
        if isinstance(target, str):
            target = self[target]

        def next_path(state, target, path):
            # this test match the target, path is valid
            if state == target:
                return path+[state]
            # candidate paths
            paths = []
            for xstate in state.exits:
                # next state is self (loop around), kill the path
                if xstate == state:
                    continue
                # next state already in upstream (loop back), kill the path
                if xstate in path:
                    continue
                # try the current path
                npath = next_path(xstate, target, path + [state])
                # downstream is a valid path, store it
                if npath:
                    paths.append(npath)
            # keep the shortest path
            return min(((len(path), path) for path in paths),
                       key=lambda x: x[0])[1] if paths else []
        return next_path(source, target, [])

    @classmethod
    def get_events(cls, path):
        """Build up an event sequence from a state sequence, so that the
           resulting event sequence allows the JTAG state machine to advance
           from the first state to the last one of the input sequence"""
        events = []
        for sstate, dstate in zip(path[:-1], path[1:]):
            for epos, xstate in enumerate(sstate.exits):
                if xstate == dstate:
                    events.append(epos)
        if len(events) != len(path) - 1:
            raise JtagError("Invalid path")
        return BitSequence(events)

    def handle_events(self, events: BitSequence) -> None:
        """State machine stepping.

           :param events: a sequence of boolean events to advance the FSM.
        """
        for event in events:
            self._current = self._current.getx(event)


class JtagController:
    """JTAG master API."""

    INSTRUCTIONS = dict(bypass=0x0, idcode=0x1)
    """Common instruction register codes."""

    def tap_reset(self, use_trst: bool = False) -> None:
        """Reset the TAP controller.

           :param use_trst: use TRST HW wire if available
        """
        raise NotImplementedError('ABC')

    def system_reset(self) -> None:
        """Reset the device."""

    def quit(self) -> None:
        """Terminate session."""

    def write_tms(self, modesel: BitSequence) -> None:
        """Change the TAP controller state.

           :note: modesel content may be consumed, i.e. emptied
           :note: last TMS bit should be stored and clocked on next write
                  request

           :param modesel: the bit sequence of TMS bits to clock in
        """
        raise NotImplementedError('ABC')

    def write(self, out: BitSequence, use_last: bool = True):
        """Write a sequence of bits to TDI.

           :note: out content may be consumed, i.e. emptied
           :param out: the bot sequence of TDI bits to clock in
           :param use_last: whether to clock in the stored TMS bits on first
                            clock cycle
        """
        raise NotImplementedError('ABC')

    def read(self, length: int) -> BitSequence:
        """Read out a sequence of bits from TDO.

           :param length: the number of bits to clock out from the remote device
           :return: the received TDO bits (length-long)
        """
        raise NotImplementedError('ABC')

    @property
    def tdi(self) -> bool:
        """Get current TDI value."""
        raise NotImplementedError('ABC')

    @tdi.setter
    def tdi(self, value: bool):
        """Set TDI value, to be clocked out on next operation."""
        raise NotImplementedError('ABC')

    @property
    def tms(self) -> bool:
        """Get current TMS value."""
        raise NotImplementedError('ABC')

    @tms.setter
    def tms(self, value: bool):
        """Set TMS value, to be clocked out on next operation."""
        raise NotImplementedError('ABC')


class JtagEngine:
    """High-level JTAG engine controller"""

    def __init__(self, ctrl: 'JtagController'):
        self._ctrl = ctrl
        self._log = getLogger('jtag.eng')
        self._fsm = JtagStateMachine()
        self._seq = bytearray()

    @property
    def fsm(self) -> JtagStateMachine:
        """Return the state machine."""
        return self._fsm

    @property
    def controller(self) -> 'JtagController':
        """Return the JTAG controller."""
        return self._ctrl

    def reset(self) -> None:
        """Reset the attached TAP controller"""
        self._ctrl.reset()
        self._fsm.reset()

    def get_available_statenames(self):
        """Return a list of supported state name"""
        return [str(s) for s in self._fsm.states]

    def change_state(self, statename) -> None:
        """Advance the TAP controller to the defined state"""
        # find the state machine path to move to the new instruction
        path = self._fsm.find_path(statename)
        self._log.debug('path: %s',
                        ', '.join((str(s).upper() for s in path[1:])))
        # convert the path into an event sequence
        events = self._fsm.get_events(path)
        # update the remote device tap controller (write TMS consumes the seq)
        self._ctrl.write_tms(events.copy())
        # update the current state machine's state
        self._fsm.handle_events(events)

    def go_idle(self) -> None:
        """Change the current TAP controller to the IDLE state"""
        self.change_state('run_test_idle')

    def run(self) -> None:
        """Change the current TAP controller to the IDLE state"""
        self.change_state('run_test_idle')

    def capture_ir(self) -> None:
        """Capture the current instruction from the TAP controller"""
        self.change_state('capture_ir')

    def write_ir(self, instruction) -> None:
        """Change the current instruction of the TAP controller"""
        self.change_state('shift_ir')
        ilength = len(instruction)  # write consumes the instruction
        self._ctrl.write(instruction)
        self.change_state('update_ir')
        # flush IR output
        self._ctrl.tms = False
        self._ctrl.read(ilength)

    def capture_dr(self) -> None:
        """Capture the current data register from the TAP controller"""
        self.change_state('capture_dr')

    def write_dr(self, data) -> None:
        """Change the data register of the TAP controller"""
        self.change_state('shift_dr')
        self._ctrl.write(data)
        self.change_state('update_dr')

    def read_dr(self, length: int) -> BitSequence:
        """Read the data register from the TAP controller"""
        self.change_state('shift_dr')
        self._ctrl.tms = False
        data = self._ctrl.read(length)
        self.change_state('update_dr')
        return data

    def write_tms(self, out) -> None:
        """Change the TAP controller state"""
        self._ctrl.write_tms(out)

    def write(self, out, use_last=False) -> None:
        """Write a sequence of bits to TDI"""
        self._ctrl.write(out, use_last)

    def read(self, length):
        """Read out a sequence of bits from TDO"""
        return self._ctrl.read(length)

    def set_tdi(self, value: bool):
        """Force default TDI value, clocked out on each cycle."""
        self._ctrl.tdi = value

    def set_tms(self, value: bool):
        """Force default TMS value clocked out on each cycle."""
        self._ctrl.tms = value
