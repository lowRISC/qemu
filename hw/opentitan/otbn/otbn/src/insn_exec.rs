// Copyright 2021 Gregory Chadwick <mail@gregchadwick.co.uk>
// Copyright 2022-2023 Rivos, Inc.
// Licensed under the Apache License Version 2.0, with LLVM Exceptions, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

use std::convert::TryInto;
use std::fmt;
use std::mem::size_of;
use std::sync::{Arc, Mutex};

use ethnum::{u256, AsU256, U256};
use paste::paste;

use super::csrs;
use super::insn_decode;
use super::insn_format;
use super::insn_proc;
use super::random;
use super::Memory;
use crate::{ExceptionCause, PRNG};

/// OTBN wide register width
const WLEN: usize = size_of::<U256>() * 8;

/// Different traps that can occur during instruction execution
#[derive(Debug, PartialEq, Eq)]
pub enum InstructionTrap {
    /// Trap is a synchronous exception, with a particular cause.
    Exception(ExceptionCause, Option<u32>),
}

/// HW loop tuple
#[derive(PartialEq, Eq, Clone, Copy)]
pub struct Loop {
    /// remaining loop to execute
    pub count: u32,
    /// absolute start address of the loop
    pub start: u32,
    /// absolute end address of the loop
    pub end: u32,
}

impl fmt::Debug for Loop {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Loop")
            .field("count", &self.count)
            .field("start", &format_args!("0x{:04x}", self.start))
            .field("end", &format_args!("0x{:04x}", self.end))
            .finish()
    }
}

/// Track updated hart state, for debugging purpose only
#[derive(Default)]
pub struct StateTracker {
    pub gpr: Option<usize>,             // GPR, if any
    pub wgpr: Option<usize>,            // wide GPR, if any
    pub csr: Option<(bool, u32)>,       // CSR/WCSR if any
    pub loophead: Option<(usize, u32)>, // head of loopstack
}

impl StateTracker {
    fn clear(&mut self) {
        self.gpr = None;
        self.wgpr = None;
        self.csr = None;
        self.loophead = None;
    }
}

/// State of a single OTBN hart
pub struct HartState {
    /// x1 - x31 register values. The contents of index 0 (the x0 zero register) are ignored.
    pub registers: [u32; 32],
    /// w0 - w31 wide register values
    pub wregisters: [u256; 32],
    /// Program counter
    pub pc: u32,
    /// 8-loop deep HW loop stack for use with loop/loopi instructions
    pub loopstack: Vec<Loop>,
    /// 8-address deep HW stack for use with x1 GPR
    pub hwstack: Vec<u32>,
    /// Special resisters (narrow and wide)
    pub csr_set: csrs::CSRSet,
    /// Track state changes (debug)
    pub updated: StateTracker,
}

impl HartState {
    pub fn new(urnd: Arc<Mutex<dyn PRNG>>, rnd: Arc<random::Rnd>) -> Self {
        HartState {
            registers: [0; 32],
            wregisters: [0.as_u256(); 32],
            pc: 0,
            loopstack: Vec::with_capacity(8),
            hwstack: Vec::with_capacity(8),
            updated: StateTracker::default(),
            csr_set: csrs::CSRSet::new(urnd, rnd),
        }
    }

    /// Write a register in the hart state. Used by executing instructions for correct zero
    /// register handling
    fn write_register(&mut self, reg_index: usize, data: u32) -> Result<(), InstructionTrap> {
        if reg_index == 0 {
            return Ok(());
        }
        if reg_index == 1 {
            if self.hwstack.len() >= self.hwstack.capacity() {
                return Err(InstructionTrap::Exception(ExceptionCause::ECallStack, None));
            }
            self.hwstack.push(data);
        }

        self.registers[reg_index] = data;
        self.updated.gpr = Some(reg_index);
        Ok(())
    }

    /// Read a register from the hart state. Used by executing instructions for correct zero
    /// register handling
    fn read_register(&mut self, reg_index: usize) -> Result<u32, InstructionTrap> {
        if reg_index == 0 {
            Ok(0)
        } else if reg_index == 1 {
            match self.hwstack.pop() {
                Some(x) => {
                    self.registers[reg_index] = x;
                    Ok(x)
                }
                None => Err(InstructionTrap::Exception(ExceptionCause::ECallStack, None)),
            }
        } else {
            Ok(self.registers[reg_index])
        }
    }

    /// Write a register in the hart state. Used by executing instructions for correct zero
    /// register handling
    fn write_wide_register(&mut self, reg_index: usize, data: u256) -> Result<(), InstructionTrap> {
        self.wregisters[reg_index] = data;
        self.updated.wgpr = Some(reg_index);
        Ok(())
    }

    /// Read a register from the hart state. Used by executing instructions for correct zero
    /// register handling
    fn read_wide_register(&mut self, reg_index: usize) -> Result<u256, InstructionTrap> {
        Ok(self.wregisters[reg_index])
    }

    fn write_csr(&mut self, csr_addr: u32, data: u32) -> Result<(), InstructionTrap> {
        let csr = self
            .csr_set
            .get_csr_mut(csr_addr)
            .ok_or(InstructionTrap::Exception(
                ExceptionCause::EIllegalInsn,
                Some(csr_addr),
            ))?;

        if let Err(exc) = csr.write(data) {
           return Err(InstructionTrap::Exception(exc, Some(csr_addr)));
        }
        self.updated.csr = Some((false, csr_addr));
        Ok(())
    }

    fn read_csr(&mut self, csr_addr: u32) -> Result<u32, InstructionTrap> {
        let csr = self
            .csr_set
            .get_csr(csr_addr)
            .ok_or(InstructionTrap::Exception(
                ExceptionCause::EIllegalInsn,
                Some(csr_addr),
            ))?;

        csr.read().map_err(|exc| InstructionTrap::Exception(exc, Some(csr_addr)))
    }

    fn write_wsr(&mut self, wsr_addr: u32, data: u256) -> Result<(), InstructionTrap> {
        let wsr = self
            .csr_set
            .get_wsr_mut(wsr_addr)
            .ok_or(InstructionTrap::Exception(
                ExceptionCause::EIllegalInsn,
                Some(wsr_addr),
            ))?;

        if let Err(exc) = wsr.write(data) {
           return Err(InstructionTrap::Exception(exc, Some(wsr_addr)));
        }
        self.updated.csr = Some((true, wsr_addr));
        Ok(())
    }

    fn read_wsr(&mut self, wsr_addr: u32) -> Result<u256, InstructionTrap> {
        let wsr = self
            .csr_set
            .get_wsr(wsr_addr)
            .ok_or(InstructionTrap::Exception(
                ExceptionCause::EIllegalInsn,
                Some(wsr_addr),
            ))?;

        wsr.read().map_err(|exc| InstructionTrap::Exception(exc, Some(wsr_addr)))
    }

    fn set_mlz_wide_flags(&mut self, fg: usize, carry: bool, value: u256) {
        let mut flags = if carry {
            insn_proc::Flags::CARRY
        } else {
            insn_proc::Flags::default()
        };
        if (value >> 255u32).as_u32() != 0 {
            flags |= insn_proc::Flags::MSB;
        }
        if (value & U256::from(1u32)) != 0 {
            flags |= insn_proc::Flags::LSB;
        }
        if value == 0 {
            flags |= insn_proc::Flags::ZERO;
        }
        self.csr_set.set_flags(fg, flags);
    }

    fn update_mlz_wide_flags(&mut self, fg: usize, value: u256) {
        let mut flags = self.csr_set.get_flags(fg) & insn_proc::Flags::CARRY;
        if (value >> 255u32).as_u32() != 0 {
            flags |= insn_proc::Flags::MSB;
        }
        if (value & U256::from(1u32)) != 0 {
            flags |= insn_proc::Flags::LSB;
        }
        if value == 0 {
            flags |= insn_proc::Flags::ZERO;
        }
        self.csr_set.set_flags(fg, flags);
    }

    pub fn wipe_internal(&mut self, prng: &Arc<Mutex<dyn PRNG>>) {
        {
            let mut prng = prng.lock().unwrap();

            // GPRs and WDRs
            for reg in self.registers.iter_mut() {
                *reg = prng.get_prng_u32();
            }
            for reg in self.wregisters.iter_mut() {
                *reg = prng.get_prng_u256();
            }
        }

        // Accumulator
        // Flags
        // Modulus
        self.csr_set.wipe_internal(prng);

        self.loopstack.clear();
        self.hwstack.clear();
        self.updated.clear();
    }

    pub fn set_test_mode(&mut self, enable: bool) {
        self.csr_set.set_test_mode(enable)
    }
}

/// An `InstructionProcessor` that execute instructions, updating `hart_state` as appropriate.
pub struct InstructionExecutor<'a, M: Memory> {
    /// Instruction memory used by fetch instructions
    pub imem: &'a mut M,
    /// Data memory used by load and store instructions
    pub dmem: &'a mut M,
    pub hart_state: &'a mut HartState,
}

impl<'a, M: Memory> InstructionExecutor<'a, M> {
    fn execute_reg_reg_op<F>(
        &mut self,
        dec_insn: insn_format::RType,
        op: F,
    ) -> Result<(), InstructionTrap>
    where
        F: Fn(u32, u32) -> u32,
    {
        let a = self.hart_state.read_register(dec_insn.rs1)?;
        let b = self.hart_state.read_register(dec_insn.rs2)?;
        let result = op(a, b);
        self.hart_state.write_register(dec_insn.rd, result)
    }

    fn execute_reg_imm_op<F>(
        &mut self,
        dec_insn: insn_format::IType,
        op: F,
    ) -> Result<(), InstructionTrap>
    where
        F: Fn(u32, u32) -> u32,
    {
        let a = self.hart_state.read_register(dec_insn.rs1)?;
        let b = dec_insn.imm as u32;
        let result = op(a, b);
        self.hart_state.write_register(dec_insn.rd, result)
    }

    fn execute_reg_imm_shamt_op<F>(
        &mut self,
        dec_insn: insn_format::ITypeShamt,
        op: F,
    ) -> Result<(), InstructionTrap>
    where
        F: Fn(u32, u32) -> u32,
    {
        let a = self.hart_state.read_register(dec_insn.rs1)?;
        let result = op(a, dec_insn.shamt);
        self.hart_state.write_register(dec_insn.rd, result)
    }

    fn execute_reg_imm_bn_op<F>(
        &mut self,
        dec_insn: insn_format::IType,
        op: F,
    ) -> Result<(), InstructionTrap>
    where
        F: Fn(u256, u256) -> (u256, bool),
    {
        let a = self.hart_state.read_wide_register(dec_insn.rs1)?;
        let b = dec_insn.imm as u32 & 0x3ff;
        let fg = ((dec_insn.imm >> 11) & 0b1) as usize;
        let (res, carry) = op(a, U256::from(b));
        self.hart_state.write_wide_register(dec_insn.rd, res)?;
        self.hart_state.set_mlz_wide_flags(fg, carry, res);
        Ok(())
    }

    fn execute_reg_reg_bn_op<F>(
        &mut self,
        dec_insn: insn_format::RType,
        op: F,
    ) -> Result<(), InstructionTrap>
    where
        F: Fn(u256, u256) -> u256,
    {
        let a = self.hart_state.read_wide_register(dec_insn.rs1)?;
        let b = self.hart_state.read_wide_register(dec_insn.rs2)?;
        let st = dec_insn.funct7 & 0b010_0000 != 0;
        let sbits = (dec_insn.funct7 & 0b001_1111) << 3;
        let fg = ((dec_insn.funct7 >> 6) & 0b1) as usize;

        let b = if st {
            b.wrapping_shr(sbits)
        } else {
            b.wrapping_shl(sbits)
        };

        let res = op(a, b);

        self.hart_state.write_wide_register(dec_insn.rd, res)?;
        self.hart_state.update_mlz_wide_flags(fg, res);

        Ok(())
    }

    fn execute_reg_reg_bn_of_op<F>(
        &mut self,
        dec_insn: insn_format::RType,
        op: F,
        bc: bool,
    ) -> Result<(), InstructionTrap>
    where
        F: Fn(u256, u256) -> (u256, bool),
    {
        let mut a = self.hart_state.read_wide_register(dec_insn.rs1)?;
        let b = self.hart_state.read_wide_register(dec_insn.rs2)?;
        let st = dec_insn.funct7 & 0b010_0000 != 0;
        let sbits = (dec_insn.funct7 & 0b001_1111) << 3;
        let fg = ((dec_insn.funct7 >> 6) & 0b1) as usize;
        let b = if st {
            b.wrapping_shr(sbits)
        } else {
            b.wrapping_shl(sbits)
        };

        // TBC: not sure if carry implementation works for all op
        let mut carry = false;
        if bc {
            let flags = self.hart_state.csr_set.get_flags(fg);
            if !(flags & insn_proc::Flags::CARRY).is_empty() {
                (a, carry) = op(a, U256::from(1u32));
            }
        }
        let (res, carry2) = op(a, b);
        carry |= carry2;
        self.hart_state.write_wide_register(dec_insn.rd, res)?;
        self.hart_state.set_mlz_wide_flags(fg, carry, res);
        Ok(())
    }

    fn execute_csr_op<F>(
        &mut self,
        dec_insn: insn_format::ITypeCSR,
        use_imm: bool,
        op: F,
    ) -> Result<(), InstructionTrap>
    where
        F: Fn(u32, u32) -> u32,
    {
        let old_csr = self.hart_state.read_csr(dec_insn.csr)?;

        let a = if use_imm {
            dec_insn.rs1 as u32
        } else {
            self.hart_state.read_register(dec_insn.rs1)?
        };

        let new_csr = op(old_csr, a);

        self.hart_state.write_csr(dec_insn.csr, new_csr)?;

        self.hart_state.write_register(dec_insn.rd, old_csr)
    }

    // Returns true if branch succeeds
    fn execute_branch<F>(
        &mut self,
        dec_insn: insn_format::BType,
        cond: F,
    ) -> Result<bool, InstructionTrap>
    where
        F: Fn(u32, u32) -> bool,
    {
        let a = self.hart_state.read_register(dec_insn.rs1)?;
        let b = self.hart_state.read_register(dec_insn.rs2)?;

        let lstack = &self.hart_state.loopstack;
        if let Some(hwloop) = lstack.last() {
            if self.hart_state.pc == hwloop.end {
                return Err(InstructionTrap::Exception(ExceptionCause::ELoop, None));
            }
        }
        if cond(a, b) {
            let new_pc = self.hart_state.pc.wrapping_add(dec_insn.imm as u32);
            self.hart_state.pc = new_pc;
            Ok(true)
        } else {
            Ok(false)
        }
    }

    fn execute_load(
        &mut self,
        dec_insn: insn_format::IType,
        signed: bool,
    ) -> Result<(), InstructionTrap> {
        let addr = self
            .hart_state
            .read_register(dec_insn.rs1)?
            .wrapping_add(dec_insn.imm as u32);

        if (addr & 0x03) != 0 {
            return Err(InstructionTrap::Exception(
                ExceptionCause::EBadDataAddr,
                Some(addr),
            ));
        }

        // Attempt to read data from memory, returning a LoadAccessFault as an error if it is not.
        let mut load_data = match self.dmem.read_mem(addr) {
            Some(d) => d,
            None => {
                return Err(InstructionTrap::Exception(
                    ExceptionCause::EBadDataAddr,
                    Some(addr),
                ));
            }
        };

        // Sign extend loaded data if required
        if signed {
            load_data = (load_data as i32) as u32;
        }

        // Write load data to destination register
        self.hart_state.write_register(dec_insn.rd, load_data)
    }

    fn execute_store(&mut self, dec_insn: insn_format::SType) -> Result<(), InstructionTrap> {
        let addr = self
            .hart_state
            .read_register(dec_insn.rs1)?
            .wrapping_add(dec_insn.imm as u32);
        let data = self.hart_state.read_register(dec_insn.rs2)?;

        // Determine if address is aligned to size
        if (addr & 0x3) != 0x0 {
            return Err(InstructionTrap::Exception(
                ExceptionCause::EBadDataAddr,
                Some(addr),
            ));
        }

        // Write store data to memory
        if self.dmem.write_mem(addr, data) {
            Ok(())
        } else {
            Err(InstructionTrap::Exception(
                ExceptionCause::EBadDataAddr,
                Some(addr),
            ))
        }
    }

    pub fn wipe_internal(&mut self, prng: &Arc<Mutex<dyn PRNG>>) {
        // "If OTBN cannot complete a secure wipe of the internal state (e.g., due to failing to
        // obtain the required randomness), it immediately becomes locked."
        // for now, wiping emulation does not involve random source, but this may be added if
        // a remote random source gets implemented. @todo
        self.hart_state.wipe_internal(prng);
    }

    pub fn reset(&mut self) {
        self.hart_state.pc = 0;
    }

    /// Execute instruction pointed to by `hart_state.pc`
    ///
    /// Returns `Ok` where instruction execution was successful. `Err` with the relevant
    /// [InstructionTrap] is returned when the instruction execution causes a trap.
    pub fn step(&mut self) -> Result<(), InstructionTrap> {
        self.hart_state.updated.clear();

        if let Some(next_insn) = self.imem.read_mem(self.hart_state.pc) {
            // Fetch next instruction from memory and eecute the instruction if fetch was
            // successful
            let step_result = insn_decode::decoder(self, next_insn);

            match step_result {
                Some(Ok(pc_updated)) => {
                    if !pc_updated {
                        let lstack = &mut self.hart_state.loopstack;
                        let loopdepth = lstack.len();
                        let loop_to = match lstack.last_mut() {
                            Some(hwloop) => {
                                if hwloop.end == self.hart_state.pc {
                                    // one less iteration to go
                                    hwloop.count -= 1;
                                    self.hart_state.updated.loophead =
                                        Some((loopdepth, hwloop.count));
                                    if hwloop.count == 0 {
                                        // loop exhausted, should be removed
                                        lstack.pop();
                                        // resume after the last loop instruction
                                        None
                                    } else {
                                        // restart from the first instruction of the loop
                                        Some(hwloop.start)
                                    }
                                } else {
                                    // current PC is not the last instruction of the loop
                                    None
                                }
                            }
                            // no HW loop is active
                            _ => None,
                        };
                        self.hart_state.pc = match loop_to {
                            Some(pc) => pc,
                            _ => self.hart_state.pc + 4,
                        };
                    }
                    Ok(())
                }
                // Instruction produced an illegal instruction error or decode failed so return an
                // IllegalInstruction as an error, supplying instruction bits
                Some(Err(InstructionTrap::Exception(ExceptionCause::EIllegalInsn, _))) | None => {
                    Err(InstructionTrap::Exception(
                        ExceptionCause::EIllegalInsn,
                        Some(next_insn),
                    ))
                }
                // Instruction produced an error so return it
                Some(Err(e)) => Err(e),
            }
        } else {
            // FetchError
            Err(InstructionTrap::Exception(
                ExceptionCause::EBadInsnAddr,
                Some(self.hart_state.pc),
            ))
        }
    }
}

// Macros to implement various repeated operations (e.g. ALU reg op reg instructions).
macro_rules! make_alu_op_reg_fn {
    ($name:ident, $op_fn:expr) => {
        paste! {
            fn [<process_ $name>](
                &mut self,
                dec_insn: insn_format::RType
            ) -> Self::InstructionResult {
                self.execute_reg_reg_op(dec_insn, $op_fn)?;
                Ok(false)
            }
        }
    };
}

// Macros to implement various repeated operations (on wide registers).
macro_rules! make_alu_bn_op_reg_fn {
    ($name:ident, $op_fn:expr) => {
        paste! {
            fn [<process_bn_ $name>](
                &mut self,
                dec_insn: insn_format::RType
            ) -> Self::InstructionResult {
                self.execute_reg_reg_bn_op(dec_insn, $op_fn)?;
                Ok(false)
            }
        }
    };
}

macro_rules! make_alu_bn_of_op_reg_fn {
    ($name:ident, $op_fn:expr, $bc:expr) => {
        paste! {
            fn [<process_bn_ $name>](
                &mut self,
                dec_insn: insn_format::RType
            ) -> Self::InstructionResult {
                self.execute_reg_reg_bn_of_op(dec_insn, $op_fn, $bc)?;
                Ok(false)
            }
        }
    };
}

macro_rules! make_alu_op_imm_fn {
    ($name:ident, $op_fn:expr) => {
        paste! {
            fn [<process_ $name i>](
                &mut self,
                dec_insn: insn_format::IType
            ) -> Self::InstructionResult {
                self.execute_reg_imm_op(dec_insn, $op_fn)?;
                Ok(false)
            }
        }
    };
}

macro_rules! make_alu_op_imm_shamt_fn {
    ($name:ident, $op_fn:expr) => {
        paste! {
            fn [<process_ $name i>](
                &mut self,
                dec_insn: insn_format::ITypeShamt
            ) -> Self::InstructionResult {
                self.execute_reg_imm_shamt_op(dec_insn, $op_fn)?;
                Ok(false)
            }
        }
    };
}

macro_rules! make_alu_op_fns {
    ($name:ident, $op_fn:expr) => {
        make_alu_op_reg_fn! {$name, $op_fn}
        make_alu_op_imm_fn! {$name, $op_fn}
    };
}

macro_rules! make_shift_op_fns {
    ($name:ident, $op_fn:expr) => {
        make_alu_op_reg_fn! {$name, $op_fn}
        make_alu_op_imm_shamt_fn! {$name, $op_fn}
    };
}

macro_rules! make_branch_op_fn {
    ($name:ident, $cond_fn:expr) => {
        paste! {
            fn [<process_ $name>](
                &mut self,
                dec_insn: insn_format::BType
            ) -> Self::InstructionResult {
                self.execute_branch(dec_insn, $cond_fn)
            }
        }
    };
}

macro_rules! make_load_op_fn_inner {
    ($name:ident, $signed: expr) => {
        paste! {
            fn [<process_ $name>](
                &mut self,
                dec_insn: insn_format::IType
            ) -> Self::InstructionResult {
                self.execute_load(dec_insn, $signed)?;
                Ok(false)
            }
        }
    };
}

macro_rules! make_load_op_fn {
    ($name:ident, signed) => {
        make_load_op_fn_inner! {$name, true}
    };
    ($name:ident, unsigned) => {
        make_load_op_fn_inner! {$name, false}
    };
}

macro_rules! make_store_op_fn {
    ($name:ident) => {
        paste! {
            fn [<process_ $name>](
                &mut self,
                dec_insn: insn_format::SType
            ) -> Self::InstructionResult {
                self.execute_store(dec_insn)?;
                Ok(false)
            }
        }
    };
}

macro_rules! make_alu_bn_op_imm_fn {
    ($name:ident, $op_fn:expr) => {
        paste! {
            fn [<process_bn_ $name i>](
                &mut self,
                dec_insn: insn_format::IType
            ) -> Self::InstructionResult {
                self.execute_reg_imm_bn_op(dec_insn, $op_fn)?;
                Ok(false)
            }
        }
    };
}

macro_rules! make_csr_op_fns {
    ($name:ident, $op_fn:expr) => {
        paste! {
            fn [<process_ $name>](
                &mut self,
                dec_insn: insn_format::ITypeCSR
            ) -> Self::InstructionResult {
                self.execute_csr_op(dec_insn, false, $op_fn)?;
                Ok(false)
            }
        }
    };
}

impl<'a, M: Memory> insn_proc::InstructionProcessor for InstructionExecutor<'a, M> {
    /// Result is `Ok` when instruction execution is successful. `Ok(true) indicates the
    /// instruction updated the PC and Ok(false) indicates it did not (so the PC must be
    /// incremented to execute the next instruction).
    type InstructionResult = Result<bool, InstructionTrap>;

    make_alu_op_fns! {add, |a, b| a.wrapping_add(b)}
    make_alu_op_reg_fn! {sub, |a, b| a.wrapping_sub(b)}
    make_alu_op_fns! {or, |a, b| a | b}
    make_alu_op_fns! {and, |a, b| a & b}
    make_alu_op_fns! {xor, |a, b| a ^ b}

    make_shift_op_fns! {sll, |a, b| a << (b & 0x1f)}
    make_shift_op_fns! {srl, |a, b| a >> (b & 0x1f)}
    make_shift_op_fns! {sra, |a, b| ((a as i32) >> (b & 0x1f)) as u32}

    fn process_lui(&mut self, dec_insn: insn_format::UType) -> Self::InstructionResult {
        self.hart_state
            .write_register(dec_insn.rd, dec_insn.imm as u32)?;
        Ok(false)
    }

    make_branch_op_fn! {beq, |a, b| a == b}
    make_branch_op_fn! {bne, |a, b| a != b}

    make_load_op_fn! {lw, unsigned}
    make_store_op_fn! {sw}

    fn process_jal(&mut self, dec_insn: insn_format::JType) -> Self::InstructionResult {
        let target_pc = self.hart_state.pc.wrapping_add(dec_insn.imm as u32);
        self.hart_state
            .write_register(dec_insn.rd, self.hart_state.pc + 4)?;
        self.hart_state.pc = target_pc;
        Ok(true)
    }

    fn process_jalr(&mut self, dec_insn: insn_format::IType) -> Self::InstructionResult {
        let target_pc = self
            .hart_state
            .read_register(dec_insn.rs1)?
            .wrapping_add(dec_insn.imm as u32)
            & !0b11;
        self.hart_state
            .write_register(dec_insn.rd, self.hart_state.pc + 4)?;
        self.hart_state.pc = target_pc;
        Ok(true)
    }

    make_csr_op_fns! {csrrw, |_old_csr, a| a}
    make_csr_op_fns! {csrrs, |old_csr, a| old_csr | a}

    fn process_ecall(&mut self) -> Self::InstructionResult {
        Err(InstructionTrap::Exception(
            ExceptionCause::ECallMMode,
            Some(0),
        ))
    }

    fn process_loop(&mut self, dec_insn: insn_format::IType) -> Self::InstructionResult {
        let bodysize = dec_insn.imm as u32 + 1;
        let hwloop = Loop {
            count: self.hart_state.read_register(dec_insn.rs1)?,
            start: self.hart_state.pc + 4, // next instruction
            end: self.hart_state.pc + bodysize * 4,
        };
        if hwloop.count == 0 {
            // to be handled properly
            return Err(InstructionTrap::Exception(ExceptionCause::ELoop, None));
        }
        let lstack = &mut self.hart_state.loopstack;
        if lstack.len() == lstack.capacity() {
            return Err(InstructionTrap::Exception(ExceptionCause::ELoop, None));
        }
        if let Some(hwloop) = lstack.last() {
            if self.hart_state.pc == hwloop.end {
                return Err(InstructionTrap::Exception(ExceptionCause::ELoop, None));
            }
        }
        // need to check this instruction is not a last instruction in an existing loop
        lstack.push(hwloop);
        self.hart_state.updated.loophead = Some((lstack.len(), hwloop.count));
        Ok(false)
    }

    fn process_loopi(&mut self, dec_insn: insn_format::IType) -> Self::InstructionResult {
        let bodysize = dec_insn.imm as u32 + 1;
        let hwloop = Loop {
            count: ((dec_insn.rs1 << 5) + dec_insn.rd) as u32,
            start: self.hart_state.pc + 4, // next instruction
            end: self.hart_state.pc + bodysize * 4,
        };
        if self.hart_state.loopstack.len() == self.hart_state.loopstack.capacity() {
            // to be handled properly
            return Err(InstructionTrap::Exception(ExceptionCause::ELoop, None));
        }
        if hwloop.count == 0 {
            return Err(InstructionTrap::Exception(ExceptionCause::ELoop, None));
        }
        // need to check this instruction is not a last instruction in an existing loop
        self.hart_state.loopstack.push(hwloop);
        self.hart_state.updated.loophead = Some((self.hart_state.loopstack.len(), hwloop.count));
        Ok(false)
    }

    fn process_bn_lid(&mut self, dec_insn: insn_format::WidType) -> Self::InstructionResult {
        let grs1_inc = (dec_insn.inc & 0b10) != 0;
        let grd_inc = (dec_insn.inc & 0b01) != 0;

        if grs1_inc && grd_inc {
            return Err(InstructionTrap::Exception(
                ExceptionCause::EIllegalInsn,
                None,
            ));
        }

        // wide register index
        let wri = self.hart_state.read_register(dec_insn.rs2)?;
        if wri > 31 {
            return Err(InstructionTrap::Exception(
                ExceptionCause::EIllegalInsn,
                None,
            ));
        }

        let addr = self.hart_state.read_register(dec_insn.rs1)?;
        if (addr as usize & (WLEN / 8 - 1)) != 0 {
            return Err(InstructionTrap::Exception(
                ExceptionCause::EBadDataAddr,
                Some(addr),
            ));
        }

        let offset = dec_insn.imm << 5;

        if (offset as usize & (WLEN / 8 - 1)) != 0 {
            return Err(InstructionTrap::Exception(
                ExceptionCause::EBadDataAddr,
                Some(offset as u32),
            ));
        }

        // check is there is a less stupid way to do this...
        let src = if offset >= 0 {
            addr.wrapping_add(offset as u32)
        } else {
            addr.wrapping_sub(-offset as u32)
        };

        let mut bytes = [0; size_of::<U256>()];
        const RSIZE: usize = size_of::<u32>();
        for (pos, bchunk) in (0..bytes.len()).step_by(RSIZE).zip(bytes.chunks_mut(RSIZE)) {
            let word = self.dmem.read_mem(src + pos as u32).unwrap();
            bchunk.copy_from_slice(&word.to_le_bytes()[..]);
        }
        let wvalue = U256::from_le_bytes(bytes);

        if grd_inc {
            self.hart_state.write_register(dec_insn.rs2, wri + 1)?;
        }

        if grs1_inc {
            let addr = addr.wrapping_add(size_of::<U256>() as u32);
            self.hart_state.write_register(dec_insn.rs1, addr)?;
        }

        self.hart_state.write_wide_register(wri as usize, wvalue)?;

        Ok(false)
    }

    fn process_bn_sid(&mut self, dec_insn: insn_format::WidType) -> Self::InstructionResult {
        let grs1_inc = (dec_insn.inc & 0b10) != 0;
        let grs2_inc = (dec_insn.inc & 0b01) != 0;

        if grs1_inc && grs2_inc {
            return Err(InstructionTrap::Exception(
                ExceptionCause::EIllegalInsn,
                None,
            ));
        }

        // wide register index
        let wri = self.hart_state.read_register(dec_insn.rs2)?;
        if wri > 31 {
            return Err(InstructionTrap::Exception(
                ExceptionCause::EIllegalInsn,
                None,
            ));
        }

        let addr = self.hart_state.read_register(dec_insn.rs1)?;
        if (addr as usize & (WLEN / 8 - 1)) != 0 {
            return Err(InstructionTrap::Exception(
                ExceptionCause::EBadDataAddr,
                Some(addr),
            ));
        }

        let offset = dec_insn.imm << 5;

        if (offset as usize & (WLEN / 8 - 1)) != 0 {
            return Err(InstructionTrap::Exception(
                ExceptionCause::EBadDataAddr,
                Some(offset as u32),
            ));
        }

        // check is there is a less stupid way to do this...
        let dst = if offset >= 0 {
            addr.wrapping_add(offset as u32)
        } else {
            addr.wrapping_sub(-offset as u32)
        };

        if grs1_inc {
            let addr = addr.wrapping_add(size_of::<U256>() as u32);
            self.hart_state.write_register(dec_insn.rs1, addr)?;
        }

        if grs2_inc {
            self.hart_state.write_register(dec_insn.rs2, wri + 1)?;
        }

        let wvalue = self.hart_state.read_wide_register(wri as usize)?;
        let bytes = wvalue.to_le_bytes();
        const RSIZE: usize = size_of::<u32>();
        for pos in (0..bytes.len()).step_by(RSIZE) {
            let word = u32::from_le_bytes(bytes[pos..pos + RSIZE].try_into().unwrap());
            self.dmem.write_mem(dst + pos as u32, word);
        }

        Ok(false)
    }

    fn process_bn_sel(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult {
        let fg = (dec_insn.funct7 >> 6) & 0b1;
        let flag = insn_proc::Flags::from_bits_truncate(1 << (dec_insn.funct7 & 0b11) as u8);
        let flags = self.hart_state.csr_set.get_flags(fg as usize);

        let val = if (flags & flag).is_empty() {
            self.hart_state.read_wide_register(dec_insn.rs2)?
        } else {
            self.hart_state.read_wide_register(dec_insn.rs1)?
        };

        self.hart_state.write_wide_register(dec_insn.rd, val)?;

        Ok(false)
    }

    fn process_bn_cmp(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult {
        let a = self.hart_state.read_wide_register(dec_insn.rs1)?;
        let b = self.hart_state.read_wide_register(dec_insn.rs2)?;
        let st = dec_insn.funct7 & 0b010_0000 != 0;
        let sbits = (dec_insn.funct7 & 0b001_1111) << 3;
        let fg = ((dec_insn.funct7 >> 6) & 0b1) as usize;

        let b = if st {
            b.wrapping_shr(sbits)
        } else {
            b.wrapping_shl(sbits)
        };

        let (res, carry) = a.overflowing_sub(b);

        self.hart_state.set_mlz_wide_flags(fg, carry, res);

        Ok(false)
    }

    fn process_bn_cmpb(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult {
        let mut a = self.hart_state.read_wide_register(dec_insn.rs1)?;
        let b = self.hart_state.read_wide_register(dec_insn.rs2)?;
        let st = dec_insn.funct7 & 0b010_0000 != 0;
        let sbits = (dec_insn.funct7 & 0b001_1111) << 3;
        let fg = ((dec_insn.funct7 >> 6) & 0b1) as usize;

        let b = if st {
            b.wrapping_shr(sbits)
        } else {
            b.wrapping_shl(sbits)
        };

        // TBC: not sure if carry implementation works
        let mut carry = false;
        let flags = self.hart_state.csr_set.get_flags(fg);
        if !(flags & insn_proc::Flags::CARRY).is_empty() {
            (a, carry) = a.overflowing_sub(U256::from(1u32));
        }
        let (res, carry2) = a.overflowing_sub(b);
        carry |= carry2;

        self.hart_state.set_mlz_wide_flags(fg, carry, res);

        Ok(false)
    }

    fn process_bn_mov(&mut self, dec_insn: insn_format::IType) -> Self::InstructionResult {
        let val = self.hart_state.read_wide_register(dec_insn.rs1)?;

        self.hart_state.write_wide_register(dec_insn.rd, val)?;

        Ok(false)
    }

    fn process_bn_movr(&mut self, dec_insn: insn_format::SType) -> Self::InstructionResult {
        let grd_inc = dec_insn.imm & 0b0001 != 0;
        let grs_inc = dec_insn.imm & 0b0100 != 0;

        if grd_inc && grs_inc {
            return Err(InstructionTrap::Exception(
                ExceptionCause::EIllegalInsn,
                None,
            ));
        }

        let grd_val = self.hart_state.read_register(dec_insn.rs2)?;
        let grs_val = self.hart_state.read_register(dec_insn.rs1)?;

        if grd_val > 31 || grs_val > 31 {
            return Err(InstructionTrap::Exception(
                ExceptionCause::EIllegalInsn,
                None,
            ));
        }

        if grd_inc {
            self.hart_state.write_register(dec_insn.rs2, grd_val + 1)?;
        }

        if grs_inc {
            self.hart_state.write_register(dec_insn.rs1, grs_val + 1)?;
        }

        let val = self.hart_state.read_wide_register(grs_val as usize)?;
        self.hart_state.write_wide_register(grd_val as usize, val)?;

        Ok(false)
    }

    fn process_bn_wsrr(&mut self, dec_insn: insn_format::IType) -> Self::InstructionResult {
        let wsr = dec_insn.imm as u32 & 0xff;
        let wval = self.hart_state.read_wsr(wsr)?;

        self.hart_state.write_wide_register(dec_insn.rd, wval)?;

        Ok(false)
    }

    fn process_bn_wsrw(&mut self, dec_insn: insn_format::IType) -> Self::InstructionResult {
        let wsr = dec_insn.imm as u32 & 0xff;
        let val = self.hart_state.read_wide_register(dec_insn.rs1)?;

        self.hart_state.write_wsr(wsr, val)?;

        Ok(false)
    }

    fn process_bn_addm(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult {
        let a = self.hart_state.read_wide_register(dec_insn.rs1)?;
        let b = self.hart_state.read_wide_register(dec_insn.rs2)?;
        let modv = self
            .hart_state
            .read_wsr(csrs::WSRAddr::r#mod.into())
            .unwrap();

        let (mut res, over) = a.overflowing_add(b);
        if over || res > modv {
            res = res.wrapping_sub(modv)
        }

        self.hart_state.write_wide_register(dec_insn.rd, res)?;

        Ok(false)
    }

    fn process_bn_subm(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult {
        let a = self.hart_state.read_wide_register(dec_insn.rs1)?;
        let b = self.hart_state.read_wide_register(dec_insn.rs2)?;
        let modv = self
            .hart_state
            .read_wsr(csrs::WSRAddr::r#mod.into())
            .unwrap();

        let (mut res, over) = a.overflowing_sub(b);
        if over {
            res = res.wrapping_add(modv)
        }

        self.hart_state.write_wide_register(dec_insn.rd, res)?;

        Ok(false)
    }

    fn process_bn_mulqacc(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult {
        let zero_acc = dec_insn.funct3 & 0b001 != 0;
        let acc_shift = 64 * (dec_insn.funct3 >> 1);
        let qwsel1 = dec_insn.funct7 & 0b11;
        let qwsel2 = dec_insn.funct7 >> 2 & 0b11;

        let a_val = self.hart_state.read_wide_register(dec_insn.rs1)?;
        let b_val = self.hart_state.read_wide_register(dec_insn.rs2)?;

        let a_qw = a_val >> (64 * qwsel1) & U256::from(u64::MAX);
        let b_qw = b_val >> (64 * qwsel2) & U256::from(u64::MAX);

        let mut mul_res = (a_qw).wrapping_mul(b_qw);

        let acc = if zero_acc {
            U256::from(0u32)
        } else {
            self.hart_state.read_wsr(csrs::WSRAddr::acc.into()).unwrap()
        };

        mul_res = mul_res.wrapping_shl(acc_shift);

        // add and truncate
        self.hart_state
            .write_wsr(csrs::WSRAddr::acc.into(), acc.wrapping_add(mul_res))
            .and(Ok(false))
    }

    fn process_bn_mulqacc_wo(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult {
        let zero_acc = dec_insn.funct3 & 0b001 != 0;
        let acc_shift = 64 * (dec_insn.funct3 >> 1);
        let qwsel1 = dec_insn.funct7 & 0b11;
        let qwsel2 = dec_insn.funct7 >> 2 & 0b11;
        let fg = ((dec_insn.funct7 >> 6) & 0b1) as usize;

        let a_val = self.hart_state.read_wide_register(dec_insn.rs1)?;
        let b_val = self.hart_state.read_wide_register(dec_insn.rs2)?;

        let a_qw = a_val >> (64 * qwsel1) & U256::from(u64::MAX);
        let b_qw = b_val >> (64 * qwsel2) & U256::from(u64::MAX);

        let mut mul_res = (a_qw).wrapping_mul(b_qw);

        let acc: u256 = if zero_acc {
            U256::from(0u32)
        } else {
            self.hart_state.read_wsr(csrs::WSRAddr::acc.into()).unwrap()
        };

        mul_res = mul_res.wrapping_shl(acc_shift);
        let truncated = acc.wrapping_add(mul_res);

        self.hart_state
            .write_wide_register(dec_insn.rd, truncated)?;
        self.hart_state
            .write_wsr(csrs::WSRAddr::acc.into(), truncated)?;

        self.hart_state.update_mlz_wide_flags(fg, truncated);
        Ok(false)
    }

    fn process_bn_mulqacc_so(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult {
        let zero_acc = dec_insn.funct3 & 0b001 != 0;
        let acc_shift = 64 * (dec_insn.funct3 >> 1);
        let qwsel1 = dec_insn.funct7 & 0b11;
        let qwsel2 = dec_insn.funct7 >> 2 & 0b11;
        let hwsel = dec_insn.funct7 >> 4 & 0b1;
        let fg = ((dec_insn.funct7 >> 6) & 0b1) as usize;

        let a_val = self.hart_state.read_wide_register(dec_insn.rs1)?;
        let b_val = self.hart_state.read_wide_register(dec_insn.rs2)?;
        let d_val = self.hart_state.read_wide_register(dec_insn.rd)?;

        let a_qw = a_val >> (64 * qwsel1) & U256::from(u64::MAX);
        let b_qw = b_val >> (64 * qwsel2) & U256::from(u64::MAX);

        let mut mul_res = a_qw.wrapping_mul(b_qw);

        let acc: u256 = if zero_acc {
            U256::from(0u32)
        } else {
            self.hart_state.read_wsr(csrs::WSRAddr::acc.into()).unwrap()
        };

        mul_res = mul_res.wrapping_shl(acc_shift);
        let truncated = acc.wrapping_add(mul_res);

        let lo_part = U256::from(*truncated.low());
        let hi_part = U256::from(*truncated.high());
        let hw_shift = 128 * hwsel;
        let hw_mask = U256::from(u128::MAX) << hw_shift;
        let wrd = (d_val & !hw_mask) | (lo_part << hw_shift);

        self.hart_state.write_wide_register(dec_insn.rd, wrd)?;
        self.hart_state
            .write_wsr(csrs::WSRAddr::acc.into(), hi_part)?;

        let flags = self.hart_state.csr_set.get_flags(fg);
        let mut new_flags;
        if hwsel != 0 {
            new_flags = flags & (insn_proc::Flags::CARRY | insn_proc::Flags::LSB);
            if ((lo_part >> 127u32).as_u32() & 0b1) != 0 {
                new_flags |= insn_proc::Flags::MSB;
            }
            if lo_part == 0 {
                new_flags |= flags & insn_proc::Flags::ZERO;
            }
        } else {
            new_flags = flags & (insn_proc::Flags::CARRY | insn_proc::Flags::MSB);
            if (lo_part & 0b1) != 0 {
                new_flags |= insn_proc::Flags::LSB;
            }
            if lo_part == 0 {
                new_flags |= insn_proc::Flags::ZERO;
            }
        }

        self.hart_state.csr_set.set_flags(fg, new_flags);

        Ok(false)
    }

    make_alu_bn_op_reg_fn! {and, |a, b| a & b}
    make_alu_bn_op_reg_fn! {or, |a, b| a | b}
    make_alu_bn_op_reg_fn! {xor, |a, b| a ^ b}
    make_alu_bn_op_imm_fn! {add, |a, b| a.overflowing_add(b)}
    make_alu_bn_op_imm_fn! {sub, |a, b| a.overflowing_sub(b)}
    make_alu_bn_of_op_reg_fn! {add, |a, b| a.overflowing_add(b), false}
    make_alu_bn_of_op_reg_fn! {sub, |a, b| a.overflowing_sub(b), false}
    make_alu_bn_of_op_reg_fn! {addc, |a, b| a.overflowing_add(b), true}
    make_alu_bn_of_op_reg_fn! {subb, |a, b| a.overflowing_sub(b), true}

    fn process_bn_not(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult {
        let a = self.hart_state.read_wide_register(dec_insn.rs2)?;
        let st = dec_insn.funct7 & 0b010_0000 != 0;
        let sbits = (dec_insn.funct7 & 0b001_1111) << 3;
        let a = if st {
            a.wrapping_shr(sbits)
        } else {
            a.wrapping_shl(sbits)
        };
        let fg = ((dec_insn.funct7 >> 6) & 0b1) as usize;

        let res = !a;
        self.hart_state.write_wide_register(dec_insn.rd, res)?;
        self.hart_state.update_mlz_wide_flags(fg, res);

        Ok(false)
    }

    fn process_bn_rshi(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult {
        let imm = (dec_insn.funct7 << 1) | (dec_insn.funct3 >> 2);
        let a = self.hart_state.read_wide_register(dec_insn.rs1)?;
        let b = self.hart_state.read_wide_register(dec_insn.rs2)?;

        let res = a.wrapping_shl(256 - imm) | b.wrapping_shr(imm);

        self.hart_state.write_wide_register(dec_insn.rd, res)?;

        Ok(false)
    }
}
