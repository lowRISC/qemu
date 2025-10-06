# Copyright (c) 2023-2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""File utilities.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from io import BufferedReader
from os import stat
from os.path import relpath
from time import localtime, strftime
from typing import BinaryIO, TextIO, Union

import re

from .elf import ElfBlob
from .recfmt import RecordSegment, VMemBuilder


def guess_file_type(file: Union[str, BufferedReader]) -> str:
    """Guess a file type from its contents.

       Only supports useful type for QEMU/OT needs.

       :return: identified content
    """
    if isinstance(file, str):
        with open(file, 'rb') as bfp:
            header = bfp.read(1024)
    elif isinstance(file, BufferedReader):
        header = file.peek(1024)
    else:
        raise TypeError('file must be a string or a binary file object')
    if header[:4] == b'\x7fELF':
        return 'elf'
    if header[:4] == b'OTPT':
        return 'spiflash'
    vmem_re = rb'(?i)^@[0-9A-F]{4,}\s([0-9A-F]{6,})'
    for line in header.split(b'\n'):
        if line.startswith(b'/*') or line.startswith(b'#'):
            continue
        vmo = re.match(vmem_re, line)
        if vmo:
            bcount = len(vmo.group(1)) // 2
            # heuristic for VMEM files:
            # - plain VMEM contain 16 or 32 data bit per block
            # - special VMEM (ECC, scrambled, ...) usually contain an extra byte
            # we only need to distinguish this kinds of files for now, it is not
            # intended to be used for any purpose outside QEMU & Verilator
            # tooling.
            reg = bcount & 1 == 0
            return 'vmem' if reg else 'svmem'
    hex_re = rb'(?i)^[0-9A-F]{6,}'
    count = 0
    for line in header.split(b'\n'):
        if re.match(hex_re, line) and (len(line) & 1) == 0:
            count += 1
        else:
            count = 0
            break
        if count > 4:
            return 'hex'
    return 'bin'


def make_vmem_from_elf(elf_file: Union[str, BinaryIO],
                       vmem_file: Union[str, TextIO],
                       offset: int = 0,
                       chunksize: int = 4,
                       offsetsize: int = 0,
                       close_elf: bool = True) -> None:
    """Create a VMEM file from an ELF file.

       :param elf_file:   ELF to convert, as either a file path or a binary
                          file object
       :param vmem_file:  ELF to convert, either a file path or a text file
                          object
       :param offset:     optional offset in bytes within the VMEM file to store
                          the ELF blob
       :param chunksize:  how many bytes per VMEM chunk to generate
       :param offsetsize: how many chars to use to emit the chunk offset
       :param close_elf:  whether to close the ELF input file object
    """
    elf = ElfBlob()
    if isinstance(elf_file, str):
        with open(elf_file, 'rb') as efp:
            elfstat = stat(efp.fileno())
            elf.load(efp)
        elfname = relpath(elf_file)
    else:
        elf.load(elf_file)
        elfname = relpath(elf_file.name)
        elfstat = stat(elf_file.fileno())
        if close_elf:
            elf_file.close()

    seg = RecordSegment()
    seg.write(elf.blob, offset=offset)

    vmem = VMemBuilder(byteorder='little', chunksize=chunksize,
                       offsetsize=offsetsize)
    vmem.build([seg])

    elftime = localtime(elfstat.st_mtime)
    is_path = isinstance(vmem_file, str)
    with open(vmem_file, 'wt') if is_path else vmem_file as vfp:
        print(f'// name:       {elfname}', file=vfp)
        print(f'// built:      {strftime("%Y/%m/%d %H:%M:%S", elftime)}', file=vfp)
        print(f'// size:       {elfstat.st_size}', file=vfp)
        print(f'// rawsize:    {elf.size}', file=vfp)
        print(f'// entrypoint: 0x{elf.entry_point:08x}', file=vfp)
        print(file=vfp)
        vfp.write(vmem.getvalue())
