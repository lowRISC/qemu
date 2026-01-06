# Copyright (c) 2023-2025 Rivos, Inc.
# Copyright (c) 2025 lowRISC contributors.
# SPDX-License-Identifier: Apache2

"""QEMU executer wrapper for OpentTitan unit test sequencer.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

import re

from ..wrapper import Wrapper


class QEMUWrapper(Wrapper):
    """Test execution wrapper using a QEMU virtual machine
    """

    NAME = 'QEMU'

    USELESS_ERR_CRE = re.compile(r'^\*{1,4}$')
    """Useless error stuff from QEMU."""

    def _discard_log(self, err: bool, line: str):
        return self.USELESS_ERR_CRE.match(line)

    def _decrease_log(self, err: bool, line: str):
        return line.find('QEMU waiting for connection') >= 0
