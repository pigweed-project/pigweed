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

//! Low-level binary payload primitive argument decoders.

use super::error::TokenizerError;
use super::varint::{decode_varint, ArgStatus};

const TOKEN_SIZE_BYTES: usize = 4;
const FLOAT32_SIZE_BYTES: usize = 4;
const STRING_TRUNCATED_BIT: u8 = 0x80;
const STRING_LENGTH_MASK: u8 = 0x7f;

/// Extracts a 32-bit little-endian token and remaining argument payload from a byte buffer.
pub(crate) fn extract_token_and_args(data: &[u8]) -> Option<(u32, &[u8])> {
    if data.is_empty() {
        return None;
    }
    let mut token_bytes = [0u8; TOKEN_SIZE_BYTES];
    let len = core::cmp::min(data.len(), TOKEN_SIZE_BYTES);
    token_bytes[..len].copy_from_slice(&data[..len]);
    Some((u32::from_le_bytes(token_bytes), &data[len..]))
}

/// Decodes a signed 64-bit integer from a zigzag-encoded varint.
pub(crate) fn decode_signed_int(cursor: &mut &[u8]) -> Result<i64, TokenizerError> {
    match decode_varint(cursor) {
        (ArgStatus::Ok, val) => Ok(val),
        (ArgStatus::Missing, _) => Err(TokenizerError::Missing),
        _ => Err(TokenizerError::DecodeError),
    }
}

/// Decodes an unsigned 64-bit integer from a zigzag-encoded varint.
pub(crate) fn decode_unsigned_int(cursor: &mut &[u8]) -> Result<u64, TokenizerError> {
    decode_signed_int(cursor).map(i64::cast_unsigned)
}

/// Decodes a Unicode scalar character from a zigzag-encoded integer argument.
pub(crate) fn decode_char(cursor: &mut &[u8]) -> Result<char, TokenizerError> {
    let val = decode_signed_int(cursor)?;
    u32::try_from(val)
        .ok()
        .and_then(char::from_u32)
        .ok_or(TokenizerError::DecodeError)
}

/// Decodes a pointer value from a zigzag-encoded integer argument.
pub(crate) fn decode_pointer(cursor: &mut &[u8]) -> Result<usize, TokenizerError> {
    let val = decode_signed_int(cursor)?;
    // 32-bit targets sign-extend pointers >= 0x8000_0000 before zigzag encoding;
    // zero-extend 32-bit negative values while preserving 64-bit addresses.
    let addr_u64 = if (i64::from(i32::MIN)..0).contains(&val) {
        val.cast_unsigned() & u64::from(u32::MAX)
    } else {
        val.cast_unsigned()
    };
    usize::try_from(addr_u64).map_err(|_| TokenizerError::DecodeError)
}

/// Decodes a 32-bit IEEE-754 little-endian floating-point argument into `f64`.
pub(crate) fn decode_float(cursor: &mut &[u8]) -> Result<f64, TokenizerError> {
    if cursor.is_empty() {
        return Err(TokenizerError::Missing);
    }
    if cursor.len() < FLOAT32_SIZE_BYTES {
        *cursor = &[];
        return Err(TokenizerError::DecodeError);
    }
    let bytes: [u8; FLOAT32_SIZE_BYTES] = cursor[..FLOAT32_SIZE_BYTES].try_into().unwrap();
    *cursor = &cursor[FLOAT32_SIZE_BYTES..];
    Ok(f64::from(f32::from_le_bytes(bytes)))
}

/// Extracts the longest valid UTF-8 prefix from a byte slice.
fn valid_utf8_prefix(bytes: &[u8]) -> &str {
    match core::str::from_utf8(bytes) {
        Ok(valid) => valid,
        Err(err) => core::str::from_utf8(&bytes[..err.valid_up_to()]).unwrap_or(""),
    }
}

/// Decodes a length-prefixed string argument from `cursor`.
///
/// The first byte encodes length (`bits 0..=6`) and truncation flag (`bit 7`).
/// If the remaining payload is shorter than the declared length, any valid UTF-8
/// prefix present in the available bytes is preserved in `TokenizerError::DecodeErrorWithDetail`.
pub(crate) fn decode_string(cursor: &mut &[u8]) -> Result<String, TokenizerError> {
    if cursor.is_empty() {
        return Err(TokenizerError::Missing);
    }

    let header = cursor[0];
    let is_truncated = (header & STRING_TRUNCATED_BIT) != 0;
    let declared_len = usize::from(header & STRING_LENGTH_MASK);
    *cursor = &cursor[1..];

    if cursor.len() < declared_len {
        let available = *cursor;
        *cursor = &[];
        let partial_str = valid_utf8_prefix(available);
        return Err(TokenizerError::DecodeErrorWithDetail(
            partial_str.to_string(),
        ));
    }

    let str_bytes = &cursor[..declared_len];
    *cursor = &cursor[declared_len..];

    match core::str::from_utf8(str_bytes) {
        Ok(decoded_str) => {
            let mut formatted_str = decoded_str.to_string();
            if is_truncated {
                formatted_str.push_str("[...]");
            }
            Ok(formatted_str)
        }
        Err(_) => Err(TokenizerError::DecodeError),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_decode_string_truncated_mid_utf8_preserves_valid_prefix() {
        // Declared length is 6 bytes, but only 4 bytes follow: "hi " (3 bytes) + 0xE2
        // (first byte of a 3-byte UTF-8 sequence '€' [0xE2, 0x82, 0xAC]).
        let payload = [0x06, b'h', b'i', b' ', 0xe2];
        let mut cursor = &payload[..];
        let err = decode_string(&mut cursor).unwrap_err();
        assert_eq!(
            err,
            TokenizerError::DecodeErrorWithDetail("hi ".to_string())
        );
        assert!(cursor.is_empty());
    }

    #[test]
    fn test_decode_float_boundaries() {
        // Empty payload -> Missing
        let mut empty = &[][..];
        assert_eq!(decode_float(&mut empty), Err(TokenizerError::Missing));

        // Fewer than 4 bytes -> DecodeError and consumes remaining
        let mut short = &[0x01, 0x02][..];
        assert_eq!(decode_float(&mut short), Err(TokenizerError::DecodeError));
        assert!(short.is_empty());

        // Valid 4-byte float (1.5f32)
        let bytes = 1.5f32.to_le_bytes();
        let mut valid = &bytes[..];
        assert_eq!(decode_float(&mut valid), Ok(1.5));
        assert!(valid.is_empty());
    }
}
