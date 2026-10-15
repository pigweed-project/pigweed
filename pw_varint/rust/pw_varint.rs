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

//! # pw_varint
//!
//! `pw_varint` provides support for encoding and decoding variable length
//! integers.  Small values require less memory than they do with fixed
//! encoding.  Signed integers are first zig-zag encoded to allow small
//! negative numbers to realize the memory savings.  For more information see
//! [Pigweed's pw_varint documentation](https://pigweed.dev/pw_varint).
//!
//! The encoding format used is compatible with
//! [Protocol Buffers](https://developers.google.com/protocol-buffers/docs/encoding#varints).
//!
//! Encoding and decoding is provided through the [VarintEncode] and
//! [VarintDecode] traits.
//!
//! # Example
//!
//! ```
//! use pw_varint::{VarintEncode, VarintDecode};
//!
//! let mut buffer = [0u8; 64];
//!
//! let encoded_len = (-1i64).varint_encode(&mut buffer).unwrap();
//!
//! let (decoded_len, val) = i64::varint_decode(&buffer ).unwrap();
//! ```

#![cfg_attr(not(feature = "std"), no_std)]

use pw_status::{Error, Result};

/// Maximum encoded size in bytes of a 64-bit LEB128 varint.
pub const MAX_VARINT_SIZE_BYTES: usize = 10;

/// A trait for objects that can be decoded from a varint.
///
/// `pw_varint` provides implementations for [`i8`], [`u8`], [`i16`], [`u16`],
/// [`i32`], [`u32`], [`i64`], and [`u64`].
pub trait VarintDecode: Sized {
    /// Decode a type from a varint encoded series of bytes.
    ///
    /// Signed values will be implicitly zig-zag decoded.
    ///
    /// # Errors
    ///
    /// Returns [`Error::OutOfRange`] if `data` does not contain a terminated
    /// varint or if the decoded value exceeds the range of `Self`.
    fn varint_decode(data: &[u8]) -> Result<(usize, Self)>;
}

/// A trait for objects that can be encoded into a varint.
///
/// `pw_varint` provides implementations for [`i8`], [`u8`], [`i16`], [`u16`],
/// [`i32`], [`u32`], [`i64`], and [`u64`].
pub trait VarintEncode: Sized {
    /// Encode a type into a varint encoded series of bytes.
    ///
    /// Signed values will be implicitly zig-zag encoded.
    ///
    /// # Errors
    ///
    /// Returns [`Error::OutOfRange`] if `data` is too small to hold the encoded
    /// varint.
    fn varint_encode(self, data: &mut [u8]) -> Result<usize>;
}

/// ZigZag encodes a signed integer. This maps small negative numbers to small,
/// unsigned positive numbers, which improves their density for LEB128 encoding.
///
/// ZigZag encoding works by moving the sign bit from the most-significant bit
/// to the least-significant bit. For the signed k-bit integer n, the formula is
///
///   `(n << 1) ^ (n >> (k - 1))`
///
/// See the following for a description of ZigZag encoding:
///   <https://protobuf.dev/programming-guides/encoding/#signed-ints>
#[inline]
#[must_use]
pub const fn zig_zag_encode(value: i64) -> u64 {
    (value.cast_unsigned() << 1) ^ (value >> (i64::BITS - 1)).cast_unsigned()
}

/// Decodes a zig-zag encoded 64-bit signed integer.
#[inline]
#[must_use]
pub const fn zig_zag_decode(value: u64) -> i64 {
    ((value >> 1) ^ (0u64.wrapping_sub(value & 1))).cast_signed()
}

macro_rules! unsigned_varint_impl {
    ($t:ty) => {
        impl VarintDecode for $t {
            #[inline]
            fn varint_decode(data: &[u8]) -> Result<(usize, Self)> {
                let (decoded_len, val) = decode_u64(data)?;
                let val = val.try_into().map_err(|_| Error::OutOfRange)?;
                Ok((decoded_len, val))
            }
        }

        impl VarintEncode for $t {
            #[inline]
            fn varint_encode(self, data: &mut [u8]) -> Result<usize> {
                encode_u64(data, u64::from(self))
            }
        }
    };
}

macro_rules! signed_varint_impl {
    ($t:ty) => {
        impl VarintDecode for $t {
            #[inline]
            fn varint_decode(data: &[u8]) -> Result<(usize, Self)> {
                let (decoded_len, val) = decode_u64(data)?;
                let val = zig_zag_decode(val)
                    .try_into()
                    .map_err(|_| Error::OutOfRange)?;
                Ok((decoded_len, val))
            }
        }

        impl VarintEncode for $t {
            #[inline]
            fn varint_encode(self, data: &mut [u8]) -> Result<usize> {
                encode_u64(data, zig_zag_encode(i64::from(self)))
            }
        }
    };
}

unsigned_varint_impl!(u8);
unsigned_varint_impl!(u16);
unsigned_varint_impl!(u32);
unsigned_varint_impl!(u64);

signed_varint_impl!(i8);
signed_varint_impl!(i16);
signed_varint_impl!(i32);
signed_varint_impl!(i64);

/// Bit mask for the MSB continuation flag on each varint byte.
const CONTINUATION_BIT: u8 = 0x80;

/// Bit mask for the 7-bit data payload on each varint byte.
const VALUE_MASK: u8 = 0x7f;

/// Number of data bits carried by each varint byte.
const BITS_PER_VARINT_BYTE: usize = 7;

/// Bit mask for the valid payload bit (bit 63 of `u64`) on the 10th byte.
const TENTH_BYTE_VALUE_MASK: u8 = 0x01;

/// Bit shift for placing the 10th varint byte's valid bit into bit 63.
const TENTH_BYTE_BIT_SHIFT: u32 = 63;

/// Decodes a 64-bit unsigned LEB128 varint with an inline fast path for 1- and
/// 2-byte values.
#[inline]
fn decode_u64(data: &[u8]) -> Result<(usize, u64)> {
    let Some((&b0, tail)) = data.split_first() else {
        return Err(Error::OutOfRange);
    };
    if (b0 & CONTINUATION_BIT) == 0 {
        return Ok((1, u64::from(b0)));
    }
    let Some(&b1) = tail.first() else {
        return Err(Error::OutOfRange);
    };
    if (b1 & CONTINUATION_BIT) == 0 {
        let val = u64::from(b0 & VALUE_MASK) | (u64::from(b1) << BITS_PER_VARINT_BYTE);
        return Ok((2, val));
    }
    decode_u64_slow(data)
}

/// Multi-byte fallback loop for [`decode_u64`], validating 10th-byte overflow
/// bits to prevent >64-bit shift overflow panics.
#[inline(never)]
fn decode_u64_slow(data: &[u8]) -> Result<(usize, u64)> {
    let mut value: u64 = 0;
    let (head, tail) = data.split_at(data.len().min(MAX_VARINT_SIZE_BYTES - 1));

    for (i, &byte) in head.iter().enumerate() {
        value |= u64::from(byte & VALUE_MASK) << (i * BITS_PER_VARINT_BYTE);

        if (byte & CONTINUATION_BIT) == 0 {
            return Ok((i + 1, value));
        }
    }

    let Some(&byte) = tail.first() else {
        return Err(Error::OutOfRange);
    };

    // 10th byte: only bit 0 is valid for a 64-bit value (9 * 7 = 63 bits
    // previously read). Any higher bit indicates overflow or a trailing
    // continuation byte.
    if (byte & !TENTH_BYTE_VALUE_MASK) != 0 {
        return Err(Error::OutOfRange);
    }

    Ok((
        MAX_VARINT_SIZE_BYTES,
        value | u64::from(byte) << TENTH_BYTE_BIT_SHIFT,
    ))
}

fn encode_u64(data: &mut [u8], mut value: u64) -> Result<usize> {
    for (i, d) in data.iter_mut().enumerate() {
        // `VALUE_MASK` (`0x7f`) intentionally selects only the low 7 bits.
        #[allow(clippy::cast_possible_truncation)]
        let byte: u8 = (value as u8) & VALUE_MASK;
        value >>= BITS_PER_VARINT_BYTE;
        if value == 0 {
            *d = byte;
            return Ok(i + 1);
        }
        *d = byte | CONTINUATION_BIT;
    }
    Err(Error::OutOfRange)
}

#[cfg(test)]
mod test {
    use super::*;

    fn success_cases_u8<T>() -> Vec<(Vec<u8>, T)>
    where
        T: From<u8>,
    {
        vec![
            // From varint_test.cc EncodeSizeUnsigned32_SmallSingleByte.
            (vec![0x00], 0x00.into()),
            (vec![0x01], 0x01.into()),
            (vec![0x02], 0x02.into()),
            // From varint_test.cc EncodeSizeUnsigned32_LargeSingleByte.
            (vec![0x3f], 0x3f.into()),
            (vec![0x40], 0x40.into()),
            (vec![0x7e], 0x7e.into()),
            (vec![0x7f], 0x7f.into()),
            // From varint_test.cc EncodeSizeUnsigned32_MultiByte.
            (vec![0x80, 0x01], 128.into()),
            (vec![0x81, 0x01], 129.into()),
            // From https://protobuf.dev/programming-guides/encoding/.
            (vec![0x96, 0x01], 150.into()),
        ]
    }

    fn success_cases_i8<T>() -> Vec<(Vec<u8>, T)>
    where
        T: From<i8>,
    {
        vec![
            // From varint_test.cc EncodeSizeSigned32_SmallSingleByte.
            (vec![0x00], 0i8.into()),
            (vec![0x01], (-1i8).into()),
            (vec![0x02], 1i8.into()),
            (vec![0x03], (-2i8).into()),
            (vec![0x04], 2i8.into()),
            // From varint_test.cc EncodeSizeSigned32_LargeSingleByte.
            (vec![125], (-63i8).into()),
            (vec![126], (63i8).into()),
            (vec![127], (-64i8).into()),
            // From varint_test.cc EncodeSizeSigned32_MultiByte.
            (vec![0x80, 0x1], 64i8.into()),
            (vec![0x81, 0x1], (-65i8).into()),
            (vec![0x82, 0x1], 65i8.into()),
        ]
    }

    fn success_cases_u32<T>() -> Vec<(Vec<u8>, T)>
    where
        T: From<u32>,
    {
        vec![
            // From varint_test.cc EncodeSizeUnsigned32_MultiByte.
            (vec![0xfe, 0xff, 0xff, 0xff, 0x0f], 0xffff_fffe.into()),
            (vec![0xff, 0xff, 0xff, 0xff, 0x0f], 0xffff_ffff.into()),
        ]
    }

    fn success_cases_i32<T>() -> Vec<(Vec<u8>, T)>
    where
        T: From<i32>,
    {
        vec![
            // From varint_test.cc EncodeSizeSigned32_MultiByte.
            (vec![0xff, 0xff, 0xff, 0xff, 0x0f], i32::MIN.into()),
            (vec![0xfe, 0xff, 0xff, 0xff, 0x0f], i32::MAX.into()),
        ]
    }

    #[test]
    fn decode_test_u8() {
        for case in success_cases_u8::<u8>() {
            assert_eq!(u8::varint_decode(&case.0), Ok((case.0.len(), case.1)));
        }

        // Truncated varint.
        assert_eq!(u8::varint_decode(&[0x96]), Err(Error::OutOfRange));
        // 256 exceeds u8::MAX (255).
        assert_eq!(u8::varint_decode(&[0x80, 0x02]), Err(Error::OutOfRange));
    }

    #[test]
    fn decode_test_i8() {
        for case in success_cases_i8::<i8>() {
            assert_eq!(i8::varint_decode(&case.0), Ok((case.0.len(), case.1)));
        }

        // Truncated varint.
        assert_eq!(i8::varint_decode(&[0x96]), Err(Error::OutOfRange));
        // 128 and -129 exceed i8::MIN..=i8::MAX (-128..=127).
        assert_eq!(i8::varint_decode(&[0x80, 0x02]), Err(Error::OutOfRange));
        assert_eq!(i8::varint_decode(&[0x81, 0x02]), Err(Error::OutOfRange));
    }

    #[test]
    fn decode_test_u16() {
        for case in success_cases_u8::<u16>() {
            assert_eq!(u16::varint_decode(&case.0), Ok((case.0.len(), case.1)));
        }

        assert_eq!(u16::varint_decode(&[0x96]), Err(Error::OutOfRange));
    }

    #[test]
    fn decode_test_i16() {
        for case in success_cases_i8::<i16>() {
            assert_eq!(i16::varint_decode(&case.0), Ok((case.0.len(), case.1)));
        }

        assert_eq!(i16::varint_decode(&[0x96]), Err(Error::OutOfRange));
    }

    #[test]
    fn decode_test_u32() {
        for case in success_cases_u8::<u32>()
            .into_iter()
            .chain(success_cases_u32::<u32>())
        {
            assert_eq!(u32::varint_decode(&case.0), Ok((case.0.len(), case.1)));
        }

        assert_eq!(u32::varint_decode(&[0x96]), Err(Error::OutOfRange));
    }

    #[test]
    fn decode_test_i32() {
        for case in success_cases_i8::<i32>()
            .into_iter()
            .chain(success_cases_i32::<i32>())
        {
            assert_eq!(i32::varint_decode(&case.0), Ok((case.0.len(), case.1)));
        }

        assert_eq!(i32::varint_decode(&[0x96]), Err(Error::OutOfRange));
    }

    #[test]
    fn decode_test_u64() {
        for case in success_cases_u8::<u64>()
            .into_iter()
            .chain(success_cases_u32::<u64>())
        {
            assert_eq!(u64::varint_decode(&case.0), Ok((case.0.len(), case.1)));
        }

        assert_eq!(u64::varint_decode(&[0x96]), Err(Error::OutOfRange));
    }

    #[test]
    fn decode_test_i64() {
        for case in success_cases_i8::<i64>()
            .into_iter()
            .chain(success_cases_i32::<i64>())
        {
            assert_eq!(i64::varint_decode(&case.0), Ok((case.0.len(), case.1)));
        }

        assert_eq!(i64::varint_decode(&[0x96]), Err(Error::OutOfRange));
    }

    #[test]
    fn decode_u64_empty_and_truncated() {
        assert_eq!(u64::varint_decode(&[]), Err(Error::OutOfRange));
        assert_eq!(u64::varint_decode(&[0x80]), Err(Error::OutOfRange));
        assert_eq!(u64::varint_decode(&[0x80, 0x80]), Err(Error::OutOfRange));
    }

    #[test]
    fn decode_u64_10th_byte_valid_and_overflow() {
        let mut valid_10th = [0x80u8; MAX_VARINT_SIZE_BYTES];
        valid_10th[MAX_VARINT_SIZE_BYTES - 1] = 0x01;
        assert_eq!(
            u64::varint_decode(&valid_10th),
            Ok((MAX_VARINT_SIZE_BYTES, 1u64 << 63))
        );

        let mut u64_max_bytes = [0xffu8; MAX_VARINT_SIZE_BYTES];
        u64_max_bytes[MAX_VARINT_SIZE_BYTES - 1] = 0x01;
        assert_eq!(
            u64::varint_decode(&u64_max_bytes),
            Ok((MAX_VARINT_SIZE_BYTES, u64::MAX))
        );

        // Two-byte fast-path upper bound and three-byte slow-path lower bound.
        assert_eq!(u64::varint_decode(&[0xff, 0x7f]), Ok((2, 0x3fff)));
        assert_eq!(u64::varint_decode(&[0x80, 0x80, 0x01]), Ok((3, 0x4000)));

        let mut overflow_bits = [0x80u8; MAX_VARINT_SIZE_BYTES];
        overflow_bits[MAX_VARINT_SIZE_BYTES - 1] = 0x02;
        assert_eq!(u64::varint_decode(&overflow_bits), Err(Error::OutOfRange));

        let mut continuation_set = [0x80u8; MAX_VARINT_SIZE_BYTES];
        continuation_set[MAX_VARINT_SIZE_BYTES - 1] = 0x81;
        assert_eq!(
            u64::varint_decode(&continuation_set),
            Err(Error::OutOfRange)
        );

        let overlong = [0x80u8; 12];
        assert_eq!(u64::varint_decode(&overlong), Err(Error::OutOfRange));
    }

    #[test]
    fn encode_test_u8() {
        for case in success_cases_u8::<u8>() {
            let mut buffer = [0u8; 64];
            let len = case.1.varint_encode(&mut buffer).unwrap();
            assert_eq!(len, case.0.len());
            assert_eq!(&buffer[0..len], case.0);
        }
    }

    #[test]
    fn encode_test_i8() {
        for case in success_cases_i8::<i8>() {
            let mut buffer = [0u8; 64];
            let len = case.1.varint_encode(&mut buffer).unwrap();
            assert_eq!(len, case.0.len());
            assert_eq!(&buffer[0..len], case.0);
        }
    }

    #[test]
    fn encode_test_u16() {
        for case in success_cases_u8::<u16>() {
            let mut buffer = [0u8; 64];
            let len = case.1.varint_encode(&mut buffer).unwrap();
            assert_eq!(len, case.0.len());
            assert_eq!(&buffer[0..len], case.0);
        }
    }

    #[test]
    fn encode_test_i16() {
        for case in success_cases_i8::<i16>() {
            let mut buffer = [0u8; 64];
            let len = case.1.varint_encode(&mut buffer).unwrap();
            assert_eq!(len, case.0.len());
            assert_eq!(&buffer[0..len], case.0);
        }
    }

    #[test]
    fn encode_test_u32() {
        for case in success_cases_u8::<u32>()
            .into_iter()
            .chain(success_cases_u32::<u32>())
        {
            let mut buffer = [0u8; 64];
            let len = case.1.varint_encode(&mut buffer).unwrap();
            assert_eq!(len, case.0.len());
            assert_eq!(&buffer[0..len], case.0);
        }
    }

    #[test]
    fn encode_test_i32() {
        for case in success_cases_i8::<i32>()
            .into_iter()
            .chain(success_cases_i32::<i32>())
        {
            let mut buffer = [0u8; 64];
            let len = case.1.varint_encode(&mut buffer).unwrap();
            assert_eq!(len, case.0.len());
            assert_eq!(&buffer[0..len], case.0);
        }
    }

    #[test]
    fn encode_test_u64() {
        for case in success_cases_u8::<u64>()
            .into_iter()
            .chain(success_cases_u32::<u64>())
        {
            let mut buffer = [0u8; 64];
            let len = case.1.varint_encode(&mut buffer).unwrap();
            assert_eq!(len, case.0.len());
            assert_eq!(&buffer[0..len], case.0);
        }
    }

    #[test]
    fn encode_test_i64() {
        for case in success_cases_i8::<i64>()
            .into_iter()
            .chain(success_cases_i32::<i64>())
        {
            let mut buffer = [0u8; 64];
            let len = case.1.varint_encode(&mut buffer).unwrap();
            assert_eq!(len, case.0.len());
            assert_eq!(&buffer[0..len], case.0);
        }
    }

    #[test]
    fn zig_zag_encode_64bit_boundaries() {
        assert_eq!(zig_zag_encode(0), 0);
        assert_eq!(zig_zag_encode(-1), 1);
        assert_eq!(zig_zag_encode(1), 2);
        assert_eq!(zig_zag_encode(i64::MAX), u64::MAX - 1);
        assert_eq!(zig_zag_encode(i64::MIN), u64::MAX);
    }

    #[test]
    fn zig_zag_decode_64bit_boundaries() {
        assert_eq!(zig_zag_decode(0), 0);
        assert_eq!(zig_zag_decode(1), -1);
        assert_eq!(zig_zag_decode(2), 1);
        assert_eq!(zig_zag_decode(u64::MAX - 1), i64::MAX);
        assert_eq!(zig_zag_decode(u64::MAX), i64::MIN);
    }
}
