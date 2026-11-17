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

//! Low-level varint decoding primitives and argument decoding status.

pub use pw_varint::zig_zag_decode as zigzag_decode;
use pw_varint::{VarintDecode, MAX_VARINT_SIZE_BYTES};

/// Status of decoding an individual argument.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ArgStatus {
    /// Decoded successfully.
    Ok,
    /// Argument was missing (input data ended).
    Missing,
    /// Corrupt data or decoding error.
    DecodeError,
}

/// Unsigned LEB128 varint decoder delegating to `pw_varint` and advancing `cursor`.
#[inline]
pub fn decode_unsigned_varint(cursor: &mut &[u8]) -> (ArgStatus, u64) {
    let input = *cursor;
    if input.is_empty() {
        return (ArgStatus::Missing, 0);
    }

    match u64::varint_decode(input) {
        Ok((len, val)) => {
            *cursor = &input[len..];
            (ArgStatus::Ok, val)
        }
        Err(_) => {
            *cursor = &input[input.len().min(MAX_VARINT_SIZE_BYTES)..];
            (ArgStatus::DecodeError, 0)
        }
    }
}

/// 64-bit signed varint (zig-zag LEB128) decoder.
///
/// Implemented directly in terms of `decode_unsigned_varint` and `zigzag_decode`.
#[inline]
pub fn decode_varint(cursor: &mut &[u8]) -> (ArgStatus, i64) {
    let (status, raw) = decode_unsigned_varint(cursor);
    if status != ArgStatus::Ok {
        return (status, 0);
    }
    (ArgStatus::Ok, zigzag_decode(raw))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_varint_empty_input() {
        let mut cursor = &[][..];
        let (status, val) = decode_unsigned_varint(&mut cursor);
        assert_eq!(status, ArgStatus::Missing);
        assert_eq!(val, 0);

        let mut cursor = &[][..];
        let (status, val) = decode_varint(&mut cursor);
        assert_eq!(status, ArgStatus::Missing);
        assert_eq!(val, 0);
    }

    #[test]
    fn test_varint_truncated_single_byte() {
        let mut cursor = &[0x80u8][..];
        let (status, val) = decode_unsigned_varint(&mut cursor);
        assert_eq!(status, ArgStatus::DecodeError);
        assert_eq!(val, 0);
        assert!(cursor.is_empty());
    }

    #[test]
    fn test_varint_truncated_multi_byte() {
        let mut cursor = &[0x80u8, 0x80u8][..];
        let (status, val) = decode_unsigned_varint(&mut cursor);
        assert_eq!(status, ArgStatus::DecodeError);
        assert_eq!(val, 0);
        assert!(cursor.is_empty());
    }

    #[test]
    fn test_varint_10th_byte_valid() {
        let mut data = vec![0x80; 9];
        data.push(0x01);
        let mut cursor = &data[..];
        let (status, _val) = decode_unsigned_varint(&mut cursor);
        assert_eq!(status, ArgStatus::Ok);
        assert!(cursor.is_empty());
    }

    #[test]
    fn test_varint_10th_byte_overflow_bits() {
        let mut data = vec![0x80; 9];
        data.push(0x02);
        let mut cursor = &data[..];
        let (status, _val) = decode_unsigned_varint(&mut cursor);
        assert_eq!(status, ArgStatus::DecodeError);
    }

    #[test]
    fn test_varint_10th_byte_continuation_set() {
        let mut data = vec![0x80; 9];
        data.push(0x81);
        let mut cursor = &data[..];
        let (status, _val) = decode_unsigned_varint(&mut cursor);
        assert_eq!(status, ArgStatus::DecodeError);
    }

    #[test]
    fn test_signed_zigzag_varint_boundaries() {
        // 0 -> 0x00
        let mut c = &[0x00u8][..];
        assert_eq!(decode_varint(&mut c), (ArgStatus::Ok, 0));

        // -1 -> zigzag 1 -> 0x01
        let mut c = &[0x01u8][..];
        assert_eq!(decode_varint(&mut c), (ArgStatus::Ok, -1));

        // 1 -> zigzag 2 -> 0x02
        let mut c = &[0x02u8][..];
        assert_eq!(decode_varint(&mut c), (ArgStatus::Ok, 1));
    }

    #[test]
    fn test_signed_zigzag_varint_truncated() {
        let mut cursor = &[0x80u8][..];
        let (status, val) = decode_varint(&mut cursor);
        assert_eq!(status, ArgStatus::DecodeError);
        assert_eq!(val, 0);
        assert!(cursor.is_empty());
    }
}
