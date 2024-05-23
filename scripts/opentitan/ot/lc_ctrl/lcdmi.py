# Copyright (c) 2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""LifeCycle DMI.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from binascii import Error as BinasciiError, hexlify, unhexlify
from logging import getLogger
from struct import pack as spack, unpack as sunpack
from typing import NamedTuple

from . import LifeCycleError
from ..dtm import DebugTransportModule
from ..util.mbb import MB8_TRUE, MB8_FALSE, MB8_MASK


class HardwareRevision(NamedTuple):
    """OpenTitan Hardware Revision."""
    silicon_creator: int
    product_id: int
    revision_id: int


class LifeCycleController:
    """LifeCycle Controller over DMI."""

    STATUS = """
        initialized
        ready
        ext_clock_switched
        transition_successful
        transition_count_error
        transition_error
        token_error
        flash_rma_error
        otp_error
        state_error
        bus_integ_error
        otp_partition_error
    """.split()
    """Status bits."""

    STATES = """
        RAW
        TEST_UNLOCKED0
        TEST_LOCKED0
        TEST_UNLOCKED1
        TEST_LOCKED1
        TEST_UNLOCKED2
        TEST_LOCKED2
        TEST_UNLOCKED3
        TEST_LOCKED3
        TEST_UNLOCKED4
        TEST_LOCKED4
        TEST_UNLOCKED5
        TEST_LOCKED5
        TEST_UNLOCKED6
        TEST_LOCKED6
        TEST_UNLOCKED7
        DEV
        PROD
        PROD_END
        RMA
        SCRAP
    """.split()

    REGS = {
        'alert_test': 0, 'status': 1, 'claim_transition_if_regwen': 2,
        'claim_transition_if': 3, 'transition_regwen': 4, 'transition_cmd': 5,
        'transition_ctrl': 6, 'transition_token': 7, 'transition_target': 11,
        'otp_vendor_test_ctrl': 12, 'otp_vendor_test_status': 13,
        'lc_state': 14, 'lc_transition_cnt': 15, 'lc_id_state': 16,
        'hw_revision': 17, 'device_id': 19, 'manuf_state': 27}
    """Registers."""

    ALERTS = 'prog_error state_error bus_integ_error'.split()
    """Alerts."""

    TOKEN_FORMAT = '<4I'

    def __init__(self, dtm: DebugTransportModule, address: int):
        self._log = getLogger('dtm.lcctrl')
        self._dtm = dtm
        self._dmi = dtm['dmi']
        self._address = address
        self._hw_rev_widths = (16, 16, 8)

    def restart_system(self) -> None:
        """Restart the remote machine."""
        self._dtm.engine.controller.system_reset()

    def set_alert_test(self, alert: str) -> None:
        """Test alert."""
        try:
            alix = self.ALERTS.index(alert.lower())
        except IndexError as exc:
            raise ValueError(f'No such alert {alert}') from exc
        self._write_reg('alert_test', 1 << alix)

    @property
    def status(self) -> dict[str, bool]:
        """Retrieve the current status."""
        value = self._read_reg('status')
        status = {self.STATUS[b]: bool(value & (1 << b))
                  for b in range(len(self.STATUS))}
        return status

    def disable_claim_transition_if_regwen(self):
        """Disable claim transition interface."""
        self._write_reg('claim_transition_if_regwen', 0)

    def claim_transition_if(self, claim: bool) -> bool:
        """Clain transition interface.

           :return: True of the interface has been successfully claimed for DMI.
        """
        reg = 'claim_transition_if'
        self._log.debug('%s interface', 'Claim' if claim else 'Release')
        if claim:
            self._write_reg(reg, MB8_TRUE)
            val = self._read_reg(reg) & MB8_MASK
            success = val == MB8_TRUE
            return success
        self._write_reg(reg, MB8_FALSE)
        val = self._read_reg(reg) & MB8_MASK
        if val == MB8_TRUE:
            raise LifeCycleError('Cannot release LC interface')
        return val == MB8_FALSE

    @property
    def transition_regwen(self) -> bool:
        """Tells whether transition registers can be written."""
        regwen = bool(self._read_reg('transition_regwen') & 0x1)
        self._log.debug('Transition regwen: %s', regwen)
        return regwen

    def transition_start(self) -> str:
        """Start the transition operation."""
        self._log.debug('Start transition')
        self._write_reg('transition_cmd', 0b1)

    @property
    def volatile_raw_unlock(self) -> bool:
        """Report whether volatile unlock is enabled."""
        reg = 'volatile_raw_unlock'
        vru = bool(self._read_reg(reg) & 0b1)
        return vru

    @volatile_raw_unlock.setter
    def volatile_raw_unlock(self, enable: bool) -> None:
        """Enable or disable volatile unlock."""
        reg = 'transition_ctrl'
        value = self._read_reg(reg)
        if enable:
            value |= 0b10
        else:
            value &= ~0b10
        self._write_reg(reg, value)

    @property
    def ext_clock_en(self) -> bool:
        """Report whether external clock is selected."""
        reg = 'transition_ctrl'
        value = self._read_reg(reg)
        value |= 0b1
        self._write_reg(reg, value)

    @ext_clock_en.setter
    def ext_clock_en(self) -> None:
        """Select external clock source."""

    @property
    def transition_token(self) -> bytes:
        """Report current transition token."""
        tokens = (self._read_reg('transition_token', pos) for pos in range(4))
        token = spack(self.TOKEN_FORMAT, *tokens)
        self._log.debug('Transition token: %s', hexlify(token).decode())
        return token

    @transition_token.setter
    def transition_token(self, token: [bytes | bytearray | str | None]) -> None:
        """Define the transition token as a 16-byte token.

           :param token: if None, use an empty zeroed token,
                         if provided as a string, it should be an hexadecimal
                         value of 32 characters, otherwise it should be a
                         16-byte sequence.
        """
        if isinstance(token, str):
            try:
                token = unhexlify(token)
            except BinasciiError as exc:
                raise ValueError('Invalid token string: {exc}') from exc
        elif token is None:
            token = bytes(16)
        elif not isinstance(token, (bytes, bytearray)):
            raise ValueError('Invalid token type')
        if len(token) != 16:
            raise ValueError('Invalid token length')
        token = bytes(reversed(token))
        for tix, tok in enumerate(sunpack('<4I', token)):
            self._log.debug('Write token[%d] 0x%08x', tix, tok)
            self._write_reg('transition_token', tok, tix)

    @property
    def transition_target(self) -> tuple[str, int]:
        """Read back the transition target."""
        target = self._read_reg('transition_target')
        starget = self._decode_state(target)
        self._log.debug('Transition target: %s', starget)
        return starget, target

    @transition_target.setter
    def transition_target(self, target: [str | int]) -> None:
        """Define the transition token as a 16-byte token."""
        if isinstance(target, str):
            itarget = self._encode_state(target)
        elif isinstance(target, int):
            if target < (1 << 5):
                itarget = self._expand_state(target)
            else:
                itarget = target
            self._check_state(itarget)
        else:
            raise ValueError('Invalid target type')
        self._log.debug('Set transition target: 0x%08x', itarget)
        self._write_reg('transition_target', itarget)

    def otp_vendor_test_ctrl(self) -> int:
        """Provide the OTP vendor test control as a 32-bit value."""
        return self._read_reg('otp_vendor_test_ctrl')

    def otp_vendor_test_status(self) -> int:
        """Provide the OTP vendor test status as a 32-bit value."""
        return self._read_reg('otp_vendor_test_status')

    @property
    def lc_state(self) -> [str | int]:
        """Report the current state."""
        istate = self._read_reg('lc_state')
        tix = self._check_state(istate)
        return self.STATES[tix], istate

    @property
    def lc_transition_count(self) -> int:
        """Report the current transition count."""
        return self._read_reg('lc_transition_cnt') & 0x1f

    @property
    def lc_id_state(self) -> str:
        """Report the current ID state."""
        value = self._read_reg('lc_id_state')
        idmap = {0: 'blank', 0x55555555: 'personalized', 0xaaaaaaaa: 'invalid'}
        try:
            return idmap[value]
        except KeyError as exc:
            raise LifeCycleError(f'Unknown ID state 0x{value:08x}') from exc

    @property
    def hw_revision(self) -> HardwareRevision:
        """Report the HW revision."""
        hw0 = self._read_reg('hw_revision', 0)
        hw1 = self._read_reg('hw_revision', 1)
        widths = self._hw_rev_widths
        creator = hw0 >> widths[1] & ((1 << widths[0]) - 1)
        product = hw0 & ((1 << widths[1]) - 1)
        revision = hw1 & ((1 << widths[2]) - 1)
        return HardwareRevision(creator, product, revision)

    @property
    def device_identifier(self) -> bytes:
        """Report the device identifier as a 32-byte value."""
        devids = (self._read_reg('device_id', pos) for pos in range(8))
        devid = spack(self.TOKEN_FORMAT, *devids)
        self._log.debug('Transition token: %s', hexlify(devid).decode())
        return devid

    @property
    def manufacturer_state(self) -> bytes:
        """Report the manufacturer state as a 32-byte value."""
        devids = (self._read_reg('manuf_state', pos) for pos in range(8))
        devid = spack(self.TOKEN_FORMAT, *devids)
        self._log.debug('Transition token: %s', hexlify(devid).decode())
        return devid

    @classmethod
    def _decode_state(cls, state: int) -> str:
        tgix = cls._check_state(state)
        return cls.STATES[tgix]

    @classmethod
    def _encode_state(cls, state: str) -> int:
        try:
            tgix = cls.STATES.index(state.upper())
        except IndexError as exc:
            raise ValueError(f"Unknwon state '{state}'") from exc
        return cls._expand_state(tgix)

    @classmethod
    def _check_state(cls, state: int) -> int:
        base = cls._base_state(state)
        if cls._expand_state(state) != state:
            raise ValueError(f'Unknown state {state:08x}')
        if not 0 <= base < len(cls.STATES):
            raise ValueError(f'Unknown state {state:08x}')
        return base

    @classmethod
    def _base_state(cls, state: int) -> int:
        return state & ((1 << 5) - 1)

    @classmethod
    def _expand_state(cls, state: int) -> int:
        base = cls._base_state(state)
        return ((base << 25) | (base << 20) | (base << 15) | (base << 10) |
                (base << 5) | base)

    def _write_reg(self, name: str, value: int, offset: int = 0) -> None:
        if value >= (1 << 32):
            raise ValueError('Invalid value')
        try:
            reg = self.REGS[name] + offset
        except KeyError as exc:
            raise ValueError(f"No such LC register: '{name}'") from exc
        self._log.info('write %s: 0x%08x', name, value)
        return self._dmi.write(self._address + reg, value)

    def _read_reg(self, name: str, offset: int = 0) -> int:
        try:
            reg = self.REGS[name] + offset
        except KeyError as exc:
            raise ValueError(f"No such LC register: '{name}'") from exc
        self._log.info('read %s', name)
        value = self._dmi.read(self._address + reg)
        self._log.info('read 0x%08x', value)
        return value
