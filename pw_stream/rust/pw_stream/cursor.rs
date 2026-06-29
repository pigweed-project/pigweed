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

use core::cmp::min;
use core::ptr;

use paste::paste;
use pw_status::{Error, Result};
use pw_varint::{VarintDecode, VarintEncode};

use super::{Read, Seek, SeekFrom, Write};

/// Wraps an <code>[AsRef]<[u8]></code> in a container implementing
/// [`Read`], [`Write`], and [`Seek`].
///
/// [`Write`] support requires the inner type also implement
/// <code>[AsMut]<[u8]></code>.
pub struct Cursor<T>
where
    T: AsRef<[u8]>,
{
    inner: T,
    pos: usize,
}

impl<T: AsRef<[u8]>> Cursor<T> {
    /// Create a new Cursor wrapping `inner` with an initial position of 0.
    ///
    /// Semantics match [`std::io::Cursor::new()`].
    pub fn new(inner: T) -> Self {
        Self { inner, pos: 0 }
    }

    /// Consumes the cursor and returns the inner wrapped data.
    pub fn into_inner(self) -> T {
        self.inner
    }

    /// Returns the number of remaining bytes in the Cursor.
    pub fn remaining(&self) -> usize {
        self.len().saturating_sub(self.pos)
    }

    /// Returns the total length of the Cursor.
    // Empty is ambiguous whether it should refer to len() or remaining() so
    // we don't provide it.
    #[allow(clippy::len_without_is_empty)]
    pub fn len(&self) -> usize {
        self.inner.as_ref().len()
    }

    /// Returns current IO position of the Cursor.
    pub fn position(&self) -> usize {
        self.pos
    }

    fn remaining_slice(&mut self) -> &[u8] {
        &self.inner.as_ref()[self.pos..]
    }
}

impl<T: AsRef<[u8]> + AsMut<[u8]>> Cursor<T> {
    fn remaining_mut(&mut self) -> &mut [u8] {
        &mut self.inner.as_mut()[self.pos..]
    }
}

// Implement `read()` as a concrete function to avoid extra monomorphization
// overhead.
fn read_impl(inner: &[u8], pos: &mut usize, buf: &mut [u8]) -> Result<usize> {
    let remaining = inner.len() - *pos;
    let read_len = min(remaining, buf.len());
    buf[..read_len].copy_from_slice(&inner[*pos..(*pos + read_len)]);
    *pos += read_len;
    Ok(read_len)
}

impl<T: AsRef<[u8]>> Read for Cursor<T> {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize> {
        read_impl(self.inner.as_ref(), &mut self.pos, buf)
    }
}

// Implement `write()` as a concrete function to avoid extra monomorphization
// overhead.
fn write_impl(inner: &mut [u8], pos: &mut usize, buf: &[u8]) -> Result<usize> {
    let remaining = inner.len().checked_sub(*pos).ok_or(Error::OutOfRange)?;
    let write_len = min(remaining, buf.len());
    // Safety: write_len has been bounds checked on buf and inner.
    // There can't be any overlap as inner is a &mut and buf is a &.
    unsafe {
        let src_ptr = buf.as_ptr();
        let dst_ptr = inner.as_mut_ptr().add(*pos);
        ptr::copy_nonoverlapping(src_ptr, dst_ptr, write_len);
    }
    // This will never saturate as pos is a private field which will
    // never be larger than inner.len().
    *pos = pos.saturating_add(write_len);
    Ok(write_len)
}

impl<T: AsRef<[u8]> + AsMut<[u8]>> Write for Cursor<T> {
    fn write(&mut self, buf: &[u8]) -> Result<usize> {
        write_impl(self.inner.as_mut(), &mut self.pos, buf)
    }

    fn flush(&mut self) -> Result<()> {
        // Cursor does not provide any buffering so flush() is a noop.
        Ok(())
    }
}

impl<T: AsRef<[u8]>> Seek for Cursor<T> {
    fn seek(&mut self, pos: SeekFrom) -> Result<u64> {
        let new_pos = match pos {
            SeekFrom::Start(pos) => pos,
            SeekFrom::Current(pos) => (self.pos as u64)
                .checked_add_signed(pos)
                .ok_or(Error::OutOfRange)?,
            SeekFrom::End(pos) => (self.len() as u64)
                .checked_add_signed(-pos)
                .ok_or(Error::OutOfRange)?,
        };

        // Since Cursor operates on in memory buffers, it's limited by usize.
        // Return an error if we are asked to seek beyond that limit.
        let new_pos: usize = new_pos.try_into().map_err(|_| Error::OutOfRange)?;

        if new_pos > self.len() {
            Err(Error::OutOfRange)
        } else {
            self.pos = new_pos;
            Ok(new_pos as u64)
        }
    }

    // Implement more efficient versions of rewind, stream_len, stream_position.
    fn rewind(&mut self) -> Result<()> {
        self.pos = 0;
        Ok(())
    }

    fn stream_len(&mut self) -> Result<u64> {
        Ok(self.len() as u64)
    }

    fn stream_position(&mut self) -> Result<u64> {
        Ok(self.pos as u64)
    }
}

macro_rules! cursor_read_type_impl {
    ($ty:ident, $endian:ident) => {
        paste! {
          fn [<read_ $ty _ $endian>](&mut self) -> Result<$ty> {
            const NUM_BYTES: usize = $ty::BITS as usize / 8;
            if NUM_BYTES > self.remaining() {
                return Err(Error::OutOfRange);
            }
            let sub_slice = self
                .inner
                .as_ref()
                .get(self.pos..self.pos + NUM_BYTES)
                .ok_or_else(|| Error::InvalidArgument)?;
            // Because we are code size conscious we want an infallible way to
            // turn `sub_slice` into a fixed sized array as opposed to using
            // something like `.try_into()?`.
            //
            // Safety:  We are both bounds checking and size constraining the
            // slice in the above lines of code.
            let sub_array: &[u8; NUM_BYTES] = unsafe { &*sub_slice.as_ptr().cast::<[u8; NUM_BYTES]>() };
            let value = $ty::[<from_ $endian _bytes>](*sub_array);

            self.pos += NUM_BYTES;
            Ok(value)
          }
        }
    };
}

macro_rules! cursor_read_bits_impl {
    ($bits:literal) => {
        paste! {
          cursor_read_type_impl!([<i $bits>], le);
          cursor_read_type_impl!([<u $bits>], le);
          cursor_read_type_impl!([<i $bits>], be);
          cursor_read_type_impl!([<u $bits>], be);
        }
    };
}

macro_rules! cursor_write_type_impl {
    ($ty:ident, $endian:ident) => {
        paste! {
          fn [<write_ $ty _ $endian>](&mut self, value: &$ty) -> Result<()> {
            const NUM_BYTES: usize = $ty::BITS as usize / 8;
            if NUM_BYTES > self.remaining() {
                return Err(Error::OutOfRange);
            }
            let value_bytes = $ty::[<to_ $endian _bytes>](*value);
            let sub_slice = self
                .inner
                .as_mut()
                .get_mut(self.pos..self.pos + NUM_BYTES)
                .ok_or_else(|| Error::InvalidArgument)?;

            sub_slice.copy_from_slice(&value_bytes[..]);

            self.pos += NUM_BYTES;
            Ok(())
          }
        }
    };
}

macro_rules! cursor_write_bits_impl {
    ($bits:literal) => {
        paste! {
          cursor_write_type_impl!([<i $bits>], le);
          cursor_write_type_impl!([<u $bits>], le);
          cursor_write_type_impl!([<i $bits>], be);
          cursor_write_type_impl!([<u $bits>], be);
        }
    };
}

impl<T: AsRef<[u8]>> crate::ReadInteger for Cursor<T> {
    cursor_read_bits_impl!(8);
    cursor_read_bits_impl!(16);
    cursor_read_bits_impl!(32);
    cursor_read_bits_impl!(64);
    cursor_read_bits_impl!(128);
}

impl<T: AsRef<[u8]> + AsMut<[u8]>> crate::WriteInteger for Cursor<T> {
    cursor_write_bits_impl!(8);
    cursor_write_bits_impl!(16);
    cursor_write_bits_impl!(32);
    cursor_write_bits_impl!(64);
    cursor_write_bits_impl!(128);
}

impl<T: AsRef<[u8]>> crate::ReadVarint for Cursor<T> {
    fn read_varint(&mut self) -> Result<u64> {
        let (len, value) = u64::varint_decode(self.remaining_slice())?;
        self.pos += len;
        Ok(value)
    }

    fn read_signed_varint(&mut self) -> Result<i64> {
        let (len, value) = i64::varint_decode(self.remaining_slice())?;
        self.pos += len;
        Ok(value)
    }
}

impl<T: AsRef<[u8]> + AsMut<[u8]>> crate::WriteVarint for Cursor<T> {
    fn write_varint(&mut self, value: u64) -> Result<()> {
        let encoded_len = value.varint_encode(self.remaining_mut())?;
        self.pos += encoded_len;
        Ok(())
    }

    fn write_signed_varint(&mut self, value: i64) -> Result<()> {
        let encoded_len = value.varint_encode(self.remaining_mut())?;
        self.pos += encoded_len;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_utils::*;
    use crate::{ReadInteger, ReadVarint, WriteInteger, WriteVarint};

    #[test]
    fn cursor_len_returns_total_bytes() {
        let cursor = Cursor {
            inner: &[0u8; 64],
            pos: 31,
        };
        assert_eq!(cursor.len(), 64);
    }

    #[test]
    fn cursor_remaining_returns_remaining_bytes() {
        let cursor = Cursor {
            inner: &[0u8; 64],
            pos: 31,
        };
        assert_eq!(cursor.remaining(), 33);
    }

    #[test]
    fn cursor_position_returns_current_position() {
        let cursor = Cursor {
            inner: &[0u8; 64],
            pos: 31,
        };
        assert_eq!(cursor.position(), 31);
    }

    #[test]
    fn cursor_read_of_partial_buffer_reads_correct_data() {
        let mut cursor = Cursor {
            inner: &[1, 2, 3, 4, 5, 6, 7, 8],
            pos: 4,
        };
        let mut buf = [0u8; 8];
        assert_eq!(cursor.read(&mut buf), Ok(4));
        assert_eq!(buf, [5, 6, 7, 8, 0, 0, 0, 0]);
    }

    #[test]
    fn cursor_write_of_partial_buffer_writes_correct_data() {
        let mut cursor = Cursor {
            inner: &mut [0, 0, 0, 0, 0, 0, 0, 0],
            pos: 4,
        };
        let buf = [1, 2, 3, 4, 5, 6, 7, 8];
        assert_eq!(cursor.write(&buf), Ok(4));
        assert_eq!(cursor.inner, &[0, 0, 0, 0, 1, 2, 3, 4]);
    }

    #[test]
    fn cursor_rewind_resets_position_to_zero() {
        test_rewind_resets_position_to_zero::<64, _>(Cursor::new(&[0u8; 64]));
    }

    #[test]
    fn cursor_stream_pos_reports_correct_position() {
        test_stream_pos_reports_correct_position::<64, _>(Cursor::new(&[0u8; 64]));
    }

    #[test]
    fn cursor_stream_len_reports_correct_length() {
        test_stream_len_reports_correct_length::<64, _>(Cursor::new(&[0u8; 64]));
    }

    macro_rules! cursor_read_n_bit_integers_unpacks_data_correctly {
        ($bits:literal) => {
            paste! {
              #[test]
              fn [<cursor_read_ $bits _bit_integers_unpacks_data_correctly>]() {
                  let (bytes, values) = [<integer_ $bits _bit_test_cases>]();
                  let mut cursor = Cursor::new(&bytes);

                  assert_eq!(cursor.[<read_i $bits _le>](), Ok(values.0));
                  assert_eq!(cursor.[<read_u $bits _le>](), Ok(values.1));
                  assert_eq!(cursor.[<read_i $bits _be>](), Ok(values.2));
                  assert_eq!(cursor.[<read_u $bits _be>](), Ok(values.3));
              }
            }
        };
    }

    macro_rules! cursor_write_n_bit_integers_packs_data_correctly {
        ($bits:literal) => {
            paste! {
              #[test]
              fn [<cursor_write_ $bits _bit_integers_packs_data_correctly>]() {
                  let (expected_bytes, values) = [<integer_ $bits _bit_test_cases>]();
                  let mut cursor = Cursor::new(vec![0u8; expected_bytes.len()]);
                  cursor.[<write_i $bits _le>](&values.0).unwrap();
                  cursor.[<write_u $bits _le>](&values.1).unwrap();
                  cursor.[<write_i $bits _be>](&values.2).unwrap();
                  cursor.[<write_u $bits _be>](&values.3).unwrap();

                  let result_bytes: Vec<u8> = cursor.into_inner().into();

                  assert_eq!(result_bytes, expected_bytes);
              }
            }
        };
    }

    fn integer_8_bit_test_cases() -> (Vec<u8>, (i8, u8, i8, u8)) {
        (
            vec![
                0x0, // le i8
                0x1, // le u8
                0x2, // be i8
                0x3, // be u8
            ],
            (0, 1, 2, 3),
        )
    }

    cursor_read_n_bit_integers_unpacks_data_correctly!(8);
    cursor_write_n_bit_integers_packs_data_correctly!(8);

    fn integer_16_bit_test_cases() -> (Vec<u8>, (i16, u16, i16, u16)) {
        (
            vec![
                0x0, 0x80, // le i16
                0x1, 0x80, // le u16
                0x80, 0x2, // be i16
                0x80, 0x3, // be u16
            ],
            (
                i16::from_le_bytes([0x0, 0x80]),
                0x8001,
                i16::from_be_bytes([0x80, 0x2]),
                0x8003,
            ),
        )
    }

    cursor_read_n_bit_integers_unpacks_data_correctly!(16);
    cursor_write_n_bit_integers_packs_data_correctly!(16);

    fn integer_32_bit_test_cases() -> (Vec<u8>, (i32, u32, i32, u32)) {
        (
            vec![
                0x0, 0x1, 0x2, 0x80, // le i32
                0x3, 0x4, 0x5, 0x80, // le u32
                0x80, 0x6, 0x7, 0x8, // be i32
                0x80, 0x9, 0xa, 0xb, // be u32
            ],
            (
                i32::from_le_bytes([0x0, 0x1, 0x2, 0x80]),
                0x8005_0403,
                i32::from_be_bytes([0x80, 0x6, 0x7, 0x8]),
                0x8009_0a0b,
            ),
        )
    }

    cursor_read_n_bit_integers_unpacks_data_correctly!(32);
    cursor_write_n_bit_integers_packs_data_correctly!(32);

    fn integer_64_bit_test_cases() -> (Vec<u8>, (i64, u64, i64, u64)) {
        (
            vec![
                0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x80, // le i64
                0x7, 0x8, 0x9, 0xa, 0xb, 0xc, 0xd, 0x80, // le u64
                0x80, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, // be i64
                0x80, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, // be u64
            ],
            (
                i64::from_le_bytes([0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x80]),
                0x800d_0c0b_0a09_0807,
                i64::from_be_bytes([0x80, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16]),
                0x8017_1819_1a1b_1c1d,
            ),
        )
    }

    cursor_read_n_bit_integers_unpacks_data_correctly!(64);
    cursor_write_n_bit_integers_packs_data_correctly!(64);

    fn integer_128_bit_test_cases() -> (Vec<u8>, (i128, u128, i128, u128)) {
        #[rustfmt::skip]
        let val = (
            vec![
                // le i128
                0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x8f,
                // le u128
                0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x8f,
                // be i128
                0x80, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
                0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
                // be u128
                0x80, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
                0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
            ],
            (
                i128::from_le_bytes([
                    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x8f,
                ]),
                0x8f1e_1d1c_1b1a_1918_1716_1514_1312_1110,
                i128::from_be_bytes([
                    0x80, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
                    0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
                ]),
                0x8031_3233_3435_3637_3839_3a3b_3c3d_3e3f,
            ),
        );
        val
    }

    cursor_read_n_bit_integers_unpacks_data_correctly!(128);
    cursor_write_n_bit_integers_packs_data_correctly!(128);

    #[test]
    pub fn read_varint_unpacks_data_correctly() {
        let mut cursor = Cursor::new(vec![0xfe, 0xff, 0xff, 0xff, 0x0f, 0x0, 0x0, 0x0]);
        let value = cursor.read_varint().unwrap();
        assert_eq!(value, 0xffff_fffe);

        let mut cursor = Cursor::new(vec![0xff, 0xff, 0xff, 0xff, 0x0f, 0x0, 0x0, 0x0]);
        let value = cursor.read_varint().unwrap();
        assert_eq!(value, 0xffff_ffff);
    }

    #[test]
    pub fn read_signed_varint_unpacks_data_correctly() {
        let mut cursor = Cursor::new(vec![0xfe, 0xff, 0xff, 0xff, 0x0f, 0x0, 0x0, 0x0]);
        let value = cursor.read_signed_varint().unwrap();
        assert_eq!(value, i32::MAX.into());

        let mut cursor = Cursor::new(vec![0xff, 0xff, 0xff, 0xff, 0x0f, 0x0, 0x0, 0x0]);
        let value = cursor.read_signed_varint().unwrap();
        assert_eq!(value, i32::MIN.into());
    }

    #[test]
    pub fn write_varint_packs_data_correctly() {
        let mut cursor = Cursor::new(vec![0u8; 8]);
        cursor.write_varint(0xffff_fffe).unwrap();
        let buf = cursor.into_inner();
        assert_eq!(buf, vec![0xfe, 0xff, 0xff, 0xff, 0x0f, 0x0, 0x0, 0x0]);

        let mut cursor = Cursor::new(vec![0u8; 8]);
        cursor.write_varint(0xffff_ffff).unwrap();
        let buf = cursor.into_inner();
        assert_eq!(buf, vec![0xff, 0xff, 0xff, 0xff, 0x0f, 0x0, 0x0, 0x0]);
    }

    #[test]
    pub fn write_signed_varint_packs_data_correctly() {
        let mut cursor = Cursor::new(vec![0u8; 8]);
        cursor.write_signed_varint(i32::MAX.into()).unwrap();
        let buf = cursor.into_inner();
        assert_eq!(buf, vec![0xfe, 0xff, 0xff, 0xff, 0x0f, 0x0, 0x0, 0x0]);

        let mut cursor = Cursor::new(vec![0u8; 8]);
        cursor.write_signed_varint(i32::MIN.into()).unwrap();
        let buf = cursor.into_inner();
        assert_eq!(buf, vec![0xff, 0xff, 0xff, 0xff, 0x0f, 0x0, 0x0, 0x0]);
    }
}
