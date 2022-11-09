// Copyright 2023 Rivos, Inc.
// Licensed under the Apache License Version 2.0, with LLVM Exceptions, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

use std::sync::{Arc, Condvar, Mutex};
use std::time::Duration;

use ethnum::{u256, U256};

use super::comm;
use super::xoshiro256pp::Xoshiro256PlusPlus;
use crate::{CSRNG, PRNG};

#[derive(Default)]
pub struct RndCache {
    value: u256,
    available: bool,
    fips: bool,
    repeat: bool
}

pub struct Rnd {
    cache: Mutex<RndCache>,
    wait: Condvar,
    entropy_req: Mutex<Option<Box<dyn comm::Callback>>>,
}

impl Default for Rnd {
    fn default() -> Self {
        let cache = RndCache::default();
        Self {
            cache: Mutex::new(cache),
            wait: Condvar::new(),
            entropy_req: Mutex::new(None),
        }
    }
}

impl Rnd {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn register_entropy_req_cb(&self, entropy_req: Box<dyn comm::Callback>) {
        let mut func = self.entropy_req.lock().unwrap();
        *func = Some(entropy_req);
    }

    pub fn clear(&mut self) {
        /* called from OTBN proxy */
        let mut cache = self.cache.lock().unwrap();
        cache.value = U256::from(0u32);
        cache.available = false;
        cache.fips = false;
        cache.repeat = false;
    }

    pub fn fill(&self, values: &[u8; 32], fips: bool) {
        /* called from OTBN proxy */
        let val = U256::from_le_bytes(*values);
        let mut cache = self.cache.lock().unwrap();
        cache.repeat = cache.value == val;
        cache.value = val;
        cache.available = true;
        cache.fips = fips;
        self.wait.notify_one();
    }

    pub fn prefetch(&self) {
        /* called from OTBN processor (CSR) */
        let cache = self.cache.lock().unwrap();
        if cache.available {
            /* cache is already loaded, nothing to do */
            return;
        }
        self.fetch();
    }

    fn fetch(&self) {
        let mut func = self.entropy_req.lock().unwrap();
        if let Some(req) = &mut *func {
            req.signal();
        }
    }
}

impl CSRNG for Rnd {
    fn get_csrng_u32(&self) -> (u32, bool, bool) {
        // "A read from the RND CSR returns the bottom 32b; the other 192b are discarded."
        let (val, fips, repeat) = self.get_csrng_u256();
        (val.as_u32(), fips, repeat)
    }

    fn get_csrng_u256(&self) -> (u256, bool, bool) {
        let mut cache = self.cache.lock().unwrap();
        let mut fetch = false;
        loop {
            let result = self
                .wait
                .wait_timeout(cache, Duration::from_millis(50))
                .unwrap();
            cache = result.0;
            if cache.available {
                break;
            }
            if !fetch {
                self.fetch();
                fetch = true;
            }
        }

        let (val, fips, repeat) = (cache.value, cache.fips, cache.repeat);
        cache.available = false;
        cache.fips = false;
        cache.repeat = false;
        (val, fips, repeat)
    }
}


#[derive(Default)]
pub struct Urnd {
    xoshiro: Xoshiro256PlusPlus,
}

impl PRNG for Urnd {
    fn get_prng_u32(&mut self) -> u32 {
        self.xoshiro.next_u32()
    }

    fn get_prng_u64(&mut self) -> u64 {
        self.xoshiro.next_u64()
    }

    fn get_prng_u256(&mut self) -> u256 {
        self.xoshiro.next_u256()
    }
}

impl Urnd {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn reseed(&mut self, seed: [u8; 32]) {
        self.xoshiro.reseed(seed)
    }
}


pub struct SyncUrnd {
    urnd: Arc<Mutex<Urnd>>,
    sync: Mutex<bool>,
    wait: Condvar,
    entropy_req: Mutex<Option<Box<dyn comm::Callback>>>,
}

impl Default for SyncUrnd {
    fn default() -> Self {
        Self {
            sync: Mutex::new(false),
            wait: Condvar::new(),
            urnd: Arc::new(Mutex::new(Urnd::default())),
            entropy_req: Mutex::new(None),
        }
    }
}

impl SyncUrnd {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn register_entropy_req_cb(&self, entropy_req: Box<dyn comm::Callback>) {
        let mut func = self.entropy_req.lock().unwrap();
        *func = Some(entropy_req);
    }

    pub fn urnd(&self) -> Arc<Mutex<Urnd>> {
        self.urnd.clone()
    }

    pub fn request_reseed(&self) {
        let mut func = self.entropy_req.lock().unwrap();
        if let Some(req) = &mut *func {
            req.signal();
        }
    }

    pub fn wait_reseed(&self) {
        loop {
            let sync = self.sync.lock().unwrap();
            let mut go = self
                .wait
                .wait_timeout(sync, Duration::from_millis(5))
                .unwrap();
            if *go.0 {
                *go.0 = false;
                break;
            }
        }
    }

    pub fn sync_reseed(&self) {
        self.request_reseed();
        self.wait_reseed();
    }

    pub fn fill(&self, seed: &[u8; 32], _fips: bool) {
        /* should fips be validated for URND? */
        self.urnd.lock().unwrap().reseed(*seed);
        *self.sync.lock().unwrap() = true;
        self.wait.notify_one();
    }
}

