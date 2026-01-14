// Copyright 2023 Rivos, Inc.
// Licensed under the Apache License Version 2.0, with LLVM Exceptions, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

use std::convert::TryInto;
use std::sync::Mutex;

use ethnum::{u256, U256};

#[derive(Default)]
pub struct KeyStore {
    pub lo: [u256; 2],
    pub hi: [u256; 2],
    pub valid: bool,
}

pub struct Key {
    pub store: Mutex<KeyStore>,
}

impl Default for Key {
    fn default() -> Self {
        let store = KeyStore::default();
        Self {
            store: Mutex::new(store),
        }
    }
}

impl Key {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn clear(&mut self) {
        /* called from OTBN proxy */
        let mut store = self.store.lock().unwrap();
        store.lo = [U256::from(0u32), U256::from(0u32)];
        store.hi = [U256::from(0u32), U256::from(0u32)];
        store.valid = false;
    }

    pub fn fill(&self, share0: &[u8; 48], share1: &[u8; 48], valid: bool) {
        /* called from OTBN proxy */
        let lo0 = U256::from_le_bytes(share0[0..32].try_into().unwrap());
        let lo1 = U256::from_words(0, u128::from_le_bytes(share0[32..48].try_into().unwrap()));

        let hi0 = U256::from_le_bytes(share1[0..32].try_into().unwrap());
        let hi1 = U256::from_words(0, u128::from_le_bytes(share1[32..48].try_into().unwrap()));

        let mut store = self.store.lock().unwrap();

        store.lo = [lo0, lo1];
        store.hi = [hi0, hi1];
        store.valid = valid;
    }
}
