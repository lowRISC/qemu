# Copyright (c) 2023-2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""File utilities.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""


def guess_test_type(file_path: str) -> str:
    """Guess a test file type from its contents.

       :return: identified content
    """
    with open(file_path, 'rb') as bfp:
        header = bfp.read(4)
    if header == b'\x7fELF':
        return 'elf'
    if header == b'OTPT':
        return 'spiflash'
    return 'bin'
