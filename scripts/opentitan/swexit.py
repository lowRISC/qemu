#!/usr/bin/env python3

# Copyright (c) 2023-2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""OpenTitan exit code generator for QEMU.

   Generate a small RV32I instruction bytestream to instruct the Ibex core to
   trigger a VM shutdown, using a dedicated HW register.

   This generator avoids adding a dependency on a RISC-V toolchain for the sake
   of smoke-testing an Ibex/OpenTitan VM.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from argparse import ArgumentParser, FileType
from struct import pack as spack
from traceback import format_exc
from typing import Union
import sys


SOC_CONSTANTS = {
    'earlgrey': (0x411f0000, 0x80),
    'darjeeling': (0x211f0000, 0x440),
    'ibexdemo': (0x20000, None)
}
"""Supported SoCs, with controller base address and register offset."""

TEST_STATUS_PASSED = 0x900d  # 'good'
"""Special code to signal a successful completion to DV SIM register."""

OPCODE = {
    'addi': 0b000_00000_0010011,
    'lui': 0b0110111,
    'srli': 0b101_00000_0010011,
    'ssli': 0b001_00000_0010011,
    'sw':  0b010_00000_0100011,
    'wfi': 0x10500073,
}
"""RV32I binary opcodes used in code generator."""


def to_int(value: Union[int, str]) -> int:
    """Argparser helper to parse decimal or hexadecimal integer strings."""
    if isinstance(value, int):
        return value
    return int(value.strip(), value.startswith('0x') and 16 or 10)


def rd_u(reg: int) -> int:
    """Destination register for U-type instruction."""
    return reg << 7


def imm_u(imm: int) -> int:
    """Immediate value for U-type instruction."""
    return imm << 12


def mask_u(imm: int) -> int:
    """Mask immediate value for U-type instruction."""
    return imm & ~((1 << 12) - 1)


def imm_i(imm: int) -> int:
    """Immediate value for I-type instruction."""
    return imm << 20


def rs1_i(reg: int) -> int:
    """Source register for I-type instruction."""
    return reg << 15


def rd_i(reg: int) -> int:
    """Destination register for I-type instruction."""
    return reg << 7


def shamt_i(imm: int) -> int:
    """Shift ammount for I-type instruction."""
    return imm << 20


def rs1_s(reg: int) -> int:
    """First source register for S-type instruction."""
    return reg << 15


def rs2_s(reg: int) -> int:
    """Second source register for S-type instruction."""
    return reg << 20


def off_s(imm: int) -> int:
    """Offset value for S-type instruction."""
    off_hi = (imm >> 5) << 25
    off_lo = (imm & 0b11111) << 7
    return off_hi | off_lo


def opentitan_code(addr: int, offset: int) -> bytes:
    """Exit code generation using DV SIM status register.

       :param addr: Ibex wrapper controller base address
       :param offset: Offset of the DV SIM status register.
       :return: RV32I instruction stream
    """
    assert 0 <= offset < (1 << 11)  # 12 bit, signed int
    instructions = [
        # lui  a0,addr       # ibex_core_wrapper base
        mask_u(addr) | rd_u(10) | OPCODE['lui'],
        # lui  a1,status     # "test" status
        imm_u(TEST_STATUS_PASSED) | rd_u(11) | OPCODE['lui'],
        # srli a1,a1,12      # shift down status
        shamt_i(12) | rs1_i(11) | rd_i(11) | OPCODE['srli'],
        # sw   a1,offset(a0) # write to DV SIM offset
        off_s(offset) | rs2_s(11) | rs1_s(10) | OPCODE['sw'],
        # wfi                # stop here
        OPCODE['wfi'],
        # illegal instruction
        0x0
    ]
    return spack(f'<{len(instructions)}I', *instructions)


def ibexdemo_code(addr: int) -> bytes:
    """Exit code generation for Ibex demo machine.

       :param addr: Controller base address
       :return: RV32I instruction stream
    """
    instructions = [
        # lui  a0,addr       # simulator_ctrl base
        mask_u(addr) | rd_u(10) | OPCODE['lui'],
        # li   a1,1          # set bit 0 to exit
        imm_i(1) | rs1_i(0) | rd_i(11) | OPCODE['addi'],
        # sw   a1,offset(a0) # write to DV SIM offset
        off_s(0x8) | rs2_s(11) | rs1_s(10) | OPCODE['sw'],
        # wfi                # stop here
        OPCODE['wfi'],
        # illegal instruction
        0x0
    ]
    return spack(f'<{len(instructions)}I', *instructions)


def main():
    """Entry point."""
    debug = False
    try:
        desc = sys.modules[__name__].__doc__.split('.', 1)[0].strip()
        argparser = ArgumentParser(description=f'{desc}')
        argparser.add_argument('-a', '--address', type=to_int,
                               help='Base address for swexit device '
                                    '(default: depends on SoC)')
        argparser.add_argument('-b', '--base', type=to_int, default=0x80,
                               help='Offset for the first instruction '
                                    '(default: 0x80)')
        argparser.add_argument('-t', '--soc', choices=list(SOC_CONSTANTS),
                               help='SoC type', required=True)
        argparser.add_argument('-o', '--output', type=FileType('wb'),
                               help='output file, default to stdout')
        argparser.add_argument('-d', '--debug', action='store_true',
                               help='enable debug mode')
        args = argparser.parse_args()
        debug = args.debug

        addr = args.address if args.address else SOC_CONSTANTS[args.soc][0]
        if args.soc == 'ibexdemo':
            bincode = ibexdemo_code(addr)
        else:
            offset = SOC_CONSTANTS.get(args.soc)[1]
            if offset is None:
                argparser.error('Unsupported SoC type')
            bincode = opentitan_code(addr, offset)
        out = args.output or sys.stdout.buffer
        padding = bytes(args.base)
        out.write(padding)
        out.write(bincode)

    # pylint: disable=broad-except
    except Exception as exc:
        print(f'\nError: {exc}', file=sys.stderr)
        if debug:
            print(format_exc(chain=False), file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(2)


if __name__ == '__main__':
    main()
