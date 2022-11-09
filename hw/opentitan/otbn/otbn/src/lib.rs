// Copyright 2021 Gregory Chadwick <mail@gregchadwick.co.uk>
// Copyright 2022-2023 Rivos, Inc.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

//! OTBN instruction set simulator library

extern crate bitflags;
extern crate ethnum;
extern crate paste;

use ethnum::u256;

pub mod comm;
pub mod csrs;
pub mod insn_decode;
pub mod insn_disasm;
pub mod insn_exec;
pub mod insn_format;
pub mod insn_proc;
pub mod memory;
pub mod otbn;
pub mod proxy;
pub mod random;
pub mod xoshiro256pp;

#[derive(PartialEq, Eq, Clone, Copy, Debug)]
#[repr(u32)]
pub enum ExceptionCause {
    EBadDataAddr = 1 << 0,
    EBadInsnAddr = 1 << 1,
    ECallStack = 1 << 2,
    EIllegalInsn = 1 << 3,
    ELoop = 1 << 4,
    ERndRepChkFail = 1 << 6,
    ERndFipsChkFail = 1 << 7,
    EFatal = 1 << 20,
    ECallMMode = 1 << 31,
}

/// Special purpose register (narrow, 32-bit)
pub trait CSR {
    fn read(&self) -> Result<u32, ExceptionCause>;
    fn write(&mut self, val: u32) -> Result<(), ExceptionCause>;
}

/// Special purpose register (wide, 256-bit)
pub trait WSR {
    fn read(&self) -> Result<u256, ExceptionCause>;
    fn write(&mut self, val: u256) -> Result<(), ExceptionCause>;
}

/// Cryptograph-secure random generator trait
pub trait CSRNG {
    fn get_csrng_u32(&self) -> (u32, bool, bool);
    fn get_csrng_u256(&self) -> (u256, bool, bool);
}

/// Pseudo random generator trait
pub trait PRNG {
    fn get_prng_u32(&mut self) -> u32;
    fn get_prng_u64(&mut self) -> u64;
    fn get_prng_u256(&mut self) -> u256;
}

/// A trait for objects which implement memory operations
pub trait Memory: Send {
    /// Read `size` bytes from `addr`.
    ///
    /// `addr` must be aligned to `size`.
    /// Returns `None` if `addr` doesn't exist in this memory.
    fn read_mem(&mut self, addr: u32) -> Option<u32>;

    /// Write `size` bytes of `store_data` to `addr`
    ///
    /// `addr` must be aligned to `size`.
    /// Returns `true` if write succeeds.
    fn write_mem(&mut self, addr: u32, store_data: u32) -> bool;

    fn wipe(&mut self, prng: &mut dyn PRNG);

    fn update_from_slice(&mut self, src: &[u32]);
}
