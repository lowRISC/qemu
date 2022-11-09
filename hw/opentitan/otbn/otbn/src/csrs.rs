// Copyright 2021 Gregory Chadwick <mail@gregchadwick.co.uk>
// Copyright 2022-2023 Rivos, Inc.
// Licensed under the Apache License Version 2.0, with LLVM Exceptions, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

use std::cell::Cell;
use std::convert::TryFrom;
use std::rc::Rc;
use std::sync::{Arc, Mutex};

use ethnum::{u256, U256};

use super::insn_proc;
use super::otbn::FlagMode;
use super::random;
use super::{CSR, WSR};
use crate::{ExceptionCause, CSRNG, PRNG};


#[derive(PartialEq, Eq, Clone, Copy, Debug)]
#[repr(u32)]
#[allow(non_camel_case_types)]
pub enum CSRAddr {
    // OTBN custom CSRs
    fg0 = 0x7c0,
    fg1 = 0x7c1,
    flags = 0x7c8,
    mod0 = 0x7d0,
    mod1 = 0x7d1,
    mod2 = 0x7d2,
    mod3 = 0x7d3,
    mod4 = 0x7d4,
    mod5 = 0x7d5,
    mod6 = 0x7d6,
    mod7 = 0x7d7,
    rnd_prefetch = 0x7d8,
    rnd = 0xfc0,  // CSRNG
    urnd = 0xfc1, // PRNG
}

impl TryFrom<u32> for CSRAddr {
    type Error = u32;

    fn try_from(value: u32) -> Result<Self, Self::Error> {
        match value {
            0x7c0 => Ok(Self::fg0),
            0x7c1 => Ok(Self::fg1),
            0x7c8 => Ok(Self::flags),
            0x7d0 => Ok(Self::mod0),
            0x7d1 => Ok(Self::mod1),
            0x7d2 => Ok(Self::mod2),
            0x7d3 => Ok(Self::mod3),
            0x7d4 => Ok(Self::mod4),
            0x7d5 => Ok(Self::mod5),
            0x7d6 => Ok(Self::mod6),
            0x7d7 => Ok(Self::mod7),
            0x7d8 => Ok(Self::rnd_prefetch),
            0xfc0 => Ok(Self::rnd),
            0xfc1 => Ok(Self::urnd),
            _ => Err(value),
        }
    }
}

#[allow(clippy::from_over_into)]
impl Into<u32> for CSRAddr {
    fn into(self) -> u32 {
        self as u32
    }
}

impl CSRAddr {
    pub fn string_name(csr_addr: u32) -> String {
        match Self::try_from(csr_addr) {
            Ok(csr) => format!("{:?}", csr),
            Err(_) => format!("0x{:03x}", csr_addr),
        }
    }
}

#[derive(PartialEq, Eq, Clone, Copy, Debug)]
#[repr(u32)]
#[allow(non_camel_case_types)]
pub enum WSRAddr {
    r#mod = 0x0,
    rnd = 0x1,
    urnd = 0x2,
    acc = 0x3,
    key_s0_l = 0x4,
    key_s0_h = 0x5,
    key_s1_l = 0x6,
    key_s1_h = 0x7,
}

impl TryFrom<u32> for WSRAddr {
    type Error = u32;

    fn try_from(value: u32) -> Result<Self, Self::Error> {
        match value {
            0x0 => Ok(Self::r#mod),
            0x1 => Ok(Self::rnd),
            0x2 => Ok(Self::urnd),
            0x3 => Ok(Self::acc),
            0x4 => Ok(Self::key_s0_l),
            0x5 => Ok(Self::key_s0_h),
            0x6 => Ok(Self::key_s1_l),
            0x7 => Ok(Self::key_s1_h),
            _ => Err(value),
        }
    }
}

#[allow(clippy::from_over_into)]
impl Into<u32> for WSRAddr {
    fn into(self) -> u32 {
        self as u32
    }
}

impl WSRAddr {
    pub fn string_name(wsr_addr: u32) -> String {
        match Self::try_from(wsr_addr) {
            Ok(wsr) => format!("{:?}", wsr),
            Err(_) => format!("0x{:03x}", wsr_addr),
        }
    }
}

#[derive(Default)]
struct CSRGeneric {
    pub val: u32,
}

impl CSR for CSRGeneric {
    fn read(&self) -> Result<u32, ExceptionCause> {
        Ok(self.val)
    }

    fn write(&mut self, val: u32) -> Result<(), ExceptionCause> {
        self.val = val;
        Ok(())
    }
}

#[derive(Default)]
struct CSRFlagGroup {
    flags: SharedFlags,
    mode: Option<FlagMode>,
}

impl CSRFlagGroup {
    pub fn plug(&mut self, flags: &SharedFlags, mode: FlagMode) {
        self.flags = Rc::clone(flags);
        self.mode = Some(mode);
    }
}

impl CSR for CSRFlagGroup {
    fn read(&self) -> Result<u32, ExceptionCause> {
        let val8 = match self.mode.as_ref().unwrap() {
            FlagMode::Fg0 => self.flags.get()[0].bits(),
            FlagMode::Fg1 => self.flags.get()[1].bits(),
            FlagMode::Flags => self.flags.get()[0].bits() | self.flags.get()[1].bits() << 4,
        };
        Ok(val8 as u32)
    }

    fn write(&mut self, val: u32) -> Result<(), ExceptionCause> {
        let val8 = val as u8;
        let mut flags = self.flags.get();
        match self.mode.as_ref().unwrap() {
            FlagMode::Fg0 => flags[0] = insn_proc::Flags::from_bits_truncate(val8 & 0x0f),
            FlagMode::Fg1 => flags[1] = insn_proc::Flags::from_bits_truncate(val8 & 0x0f),
            FlagMode::Flags => {
                flags[0] = insn_proc::Flags::from_bits_truncate(val8 & 0x0f);
                flags[1] = insn_proc::Flags::from_bits_truncate(val8 & 0x0f);
            }
        };
        self.flags.set(flags);
        Ok(())
    }
}

type SharedFlags = Rc<Cell<[insn_proc::Flags; 2]>>;

#[derive(Default)]
struct CSRMod {
    wsg: Rc<Cell<u256>>,
    pos: Option<u32>,
}

impl CSR for CSRMod {
    fn read(&self) -> Result<u32, ExceptionCause> {
        let mut val256: u256 = self.wsg.get();
        val256 >>= self.pos.unwrap() * 32;
        val256 &= U256::from(u32::MAX);
        Ok(val256.as_u32())
    }

    fn write(&mut self, val: u32) -> Result<(), ExceptionCause> {
        let mut val256: u256 = self.wsg.get();
        let shift = self.pos.unwrap() * 32;
        val256 &= !(U256::from(u32::MAX).wrapping_shl(shift));
        val256 |= U256::from(val).wrapping_shl(shift);
        self.wsg.set(val256);
        Ok(())
    }
}

impl CSRMod {
    fn plug(&mut self, wsg: &CSRWideSharedGeneric, pos: usize) {
        self.wsg = Rc::clone(&wsg.val);
        self.pos = Some(pos as u32);
    }
}

/// CryptoSecure Random generator
struct CSRRnd {
    rng: Arc<random::Rnd>,
}

impl CSRRnd {
    pub fn new(rng: Arc<random::Rnd>) -> Self {
        Self { rng }
    }
}

impl CSR for CSRRnd {
    fn read(&self) -> Result<u32, ExceptionCause> {
        let (val, fips, repeat) = self.rng.get_csrng_u32();
        if !fips {
            Err(ExceptionCause::ERndFipsChkFail)
        } else if repeat {
            Err(ExceptionCause::ERndRepChkFail)
        } else {
            Ok(val)
        }
    }

    fn write(&mut self, _val: u32) -> Result<(), ExceptionCause> {
        // as per OTBN definition, do not generate an error for R/O CSR
        Ok(())
    }
}

/// CryptoSecure Random generator
struct CSRWideRnd {
    rng: Arc<random::Rnd>,
}

impl CSRWideRnd {
    pub fn new(rng: Arc<random::Rnd>) -> Self {
        Self { rng }
    }
}

impl WSR for CSRWideRnd {
    fn read(&self) -> Result<u256, ExceptionCause> {
        let (val, fips, repeat) = self.rng.get_csrng_u256();
        if !fips {
            Err(ExceptionCause::ERndFipsChkFail)
        } else if repeat {
            Err(ExceptionCause::ERndRepChkFail)
        } else {
            Ok(val)
        }
    }

    fn write(&mut self, _val: u256) -> Result<(), ExceptionCause> {
        // as per OTBN definition, do not generate an error for R/O CSR
        Ok(())
    }
}

/// Pseudo Random generator
struct CSRUrnd {
    prng: Arc<Mutex<dyn PRNG>>,
}

impl CSRUrnd {
    pub fn new(prng: Arc<Mutex<dyn PRNG>>) -> Self {
        Self { prng }
    }
}

impl CSR for CSRUrnd {
    fn read(&self) -> Result<u32, ExceptionCause> {
        Ok(self.prng.lock().unwrap().get_prng_u32())
    }

    fn write(&mut self, _val: u32) -> Result<(), ExceptionCause> {
        // as per OTBN definition, do not generate an error for R/O CSR
        Ok(())
    }
}

/// Pseudo Random generator
struct CSRWideUrnd {
    prng: Arc<Mutex<dyn PRNG>>,
    test_mode: bool,
}

impl CSRWideUrnd {
    pub fn new(prng: Arc<Mutex<dyn PRNG>>) -> Self {
        Self {
            prng,
            test_mode: false,
        }
    }
}

impl WSR for CSRWideUrnd {
    fn read(&self) -> Result<u256, ExceptionCause> {
        if !self.test_mode {
            Ok(self.prng.lock().unwrap().get_prng_u256())
        } else {
            Ok(U256::from_str_radix(
                "AAAAAAAA99999999AAAAAAAA99999999AAAAAAAA99999999AAAAAAAA99999999",
                16,
            )
            .unwrap())
        }
    }

    fn write(&mut self, _val: u256) -> Result<(), ExceptionCause> {
        // as per OTBN definition, do not generate an error for R/O CSR
        Ok(())
    }
}

struct CSRRndPrefetcher {
    rng: Arc<random::Rnd>,
}

impl CSRRndPrefetcher {
    pub fn new(rng: Arc<random::Rnd>) -> Self {
        Self { rng }
    }
}

impl CSR for CSRRndPrefetcher {
    fn read(&self) -> Result<u32, ExceptionCause> {
        Ok(0) // always 0
    }

    fn write(&mut self, _val: u32) -> Result<(), ExceptionCause> {
        // "Writing any value to the RND_PREFETCH CSR initiates a prefetch."
        self.rng.prefetch();
        Ok(())
    }
}

#[derive(Default)]
struct CSRWideGeneric {
    pub val: u256,
}

impl WSR for CSRWideGeneric {
    fn read(&self) -> Result<u256, ExceptionCause> {
        Ok(self.val)
    }

    fn write(&mut self, val: u256) -> Result<(), ExceptionCause> {
        self.val = val;
        Ok(())
    }
}

#[derive(Default)]
struct CSRWideSharedGeneric {
    // TBC: why can't I use an Option<Rc<Cell<u256>>> here (Copy trait)?
    pub val: Rc<Cell<u256>>,
}

impl WSR for CSRWideSharedGeneric {
    fn read(&self) -> Result<u256, ExceptionCause> {
        Ok(self.val.get())
    }

    fn write(&mut self, val: u256) -> Result<(), ExceptionCause> {
        self.val.set(val);
        Ok(())
    }
}

pub struct CSRSet {
    fg0: CSRFlagGroup,
    fg1: CSRFlagGroup,
    flags: CSRFlagGroup,
    mods: [CSRMod; 8],
    rndprefetch: CSRRndPrefetcher,
    rnd: CSRRnd,
    urnd: CSRUrnd,

    r#mod: CSRWideSharedGeneric,
    wrnd: CSRWideRnd,
    wurnd: CSRWideUrnd,
    acc: CSRWideGeneric,
    key_s0_l: CSRWideGeneric,
    key_s0_h: CSRWideGeneric,
    key_s1_l: CSRWideGeneric,
    key_s1_h: CSRWideGeneric,

    shared_flags: SharedFlags,
}

impl CSRSet {
    pub fn new(urnd: Arc<Mutex<dyn PRNG>>, rnd: Arc<random::Rnd>) -> Self {
        let mut csrs = Self {
            fg0: CSRFlagGroup::default(),
            fg1: CSRFlagGroup::default(),
            flags: CSRFlagGroup::default(),
            mods: <[CSRMod; 8]>::default(),
            rndprefetch: CSRRndPrefetcher::new(rnd.clone()),
            rnd: CSRRnd::new(rnd.clone()),
            urnd: CSRUrnd::new(urnd.clone()),
            r#mod: CSRWideSharedGeneric::default(),
            wrnd: CSRWideRnd::new(rnd),
            wurnd: CSRWideUrnd::new(urnd.clone()),
            acc: CSRWideGeneric::default(),
            key_s0_l: CSRWideGeneric::default(),
            key_s0_h: CSRWideGeneric::default(),
            key_s1_l: CSRWideGeneric::default(),
            key_s1_h: CSRWideGeneric::default(),
            shared_flags: SharedFlags::default()
        };
        csrs.fg0.plug(&csrs.shared_flags, FlagMode::Fg0);
        csrs.fg1.plug(&csrs.shared_flags, FlagMode::Fg1);
        csrs.flags.plug(&csrs.shared_flags, FlagMode::Flags);
        for (pos, otmod) in csrs.mods.iter_mut().enumerate() {
            otmod.plug(&csrs.r#mod, pos)
        }
        csrs
    }

    pub fn get_flags(&self, fg: usize) -> insn_proc::Flags {
        self.shared_flags.get()[fg]
    }

    pub fn set_flags(&mut self, fg: usize, value: insn_proc::Flags) {
        let mut flags = self.shared_flags.get();
        flags[fg] = value;
        self.shared_flags.set(flags);
    }

    pub fn get_csr(&self, addr: u32) -> Option<&dyn CSR> {
        let csr_addr = CSRAddr::try_from(addr).ok()?;

        // note: we do not want to use num_enum here that draws to many dependencies
        Some(match csr_addr {
            CSRAddr::fg0 => &self.fg0,
            CSRAddr::fg1 => &self.fg1,
            CSRAddr::flags => &self.flags,
            CSRAddr::mod0 => &self.mods[0],
            CSRAddr::mod1 => &self.mods[1],
            CSRAddr::mod2 => &self.mods[2],
            CSRAddr::mod3 => &self.mods[3],
            CSRAddr::mod4 => &self.mods[4],
            CSRAddr::mod5 => &self.mods[5],
            CSRAddr::mod6 => &self.mods[6],
            CSRAddr::mod7 => &self.mods[7],
            CSRAddr::rnd_prefetch => &self.rndprefetch,
            CSRAddr::rnd => &self.rnd,
            CSRAddr::urnd => &self.urnd,
        })
    }

    pub fn get_csr_mut(&mut self, addr: u32) -> Option<&mut dyn CSR> {
        let csr_addr = CSRAddr::try_from(addr).ok()?;

        // note: we do not want to use num_enum here that draws to many dependencies
        Some(match csr_addr {
            CSRAddr::fg0 => &mut self.fg0,
            CSRAddr::fg1 => &mut self.fg1,
            CSRAddr::flags => &mut self.flags,
            CSRAddr::mod0 => &mut self.mods[0],
            CSRAddr::mod1 => &mut self.mods[1],
            CSRAddr::mod2 => &mut self.mods[2],
            CSRAddr::mod3 => &mut self.mods[3],
            CSRAddr::mod4 => &mut self.mods[4],
            CSRAddr::mod5 => &mut self.mods[5],
            CSRAddr::mod6 => &mut self.mods[6],
            CSRAddr::mod7 => &mut self.mods[7],
            CSRAddr::rnd_prefetch => &mut self.rndprefetch,
            CSRAddr::rnd => &mut self.rnd,
            CSRAddr::urnd => &mut self.urnd,
        })
    }

    pub fn get_wsr(&self, addr: u32) -> Option<&dyn WSR> {
        let wsr_addr = WSRAddr::try_from(addr).ok()?;

        Some(match wsr_addr {
            WSRAddr::r#mod => &self.r#mod,
            WSRAddr::rnd => &self.wrnd,
            WSRAddr::urnd => &self.wurnd,
            WSRAddr::acc => &self.acc,
            WSRAddr::key_s0_l => &self.key_s0_l,
            WSRAddr::key_s0_h => &self.key_s0_h,
            WSRAddr::key_s1_l => &self.key_s1_l,
            WSRAddr::key_s1_h => &self.key_s1_h,
        })
    }

    pub fn get_wsr_mut(&mut self, addr: u32) -> Option<&mut dyn WSR> {
        let wsr_addr = WSRAddr::try_from(addr).ok()?;

        Some(match wsr_addr {
            WSRAddr::r#mod => &mut self.r#mod,
            WSRAddr::rnd => &mut self.wrnd,
            WSRAddr::urnd => &mut self.wurnd,
            WSRAddr::acc => &mut self.acc,
            WSRAddr::key_s0_l => &mut self.key_s0_l,
            WSRAddr::key_s0_h => &mut self.key_s0_h,
            WSRAddr::key_s1_l => &mut self.key_s1_l,
            WSRAddr::key_s1_h => &mut self.key_s1_h,
        })
    }

    pub fn wipe_internal(&mut self, prng: &Arc<Mutex<dyn PRNG>>) {
        /*
         * real HW performs a two-step process:
         * - wipe with PRNG randomness
         * - zero
         */
        for flag in self.shared_flags.get().iter_mut() {
            *flag = insn_proc::Flags::empty();
        }

        let mut prng = prng.lock().unwrap();

        let _ = self.acc.write(U256::from(0u32));
        let _ = self.r#mod.write(U256::from(0u32));

        let _ = self.acc.write(prng.get_prng_u256());
        let _ = self.r#mod.write(prng.get_prng_u256());
    }

    pub fn set_test_mode(&mut self, enable: bool) {
        if enable {
            self.key_s0_l.val = U256::from_str_radix(
                "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef",
                16,
            )
            .unwrap();
            self.key_s0_h.val =
                U256::from_str_radix("deadbeefdeadbeefdeadbeefdeadbeef", 16).unwrap();
            self.key_s1_l.val = U256::from_str_radix(
                "baadf00dbaadf00dbaadf00dbaadf00dbaadf00dbaadf00dbaadf00dbaadf00d",
                16,
            )
            .unwrap();
            self.key_s1_h.val =
                U256::from_str_radix("baadf00dbaadf00dbaadf00dbaadf00d", 16).unwrap();
        } else {
            self.key_s0_l.val = U256::from(0u32);
            self.key_s0_h.val = U256::from(0u32);
            self.key_s1_l.val = U256::from(0u32);
            self.key_s1_h.val = U256::from(0u32);
        }

        // self.wrnd.test_mode = enable;
        self.wurnd.test_mode = enable;
    }
}
