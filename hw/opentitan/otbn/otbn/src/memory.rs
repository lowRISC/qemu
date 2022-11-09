// Copyright 2021 Gregory Chadwick <mail@gregchadwick.co.uk>
// Copyright 2022-2023 Rivos, Inc.
// Licensed under the Apache License Version 2.0, with LLVM Exceptions, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

use crate::PRNG;
use std::io::{self, ErrorKind, Read};

use super::Memory;

/// Read bytes from an [std::io::Read] into a [Memory] starting at the given address
pub fn read_to_memory(
    reader: &mut impl Read,
    memory: &mut impl Memory,
    size: usize,
) -> io::Result<()> {
    let mut addr: usize = 0;
    loop {
        let mut buffer = [0; 4];
        if let Err(error) = reader.read_exact(&mut buffer) {
            match error.kind() {
                ErrorKind::UnexpectedEof => break,
                _ => return Err(error),
            }
        }
        let word = u32::from_le_bytes(buffer);
        if !memory.write_mem(addr as u32, word) {
            return Err(io::Error::new(
                io::ErrorKind::Other,
                format!("Could not write byte at address 0x{:08x}", addr),
            ));
        }
        addr += 4;
        if addr >= size {
            break;
        }
    }

    Ok(())
}

/// [Vec] backed memory.
///
/// The [Vec] uses `u32` as the base type. Any read or write that falls out of the [Vec]s size will
/// result in a failed read or write.
pub struct VecMemory {
    pub mem: Vec<u32>,
}

impl VecMemory {
    pub fn new(size: usize) -> Self {
        Self {
            mem: vec![0u32; size / 4],
        }
    }

    fn random_wipe(&mut self, prng: &mut dyn PRNG) {
        for cell in self.mem.iter_mut() {
            *cell = prng.get_prng_u32();
        }
    }
}

impl Memory for VecMemory {
    fn read_mem(&mut self, addr: u32) -> Option<u32> {
        if (addr & 0x3) != 0 {
            panic!("Memory read must be aligned");
        }

        // Calculate vector index required data is contained in
        let word_addr = addr >> 2;

        // Read data from vector
        let read_data = self.mem.get(word_addr as usize).copied()?;

        // Apply mask and shift to extract required data from word
        Some(read_data)
    }

    fn write_mem(&mut self, addr: u32, store_data: u32) -> bool {
        // Calculate a mask and shift needed to update 32-bit word
        if (addr & 0x3) != 0 {
            panic!("Memory write must be aligned");
        }

        // Calculate vector index data to update is contained in
        let word_addr = (addr >> 2) as usize;

        self.mem[word_addr] = store_data;
        true
    }

    fn update_from_slice(&mut self, src: &[u32]) {
        if src.len() < self.mem.len() {
            let (head, _) = self.mem.split_at_mut(src.len());
            head.copy_from_slice(src);
        } else {
            self.mem.copy_from_slice(src);
        }
    }

    fn wipe(&mut self, prng: &mut dyn PRNG) {
        self.random_wipe(prng);
    }
}

pub struct MemoryRegion {
    memory: Box<dyn Memory>,
}

impl MemoryRegion {
    pub fn new(size: usize) -> Self {
        Self {
            memory: Box::new(VecMemory::new(size)),
        }
    }
}

impl Memory for MemoryRegion {
    fn read_mem(&mut self, addr: u32) -> Option<u32> {
        self.memory.read_mem(addr)
    }

    fn write_mem(&mut self, addr: u32, store_data: u32) -> bool {
        self.memory.write_mem(addr, store_data)
    }

    fn update_from_slice(&mut self, src: &[u32]) {
        self.memory.update_from_slice(src);
    }

    fn wipe(&mut self, prng: &mut dyn PRNG) {
        self.memory.wipe(prng)
    }
}
