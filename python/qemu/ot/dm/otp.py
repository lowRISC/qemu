# Copyright (c) 2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""One-Time Programmable controller.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from binascii import unhexlify
from enum import IntEnum
from logging import getLogger
from time import sleep, time as now
from typing import Optional

from .dm import DebugModule
from ..bitfield import BitField
from ..otp import OtpMap, OtpPartition
from ..util.misc import HexInt

# pylint: disable=missing-function-docstring


class OTPException(Exception):
    """Base exception."""


class OTPBusyError(OTPException):
    """OTP controller is busy."""


class OTPError(OTPException):
    """OTP controller error."""

    def __init__(self, err_code: 'OTPController.ERR_CODE'):
        super().__init__(f'{err_code!r}')
        self.err_code = err_code


class OTPController:
    """
       Only support Darjeeling variant of the controller
       Only support the Direct Access Interface for now
    """

    TOKENS = [
        'invalid',
        'zero',
        'raw_unlock',
        'test_unlock',
        'test_exit',
        'rma',
    ]

    REGISTERS = {
        'status': 0x10,
        'err_code': 0x14,
        'regwen': 0x74,
        'cmd': 0x78,
        'address': 0x7c,
        'wdata_0': 0x80,
        'wdata_1': 0x84,
        'rdata_0': 0x88,
        'rdata_1': 0x8c,
    }
    """Darjeeling registers"""

    ERR_CODE = IntEnum('err_code',
                       ['none', 'macro', 'macro_ecc_corr', 'macro_ecc_uncorr',
                        'macro_write_blank', 'access', 'check_fail',
                        'fsm_state'], start=0)

    ERROR_COUNT = 24

    BITFIELDS = {
        'STATUS': BitField({
            'vendor_test_error': (0, 1),
            'creator_sw_cfg_error': (1, 1),
            'owner_sw_cfg_error': (2, 1),
            'ownership_slot_state_error': (3, 1),
            'rot_creator_auth_error': (4, 1),
            'rot_owner_auth_slot0_error': (5, 1),
            'rot_owner_auth_slot1_error': (6, 1),
            'plat_integ_auth_slot0_error': (7, 1),
            'plat_integ_auth_slot1_error': (8, 1),
            'plat_owner_auth_slot0_error': (9, 1),
            'plat_owner_auth_slot1_error': (10, 1),
            'plat_owner_auth_slot2_error': (11, 1),
            'plat_owner_auth_slot3_error': (12, 1),
            'ext_nvm_error': (13, 1),
            'rom_patch_error': (14, 1),
            'hw_cfg0_error': (15, 1),
            'hw_cfg1_error': (16, 1),
            'secret0_error': (17, 1),
            'secret1_error': (18, 1),
            'secret2_error': (19, 1),
            'secret3_error': (20, 1),
            'life_cycle_error': (21, 1),
            'dai_error': (22, 1),
            'lci_error': (23, 1),
            'timeout_error': (24, 1),
            'lfsr_fsm_error': (25, 1),
            'scrambling_fsm_error': (26, 1),
            'key_deriv_fsm_error': (27, 1),
            'bus_integ_error': (28, 1),
            'dai_idle': (29, 1),
            'check_pending': (30, 1),
        }),
        'REGWEN': BitField({
            'en': (0, 1),
        }),
        'CMD': BitField({
            'rd': (0, 1),
            'wr': (1, 1),
            'digest': (2, 1)
        }),
        'ADDRESS': BitField({
            'address': (0, 14),
        }),
        'ERR_CODE': BitField({
            'err_code': (0, 3, ERR_CODE),
        })
    }

    def __init__(self, dbgmod: DebugModule, base: int):
        self._log = getLogger('dtm.otp')
        self._dm = dbgmod
        self._base = base
        self._max_addr = self.BITFIELDS['ADDRESS'].encode(address=-1)
        self._map: Optional[OtpMap] = None
        self._partitions: dict[str, OtpPartition] = {}
        self._item_offsets: dict[str,  # partition name
                                 dict[str,  # item name
                                      tuple[int,  # offset
                                            int]]] = {}  # size

    def set_map(self, otpmap: 'OtpMap'):
        self._map = otpmap
        self._partitions = {p.name: p for p in self._map.enumerate_partitions()}
        self._fill_item_offsets()

    def get_hw_partition_digest(self, partname: str) -> int:
        if not self._map:
            raise RuntimeError('Partition map not loaded')
        try:
            part = self._partitions[partname.upper()]
        except KeyError as exc:
            raise ValueError(f"No such partition '{partname}'") from exc
        if not part.hw_digest:
            raise ValueError(f"No HW digest in partition '{partname}'")
        self._dai_prepare(part.digest_offset)
        self._dai_execute_command('read')
        digest = self._dai_read64()
        self._log.info('%s HW digest %016x', partname, digest)
        return digest

    def get_hw_partition_digests(self) -> dict[str, int]:
        digests: dict[str, int] = {}
        for name, part in self._partitions.items():
            if not part.hw_digest:
                continue
            digests[name] = self.get_hw_partition_digest(name)
        return digests

    @classmethod
    def is_wide_granule(cls, partition: OtpPartition, offset: int) -> bool:
        return partition.secret or (partition.digest_offset == offset & ~0b111)

    def read_partition_item(self, partname: str, itemname: str) \
            -> [int | bytes]:
        pname = partname.upper()
        try:
            part = self._partitions[pname]
            items = self._item_offsets[pname]
        except KeyError as exc:
            raise ValueError(f"No such partition '{partname}'") from exc
        try:
            ioffset, size = items[itemname.upper()]
        except KeyError as exc:
            raise ValueError(f"No such item '{itemname}' in partition "
                             f"'{partname}'") from exc
        if size == 4:
            self._dai_execute_read(part.offset + ioffset)
            return self._dai_read32()
        wide_granule = self.is_wide_granule(part, ioffset)
        if wide_granule and size == 8:
            self._dai_execute_read(part.offset + ioffset)
            return self._dai_read64()
        buffer = bytearray()
        pos = 0
        while size > 0:
            self._dai_execute_read(part.offset + ioffset + pos)
            if wide_granule and size >= 8:
                val = self._dai_read64()
                buffer.extend(val.to_bytes(8, 'little'))
                pos += 8
                size -= 8
                continue
            if size >= 4:
                val = self._dai_read32()
                buffer.extend(val.to_bytes(4, 'little'))
                pos += 4
                size -= 4
                continue
            raise RuntimeError('Invalid item size')
        return bytes(buffer)

    def write_partition_item(self, partname: str, itemname: str,
                             value: [int | bytes | bytearray | str]) -> None:
        pname = partname.upper()
        try:
            part = self._partitions[pname]
            items = self._item_offsets[pname]
        except KeyError as exc:
            raise ValueError(f"No such partition '{partname}'") from exc
        try:
            ioffset, size = items[itemname.upper()]
        except KeyError as exc:
            raise ValueError(f"No such item '{itemname}' in partition "
                             f"'{partname}'") from exc
        if isinstance(value, int):
            if size not in (4, 8):
                raise ValueError(f'{itemname} expects a {size}-byte long value')
        elif size in (4, 8):
            value = HexInt.parse(value)
        else:
            if isinstance(value, str):
                try:
                    value = unhexlify(value)
                except ValueError as exc:
                    raise ValueError(f'Invalid hexa string: {exc}') from exc
            if isinstance(value, (bytes, bytearray)):
                vlen = len(value)
                if size != vlen:
                    raise ValueError(f'{itemname} expects a {size}-byte long '
                                     f'value, value is {vlen}-byte long')
            else:
                raise TypeError(f'Invalid value type for {itemname}')
        if size == 4:
            self._dai_prepare(part.offset + ioffset)
            self._dai_write32(value)
            self._dai_execute_write()
            return
        wide_granule = self.is_wide_granule(part, ioffset)
        if wide_granule and size == 8:
            self._dai_prepare(part.offset + ioffset)
            self._dai_write64(value)
            self._dai_execute_write()
            return
        pos = 0
        while size > 0:
            self._dai_prepare(part.offset + ioffset + pos)
            if wide_granule and size >= 8:
                val = int.from_bytes(value[pos:pos+8], 'little')
                self._dai_write64(val)
                self._dai_execute_write()
                pos += 8
                size -= 8
                continue
            if size >= 4:
                val = int.from_bytes(value[pos:pos+4], 'little')
                self._dai_write32(val)
                self._dai_execute_write()
                pos += 4
                size -= 4
                continue
            raise RuntimeError('Invalid item size')

    def _dai_execute_read(self, offset: int) -> int:
        self._dai_prepare(offset)
        self._dai_execute_command('read')
        self._dai_wait_completion()

    def _dai_execute_write(self) -> int:
        self._dai_execute_command('write')
        self._dai_wait_completion()

    def _dai_prepare(self, offset: int) -> int:
        self._expect_dai_idle()
        assert not self._is_dai_busy(), "DAI busy"
        self._dai_set_address(offset)

    def _dai_wait_completion(self):
        self._expect_dai_idle()
        assert not self._is_dai_busy(), "DAI busy"
        dai_error = self._get_dai_error()
        if dai_error != self.ERR_CODE.none:
            raise OTPError(dai_error)
        if not self._is_dai_regwen():
            raise OTPBusyError()

    def _dai_execute_command(self, command: str) -> None:
        if not self._is_dai_regwen():
            raise OTPBusyError()
        try:
            cmd = {'read': 'rd', 'write': 'wr', 'digest': 'digest'}[command]
        except KeyError as exc:
            raise ValueError(f'Unsupported command {command}') from exc
        cmdargs = {cmd: True}
        val = self.BITFIELDS['CMD'].encode(**cmdargs)
        self._dm.write32(self._base + self.REGISTERS['cmd'], val)

    def _dai_set_address(self, address: int) -> None:
        if address > self._max_addr:
            raise ValueError(f'Invalid OTP address {address}')
        if not self._is_dai_regwen():
            raise OTPBusyError()
        self._dm.write32(self._base + self.REGISTERS['address'], address)

    def _dai_read32(self) -> int:
        return self._dm.read32(self._base + self.REGISTERS['rdata_0'])

    def _dai_read64(self) -> int:
        return self._dm.read64(self._base + self.REGISTERS['rdata_0'])

    def _dai_write32(self, val: int) -> None:
        self._dm.write32(self._base + self.REGISTERS['wdata_0'], val)

    def _dai_write64(self, val: int) -> None:
        self._dm.write64(self._base + self.REGISTERS['wdata_0'], val)

    def _is_dai_regwen(self) -> bool:
        val = self._dm.read32(self._base + self.REGISTERS['regwen'])
        return self.BITFIELDS['REGWEN'].decode(val)['en']

    def _is_dai_busy(self) -> bool:
        val = self._dm.read32(self._base + self.REGISTERS['status'])
        return not self.BITFIELDS['STATUS'].decode(val)['dai_idle']

    def _get_dai_error(self) -> 'OTPController.ERR_CODE':
        return self._get_error_code(self.ERROR_COUNT-2)

    def _get_error_code(self, slot: int) -> 'OTPController.ERR_CODE':
        if slot >= self.ERROR_COUNT:
            raise ValueError(f'Invalid slot {slot}')
        val = self._dm.read32(self._base + self.REGISTERS['err_code'] +
                              4 * slot)
        return self.BITFIELDS['ERR_CODE'].decode(val)['err_code']

    def _expect_dai_idle(self, timeout: float = 0.5):
        timeout += now()
        while now() < timeout:
            if self._is_dai_regwen():
                return
            sleep(0.05)
        raise TimeoutError('DAI stalled')

    def _fill_item_offsets(self) -> None:
        for pname, part in self._partitions.items():
            items = self._item_offsets[pname] = {}
            offset = 0
            for iname, item in part.items.items():
                size = item['size']
                items[iname] = offset, size
                offset += size
