// Copyright 2025 The Pigweed Authors
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

/// Converts a slice into syscall args for a SyscallBuffer in the kernel.
/// The kernel SyscallBuffer can handle either single buffers or a vector
/// of buffers.  We distinguish between them via the sign of the length.
/// - Positive lengths represent the size of a single buffer.
/// - Negative lengths represent the length of the vector of buffers.
pub trait AsSyscallBuffer {
    fn as_raw(&self) -> (*const u8, usize);
    fn as_raw_mut(&mut self) -> (*mut u8, usize);
    fn total_size(&self) -> usize;
}

// Converts a simple u8 slice.
impl AsSyscallBuffer for [u8] {
    fn as_raw(&self) -> (*const u8, usize) {
        (self.as_ptr(), self.len())
    }
    fn as_raw_mut(&mut self) -> (*mut u8, usize) {
        (self.as_mut_ptr(), self.len())
    }
    fn total_size(&self) -> usize {
        self.len()
    }
}

// Converts a simple u8 array.
impl<const N: usize> AsSyscallBuffer for [u8; N] {
    fn as_raw(&self) -> (*const u8, usize) {
        (self.as_ptr(), self.len())
    }
    fn as_raw_mut(&mut self) -> (*mut u8, usize) {
        (self.as_mut_ptr(), self.len())
    }
    fn total_size(&self) -> usize {
        self.len()
    }
}

// Converts a slice of u8 slices.
impl AsSyscallBuffer for [&[u8]] {
    fn as_raw(&self) -> (*const u8, usize) {
        (self.as_ptr().cast::<u8>(), self.len().wrapping_neg())
    }
    fn as_raw_mut(&mut self) -> (*mut u8, usize) {
        (self.as_mut_ptr().cast::<u8>(), self.len().wrapping_neg())
    }
    fn total_size(&self) -> usize {
        self.iter().fold(0, |total, item| total + item.len())
    }
}

impl AsSyscallBuffer for [&mut [u8]] {
    fn as_raw(&self) -> (*const u8, usize) {
        (self.as_ptr().cast::<u8>(), self.len().wrapping_neg())
    }
    fn as_raw_mut(&mut self) -> (*mut u8, usize) {
        (self.as_mut_ptr().cast::<u8>(), self.len().wrapping_neg())
    }
    fn total_size(&self) -> usize {
        self.iter().fold(0, |total, item| total + item.len())
    }
}

// Converts an array of u8 slices.
impl<const N: usize> AsSyscallBuffer for [&[u8]; N] {
    fn as_raw(&self) -> (*const u8, usize) {
        (self.as_ptr().cast::<u8>(), self.len().wrapping_neg())
    }
    fn as_raw_mut(&mut self) -> (*mut u8, usize) {
        (self.as_mut_ptr().cast::<u8>(), self.len().wrapping_neg())
    }
    fn total_size(&self) -> usize {
        self.iter().fold(0, |total, item| total + item.len())
    }
}

impl<const N: usize> AsSyscallBuffer for [&mut [u8]; N] {
    fn as_raw(&self) -> (*const u8, usize) {
        (self.as_ptr().cast::<u8>(), self.len().wrapping_neg())
    }
    fn as_raw_mut(&mut self) -> (*mut u8, usize) {
        (self.as_mut_ptr().cast::<u8>(), self.len().wrapping_neg())
    }
    fn total_size(&self) -> usize {
        self.iter().fold(0, |total, item| total + item.len())
    }
}
