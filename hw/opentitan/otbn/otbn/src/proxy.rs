// Copyright 2022-2023 Rivos, Inc.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

use std::convert::TryFrom;
use std::ffi::CStr;
use std::fs::File;
use std::io;
use std::os::raw::{c_char, c_int, c_void};
use std::slice;
use std::sync::atomic::Ordering;
use std::sync::mpsc;
use std::sync::{Arc, Mutex};
use std::thread;

use super::comm;
use super::memory;
use super::otbn;
use super::random;
use super::Memory;

/// Run from the "main" thread
pub struct Proxy {
    /// Instruction memory
    imem: Arc<Mutex<memory::MemoryRegion>>,

    /// Data memory
    dmem: Arc<Mutex<memory::MemoryRegion>>,

    /// Shared registers with the core
    registers: Arc<otbn::Registers>,

    /// Thread handle of the core executer
    join_handle: Option<thread::JoinHandle<()>>,

    /// Communication channel with the core
    channel: Option<comm::DownChannel>,

    /// thread identifier of the core runner
    core_id: Option<thread::ThreadId>,

    /// Pesudo random generator, reseeded from EDN
    syncurnd: Arc<random::SyncUrnd>,

    /// True random generator from EDN
    rnd: Arc<random::Rnd>,

    /// Transient optional callback to invoke on completion
    on_complete: Option<Box<dyn comm::Callback>>,
}

impl Default for Proxy {
    fn default() -> Self {
        Self {
            imem: Arc::new(Mutex::new(memory::MemoryRegion::new(otbn::IMEM_SIZE))),
            dmem: Arc::new(Mutex::new(memory::MemoryRegion::new(otbn::DMEM_SIZE))),
            join_handle: None,
            channel: None,
            registers: Arc::new(otbn::Registers::default()),
            core_id: None,
            syncurnd: Arc::new(random::SyncUrnd::new()),
            rnd: Arc::new(random::Rnd::new()),
            on_complete: None,
        }
    }
}

impl Proxy {
    pub fn new() -> Self {
        Default::default()
    }

    /// Report the state of the core executer
    pub fn get_status(&mut self) -> otbn::Status {
        otbn::Status::from_u32(self.registers.status.load(Ordering::Relaxed))
    }

    /// Tell the core executer the operation completion has been acknowledged
    /// All registers are managed by the executer, however with QEMU sycnhronous
    /// implementation we need a way to defer execution completion to better
    /// simulate hardware behavior. When an operation is over, it is signalled
    /// through the C callback (see on_complete), which is then handled at C
    /// level through a QEMU timer that defer actual completion. The operational
    /// status of the OTBN core is not changed till this timer is exhausted,
    /// which gave back execution to the QEMU vCPU where the guest code can poll
    /// the status of the OTBN registers. Without this hack, the guest code
    /// would only see the OTBN idle since the executed command would likely
    /// complete before the vCPU has a chance to get scheduled.
    pub fn acknowledge_execution(&mut self) -> bool {
        if self.get_status() == otbn::Status::Locked {
            return false;
        }
        self.registers
            .status
            .store(otbn::Status::Idle as u32, Ordering::Relaxed);
        true
    }

    /// Register a callback for requesting entropy from EDN
    pub fn register_entropy_req_cb(&mut self, urnd_entropy_req: Box<dyn comm::Callback>,
                                   rnd_entropy_req: Box<dyn comm::Callback>) {
        self.syncurnd.register_entropy_req_cb(urnd_entropy_req);
        self.rnd.register_entropy_req_cb(rnd_entropy_req);
    }

    /// Register a callback for signalling a client on completion
    pub fn register_signal_cb(&mut self, on_complete: Box<dyn comm::Callback>) {
        self.on_complete = Some(on_complete);
    }

    /// Kick off the core executer, and create the communication channels
    pub fn start(&mut self, test_mode: bool, log_name: Option<&str>) {
        if self.core_id.is_some() {
            // already started, only reset
            // hartstate is not reset, as it is done after each execution
            match self.get_status() {
                otbn::Status::Idle | otbn::Status::Locked => {
                    let regs = &self.registers;
                    regs.status
                        .store(otbn::Status::Idle as u32, Ordering::Relaxed);
                    regs.err_bits.store(0, Ordering::Relaxed);
                    regs.fatal_bits.store(0, Ordering::Relaxed);
                    regs.ctrl.store(false, Ordering::Relaxed);
                    regs.insn_count.store(0, Ordering::Relaxed);
                }
                _ => (),
            }
            return;
        }

        let (cmdtx, cmdrx) = mpsc::channel::<comm::Command>();
        let (resptx, resprx) = mpsc::channel::<comm::Response>();
        self.channel = Some((cmdtx, resprx));
        if self.channel.is_some() {
            let registers = Arc::clone(&self.registers);
            let imem = Arc::clone(&self.imem);
            let dmem = Arc::clone(&self.dmem);
            let executer = thread::Builder::new().name("otbn_exec".into());
            let on_complete = self.on_complete.take();
            let urnd = self.syncurnd.clone();
            let rnd = Arc::clone(&self.rnd);
            let log_name: Option<String> = log_name.map(|l| l.into());
            self.join_handle = Some(
                executer
                    .spawn(move || {
                        otbn::Executer::run(
                            (cmdrx, resptx),
                            registers,
                            imem,
                            dmem,
                            urnd,
                            rnd,
                            on_complete,
                            log_name,
                        )
                    })
                    .unwrap(),
            );
            self.identify();
        }
        self.set_test_mode(test_mode);
    }

    /// Attach a log file to trace core execution
    pub fn log_to(&mut self, logfile: Box<io::BufWriter<File>>) {
        self.check_request();
        let channel = self.get_channel();
        channel.0.send(comm::Command::LogTo(logfile)).unwrap();
        match channel.1.recv().unwrap() {
            comm::Response::Ack => (),
            comm::Response::Error(e) => panic!("Error: {}", e),
            _ => panic!("Unexpected response"),
        }
    }

    /// Read one 32-bit from memory
    fn read_memory(&mut self, doi: bool, addr: u32) -> u32 {
        match self.get_status() {
            otbn::Status::Idle => (),
            otbn::Status::Locked => return 0, // Reads return zero in Locked state
            _ => {
                self.registers
                    .fatal_bits
                    .fetch_or(otbn::ErrBits::ILLEGAL_BUS_ACCESS.bits(), Ordering::Relaxed);
                return 0;
            }
        }

        let mem = if doi {
            &self.imem
        } else {
            if addr as usize >= otbn::DMEM_PUB_SIZE {
                return 0;
            }
            &self.dmem
        };

        if let Some(data) = mem.lock().unwrap().read_mem(addr) {
            data
        } else {
            0
        }
    }

    /// Write one 32-bit to memory
    fn write_memory(&mut self, doi: bool, addr: u32, data: u32) -> bool {
        match self.get_status() {
            otbn::Status::Idle => (),
            otbn::Status::Locked => return false, // Writes have no effect in Locked state
            _ => {
                self.registers
                    .fatal_bits
                    .fetch_or(otbn::ErrBits::ILLEGAL_BUS_ACCESS.bits(), Ordering::Relaxed);
                return false;
            }
        }

        let mem = if doi {
            &self.imem
        } else {
            if addr as usize >= otbn::DMEM_PUB_SIZE {
                return false;
            }
            &self.dmem
        };

        mem.lock().unwrap().write_mem(addr, data)
    }

    /// Push a 256-bit entropy buffer
    pub fn push_entropy(&mut self, rndix: usize, seed: &[u8], fips: bool) -> bool {
        if seed.len() != 32 {
            return false;
        }
        if let Ok(seed) = <&[u8; 32]>::try_from(seed) {
            match rndix {
                /* Pseudo URND */
                0 => {
                    self.syncurnd.fill(seed, fips);
                    /* URND has been reseeded, OTBN engine can now start */
                }
                /* Secure RND */
                1 => self.rnd.fill(seed, fips),
                _ => return false,
            }
            return true;
        }

        false
    }

    /// Execute the loaded code
    pub fn execute(&mut self, dump: bool) -> bool {
        self.check_request();
        if self.join_handle.is_some() {
            let channel = self.get_channel();
            channel.0.send(comm::Command::Execute(dump)).unwrap();
            match channel.1.recv().unwrap() {
                comm::Response::Ack => true,
                comm::Response::Error(_) => false,
                _ => panic!("Unexpected response"),
            }
        } else {
            false
        }
    }

    /// Wipe memory
    pub fn wipe_memory(&mut self, doi: bool) -> bool {
        self.check_request();
        if self.join_handle.is_some() {
            let channel = self.get_channel();
            let command = if doi {
                comm::Command::WipeIMem
            } else {
                comm::Command::WipeDMem
            };
            channel.0.send(command).unwrap();
            match channel.1.recv().unwrap() {
                comm::Response::Ack => true,
                comm::Response::Error(_) => false,
                _ => panic!("Unexpected response"),
            }
        } else {
            false
        }
    }

    /// Shutdown the core executer.
    /// Note: should only be called once, i.e. not for each execution session.
    pub fn terminate(&mut self) {
        self.check_request();
        let channel = self.get_channel();
        channel.0.send(comm::Command::Terminate).unwrap();
        match channel.1.recv().unwrap() {
            comm::Response::Ack => (),
            comm::Response::Error(e) => panic!("Error: {}", e),
            _ => panic!("Unexpected response"),
        }
        self.join();
    }

    /// Helper function to use the communication channel w/ the executer
    fn get_channel(&mut self) -> &mut comm::DownChannel {
        match &mut self.channel {
            Some(channel) => channel,
            // programming error
            _ => panic!("No communication channel"),
        }
    }

    /// Enable test mode (predefined RND values, ...)
    fn set_test_mode(&mut self, enable: bool) {
        let channel = self.get_channel();
        channel.0.send(comm::Command::SetTestMode(enable)).unwrap();
        match channel.1.recv().unwrap() {
            comm::Response::Ack => (),
            comm::Response::Error(e) => panic!("Error: {}", e),
            _ => panic!("Unexpected response"),
        }
    }

    /// Store identification of the runner thread
    fn identify(&mut self) {
        let channel = self.get_channel();
        // runner starts with pushing a message w/o prior solicitation from
        // the parent, i.e. no explicit command
        match channel.1.recv().unwrap() {
            comm::Response::Active(threadid) => self.core_id = Some(threadid),
            comm::Response::Ack => panic!("Unexpected ack"),
            comm::Response::Error(e) => panic!("Error: {}", e),
        }
    }

    fn check_request(&self) {
        if let Some(core_id) = self.core_id {
            if thread::current().id() == core_id {
                panic!("Command requested from runner thread");
            }
        }
    }

    /// Sync on executer thread completion
    fn join(&mut self) {
        self.join_handle.take().map(thread::JoinHandle::join);
    }
}

//--------------------------------------------------------------------------------------------------
// C API
//--------------------------------------------------------------------------------------------------

type CCallbackArg = *mut c_void;
type CCallbackFunc = unsafe extern "C" fn(arg: CCallbackArg);

/// Proxy callback for C implementation
pub struct CCallback {
    /// C callback
    cb: CCallbackFunc,
    /// optional argument to the C callback, default to C NULL
    arg: Arc<Mutex<CCallbackArg>>,
}

impl CCallback {
    pub fn new(cb: CCallbackFunc, arg: CCallbackArg) -> Self {
        Self {
            cb,
            arg: Arc::new(Mutex::new(arg)),
        }
    }
}

unsafe impl Send for CCallback {}

impl comm::Callback for CCallback {
    fn signal(&mut self) {
        unsafe {
            (self.cb)(*self.arg.lock().unwrap());
        }
    }
}

#[no_mangle]
pub extern "C" fn ot_otbn_proxy_new(
    urnd_entropy_req: CCallbackFunc, urnd_opaque: CCallbackArg,
    rnd_entropy_req: CCallbackFunc, rnd_opaque: CCallbackArg,
    on_complete: CCallbackFunc, on_comp_opaque: CCallbackArg,
) -> Box<Proxy> {
    let mut proxy = Box::new(Proxy::new());
    proxy.register_entropy_req_cb(Box::new(CCallback::new(urnd_entropy_req, urnd_opaque)),
                                  Box::new(CCallback::new(rnd_entropy_req, rnd_opaque)));
    proxy.register_signal_cb(Box::new(CCallback::new(on_complete, on_comp_opaque)));
    proxy
}

/// # Safety
#[no_mangle]
pub unsafe extern "C" fn ot_otbn_proxy_start(
    proxy: Option<&mut Proxy>,
    test_mode: bool,
    logname: *const c_char,
) {
    let log_name: Option<&str> = if !logname.is_null() {
        Some(CStr::from_ptr(logname).to_str().unwrap())
    } else {
        None
    };
    proxy.unwrap().start(test_mode, log_name);
}

#[no_mangle]
pub extern "C" fn ot_otbn_proxy_terminate(proxy: Option<&mut Proxy>) {
    proxy.unwrap().terminate();
}

/// # Safety
#[no_mangle]
pub unsafe extern "C" fn ot_otbn_proxy_push_entropy(
    proxy: Option<&mut Proxy>,
    rndix: u32,
    seed: *const u8,
    len: u32,
    fips: bool,
) -> c_int {
    assert!(!seed.is_null());
    let rust_seed = slice::from_raw_parts(seed, len as usize);
    if proxy.unwrap().push_entropy(rndix as usize, rust_seed, fips) {
        0
    } else {
        -1
    }
}

#[no_mangle]
pub extern "C" fn ot_otbn_proxy_execute(proxy: Option<&mut Proxy>, dump: bool) -> c_int {
    if proxy.unwrap().execute(dump) {
        0
    } else {
        -1
    }
}

#[no_mangle]
pub extern "C" fn ot_otbn_proxy_wipe_memory(proxy: Option<&mut Proxy>, doi: bool) -> c_int {
    if proxy.unwrap().wipe_memory(doi) {
        0
    } else {
        -1
    }
}

#[no_mangle]
pub extern "C" fn ot_otbn_proxy_acknowledge_execution(proxy: Option<&mut Proxy>) -> bool {
    proxy.unwrap().acknowledge_execution()
}

#[no_mangle]
pub extern "C" fn ot_otbn_proxy_read_memory(
    proxy: Option<&mut Proxy>,
    doi: bool,
    addr: u32,
) -> u32 {
    proxy.unwrap().read_memory(doi, addr)
}

#[no_mangle]
pub extern "C" fn ot_otbn_proxy_write_memory(
    proxy: Option<&mut Proxy>,
    doi: bool,
    addr: u32,
    val: u32,
) -> bool {
    proxy.unwrap().write_memory(doi, addr, val)
}

#[no_mangle]
pub extern "C" fn ot_otbn_proxy_get_status(proxy: Option<&mut Proxy>) -> c_int {
    proxy.unwrap().get_status() as c_int
}

#[no_mangle]
pub extern "C" fn ot_otbn_proxy_get_instruction_count(proxy: Option<&mut Proxy>) -> u32 {
    let proxy = proxy.unwrap();
    if proxy.get_status() != otbn::Status::Locked {
        proxy.registers.insn_count.load(Ordering::Relaxed) as u32
    } else {
        0
    }
}

#[no_mangle]
pub extern "C" fn ot_otbn_proxy_set_instruction_count(proxy: Option<&mut Proxy>, value: u32) {
    let proxy = proxy.unwrap();
    if proxy.get_status() == otbn::Status::Idle {
        proxy
            .registers
            .insn_count
            .store(value as usize, Ordering::Relaxed)
    }
}

#[no_mangle]
pub extern "C" fn ot_otbn_proxy_get_err_bits(proxy: Option<&mut Proxy>) -> u32 {
    proxy.unwrap().registers.err_bits.load(Ordering::Relaxed)
}

#[no_mangle]
pub extern "C" fn ot_otbn_proxy_set_err_bits(proxy: Option<&mut Proxy>, value: u32) {
    let proxy = proxy.unwrap();
    if proxy.get_status() == otbn::Status::Idle {
        proxy.registers.err_bits.store(
            otbn::ErrBits::from_bits_truncate(value).bits(),
            Ordering::Relaxed,
        )
    }
}

#[no_mangle]
pub extern "C" fn ot_otbn_proxy_get_ctrl(proxy: Option<&mut Proxy>) -> bool {
    proxy.unwrap().registers.ctrl.load(Ordering::Relaxed)
}

#[no_mangle]
pub extern "C" fn ot_otbn_proxy_set_ctrl(proxy: Option<&mut Proxy>, value: bool) {
    let proxy = proxy.unwrap();
    if proxy.get_status() == otbn::Status::Idle {
        proxy.registers.ctrl.store(value, Ordering::Relaxed)
    }
}
