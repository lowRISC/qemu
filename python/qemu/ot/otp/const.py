# Copyright (c) 2023-2025 Rivos, Inc.
# Copyright (c) 2025 lowRISC contributors.
# SPDX-License-Identifier: Apache2

"""Lifecycle helpers.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from binascii import hexlify, unhexlify
from logging import getLogger
from typing import Any, Optional, TextIO
import re

from ot.util.misc import camel_to_snake_case


class OtpConstants:
    """OTP constant manager.
    """

    def __init__(self):
        self._log = getLogger('otp.const')
        self._consts: dict[str, list[str]] = {}
        self._enums: dict[str, dict[str, int]] = {}
        self._inv_defaults: list[Optional[str]] = []

    def load_sv(self, svp: TextIO) -> bool:
        """Decode OTP information from a System Verilog file.

           :param svp: System Verilog stream with OTP definitions.
           :return: True if some OTP information has been detected
        """
        svdata = svp.read()
        for smo in re.finditer(r"\stypedef\s+enum\s+logic\s+\[[^]]+\]\s"
                               r"{((?:\s+\w+,?)+)\s*}\s(\w+)_sel_e;", svdata):
            values, name = smo.groups()
            if name in self._consts:
                raise ValueError(f'Multiple definitions of enumeration {name}')
            enums = self._enums[name] = {}
            for emo in re.finditer(r"\s+(\w+),?", values):
                vname = camel_to_snake_case(emo.group(1))
                enums[vname] = len(enums)

        for amo in re.finditer(r"\s+parameter\s+(\w+)_array_t\s+(\w+)\s+=\s+"
                               r"{(\s+(?:(?:64|128)'h[0-9A-F]+,?\s+)+)};",
                               svdata):
            _type, name, values = amo.groups()
            sc_name = camel_to_snake_case(name)
            sc_parts = sc_name.split('_')
            if sc_parts[0] == 'rnd':
                sc_parts.pop(0)
            if sc_parts[0] == 'cnst':
                sc_parts.pop(0)
            name = '_'.join(sc_parts)
            if name in self._consts:
                raise ValueError(f'Multiple definitions of constant {name}')
            consts = self._consts[name] = []
            for cmo in re.finditer(r"(64|128)'h([0-9A-F]+),?", values):
                nibble_count = (int(cmo.group(1)) + 3) // 4
                hexa_str = cmo.group(2).lower()
                hexa_str_len = len(hexa_str)
                if hexa_str_len < nibble_count:
                    pad_str = '0' * (nibble_count - hexa_str_len)
                    hexa_str = f'{pad_str}{hexa_str}'
                assert len(hexa_str) == nibble_count
                consts.append(hexa_str)
            # RTL order in array is reversed
            consts.reverse()
        inv_default = []
        total_byte_count = 0
        for imo in re.finditer(r"(?s) +parameter +logic +\[\d+:0\] +"
                               r"PartInvDefault += +(\d+)'\(\{(.*)\}\);",
                               svdata):
            if inv_default:
                raise ValueError('PartInvDefault redefined')
            total_byte_count = int(imo.group(1)) // 8
            for pmo in re.finditer(r"(\d+)'\(\{([^\}]*)\}\)", imo.group(2)):
                part_byte_count = int(pmo.group(1)) // 8
                chunks = []
                for bmo in re.finditer(r"(\d+)'h([0-9A-Fa-f]+)", pmo.group(2)):
                    byte_count = int(bmo.group(1)) // 8
                    hexa_str = bmo.group(2)
                    hexa_str_len = len(hexa_str)
                    exp_len = byte_count * 2
                    if hexa_str_len < exp_len:
                        pad_str = '0' * (exp_len - hexa_str_len)
                        hexa_str = f'{pad_str}{hexa_str}'
                    hexa_bytes = unhexlify(hexa_str)
                    assert len(hexa_bytes) == byte_count
                    chunks.append(hexa_bytes)
                chunk_byte_count = sum(len(c) for c in chunks)
                if chunk_byte_count != part_byte_count:
                    raise RuntimeError('Invalid partition default bytes')
                # RTL order is last-to-first
                inv_default.append(list(reversed(chunks)))
        byte_count = sum(len(c) for part in inv_default for c in part)
        if byte_count != total_byte_count:
            raise RuntimeError('Invalid partition default bytes')
        # RTL order is last-to-first
        inv_default.reverse()
        self._inv_defaults.clear()
        for pno, part_chunks in enumerate(inv_default):
            last_part = pno == len(inv_default) - 1
            # last partition does not have digest,
            # all digests are 8-byte long
            if not last_part and len(part_chunks[-1]) == 8:
                check_chunks = part_chunks[:-1]
            else:
                check_chunks = part_chunks
            if not any(any(c) for c in check_chunks):
                self._inv_defaults.append(None)
                continue
            defaults = b''.join(bytes(reversed(c)) for c in part_chunks)
            self._inv_defaults.append(hexlify(defaults).decode())
        return any(self._inv_defaults) and any(self._consts)

    def load_secrets(self, secrets: dict[str, Any]) -> bool:
        """Load OTP secrets from a (HJSON) map.

           :param secrets: secret definitions as a map
        """
        modules = secrets.get('module', [])
        otp: Optional[dict] = None
        for mod in modules:
            if mod.get('type') == 'otp_ctrl':
                otp = mod
                break
        if not otp:
            raise ValueError('Cannot locate OTP secrets')

        try:
            otp_map = otp['otp_mmap']
            otp_size = otp_map['otp']['size']
            scrambling = otp_map['scrambling']
            parts = otp_map['partitions']
        except KeyError as exc:
            raise ValueError(f'Cannot find partition secrets: {exc}') from exc

        sizes = {kp[0]: v for k, v in scrambling.items()
                 if (kp := k.split('_', 1))[-1] == 'size'}
        self._consts['key'] = [
            hexlify(kv.to_bytes(sizes['key'], 'big')).decode()
            for kd in scrambling['keys']
            for kn, kv in kd.items() if kn == 'value'
        ]
        self._consts['digest_const'] = [
            hexlify(kv.to_bytes(sizes['cnst'], 'big')).decode()
            for kd in scrambling['digests']
            for kn, kv in kd.items() if kn == 'cnst_value'
        ]
        self._consts['digest_iv'] = [
            hexlify(kv.to_bytes(sizes['iv'], 'big')).decode()
            for kd in scrambling['digests']
            for kn, kv in kd.items() if kn == 'iv_value'
        ]

        offset = 0
        total_size = 0
        for part in parts or []:
            part_name = part['name']
            part_size = 0
            part_inv_bytes: list[bytes] = []
            part_inv_sum = 0
            for item in part['items']:
                size = item['size']
                name = item['name']
                part_size += size
                if item['offset'] != offset:
                    missing = item['offset'] - offset
                    self._log.info('Gap before %s.%s: %u bytes',
                                   part_name, name, missing)
                    assert missing > 0, 'Invalid negative offset'
                    part_inv_bytes[-1] = b''.join((part_inv_bytes[-1],
                                                   bytes(missing)))
                    part_size += missing
                offset = item['offset'] + size
                inv_default = item['inv_default']
                inv_bytes = inv_default.to_bytes(size, 'big')
                if inv_default and not item['isdigest']:
                    part_inv_sum += inv_default
                part_inv_bytes.append(inv_bytes)
            if part_size != part['size']:
                raise ValueError(f'Invalid byte count for part {part_name}: '
                                 f"{part_size} != {part['size']}")
            if part_inv_sum:
                part_inv_str = hexlify(b''.join(part_inv_bytes)).decode()
                self._inv_defaults.append(part_inv_str)
            else:
                self._inv_defaults.append(None)
            total_size += part_size
        if total_size != otp_size:
            raise ValueError(f'Invalid byte count for OTP '
                             f'{total_size} != {otp_size}')

    def get_enums(self) -> list[str]:
        """Return a list of parsed enumerations."""
        return list(self._enums.keys())

    def get_digests(self) -> list[str]:
        """Return a list of parsed digests."""
        try:
            return list(self._enums['digest'])
        except KeyError as exc:
            raise ValueError("No 'digest' enum found") from exc

    def get_scrambling_keys(self) -> list[str]:
        """Return a list of parsed scrambling keys for secret partitions."""
        try:
            return list(self._enums['key'])
        except KeyError as exc:
            raise ValueError("No 'key' enum found") from exc

    def get_digest_pair(self, name: str, prefix: str) -> dict[str, str]:
        """Return a dict of digest pair.
           :param name: one of the enumerated values, see #get_digests
           :param prefix: the prefix to add to each dict key.
        """
        try:
            idx = self._enums['digest'][name]
        except KeyError as exc:
            raise ValueError(f'Unknown digest pair {name}') from exc
        odict = {}
        for kname, values in self._consts.items():
            if kname.startswith('digest_'):
                if len(values) < idx:
                    raise ValueError(f'No such digest {name}')
                oname = f"{prefix}_{kname.split('_', 1)[-1]}"
                odict[oname] = values[idx]
        return odict

    def get_scrambling_key(self, name: str) -> str:
        """Get the value of a scrambling key.
           :param name: One of the enumerated values, see #get_scrambling_keys
        """
        try:
            idx = self._enums['key'][name]
        except KeyError as exc:
            raise ValueError(f'Unknown scrambling key {name}') from exc
        try:
            key_values = self._consts['key']
        except KeyError as exc:
            raise ValueError('No scrambling key constants found') from exc
        if len(key_values) <= idx:
            raise ValueError(f'No such key {name} in the key array')
        return key_values[idx]

    def get_partition_inv_defaults(self, partition: int) -> Optional[str]:
        """Return the invalid default values for a partition, if any.
           Partitions with only digest defaults are considered without default
           values.

           :param partition: the partition index
           :return: either None or the default hex-encoded bytes, including any
                    digest and zero-ing trailing bytes
        """
        try:
            return self._inv_defaults[partition]
        except IndexError as exc:
            raise ValueError(f'No such partition:{partition}') from exc
