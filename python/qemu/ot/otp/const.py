# Copyright (c) 2023-2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Lifecycle helpers.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from binascii import hexlify, unhexlify
from logging import getLogger
from typing import Optional, TextIO
import re

from ot.util.misc import camel_to_snake_case


class OtpConstants:
    """OTP constant manager.
    """

    def __init__(self):
        self._log = getLogger('otp.const')
        self._consts: dict[str, list[str]] = {}
        self._enums: dict[str, dict[str, int]] = {}
        self._defaults: list[Optional[bytes]] = []

    def load(self, svp: TextIO):
        """Decode OTP information.

           :param svp: System Verilog stream with OTP definitions.
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
        #  +parameter +logic +\[\d+:0\] +PartInvDefault += +\d+'\(\{(.*)\}\);
        inv_default = []
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
        self._defaults.clear()
        for pno, part_chunks in enumerate(inv_default):
            last_part = pno == len(inv_default) - 1
            # last partition does not have digest,
            # all digests are 8-byte long
            if not last_part and len(part_chunks[-1]) == 8:
                check_chunks = part_chunks[:-1]
            else:
                check_chunks = part_chunks
            if not any(any(c) for c in check_chunks):
                self._defaults.append(None)
                continue
            defaults = b''.join(bytes(reversed(c)) for c in part_chunks)
            self._defaults.append(defaults)

    def get_enums(self) -> list[str]:
        """Return a list of parsed enumerations."""
        return list(self._enums.keys())

    def get_digest_pair(self, name: str, prefix: str) -> dict[str, str]:
        """Return a dict of digest pair.
           :param name: one of the enumerated values, see #get_enums
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

    def get_partition_inv_defaults(self, partition: int) -> Optional[list[str]]:
        """Return the invalid default values for a partition, if any.
           Partition with only digest defaults are considered without default
           values.

           :param partition: the partition index
           :return: either None or the default hex-encoded bytes, including the
                    digest
        """
        try:
            defaults = self._defaults[partition]
            if not defaults:
                return defaults
            return hexlify(defaults).decode()
        except IndexError as exc:
            raise ValueError(f'No such partition:{partition}') from exc
