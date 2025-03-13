# Copyright (c) 2023-2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""File utilities.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

import re


def guess_test_type(file_path: str) -> str:
    """Guess a test file type from its contents.

       :return: identified content
    """
    with open(file_path, 'rb') as bfp:
        header = bfp.read(1024)
    if header[:4] == b'\x7fELF':
        return 'elf'
    if header[:4] == b'OTPT':
        return 'spiflash'
    vmem_re = rb'(?i)^@[0-9A-F]{4,}\s[0-9A-F]{6,}'
    for line in header.split(b'\n'):
        if line.startswith(b'/*') or line.startswith(b'#'):
            continue
        if re.match(vmem_re, line):
            return 'vmem'
    return 'bin'
