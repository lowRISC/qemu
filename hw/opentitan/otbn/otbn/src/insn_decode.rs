// Copyright 2021 Gregory Chadwick <mail@gregchadwick.co.uk>
// Copyright 2022-2023 Rivos, Inc.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

use super::insn_format;
use super::insn_proc;

fn process_opcode_op<T: insn_proc::InstructionProcessor>(
    processor: &mut T,
    insn_bits: u32,
) -> Option<T::InstructionResult> {
    let dec_insn = insn_format::RType::new(insn_bits);

    match dec_insn.funct3 {
        0b000 => match dec_insn.funct7 {
            0b000_0000 => Some(processor.process_add(dec_insn)),
            0b010_0000 => Some(processor.process_sub(dec_insn)),
            _ => None,
        },
        0b001 => match dec_insn.funct7 {
            0b000_0000 => Some(processor.process_sll(dec_insn)),
            _ => None,
        },
        0b100 => match dec_insn.funct7 {
            0b000_0000 => Some(processor.process_xor(dec_insn)),
            _ => None,
        },
        0b101 => match dec_insn.funct7 {
            0b000_0000 => Some(processor.process_srl(dec_insn)),
            0b010_0000 => Some(processor.process_sra(dec_insn)),
            _ => None,
        },
        0b110 => match dec_insn.funct7 {
            0b000_0000 => Some(processor.process_or(dec_insn)),
            _ => None,
        },
        0b111 => match dec_insn.funct7 {
            0b000_0000 => Some(processor.process_and(dec_insn)),
            _ => None,
        },
        _ => None,
    }
}

fn process_opcode_op_imm<T: insn_proc::InstructionProcessor>(
    processor: &mut T,
    insn_bits: u32,
) -> Option<T::InstructionResult> {
    let dec_insn = insn_format::IType::new(insn_bits);

    match dec_insn.funct3 {
        0b000 => Some(processor.process_addi(dec_insn)),
        0b001 => Some(processor.process_slli(insn_format::ITypeShamt::new(insn_bits))),
        0b100 => Some(processor.process_xori(dec_insn)),
        0b101 => {
            let dec_insn_shamt = insn_format::ITypeShamt::new(insn_bits);
            match dec_insn_shamt.funct7 {
                0b000_0000 => Some(processor.process_srli(dec_insn_shamt)),
                0b010_0000 => Some(processor.process_srai(dec_insn_shamt)),
                _ => None,
            }
        }
        0b110 => Some(processor.process_ori(dec_insn)),
        0b111 => Some(processor.process_andi(dec_insn)),
        _ => None,
    }
}

fn process_opcode_op32<T: insn_proc::InstructionProcessor>(
    processor: &mut T,
    insn_bits: u32,
) -> Option<T::InstructionResult> {
    let dec_insn = insn_format::RType::new(insn_bits);

    let (b30, b29) = (
        (dec_insn.funct7 >> 4 & 0b10) != 0,
        (dec_insn.funct7 >> 4 & 0b01) != 0,
    );
    match (b30, b29) {
        (false, false) => Some(processor.process_bn_mulqacc(dec_insn)),
        (false, true) => Some(processor.process_bn_mulqacc_wo(dec_insn)),
        (true, _) => Some(processor.process_bn_mulqacc_so(dec_insn)),
    }
}

fn process_opcode_branch<T: insn_proc::InstructionProcessor>(
    processor: &mut T,
    insn_bits: u32,
) -> Option<T::InstructionResult> {
    let dec_insn = insn_format::BType::new(insn_bits);

    match dec_insn.funct3 {
        0b000 => Some(processor.process_beq(dec_insn)),
        0b001 => Some(processor.process_bne(dec_insn)),
        _ => None,
    }
}

fn process_opcode_load<T: insn_proc::InstructionProcessor>(
    processor: &mut T,
    insn_bits: u32,
) -> Option<T::InstructionResult> {
    let dec_insn = insn_format::IType::new(insn_bits);

    match dec_insn.funct3 {
        0b010 => Some(processor.process_lw(dec_insn)),
        _ => None,
    }
}

fn process_opcode_custom0<T: insn_proc::InstructionProcessor>(
    processor: &mut T,
    insn_bits: u32,
) -> Option<T::InstructionResult> {
    match (insn_bits >> 12) & 0x7 {
        0b000..=0b011 => {
            let dec_insn = insn_format::RType::new(insn_bits);
            match dec_insn.funct3 {
                0b000 => Some(processor.process_bn_sel(dec_insn)),
                0b001 => Some(processor.process_bn_cmp(dec_insn)),
                0b011 => Some(processor.process_bn_cmpb(dec_insn)),
                _ => None,
            }
        }
        0b100..=0b101 => {
            let dec_insn = insn_format::WidType::new(insn_bits);
            match dec_insn.funct3 {
                0b100 => Some(processor.process_bn_lid(dec_insn)),
                0b101 => Some(processor.process_bn_sid(dec_insn)),
                _ => None,
            }
        }
        0b110 => match insn_bits >> 31 & 0b1 {
            0b0 => {
                let dec_insn = insn_format::IType::new(insn_bits);
                Some(processor.process_bn_mov(dec_insn))
            }
            0b1 => {
                let dec_insn = insn_format::SType::new(insn_bits);
                Some(processor.process_bn_movr(dec_insn))
            }
            _ => None,
        },
        0b111 => {
            let dec_insn = insn_format::IType::new(insn_bits);

            match insn_bits >> 31 {
                0b0 => Some(processor.process_bn_wsrr(dec_insn)),
                0b1 => Some(processor.process_bn_wsrw(dec_insn)),
                _ => None,
            }
        }
        _ => None,
    }
}

fn process_opcode_store<T: insn_proc::InstructionProcessor>(
    processor: &mut T,
    insn_bits: u32,
) -> Option<T::InstructionResult> {
    let dec_insn = insn_format::SType::new(insn_bits);

    match dec_insn.funct3 {
        0b010 => Some(processor.process_sw(dec_insn)),
        _ => None,
    }
}

fn process_opcode_custom1<T: insn_proc::InstructionProcessor>(
    processor: &mut T,
    insn_bits: u32,
) -> Option<T::InstructionResult> {
    match (insn_bits >> 12) & 0x7 {
        0b100 => {
            let dec_insn = insn_format::IType::new(insn_bits);
            match insn_bits >> 30 & 0b1 {
                0 => Some(processor.process_bn_addi(dec_insn)),
                1 => Some(processor.process_bn_subi(dec_insn)),
                _ => None,
            }
        }
        _ => {
            let dec_insn = insn_format::RType::new(insn_bits);
            match dec_insn.funct3 {
                0b000 => Some(processor.process_bn_add(dec_insn)),
                0b001 => Some(processor.process_bn_sub(dec_insn)),
                0b010 => Some(processor.process_bn_addc(dec_insn)),
                0b011 => Some(processor.process_bn_subb(dec_insn)),
                0b101 => match insn_bits >> 30 & 0b1 {
                    0b0 => Some(processor.process_bn_addm(dec_insn)),
                    0b1 => Some(processor.process_bn_subm(dec_insn)),
                    _ => None,
                },
                _ => None,
            }
        }
    }
}

fn process_opcode_system<T: insn_proc::InstructionProcessor>(
    processor: &mut T,
    insn_bits: u32,
) -> Option<T::InstructionResult> {
    let dec_insn = insn_format::ITypeCSR::new(insn_bits);

    match dec_insn.funct3 {
        0b000 => {
            if dec_insn.rd != 0 || dec_insn.rs1 != 0 || dec_insn.csr != 0 {
                None
            } else {
                Some(processor.process_ecall())
            }
        }
        0b001 => Some(processor.process_csrrw(dec_insn)),
        0b010 => Some(processor.process_csrrs(dec_insn)),
        _ => None,
    }
}

fn process_opcode_custom3<T: insn_proc::InstructionProcessor>(
    processor: &mut T,
    insn_bits: u32,
) -> Option<T::InstructionResult> {
    match (insn_bits >> 12) & 0x7 {
        0b000..=0b001 => {
            let dec_insn = insn_format::IType::new(insn_bits);
            match dec_insn.funct3 {
                0b000 => Some(processor.process_loop(dec_insn)),
                0b001 => Some(processor.process_loopi(dec_insn)),
                _ => None,
            }
        }
        0b010..=0b111 => {
            let dec_insn = insn_format::RType::new(insn_bits);
            match dec_insn.funct3 {
                0b010 => Some(processor.process_bn_and(dec_insn)),
                0b011 => Some(processor.process_bn_rshi(dec_insn)),
                0b100 => Some(processor.process_bn_or(dec_insn)),
                0b101 => Some(processor.process_bn_not(dec_insn)),
                0b110 => Some(processor.process_bn_xor(dec_insn)),
                0b111 => Some(processor.process_bn_rshi(dec_insn)),
                _ => None,
            }
        }
        _ => None,
    }
}

/// Decodes instruction in `insn_bits` calling the appropriate function in `processor` returning
/// the result it produces.
///
/// Returns `None` if instruction doesn't decode into a valid instruction.
pub fn decoder<T: insn_proc::InstructionProcessor>(
    processor: &mut T,
    insn_bits: u32,
) -> Option<T::InstructionResult> {
    let opcode: u32 = insn_bits & 0x7f;

    match opcode {
        // LOAD
        0b000_0011 => process_opcode_load(processor, insn_bits),
        // custom-0
        0b000_1011 => process_opcode_custom0(processor, insn_bits),
        // OP_IMM
        0b001_0011 => process_opcode_op_imm(processor, insn_bits),
        // STORE
        0b010_0011 => process_opcode_store(processor, insn_bits),
        // custom-1
        0b010_1011 => process_opcode_custom1(processor, insn_bits),
        // OP
        0b011_0011 => process_opcode_op(processor, insn_bits),
        // LUI
        0b011_0111 => Some(processor.process_lui(insn_format::UType::new(insn_bits))),
        // OP32
        // this one is suspicious, should not it be custom-2 (0b10_110_11) instead?
        0b011_1011 => process_opcode_op32(processor, insn_bits),
        // BRANCH
        0b110_0011 => process_opcode_branch(processor, insn_bits),
        // JALR
        0b110_0111 => Some(processor.process_jalr(insn_format::IType::new(insn_bits))),
        // JAL
        0b110_1111 => Some(processor.process_jal(insn_format::JType::new(insn_bits))),
        // SYSTEM
        0b111_0011 => process_opcode_system(processor, insn_bits),
        // custom-3
        0b111_1011 => process_opcode_custom3(processor, insn_bits),
        _ => None,
    }
}
