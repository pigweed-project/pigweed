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

use core::num::Wrapping;

use pw_status::{Error, Result};

/// A trait for objects than can be decoded from a varint.
///
/// `pw_varint` provides implementations for [i16], [u16], [i32], [u32],
/// [i64], and [u64].
pub trait VarintDecode: Sized {
    /// Decode a type from a varint encoded series of bytes.
    ///
    /// Signed values will be implicitly zig-zag decoded.
    fn varint_decode(data: &[u8]) -> Result<(usize, Self)>;
}

/// A trait for objects than can be encoded into a varint.
///
/// `pw_varint` provides implementations for [i16], [u16], [i32], [u32],
/// [i64], and [u64].
pub trait VarintEncode: Sized {
    /// Encode a type into a varint encoded series of bytes.
    ///
    /// Signed values will be implicitly zig-zag encoded.
    fn varint_encode(self, data: &mut [u8]) -> Result<usize>;
}

macro_rules! unsigned_varint_impl {
    ($t:ty) => {
        impl VarintDecode for $t {
            fn varint_decode(data: &[u8]) -> Result<(usize, Self)> {
                let (data, val) = decode_u64(data)?;
                let val = val.try_into().map_err(|_| Error::OutOfRange)?;
                Ok((data, val))
            }
        }

        impl VarintEncode for $t {
            fn varint_encode(self, data: &mut [u8]) -> Result<usize> {
                encode_u64(data, u64::from(self))
            }
        }
    };
}

macro_rules! signed_varint_impl {
    ($t:ty) => {
        impl VarintDecode for $t {
            fn varint_decode(data: &[u8]) -> Result<(usize, Self)> {
                let (data, val) = decode_u64(data)?;
                let val = zig_zag_decode(val)
                    .try_into()
                    .map_err(|_| Error::OutOfRange)?;
                Ok((data, val))
            }
        }

        impl VarintEncode for $t {
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

fn decode_u64(data: &[u8]) -> Result<(usize, u64)> {
    let mut value: u64 = 0;
    for (i, d) in data.iter().enumerate() {
        value |= (u64::from(*d) & 0x7f) << (i * 7);

        if (*d & 0x80) == 0 {
            return Ok((i + 1, value));
        }
    }
    Err(Error::OutOfRange)
}

fn encode_u64(data: &mut [u8], value: u64) -> Result<usize> {
    let mut value = value;
    for (i, d) in data.iter_mut().enumerate() {
        let mut byte: u8 = (value & 0x7f) as u8;
        value >>= 7;
        if value > 0 {
            byte |= 0x80;
        }
        *d = byte;
        if value == 0 {
            return Ok(i + 1);
        }
    }
    Err(Error::OutOfRange)
}

// ZigZag encodes a signed integer. This maps small negative numbers to small,
// unsigned positive numbers, which improves their density for LEB128 encoding.
//
// ZigZag encoding works by moving the sign bit from the most-significant bit to
// the least-significant bit. For the signed k-bit integer n, the formula is
//
//   (n << 1) ^ (n >> (k - 1))
//
// See the following for a description of ZigZag encoding:
//   https://protobuf.dev/programming-guides/encoding/#signed-ints
fn zig_zag_encode(value: i64) -> u64 {
    // TODO: https://pwbug.dev/524779003 - Investigate lossy casts
    (value.cast_unsigned() << 1) ^ (value >> (i64::BITS - 1)).cast_unsigned()
}

fn zig_zag_decode(value: u64) -> i64 {
    let value = Wrapping(value);
    // TODO: https://pwbug.dev/524779003 - Investigate lossy casts
    ((value >> 1) ^ (!(value & Wrapping(1)) + Wrapping(1)))
        .0
        .cast_signed()
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
            assert_eq!(len, len);
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
}
