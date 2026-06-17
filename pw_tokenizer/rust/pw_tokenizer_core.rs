// Copyright 2023 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

//! # pw_tokenizer_core
//!
//! This crate provides the core functionality of calculating a token hash
//! for a string or byte sequence.  This is intended to provide a minimal core
//! for both the main `pw_tokenizer` and `pw_tokenizer_macro` crates.  Users
//! should prefer depending `pw_tokenizer` instead of this crate.
#![cfg_attr(not(feature = "std"), no_std)]

pub const HASH_CONSTANT: u32 = 65599u32;

/// Hasher is used to calculate a token's hash value.
///
/// The hash state is encapsulated in the `Hasher` to allow the hashing of
/// multi-part strings where the concatenated value can not be know at macro
/// expansion time.  All functions are `const`.
pub struct Hasher {
    coefficient: u32,
    hash: u32,
    bytes_hashed: usize,
    hash_len: usize,
}

impl Hasher {
    /// Create a new `Hasher`
    ///
    /// `data_len` is the total length of the data to be hashed.  `hash_len`
    /// is the number of bytes of data to be used in calculating the hash.
    /// `data_len` is used to seed  the hash while `hash_len` controls how many
    /// bytes are hashed.
    #[must_use]
    pub const fn new(data_len: usize, hash_len: usize) -> Self {
        {
            Self {
                coefficient: HASH_CONSTANT,
                hash: data_len as u32,
                bytes_hashed: 0,
                hash_len,
            }
        }
    }

    /// Processes `bytes` and updates hash state.
    ///
    /// Consumes `self` and returns a [`Hasher`] with the updated state.
    #[must_use]
    pub const fn process_bytes(mut self, bytes: &[u8]) -> Self {
        let bytes_left = self.hash_len - self.bytes_hashed;

        let bytes = if bytes.len() > bytes_left {
            bytes.split_at(bytes_left).0
        } else {
            bytes
        };

        // For loops are not allowed in const functions.
        let mut i = 0;
        while i < bytes.len() {
            // We call `u32::wrapping_*` instead of using `core::num::Wrapping` to
            // avoid using traits in a const function.
            self.hash = self
                .hash
                .wrapping_add(self.coefficient.wrapping_mul(bytes[i] as u32));
            self.coefficient = self.coefficient.wrapping_mul(HASH_CONSTANT);
            i += 1;
        }
        self.bytes_hashed += i;

        self
    }

    /// Consume `self` and return the hash.
    #[must_use]
    pub const fn hash(self) -> u32 {
        self.hash
    }
}
/// Calculate the hash for a sequence of bytes.
///
/// ```
/// use pw_tokenizer_core::hash_bytes;
///
/// let hash = hash_bytes(&[0x34, 0xd8, 0x3a, 0xbb, 0xf1, 0x0e, 0x07]);
/// assert_eq!(hash, 0x9e624642);
/// ```
#[must_use]
pub const fn hash_bytes(bytes: &[u8]) -> u32 {
    hash_bytes_fixed(bytes, bytes.len())
}

/// Calculate the hash for a sequence of bytes, examining at most `len` bytes.
///
/// ```
/// use pw_tokenizer_core::hash_bytes_fixed;
///
/// let hash = hash_bytes_fixed(&[0x34, 0xd8, 0x3a, 0xbb, 0xf1, 0x0e, 0x07], 4);
/// assert_eq!(hash, 0x92c5d2ac);
/// ```
#[must_use]
pub const fn hash_bytes_fixed(bytes: &[u8], len: usize) -> u32 {
    Hasher::new(bytes.len(), len).process_bytes(bytes).hash()
}

/// Calculate the hash for a string.
///
/// ```
/// use pw_tokenizer_core::hash_string;
///
/// let hash = hash_string("I 💖 Pigweed");
/// assert_eq!(hash, 0xe318d1b3);
/// ```
#[must_use]
pub const fn hash_string(s: &str) -> u32 {
    hash_bytes(s.as_bytes())
}

pub const TOKENIZER_ENTRY_MAGIC: u32 = 0xbaa98dee;

#[cfg(test)]
mod tests {
    use super::{hash_bytes_fixed, Hasher};

    struct TestCase {
        string: &'static [u8],
        hash_length: usize,
        hash: u32,
    }

    // Generated file defines `test_cases()`.
    include!("pw_tokenizer_core_test_cases.rs");

    #[test]
    fn hash_bytes_fixed_generates_correct_hash() {
        for test in test_cases() {
            let hash = hash_bytes_fixed(test.string, test.hash_length);
            assert_eq!(
                hash, test.hash,
                "{:08x} != {:08x} string: {:x?} hash_size: {}",
                hash, test.hash, test.string, test.hash_length
            );
        }
    }

    #[test]
    fn multi_part_data_generates_correct_hash() {
        for test in test_cases() {
            let mut hasher = Hasher::new(test.string.len(), test.hash_length);
            // Break each test string into 8 byte chunks and feed them to the
            // hasher separately.
            for chunk in test.string.chunks(8) {
                hasher = hasher.process_bytes(chunk);
            }
            let hash = hasher.hash();
            assert_eq!(
                hash, test.hash,
                "{:08x} != {:08x} string: {:x?} hash_size: {}",
                hash, test.hash, test.string, test.hash_length
            );
        }
    }
}
