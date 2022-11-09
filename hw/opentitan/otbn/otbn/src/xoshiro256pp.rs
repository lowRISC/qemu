// Copyright 2018 Developers of the Rand project.
//
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use std::convert::TryInto;

use ethnum::{u256, U256};

/// A xoshiro256++ random number generator.
///
/// The xoshiro256++ algorithm is not suitable for cryptographic purposes, but
/// is very fast and has excellent statistical properties.
///
/// The algorithm used here is translated from [the `xoshiro256plusplus.c`
/// reference source code](http://xoshiro.di.unimi.it/xoshiro256plusplus.c) by
/// David Blackman and Sebastiano Vigna.

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Xoshiro256PlusPlus {
    s: [u64; 4],
}

/// Reads unsigned 64 bit integers from `src` into `dst`.
#[inline]
pub fn read_u64_into(src: &[u8], dst: &mut [u64]) {
    assert!(src.len() >= 8 * dst.len());
    for (out, chunk) in dst.iter_mut().zip(src.chunks_exact(8)) {
        *out = u64::from_le_bytes(chunk.try_into().unwrap());
    }
}

impl Default for Xoshiro256PlusPlus {
    /// Create a new `Xoshiro256PlusPlus`.
    fn default() -> Self {
        // the generator is *not* initialized here.
        Xoshiro256PlusPlus::from_seed([0u8; 32])
    }
}

impl Xoshiro256PlusPlus {
    #[inline]
    pub fn from_seed(seed: [u8; 32]) -> Xoshiro256PlusPlus {
        let mut state = [0; 4];
        read_u64_into(&seed, &mut state);
        Xoshiro256PlusPlus { s: state }
    }

    pub fn reseed(&mut self, seed: [u8; 32]) {
        let mut state = [0; 4];
        read_u64_into(&seed, &mut state);
        self.s = state;
    }

    #[inline]
    pub fn next_u32(&mut self) -> u32 {
        // The lowest bits have some linear dependencies, so we use the
        // upper bits instead.
        (self.next_u64() >> 32) as u32
    }

    #[inline]
    pub fn next_u64(&mut self) -> u64 {
        let result_plusplus = self.s[0]
            .wrapping_add(self.s[3])
            .rotate_left(23)
            .wrapping_add(self.s[0]);

        let t = self.s[1] << 17;

        self.s[2] ^= self.s[0];
        self.s[3] ^= self.s[1];
        self.s[1] ^= self.s[2];
        self.s[0] ^= self.s[3];

        self.s[2] ^= t;

        self.s[3] = self.s[3].rotate_left(45);

        result_plusplus
    }

    pub fn next_u256(&mut self) -> u256 {
        let mut val = U256::from(0u32);
        for _ in 0..4 {
            val = val.wrapping_shl(64);
            val |= U256::from(self.next_u64());
        }
        val
    }

    /// Implement `fill_bytes` via `next_u64` and `next_u32`, little-endian order.
    ///
    /// The fastest way to fill a slice is usually to work as long as possible with
    /// integers. That is why this method mostly uses `next_u64`, and only when
    /// there are 4 or less bytes remaining at the end of the slice it uses
    /// `next_u32` once.
    pub fn fill_bytes_via_next(&mut self, dest: &mut [u8]) {
        let mut left = dest;
        while left.len() >= 8 {
            let (l, r) = { left }.split_at_mut(8);
            left = r;
            let chunk: [u8; 8] = self.next_u64().to_le_bytes();
            l.copy_from_slice(&chunk);
        }
        let n = left.len();
        if n > 4 {
            let chunk: [u8; 8] = self.next_u64().to_le_bytes();
            left.copy_from_slice(&chunk[..n]);
        } else if n > 0 {
            let chunk: [u8; 4] = self.next_u32().to_le_bytes();
            left.copy_from_slice(&chunk[..n]);
        }
    }

    #[inline]
    pub fn fill_bytes(&mut self, dest: &mut [u8]) {
        self.fill_bytes_via_next(dest);
    }
}
