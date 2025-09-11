# Copyright (c) 2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""OTP secret helpers.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from typing import Any, Optional, Union
import re

from ot.util.misc import camel_to_snake_case

OtParamRegex = str
"""Definition of a parameter to seek and how to shorten it."""


class OtpSecretConstants:
    """OTP secret constant helper.
    """

    @classmethod
    def load_values(cls, module: dict[str, Any],
                    odict: Union[dict[str, str],
                                 dict[Optional[int], dict[str, str]]],
                    multi: bool, *regexes: tuple[OtParamRegex, ...]) \
            -> None:
        """Load values from OTP secret dictionary.

           :param module: the OTP secret module source
           :param odict: output dictionary. If multi is false, a simple
                         key-value map is generated, otherwise a map of indices
                         as keys for a sub dictionary of key-values is generated
           :param multi: whether to generate a single level or index-based map.
           :param regexes: one of more regular expression to match in the
                           input module. First group define the part of the
                           matched entry to keep as key names, which are
                           converted to snake cases.
        """
        modname = module.get('name')
        if not modname:
            return
        for params in module.get('param_list', []):
            if not isinstance(params, dict):
                continue
            for regex in regexes:
                pmo = re.match(regex, params['name'])
                if not pmo:
                    continue
                value = params.get('default')
                if not value:
                    continue
                if value.startswith('0x'):
                    value = value[2:]
                if len(value) & 1:
                    value = f'0{value}'
                kname = camel_to_snake_case(pmo.group(1))
                if multi:
                    imo = re.search(r'(\d+)$', modname)
                    idx = int(imo.group(1)) if imo else None
                    if idx not in odict:
                        odict[idx] = {}
                    odict[idx][kname] = value
                else:
                    odict[kname] = value
