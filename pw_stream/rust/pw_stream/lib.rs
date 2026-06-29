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

//! `pw_stream` provides `no_std` versions of Rust's [`std::io::Read`],
//! [`std::io::Write`], and [`std::io::Seek`] traits as well as a simplified
//! version of [`std::io::Cursor`].  One notable difference is that
//! [`pw_status::Error`] is used to avoid needing to do error conversion or
//! encapsulation.
#![deny(missing_docs)]
// Allows docs to reference `std`
#![cfg_attr(feature = "no_std", no_std)]

use pw_status::{Error, Result};

#[doc(hidden)]
mod cursor;
mod integer;

pub use cursor::Cursor;
pub use integer::{ReadInteger, ReadVarint, WriteInteger, WriteVarint};

/// A trait for objects that provide streaming read capability.
pub trait Read {
    /// Read from a stream into a buffer.
    ///
    /// Semantics match [`std::io::Read::read()`].
    fn read(&mut self, buf: &mut [u8]) -> Result<usize>;

    /// Read exactly enough bytes to fill the buffer.
    ///
    /// Semantics match [`std::io::Read::read_exact()`].
    fn read_exact(&mut self, mut buf: &mut [u8]) -> Result<()> {
        while !buf.is_empty() {
            let len = self.read(buf)?;

            // End of stream
            if len == 0 {
                break;
            }

            buf = &mut buf[len..];
        }

        if !buf.is_empty() {
            Err(Error::OutOfRange)
        } else {
            Ok(())
        }
    }
}

/// A trait for objects that provide streaming write capability.
pub trait Write {
    /// Write a buffer to a stream.
    ///
    /// Semantics match [`std::io::Write::write()`].
    fn write(&mut self, buf: &[u8]) -> Result<usize>;

    /// Commit any outstanding buffered writes to underlying storage.
    ///
    /// Semantics match [`std::io::Write::flush()`].
    fn flush(&mut self) -> Result<()>;

    /// Writes entire buffer to stream.
    ///
    /// Semantics match [`std::io::Write::write_all()`].
    fn write_all(&mut self, mut buf: &[u8]) -> Result<()> {
        while !buf.is_empty() {
            let len = self.write(buf)?;

            // End of stream
            if len == 0 {
                break;
            }

            buf = buf.get(len..).ok_or(Error::OutOfRange)?;
        }

        if !buf.is_empty() {
            Err(Error::OutOfRange)
        } else {
            Ok(())
        }
    }
}

/// A description of a seek operation in a stream.
///
/// While `pw_stream` targets embedded platforms which are often natively
/// 32 bit, we believe that seek operation are relatively rare and the added
/// overhead of using 64 bit values for seeks is balanced by the ability
/// to support objects and operations over 4 GiB.
pub enum SeekFrom {
    /// Seek from the start of the stream.
    Start(u64),

    /// Seek from the end of the stream.
    End(i64),

    /// Seek from the current position of the stream.
    Current(i64),
}

/// A trait for objects that provide the ability to seek withing a stream.
pub trait Seek {
    /// Adjust the current position of the stream.
    ///
    /// Semantics match [`std::io::Seek::seek()`].
    fn seek(&mut self, pos: SeekFrom) -> Result<u64>;

    /// Set the current position of the stream to its beginning.
    ///
    /// Semantics match [`std::io::Seek::rewind()`].
    fn rewind(&mut self) -> Result<()> {
        self.seek(SeekFrom::Start(0)).map(|_| ())
    }

    /// Returns the length of the stream.
    ///
    /// Semantics match [`std::io::Seek::stream_len()`].
    fn stream_len(&mut self) -> Result<u64> {
        // Save original position.
        let orig_pos = self.seek(SeekFrom::Current(0))?;

        // Seed to the end to discover stream length.
        let end_pos = self.seek(SeekFrom::End(0))?;

        // Go back to original position.
        self.seek(SeekFrom::Start(orig_pos))?;

        Ok(end_pos)
    }

    /// Returns the current position of the stream.
    ///
    /// Semantics match [`std::io::Seek::stream_position()`].
    fn stream_position(&mut self) -> Result<u64> {
        self.seek(SeekFrom::Current(0))
    }
}

#[cfg(test)]
pub(crate) mod test_utils {
    use super::{Seek, SeekFrom};

    pub(crate) fn test_rewind_resets_position_to_zero<const LEN: u64, T: Seek>(mut seeker: T) {
        seeker
            .seek(SeekFrom::Current((LEN / 2).cast_signed()))
            .unwrap();
        assert_eq!(seeker.stream_position().unwrap(), LEN / 2);
        seeker.rewind().unwrap();
        assert_eq!(seeker.stream_position().unwrap(), 0);
    }

    pub(crate) fn test_stream_pos_reports_correct_position<const LEN: u64, T: Seek>(mut seeker: T) {
        assert_eq!(seeker.stream_position().unwrap(), 0);
        seeker.seek(SeekFrom::Current(1)).unwrap();
        assert_eq!(seeker.stream_position().unwrap(), 1);
        seeker
            .seek(SeekFrom::Current((LEN / 2).cast_signed() - 1))
            .unwrap();
        assert_eq!(seeker.stream_position().unwrap(), LEN / 2);
        seeker.seek(SeekFrom::Current(0)).unwrap();
        assert_eq!(seeker.stream_position().unwrap(), LEN / 2);
        seeker.seek(SeekFrom::End(0)).unwrap();
        assert_eq!(seeker.stream_position().unwrap(), LEN);
    }

    pub(crate) fn test_stream_len_reports_correct_length<const LEN: u64, T: Seek>(mut seeker: T) {
        assert_eq!(seeker.stream_len().unwrap(), LEN);
    }
}

#[cfg(test)]
mod tests {
    use core::cmp::min;

    use super::test_utils::*;
    use super::*;

    struct TestSeeker {
        len: u64,
        pos: u64,
    }

    impl Seek for TestSeeker {
        fn seek(&mut self, pos: SeekFrom) -> Result<u64> {
            let new_pos = match pos {
                SeekFrom::Start(pos) => pos,
                SeekFrom::Current(pos) => {
                    self.pos.checked_add_signed(pos).ok_or(Error::OutOfRange)?
                }
                SeekFrom::End(pos) => self.len.checked_add_signed(-pos).ok_or(Error::OutOfRange)?,
            };

            if new_pos > self.len {
                Err(Error::OutOfRange)
            } else {
                self.pos = new_pos;
                Ok(new_pos)
            }
        }
    }

    // A stream wrapper that limits reads and writes to a maximum chunk size.
    struct ChunkedStreamAdapter<S: Read + Write + Seek> {
        inner: S,
        chunk_size: usize,
        num_reads: u32,
        num_writes: u32,
    }

    impl<S: Read + Write + Seek> ChunkedStreamAdapter<S> {
        fn new(inner: S, chunk_size: usize) -> Self {
            Self {
                inner,
                chunk_size,
                num_reads: 0,
                num_writes: 0,
            }
        }
    }

    impl<S: Read + Write + Seek> Read for ChunkedStreamAdapter<S> {
        fn read(&mut self, buf: &mut [u8]) -> Result<usize> {
            let read_len = min(self.chunk_size, buf.len());
            self.num_reads += 1;
            self.inner.read(&mut buf[..read_len])
        }
    }

    impl<S: Read + Write + Seek> Write for ChunkedStreamAdapter<S> {
        fn write(&mut self, buf: &[u8]) -> Result<usize> {
            let write_len = min(self.chunk_size, buf.len());
            self.num_writes += 1;
            self.inner.write(&buf[..write_len])
        }

        fn flush(&mut self) -> Result<()> {
            self.inner.flush()
        }
    }

    struct ErrorStream {
        error: Error,
    }

    impl Read for ErrorStream {
        fn read(&mut self, _buf: &mut [u8]) -> Result<usize> {
            Err(self.error)
        }
    }

    impl Write for ErrorStream {
        fn write(&mut self, _buf: &[u8]) -> Result<usize> {
            Err(self.error)
        }

        fn flush(&mut self) -> Result<()> {
            Err(self.error)
        }
    }

    #[test]
    fn default_rewind_impl_resets_position_to_zero() {
        test_rewind_resets_position_to_zero::<64, _>(TestSeeker { len: 64, pos: 0 });
    }

    #[test]
    fn default_stream_pos_impl_reports_correct_position() {
        test_stream_pos_reports_correct_position::<64, _>(TestSeeker { len: 64, pos: 0 });
    }

    #[test]
    fn default_stream_len_impl_reports_correct_length() {
        test_stream_len_reports_correct_length::<64, _>(TestSeeker { len: 64, pos: 32 });
    }

    #[test]
    fn read_exact_reads_full_buffer_on_short_reads() {
        let cursor = Cursor::new((0x0..=0xff).collect::<Vec<u8>>());
        // Limit reads to 10 bytes per read.
        let mut wrapper = ChunkedStreamAdapter::new(cursor, 10);
        let mut read_buffer = vec![0u8; 256];

        wrapper.read_exact(&mut read_buffer).unwrap();

        // Ensure that the correct bytes were read.
        assert_eq!(wrapper.inner.into_inner(), read_buffer);

        // Verify that the read was broken up into the correct number of reads.
        assert_eq!(wrapper.num_reads, 26);
    }

    #[test]
    fn read_exact_returns_error_on_too_little_data() {
        let cursor = Cursor::new((0x0..=0x7f).collect::<Vec<u8>>());
        // Limit reads to 10 bytes per read.
        let mut wrapper = ChunkedStreamAdapter::new(cursor, 10);
        let mut read_buffer = vec![0u8; 256];

        assert_eq!(wrapper.read_exact(&mut read_buffer), Err(Error::OutOfRange));
    }

    #[test]
    fn read_exact_propagates_read_errors() {
        let mut error_stream = ErrorStream {
            error: Error::Internal,
        };
        let mut read_buffer = vec![0u8; 256];
        assert_eq!(
            error_stream.read_exact(&mut read_buffer),
            Err(Error::Internal)
        );
    }

    #[test]
    fn write_all_writes_full_buffer_on_short_writes() {
        let cursor = Cursor::new(vec![0u8; 256]);
        // Limit writes to 10 bytes per write.
        let mut wrapper = ChunkedStreamAdapter::new(cursor, 10);
        let write_buffer = (0x0..=0xff).collect::<Vec<u8>>();

        wrapper.write_all(&write_buffer).unwrap();

        // Ensure that the correct bytes were written.
        assert_eq!(wrapper.inner.into_inner(), write_buffer);

        // Verify that the write was broken up into the correct number of writes.
        assert_eq!(wrapper.num_writes, 26);
    }

    #[test]
    fn write_all_returns_error_on_too_little_data() {
        let cursor = Cursor::new(vec![0u8; 128]);
        // Limit writes to 10 bytes per write.
        let mut wrapper = ChunkedStreamAdapter::new(cursor, 10);
        let write_buffer = (0x0..=0xff).collect::<Vec<u8>>();

        assert_eq!(wrapper.write_all(&write_buffer), Err(Error::OutOfRange));
    }

    #[test]
    fn write_all_propagates_write_errors() {
        let mut error_stream = ErrorStream {
            error: Error::Internal,
        };
        let write_buffer = (0x0..=0xff).collect::<Vec<u8>>();
        assert_eq!(error_stream.write_all(&write_buffer), Err(Error::Internal));
    }
}
