// Copyright 2022-2023 Rivos, Inc.
// Licensed under the Apache License Version 2.0, with LLVM Exceptions, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

use std::fs::File;
use std::io;
use std::sync::mpsc;
use std::thread;

/// Commands the client may send to the executer
pub enum Command {
    SetTestMode(bool),
    LogTo(Box<io::BufWriter<File>>),
    Execute(bool),
    WipeDMem,
    WipeIMem,
    Terminate,
}

/// Replies the executer may send back to the client
pub enum Response {
    Active(thread::ThreadId),
    Ack,
    Error(String),
}

/// Channel to send commands from the client to the executer and to receive response back from it
pub type DownChannel = (mpsc::Sender<Command>, mpsc::Receiver<Response>);
/// Channel to receive commands from the client and to send it back responses
pub type UpChannel = (mpsc::Receiver<Command>, mpsc::Sender<Response>);

/// A callback trait for core signalling
pub trait Callback: Send {
    fn signal(&mut self);
}
