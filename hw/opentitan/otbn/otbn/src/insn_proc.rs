// Copyright 2021 Gregory Chadwick <mail@gregchadwick.co.uk>
// Copyright 2022-2023 Rivos, Inc.
// Licensed under the Apache License Version 2.0, with LLVM Exceptions, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

use bitflags::bitflags;

use super::insn_format;

/// A trait for objects which do something with OTBN instructions.
///
/// There is one function per OTBN instruction. Each function takes the appropriate struct from
/// [insn_format] giving access to the decoded fields of the instruction. All functions
/// return the [InstructionProcessor::InstructionResult] associated type.
pub trait InstructionProcessor {
    type InstructionResult;

    fn process_add(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult;
    fn process_sub(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult;
    fn process_sll(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult;
    fn process_xor(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult;
    fn process_srl(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult;
    fn process_sra(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult;
    fn process_or(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult;
    fn process_and(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult;

    fn process_addi(&mut self, dec_insn: insn_format::IType) -> Self::InstructionResult;
    fn process_slli(&mut self, dec_insn: insn_format::ITypeShamt) -> Self::InstructionResult;
    fn process_xori(&mut self, dec_insn: insn_format::IType) -> Self::InstructionResult;
    fn process_srli(&mut self, dec_insn: insn_format::ITypeShamt) -> Self::InstructionResult;
    fn process_srai(&mut self, dec_insn: insn_format::ITypeShamt) -> Self::InstructionResult;
    fn process_ori(&mut self, dec_insn: insn_format::IType) -> Self::InstructionResult;
    fn process_andi(&mut self, dec_insn: insn_format::IType) -> Self::InstructionResult;

    fn process_lui(&mut self, dec_insn: insn_format::UType) -> Self::InstructionResult;

    fn process_beq(&mut self, dec_insn: insn_format::BType) -> Self::InstructionResult;
    fn process_bne(&mut self, dec_insn: insn_format::BType) -> Self::InstructionResult;

    fn process_lw(&mut self, dec_insn: insn_format::IType) -> Self::InstructionResult;

    fn process_sw(&mut self, dec_insn: insn_format::SType) -> Self::InstructionResult;

    fn process_jal(&mut self, dec_insn: insn_format::JType) -> Self::InstructionResult;
    fn process_jalr(&mut self, dec_insn: insn_format::IType) -> Self::InstructionResult;

    fn process_csrrw(&mut self, dec_insn: insn_format::ITypeCSR) -> Self::InstructionResult;
    fn process_csrrs(&mut self, dec_insn: insn_format::ITypeCSR) -> Self::InstructionResult;

    fn process_ecall(&mut self) -> Self::InstructionResult;

    // custom-0
    fn process_bn_lid(&mut self, dec_insn: insn_format::WidType) -> Self::InstructionResult;
    fn process_bn_sid(&mut self, dec_insn: insn_format::WidType) -> Self::InstructionResult;
    fn process_bn_sel(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult;

    fn process_bn_cmp(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult;
    fn process_bn_cmpb(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult;

    fn process_bn_mov(&mut self, dec_insn: insn_format::IType) -> Self::InstructionResult;
    fn process_bn_movr(&mut self, dec_insn: insn_format::SType) -> Self::InstructionResult;

    fn process_bn_wsrr(&mut self, dec_insn: insn_format::IType) -> Self::InstructionResult;
    fn process_bn_wsrw(&mut self, dec_insn: insn_format::IType) -> Self::InstructionResult;

    // custom-1
    fn process_bn_add(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult;
    fn process_bn_addc(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult;
    fn process_bn_addm(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult;
    fn process_bn_addi(&mut self, dec_insn: insn_format::IType) -> Self::InstructionResult;
    fn process_bn_sub(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult;
    fn process_bn_subb(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult;
    fn process_bn_subm(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult;
    fn process_bn_subi(&mut self, dec_insn: insn_format::IType) -> Self::InstructionResult;

    // op-32
    fn process_bn_mulqacc(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult;
    fn process_bn_mulqacc_wo(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult;
    fn process_bn_mulqacc_so(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult;

    // custom-3
    fn process_loop(&mut self, dec_insn: insn_format::IType) -> Self::InstructionResult;
    fn process_loopi(&mut self, dec_insn: insn_format::IType) -> Self::InstructionResult;

    fn process_bn_and(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult;
    fn process_bn_or(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult;
    fn process_bn_not(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult;
    fn process_bn_xor(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult;
    fn process_bn_rshi(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult;
}

bitflags! {
    #[derive(Default)]
    pub struct Flags: u8 {
        const CARRY = 0b0001;
        const MSB = 0b0010;
        const LSB = 0b0100;
        const ZERO = 0b1000;
    }
}
