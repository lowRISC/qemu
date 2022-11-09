// Copyright 2022-2023 Rivos, Inc.
// Licensed under the Apache License Version 2.0, with LLVM Exceptions, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

extern crate ethnum;

use std::fs::File;
use std::io;
use std::io::Write;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use bitflags::bitflags;

use ethnum::U256;

use super::comm;
use super::csrs;
use super::insn_decode;
use super::insn_disasm;
use super::insn_exec;
use super::memory;
use super::random;
use super::Memory;
use crate::{ExceptionCause, PRNG};

/// Instruction memory size
pub const IMEM_SIZE: usize = 4 << 10;
/// Data memory size
pub const DMEM_SIZE: usize = 4 << 10;
/// Data public memory size
pub const DMEM_PUB_SIZE: usize = 3 << 10;

bitflags! {
    /// List of bits in the ERR_BITS register
    #[derive(Default)]
    pub struct ErrBits: u32 {
        const BAD_DATA_ADDR = 1 << 0;
        const BAD_INSN_ADDR = 1 << 1;
        const CALL_STACK = 1 << 2;
        const ILLEGAL_INSN = 1 << 3;
        const LOOP = 1 << 4;
        const KEY_INVALID = 1 << 5;
        const RND_REP_CHK_FAIL = 1 << 6;
        const RND_FIPS_CHK_FAIL = 1 << 7;
        const IMEM_INTG_VIOLATION = 1 << 16;
        const DMEM_INTG_VIOLATION = 1 << 17;
        const REG_INTG_VIOLATION = 1 << 18;
        const BUS_INTG_VIOLATION = 1 << 19;
        const BAD_INTERNAL_STATE = 1 << 20;
        const ILLEGAL_BUS_ACCESS = 1 << 21;
        const LIFECYCLE_ESCALATION = 1 << 22;
        const FATAL_SOFTWARE = 1 << 23;
    }
}

pub enum FlagMode {
    Fg0 = 0x0,
    Fg1 = 0x1,
    Flags = 0x2,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Status {
    #[default]
    Idle = 0,
    BusyExecute = 1,
    BusySecWipeDMem = 2,
    BusySecWipeIMem = 3,
    BusySecWipeInt = 4,
    Locked = 0xff,
}

impl Status {
    pub fn from_u32(val: u32) -> Self {
        match val {
            val if val == Status::Idle as u32 => Status::Idle,
            val if val == Status::BusyExecute as u32 => Status::BusyExecute,
            val if val == Status::BusySecWipeDMem as u32 => Status::BusySecWipeDMem,
            val if val == Status::BusySecWipeIMem as u32 => Status::BusySecWipeIMem,
            val if val == Status::BusySecWipeInt as u32 => Status::BusySecWipeInt,
            _ => Status::Locked,
        }
    }
}

#[derive(Default)]
pub struct Registers {
    /// Status bitfield
    pub status: Arc<AtomicU32>,
    /// ErrBits bitfield
    pub err_bits: Arc<AtomicU32>,
    /// ErrBits bitfield,
    pub fatal_bits: Arc<AtomicU32>,
    pub ctrl: Arc<AtomicBool>,
    pub insn_count: Arc<AtomicUsize>,
}

/// OTBN executer
/// Run from a worker thread
/// Use two channels to communicate w/ the proxy and shared registers
pub struct Executer {
    hart_state: insn_exec::HartState,
    imem: Arc<Mutex<memory::MemoryRegion>>,
    dmem: Arc<Mutex<memory::MemoryRegion>>,
    channel: comm::UpChannel,
    registers: Arc<Registers>,
    syncurnd: Arc<random::SyncUrnd>,
    on_complete: Option<Box<dyn comm::Callback>>,
    log_file: Option<Box<dyn io::Write>>,
}

impl Executer {
    #[allow(clippy::too_many_arguments)]
    fn new(
        channel: comm::UpChannel,
        registers: Arc<Registers>,
        imem: Arc<Mutex<memory::MemoryRegion>>,
        dmem: Arc<Mutex<memory::MemoryRegion>>,
        syncurnd: Arc<random::SyncUrnd>,
        rnd: Arc<random::Rnd>,
        on_complete: Option<Box<dyn comm::Callback>>,
        log_name: Option<String>,
    ) -> Self {
        let log_file: Option<Box<dyn io::Write>>;
        if let Some(logname) = log_name {
            if logname == "stderr" {
                log_file = Some(Box::new(io::stderr()));
            } else {
                log_file = Some(Box::new(File::create(logname).unwrap()));
            }
        } else {
            log_file = None;
        }
        Self {
            hart_state: insn_exec::HartState::new(syncurnd.urnd(), rnd),
            imem,
            dmem,
            channel,
            registers,
            syncurnd,
            on_complete,
            log_file,
        }
    }

    #[allow(clippy::too_many_arguments)]
    pub fn run(
        channel: comm::UpChannel,
        registers: Arc<Registers>,
        imem: Arc<Mutex<memory::MemoryRegion>>,
        dmem: Arc<Mutex<memory::MemoryRegion>>,
        urnd: Arc<random::SyncUrnd>,
        rnd: Arc<random::Rnd>,
        on_complete: Option<Box<dyn comm::Callback>>,
        log_name: Option<String>,
    ) {
        Self::new(
            channel,
            registers,
            imem,
            dmem,
            urnd,
            rnd,
            on_complete,
            log_name,
        )
        .enter();
    }

    fn enter(&mut self) {
        self.set_status(Status::Idle);
        let threadid = thread::current().id();
        self.channel
            .1
            .send(comm::Response::Active(threadid))
            .unwrap();
        loop {
            let cmd = self.channel.0.recv().unwrap();
            let state = self.get_status();
            match state {
                Status::Idle => self.handle_comm(cmd),
                Status::BusyExecute
                | Status::BusySecWipeDMem
                | Status::BusySecWipeIMem
                | Status::BusySecWipeInt => {
                    self.channel
                        .1
                        .send(comm::Response::Error("busy".to_string()))
                        .unwrap();
                }
                Status::Locked => {
                    self.channel
                        .1
                        .send(comm::Response::Error("locked".to_string()))
                        .unwrap();
                }
            }
        }
    }

    fn get_status(&self) -> Status {
        let val = self.registers.status.load(Ordering::Relaxed);
        Status::from_u32(val)
    }

    fn set_status(&mut self, status: Status) {
        self.registers
            .status
            .store(status as u32, Ordering::Relaxed);
    }

    /// Handle requests from the proxy
    fn handle_comm(&mut self, cmd: comm::Command) {
        match cmd {
            comm::Command::SetTestMode(enable) => {
                self.hart_state.set_test_mode(enable);
                self.channel.1.send(comm::Response::Ack).unwrap();
                self.signal_completion();
            }
            comm::Command::LogTo(logfile) => {
                self.log_file = Some(logfile);
                self.channel.1.send(comm::Response::Ack).unwrap();
                self.signal_completion();
            }
            comm::Command::Execute(dump) => {
                // always update the state before replying
                self.set_status(Status::BusyExecute);
                self.channel.1.send(comm::Response::Ack).unwrap();
                self.execute(dump);
                self.signal_completion();
            }
            comm::Command::WipeDMem => {
                // always update the state before replying
                self.set_status(Status::BusySecWipeDMem);
                self.channel.1.send(comm::Response::Ack).unwrap();
                self.wipe_memory(false);
                self.signal_completion();
            }
            comm::Command::WipeIMem => {
                // always update the state before replying
                self.set_status(Status::BusySecWipeIMem);
                self.channel.1.send(comm::Response::Ack).unwrap();
                self.wipe_memory(true);
                self.signal_completion();
            }
            comm::Command::Terminate => {
                self.set_status(Status::Locked);
                self.channel.1.send(comm::Response::Ack).unwrap();
                self.signal_completion();
            }
        }
    }

    /// Execute the uploaded OTBN program
    fn execute(&mut self, dump: bool) {
        let mut fatal;
        // "Each new execution of OTBN will reseed the URND PRNG."
        // Stall OTBN execution till entropy is injected
        self.syncurnd.sync_reseed();
        match self.do_execute(dump) {
            insn_exec::InstructionTrap::Exception(cause, _val) => {
                let mut error: u32;
                // let mut registers = self.registers.lock().unwrap();
                (fatal, error) = match cause {
                    ExceptionCause::ECallMMode => (false, ErrBits::empty().bits()),
                    ExceptionCause::EBadDataAddr => (false, ErrBits::BAD_DATA_ADDR.bits()),
                    ExceptionCause::EBadInsnAddr => (false, ErrBits::BAD_INSN_ADDR.bits()),
                    ExceptionCause::ECallStack => (false, ErrBits::CALL_STACK.bits()),
                    ExceptionCause::EIllegalInsn => (false, ErrBits::ILLEGAL_INSN.bits()),
                    ExceptionCause::ELoop => (false, ErrBits::LOOP.bits()),
                    ExceptionCause::ERndRepChkFail => (false, ErrBits::RND_REP_CHK_FAIL.bits()),
                    ExceptionCause::ERndFipsChkFail => (false, ErrBits::RND_FIPS_CHK_FAIL.bits()),
                    ExceptionCause::EFatal => {
                        (true, self.registers.fatal_bits.load(Ordering::Relaxed))
                    }
                };

                // ctrl: "Controls the reaction to software errors.
                // When set software errors produce fatal errors, rather than recoverable errors."
                if error != 0 && self.registers.ctrl.load(Ordering::Relaxed) {
                    fatal = true;
                    error |= ErrBits::FATAL_SOFTWARE.bits();
                }
                // update error bits
                self.registers.err_bits.fetch_or(error, Ordering::Relaxed);
            }
        }
        if fatal {
            let x: Arc<Mutex<dyn PRNG>> = self.syncurnd.urnd();
            self.dmem.try_lock().unwrap().wipe(&mut *x.lock().unwrap());
            self.imem.try_lock().unwrap().wipe(&mut *x.lock().unwrap());
            self.registers.insn_count.store(0, Ordering::Relaxed);
            self.set_status(Status::Locked);
        };
        // note: it is required that the OTBN client calls
        // Proxy::acknowledge_execution() to reset the OTBN status once the
        // operation on its side is over. Busy status is updated on that ack.
    }

    fn do_execute(&mut self, dump: bool) -> insn_exec::InstructionTrap {
        let mut executor = insn_exec::InstructionExecutor {
            hart_state: &mut self.hart_state,
            imem: &mut *self.imem.try_lock().unwrap(),
            dmem: &mut *self.dmem.try_lock().unwrap(),
        };

        executor.reset();

        self.registers
            .err_bits
            .store(ErrBits::empty().bits(), Ordering::Relaxed);
        self.registers.insn_count.store(0, Ordering::Relaxed);

        loop {
            // Debug/traces
            if let Some(log_file) = &mut self.log_file {
                // Output current instruction disassembly to log
                if let Some(insn_bits) = executor.imem.read_mem(executor.hart_state.pc) {
                    let mut outputter = insn_disasm::InstructionStringOutputter {
                        insn_pc: executor.hart_state.pc,
                    };
                    if let Some(inst) = insn_decode::decoder(&mut outputter, insn_bits) {
                        writeln!(
                            log_file,
                            "{:04x}: {:08x} {}",
                            executor.hart_state.pc, insn_bits, inst
                        )
                        .expect("Log file write failed");
                    } else {
                        let base = (insn_bits >> 2) & 0b11111;
                        let funct3 = (insn_bits >> 12) & 0b111;
                        writeln!(
                            log_file,
                            "Unable to decode instruction @ {:x}: {:x} [{:03b}..{:05b}]",
                            executor.hart_state.pc, insn_bits, funct3, base
                        )
                        .expect("Log file write failed");
                    }
                } else {
                    writeln!(log_file, "Could not read PC {:08x}", executor.hart_state.pc)
                        .expect("Log file write failed");
                }
            }

            let fatalbits = self.registers.fatal_bits.load(Ordering::Relaxed);
            let result = if fatalbits == 0 {
                // Execute instruction
                executor.step()
            } else {
                Err(insn_exec::InstructionTrap::Exception(
                    ExceptionCause::EFatal,
                    Some(fatalbits),
                ))
            };
            if let Err(trap) = result {
                if let Some(log_file) = &mut self.log_file {
                    let log_line = match trap {
                        insn_exec::InstructionTrap::Exception(cause, val) => {
                            if let Some(val) = val {
                                format!("[{:?} Exception, value:{:08x}]", cause, val)
                            } else {
                                format!("[{:?} Exception]", cause)
                            }
                        }
                    };

                    writeln!(log_file, "{} @ PC {:08x}", log_line, executor.hart_state.pc)
                        .expect("Log file write failed");
                }

                if dump {
                    Self::dump_hart_state(executor.hart_state, true);
                }

                // on a fatal error
                // on recoverable error
                // on operation completion
                self.registers
                    .status
                    .store(Status::BusySecWipeInt as u32, Ordering::Relaxed);

                // "The wiping procedure is a two-step process:
                //   Overwrite the state with randomness from URND and request a reseed of URND.
                //   Overwrite the state with randomness from reseeded URND."
                let prng: Arc<Mutex<dyn PRNG>> = self.syncurnd.urnd();
                executor.wipe_internal(&prng);
                self.syncurnd.sync_reseed();
                executor.wipe_internal(&prng);

                let insn_exec::InstructionTrap::Exception(cause, _) = trap;
                if cause == ExceptionCause::ECallMMode {
                    // if the exception has been triggered by an ecall instruction,
                    // the actual instruction has been executed; otherwise the instruction failed
                    // to execute
                    self.registers.insn_count.fetch_add(1, Ordering::Relaxed);
                } else {
                    executor.hart_state.pc = 0;
                }

                return trap;
            }

            self.registers.insn_count.fetch_add(1, Ordering::Relaxed);

            if let Some(log_file) = &mut self.log_file {
                Executer::log_changes(executor.hart_state, log_file);
            }
        }
    }

    fn wipe_memory(&mut self, doi: bool) {
        let memory = if doi { &mut self.imem } else { &mut self.dmem };

        memory
            .try_lock()
            .unwrap()
            .wipe(&mut *self.syncurnd.urnd().lock().unwrap());

        // simulate a "long lasting" op
        thread::sleep(Duration::from_micros(200));

        // note: it is expected that the OTBN client calls
        // Proxy::acknowledge_execution() to reset the OTBN status once the
        // operation on its side is over
    }

    fn signal_completion(&mut self) {
        if let Some(cb) = &mut self.on_complete {
            cb.signal();
        }
    }

    /// Set the log file to which execution traces should be dumped - if any
    fn log_changes(hart_state: &insn_exec::HartState, log_file: &mut Box<dyn Write>) {
        let state = &hart_state.updated;
        if let Some(reg_index) = state.gpr {
            writeln!(
                log_file,
                "\t\t\tx{} = 0x{:08x}",
                reg_index, hart_state.registers[reg_index]
            )
            .expect("Log file write failed");
        }
        if let Some(reg_index) = state.wgpr {
            writeln!(
                log_file,
                "\t\t\tw{} = 0x{:064x}",
                reg_index, hart_state.wregisters[reg_index]
            )
            .expect("Log file write failed");
        }
        if let Some((wide_reg, reg_addr)) = state.csr {
            // Output special register written by instruction to log if it wrote to one
            if !wide_reg {
                let csr = hart_state.csr_set.get_csr(reg_addr);
                match csr.unwrap().read() {
                    Ok(val) => writeln!(log_file,
                                        "\t\t\tcsr:{} = 0x{:08x}",
                                        csrs::CSRAddr::string_name(reg_addr),
                                        val),
                    Err(exc) => writeln!(log_file,
                                        "\t\t\tcsr:{} => {:?}",
                                        csrs::CSRAddr::string_name(reg_addr),
                                        exc),
                }
                .expect("Log file write failed");
            } else {
                let wsr = hart_state.csr_set.get_wsr(reg_addr);
                match wsr.unwrap().read() {
                    Ok(val) => writeln!(log_file,
                                        "\t\t\tcsr:{} = 0x{:064x}",
                                        csrs::WSRAddr::string_name(reg_addr),
                                        val),
                    Err(exc) => writeln!(log_file,
                                        "\t\t\tcsr:{} => {:?}",
                                        csrs::WSRAddr::string_name(reg_addr),
                                        exc),
                }
                .expect("Log file write failed");
            }
        }
        if let Some((depth, count)) = state.loophead {
            writeln!(log_file, "\t\t\tloop[{}] = {}", depth, count).expect("Log file write failed");
        }
    }

    /// Dump current content of OTBN register (RISC-V and Wide)
    /// compact: only dump register whose content differ than zero
    fn dump_hart_state(hart_state: &insn_exec::HartState, compact: bool) {
        println!();

        if !compact || !hart_state.hwstack.is_empty() {
            println!("Call Stack:");
            println!("-----------");
            for addr in hart_state.hwstack.iter() {
                println!("0x{:08x}", addr);
            }
            println!();
        }

        println!("Final Base Register Values:");
        println!("Reg | Value");
        println!("----------------");
        for (reg, val) in hart_state.registers[2..].iter().enumerate() {
            if compact && (*val == 0) {
                continue;
            }
            println!("x{:<2} | 0x{:08x}", reg + 2, val);
        }
        println!();

        println!("Final Bignum Register Values:");
        println!("Reg | Value");
        println!("-------------------------------------------------------------------------------");
        for (reg, val) in hart_state.wregisters.iter().enumerate() {
            if compact && (*val == U256::from(0u32)) {
                continue;
            }
            let mut w: Vec<u32> = Vec::new();
            let mut val256 = *val;
            for _ in (0..256).step_by(32) {
                w.push(val256.as_u32());
                val256 = val256.wrapping_shr(32);
            }
            println!(
                "w{:<2} | 0x{:08x}_{:08x}_{:08x}_{:08x}_{:08x}_{:08x}_{:08x}_{:08x}",
                reg, w[7], w[6], w[5], w[4], w[3], w[2], w[1], w[0]
            );
        }
    }
}
