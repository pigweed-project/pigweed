// Copyright 2026 The Pigweed Authors
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

//! # Line Reader Crate
//!
//! A pure, target-agnostic, and memory-safe console line accumulator designed
//! specifically for `no_std` embedded systems.
//!
//! ## Design & Borrow Safety
//!
//! The `LineReader` implements **Lazy Compaction** to parse complete, newline-terminated
//! command strings without dynamic memory allocations or copies.
//!
//! ## Example
//!
//! ```rust
//! use line_reader::LineReader;
//!
//! // Instantiate with default 128-byte buffer size
//! let mut reader = LineReader::<128>::new();
//!
//! // Feed a partial chunk of bytes
//! reader.handle_input(b"h");
//! assert_eq!(reader.next_line(), None);
//!
//! // Feed the rest of the command
//! reader.handle_input(b"elp\n");
//! assert_eq!(reader.next_line(), Some("help"));
//! assert_eq!(reader.next_line(), None);
//! ```

#![no_std]

/// A lazy-compacting line reader templated on its internal buffer size.
///
/// Accumulates raw incoming bytes inside a fixed-size buffer of size `SIZE`
/// and yields newline-delimited command strings cleanly.
pub struct LineReader<const SIZE: usize> {
    buffer: [u8; SIZE],
    start: usize,
    end: usize,
}

impl<const SIZE: usize> LineReader<SIZE> {
    /// Creates a new, empty `LineReader` instance with the configured buffer size.
    ///
    /// # Example
    ///
    /// ```rust
    /// use line_reader::LineReader;
    ///
    /// let reader = LineReader::<128>::new();
    /// ```
    pub const fn new() -> Self {
        Self {
            buffer: [0; SIZE],
            start: 0,
            end: 0,
        }
    }

    /// Feeds a raw chunk of bytes into the reader.
    ///
    /// # Arguments
    ///
    /// * `chunk` - A slice of raw bytes read from the input stream.
    ///
    /// # Example
    ///
    /// ```rust
    /// use line_reader::LineReader;
    /// let mut reader = LineReader::<128>::new();
    /// reader.handle_input(b"look\n");
    /// ```
    pub fn handle_input(&mut self, chunk: &[u8]) {
        // Lazy Compaction: Shift remaining unconsumed bytes to the front of the buffer.
        if self.start > 0 {
            self.buffer.copy_within(self.start..self.end, 0);
            self.end -= self.start;
            self.start = 0;
        }

        // Append the new chunk
        for &b in chunk {
            if self.end < self.buffer.len() {
                self.buffer[self.end] = b;
                self.end += 1;
            }
        }
    }

    /// Returns the next complete command line (trimmed of whitespace) if available,
    /// returning `None` otherwise.
    ///
    /// # Example
    ///
    /// ```rust
    /// use line_reader::LineReader;
    /// let mut reader = LineReader::<128>::new();
    /// reader.handle_input(b"help\n");
    /// assert_eq!(reader.next_line(), Some("help"));
    /// ```
    pub fn next_line(&mut self) -> Option<&str> {
        let search_slice = &self.buffer[self.start..self.end];
        let Some(pos) = search_slice.iter().position(|&b| b == b'\n' || b == b'\r') else {
            return None;
        };

        let line_end = self.start + pos;
        let line = core::str::from_utf8(&self.buffer[self.start..line_end]).ok()?;
        self.start = line_end + 1;
        Some(line.trim())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_line_reader_basic() {
        let mut reader = LineReader::<128>::new();

        // 1. Feed partial input
        reader.handle_input(b"h");
        assert_eq!(reader.next_line(), None);

        // 2. Complete the command
        reader.handle_input(b"elp\n");
        assert_eq!(reader.next_line(), Some("help"));
        assert_eq!(reader.next_line(), None);
    }

    #[test]
    fn test_line_reader_multiple_commands() {
        let mut reader = LineReader::<128>::new();

        reader.handle_input(b"look\ngo north\n");
        assert_eq!(reader.next_line(), Some("look"));
        assert_eq!(reader.next_line(), Some("go north"));
        assert_eq!(reader.next_line(), None);
    }

    #[test]
    fn test_line_reader_lazy_compaction() {
        let mut reader = LineReader::<128>::new();

        reader.handle_input(b"help\n");
        assert_eq!(reader.next_line(), Some("help"));

        // The buffer start index has advanced. Writing again triggers compaction!
        reader.handle_input(b"look\n");
        assert_eq!(reader.next_line(), Some("look"));
        assert_eq!(reader.next_line(), None);
    }

    #[test]
    fn test_line_reader_empty_and_whitespace() {
        let mut reader = LineReader::<128>::new();

        reader.handle_input(b"   help   \n\n");
        assert_eq!(reader.next_line(), Some("help"));
        assert_eq!(reader.next_line(), Some(""));
        assert_eq!(reader.next_line(), None);
    }

    #[test]
    fn test_line_reader_custom_sizes() {
        // Verifies that const generics are working and compile correctly!
        let mut small_reader = LineReader::<16>::new();
        small_reader.handle_input(b"help\n");
        assert_eq!(small_reader.next_line(), Some("help"));

        let mut large_reader = LineReader::<1024>::new();
        large_reader.handle_input(b"help\n");
        assert_eq!(large_reader.next_line(), Some("help"));
    }
}
