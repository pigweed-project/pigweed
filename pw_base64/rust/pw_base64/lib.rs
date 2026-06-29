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
#![cfg_attr(not(feature = "std"), no_std)]
#![deny(missing_docs)]

//! `pw_base64` provides simple encoding of data into base64.
//!
//! ```
//! const INPUT: &'static [u8] = "I 💖 Pigweed".as_bytes();
//!
//! // [`encoded_size`] can be used to calculate the size of the output buffer.
//! let mut output = [0u8; pw_base64::encoded_size(INPUT.len())];
//!
//! // Data can be encoded to a `&mut [u8]`.
//! let output_size = pw_base64::encode(INPUT, &mut output).unwrap();
//! assert_eq!(&output[0..output_size], b"SSDwn5KWIFBpZ3dlZWQ=");
//!
//! // The output buffer can also be automatically converted to a `&str`.
//! let output_str = pw_base64::encode_str(INPUT, &mut output).unwrap();
//! assert_eq!(output_str, "SSDwn5KWIFBpZ3dlZWQ=");
//! ```

use pw_status::{Error, Result};
use pw_stream::{Cursor, ReadInteger, Seek, Write};

// Helper macro to make declaring the base 64 encode table more concise.
macro_rules! b {
    ($char:tt) => {
        stringify!($char).as_bytes()[0]
    };
}

// We use `u8`s in our encoding table instead of `char`s in order to avoid the
// overhead of 1) storing each entry as 4 bytes and 2) overhead of converting
// from `char` to `u8` while building the output.
//
// When constructing this table, the `b!` macro makes the assumption that
// all the characters are a single byte in utf8.  This is true as base64
// only outputs ASCII characters.
#[rustfmt::skip]
const BASE64_ENCODE_TABLE: [u8; 64] = [
    b!(A), b!(B), b!(C), b!(D), b!(E), b!(F), b!(G), b!(H),
    b!(I), b!(J), b!(K), b!(L), b!(M), b!(N), b!(O), b!(P),
    b!(Q), b!(R), b!(S), b!(T), b!(U), b!(V), b!(W), b!(X),
    b!(Y), b!(Z), b!(a), b!(b), b!(c), b!(d), b!(e), b!(f),
    b!(g), b!(h), b!(i), b!(j), b!(k), b!(l), b!(m), b!(n),
    b!(o), b!(p), b!(q), b!(r), b!(s), b!(t), b!(u), b!(v),
    b!(w), b!(x), b!(y), b!(z), b!(0), b!(1), b!(2), b!(3),
    b!(4), b!(5), b!(6), b!(7), b!(8), b!(9), b!(+), b!(/),
];
const BASE64_PADDING: u8 = b!(=);

const MIN_VALID_CHAR: u8 = b'+';
const MAX_VALID_CHAR: u8 = b'z';
const INVALID_CHAR: u8 = 0xff;
const IVLD: u8 = INVALID_CHAR;

#[rustfmt::skip]
const BASE64_DECODE_TABLE: [u8; 80] = [
    0x3e, IVLD, 0x3e, IVLD, 0x3f, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3a, 0x3b, 0x3c, 0x3d, IVLD, IVLD, IVLD, IVLD, IVLD,
    IVLD, IVLD, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11,
    0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, IVLD, IVLD,
    IVLD, IVLD, 0x3f, IVLD, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29,
    0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33,

];

const fn char_to_bits(c: u8) -> u8 {
    BASE64_DECODE_TABLE[(c - MIN_VALID_CHAR) as usize]
}

const fn bits_0_1(char0: u8, char1: u8) -> u8 {
    (char0 << 2) | ((char1 & 0b110000) >> 4)
}

const fn bits_1_2(char1: u8, char2: u8) -> u8 {
    ((char1 & 0b001111) << 4) | ((char2 & 0b111100) >> 2)
}

const fn bits_2_3(char2: u8, char3: u8) -> u8 {
    ((char2 & 0b000011) << 6) | char3
}

/// Returns the size of the output buffer needed to encode an input buffer of
/// size `input_size`.
#[must_use]
pub const fn encoded_size(input_size: usize) -> usize {
    input_size.div_ceil(3) * 4 // round up to a 3-byte group
}

/// Returns the maximum size of the decoded output for a given encoded size.
#[must_use]
pub const fn max_decoded_size(encoded_size: usize) -> usize {
    if encoded_size.is_multiple_of(4) {
        encoded_size / 4 * 3
    } else {
        0
    }
}

/// Returns the exact size of the decoded output for a valid Base64 string.
#[must_use]
pub fn decoded_size(encoded: &[u8]) -> usize {
    if !encoded.len().is_multiple_of(4) || encoded.is_empty() {
        return 0;
    }
    let max_bytes = max_decoded_size(encoded.len());
    let mut padding = 0;
    if encoded[encoded.len() - 2] == BASE64_PADDING {
        padding = 2;
    } else if encoded[encoded.len() - 1] == BASE64_PADDING {
        padding = 1;
    }
    max_bytes - padding
}

/// Returns true if the provided character is a valid Base64 character.
#[must_use]
pub fn is_valid_char(c: char) -> bool {
    if !c.is_ascii() {
        return false;
    }
    let val = c as u8;
    (MIN_VALID_CHAR..=MAX_VALID_CHAR).contains(&val) && char_to_bits(val) != INVALID_CHAR
}

/// Returns true if the provided data is valid Base64.
#[must_use]
pub fn is_valid(encoded: &[u8]) -> bool {
    if encoded.is_empty() {
        return true;
    }
    if !encoded.len().is_multiple_of(4) {
        return false;
    }
    // Check all characters except the last two.
    for &byte in encoded.iter().take(encoded.len() - 2) {
        if !is_valid_char(byte as char) {
            return false;
        }
    }
    // Check the last two characters.
    let penultimate = encoded[encoded.len() - 2];
    let last = encoded[encoded.len() - 1];

    if penultimate == BASE64_PADDING {
        last == BASE64_PADDING
    } else if is_valid_char(penultimate as char) {
        is_valid_char(last as char) || last == BASE64_PADDING
    } else {
        false
    }
}

/// Decodes the provided Base64 data into raw binary.
///
/// Returns the number of bytes written to `output` on success, or an error.
pub fn decode(encoded: &[u8], output: &mut [u8]) -> Result<usize> {
    if encoded.is_empty() {
        return Ok(0);
    }
    if !is_valid(encoded) {
        return Err(Error::InvalidArgument);
    }
    if output.len() < max_decoded_size(encoded.len()) {
        return Err(Error::OutOfRange);
    }

    let mut binary_len = 0;
    let mut ch = 0;
    while ch < encoded.len() - 4 {
        let char0 = char_to_bits(encoded[ch]);
        let char1 = char_to_bits(encoded[ch + 1]);
        let char2 = char_to_bits(encoded[ch + 2]);
        let char3 = char_to_bits(encoded[ch + 3]);

        output[binary_len] = bits_0_1(char0, char1);
        output[binary_len + 1] = bits_1_2(char1, char2);
        output[binary_len + 2] = bits_2_3(char2, char3);

        binary_len += 3;
        ch += 4;
    }

    // Decode the final group, which may include padding.
    let char0 = char_to_bits(encoded[ch]);
    let char1 = char_to_bits(encoded[ch + 1]);
    let char2 = char_to_bits(encoded[ch + 2]);
    let char3 = char_to_bits(encoded[ch + 3]);

    output[binary_len] = bits_0_1(char0, char1);
    binary_len += 1;

    if encoded[ch + 2] != BASE64_PADDING {
        output[binary_len] = bits_1_2(char1, char2);
        binary_len += 1;
        if encoded[ch + 3] != BASE64_PADDING {
            output[binary_len] = bits_2_3(char2, char3);
            binary_len += 1;
        }
    }

    Ok(binary_len)
}

// Base 64 encoding represents every 3 bytes with 4 ascii characters.  Each
// of these 4 ascii characters represents 6 bits of data from the 3 bytes of
// input.  The below helpers calculate each of the 4 characters form the 3 bytes
// of input.
const fn char_0(b: &[u8; 3]) -> u8 {
    BASE64_ENCODE_TABLE[((b[0] & 0b11111100) >> 2) as usize]
}

const fn char_1(b: &[u8; 3]) -> u8 {
    BASE64_ENCODE_TABLE[(((b[0] & 0b00000011) << 4) | ((b[1] & 0b11110000) >> 4)) as usize]
}

const fn char_2(b: &[u8; 3]) -> u8 {
    BASE64_ENCODE_TABLE[(((b[1] & 0b00001111) << 2) | ((b[2] & 0b11000000) >> 6)) as usize]
}

const fn char_3(b: &[u8; 3]) -> u8 {
    BASE64_ENCODE_TABLE[(b[2] & 0b00111111) as usize]
}

/// Encode `input` as base64 into the `output_buffer`.
///
/// Returns the number of bytes written to `output_buffer` on success or
/// `Error::OutOfRange` if `output_buffer` is not large enough.
pub fn encode(input: &[u8], output: &mut [u8]) -> Result<usize> {
    if output.len() < encoded_size(input.len()) {
        return Err(Error::OutOfRange);
    }
    let mut input = Cursor::new(input);
    let mut output = Cursor::new(output);

    let mut remaining_bytes = input.len();
    while remaining_bytes > 0 {
        let bytes = [
            input.read_u8_le().unwrap_or(0),
            input.read_u8_le().unwrap_or(0),
            input.read_u8_le().unwrap_or(0),
        ];

        output.write(&[
            char_0(&bytes),
            char_1(&bytes),
            if remaining_bytes > 1 {
                char_2(&bytes)
            } else {
                BASE64_PADDING
            },
            if remaining_bytes > 2 {
                char_3(&bytes)
            } else {
                BASE64_PADDING
            },
        ])?;
        remaining_bytes = remaining_bytes.saturating_add_signed(-3);
    }

    let len = output.stream_position()?;
    usize::try_from(len).map_err(|_| Error::OutOfRange)
}

/// Encode `input` as base64 into `output_buffer` and interprets it as a
/// string.
///
/// Returns a `&str` referencing the `output_buffer` buffer on success or
/// `Error::OutOfRange` if `output_buffer` is not large enough.
///
/// Using this method avoids having to do unicode checking as it can guarantee
/// that the data written to `output_buffer` is only valid ASCII bytes.
pub fn encode_str<'a>(input: &[u8], output_buffer: &'a mut [u8]) -> Result<&'a str> {
    let encode_len = encode(input, output_buffer)?;
    // Safety: Since we are building the output buffer strictly from ASCII
    // characters, it is guaranteed to be valid UTF-8.
    // encode_len has already been checked to be less than output_buffer
    // in the encode() call.
    unsafe {
        Ok(core::str::from_utf8_unchecked(
            output_buffer.get(0..encode_len).unwrap_unchecked(),
        ))
    }
}

#[cfg(test)]
mod tests;
