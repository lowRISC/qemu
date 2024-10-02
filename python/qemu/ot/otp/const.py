# Copyright (c) 2023-2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Lifecycle helpers.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from logging import getLogger
from typing import TextIO
import re

from ot.util.misc import camel_to_snake_case


class OtpConstants:
    """OTP constant manager.
    """

    def __init__(self):
        self._log = getLogger('otp.const')
        self._consts: dict[str, list[str]] = {}
        self._enums: dict[str, dict[str, int]] = {}

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
                consts.append(cmo.group(2).lower())
            # RTL order in array is reversed
            consts.reverse()

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
