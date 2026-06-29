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

use pw_status::{Error, Result};
use pw_stream::{Cursor, Write};
use pw_varint::VarintEncode;

use crate::MessageWriter;

// The `Argument` enum is used to marshal arguments to pass to the tokenization
// engine.
pub enum Argument<'a> {
    String(&'a str),
    Varint(i64),
}

impl<'a> From<&'a str> for Argument<'a> {
    fn from(val: &'a str) -> Self {
        Self::String(val)
    }
}

impl From<char> for Argument<'_> {
    fn from(val: char) -> Self {
        Self::Varint(val as i64)
    }
}

impl From<u8> for Argument<'_> {
    fn from(val: u8) -> Self {
        Self::Varint(i64::from(val))
    }
}

impl From<i32> for Argument<'_> {
    fn from(val: i32) -> Self {
        Self::Varint(i64::from(val))
    }
}

impl From<u32> for Argument<'_> {
    fn from(val: u32) -> Self {
        Self::Varint(i64::from(val))
    }
}

// TODO: b/400978670 - investigate whether changing these
// 64bit values to references saves space on 32bit systems.
impl From<i64> for Argument<'_> {
    fn from(val: i64) -> Self {
        Self::Varint(val)
    }
}

impl From<u64> for Argument<'_> {
    // TODO: https://pwbug.dev/524779003 - Investigate lossy casts
    #[allow(clippy::cast_possible_wrap)]
    fn from(val: u64) -> Self {
        Self::Varint(val as i64)
    }
}

impl From<usize> for Argument<'_> {
    // TODO: https://pwbug.dev/524779003 - Investigate lossy casts
    #[allow(clippy::cast_possible_wrap)]
    fn from(val: usize) -> Self {
        Self::Varint(val as i64)
    }
}

// Wraps a `Cursor` so that `tokenize_to_buffer` and `tokenize_to_writer` can
// share implementations.  It is not meant to be used outside of
// `tokenize_to_buffer`.
struct CursorMessageWriter<'a> {
    cursor: Cursor<&'a mut [u8]>,
}

impl MessageWriter for CursorMessageWriter<'_> {
    fn write(&mut self, data: &[u8]) -> Result<()> {
        self.cursor.write_all(data)
    }

    fn remaining(&self) -> usize {
        self.cursor.remaining()
    }

    fn finalize(self) -> Result<()> {
        // Ensure `tokenize_to_buffer` never calls `finalize()`.
        Err(Error::Unimplemented)
    }
}

// Encode a string in Tokenizer format: length byte + data with the high bit of
// the length byte used to signal that the string was truncated.
pub fn encode_string<W: MessageWriter>(writer: &mut W, value: &str) -> Result<()> {
    const MAX_STRING_LENGTH: usize = 0x7f;

    let string_bytes = value.as_bytes();

    // Limit the encoding to the lesser of 127 or the available space in the buffer.
    let max_len = min(MAX_STRING_LENGTH, writer.remaining().saturating_sub(1));
    let overflow = max_len < string_bytes.len();
    let len = min(max_len, string_bytes.len());

    // First byte of an encoded string is it's length.
    // TODO: https://pwbug.dev/524779003 - Investigate lossy casts
    #[allow(clippy::cast_possible_truncation)]
    let mut header = len as u8;

    // The high bit of the first byte is used to indicate if the string was
    // truncated.
    if overflow {
        header |= 0x80;
    }
    writer.write(&[header])?;

    writer.write(&string_bytes[..len])
}

// Write out a tokenized message to an already created `MessageWriter`.
fn tokenize_engine<W: crate::MessageWriter>(
    writer: &mut W,
    token: u32,
    args: &[Argument<'_>],
) -> Result<()> {
    writer.write(&token.to_le_bytes()[..])?;
    for arg in args {
        match arg {
            Argument::String(s) => encode_string(writer, s)?,
            Argument::Varint(i) => {
                let mut encode_buffer = [0u8; 10];
                let len = i.varint_encode(&mut encode_buffer)?;
                let encoded_slice = encode_buffer.get(..len).ok_or(Error::OutOfRange)?;
                writer.write(encoded_slice)?;
            }
        }
    }

    Ok(())
}

#[inline(never)]
pub fn tokenize_to_buffer(buffer: &mut [u8], token: u32, args: &[Argument<'_>]) -> Result<usize> {
    let mut writer = CursorMessageWriter {
        cursor: Cursor::new(buffer),
    };
    tokenize_engine(&mut writer, token, args)?;
    Ok(writer.cursor.position())
}

#[inline(never)]
pub fn tokenize_to_buffer_no_args(buffer: &mut [u8], token: u32) -> Result<usize> {
    let token_bytes = &token.to_le_bytes()[..];
    let token_len = token_bytes.len();
    if buffer.len() < token_len {
        return Err(Error::OutOfRange);
    }
    buffer[..token_len].copy_from_slice(token_bytes);

    Ok(token_len)
}

#[inline(never)]
pub fn tokenize_to_default_writer<W: crate::MessageWriter + Default>(
    token: u32,
    args: &[Argument<'_>],
) -> Result<()> {
    let mut writer = W::default();
    match tokenize_engine(&mut writer, token, args) {
        // Still finalize the writer even if the buffer
        // is full so as to avoid loosing the entire
        // log message.
        Ok(_) | Err(Error::OutOfRange) => writer.finalize(),
        Err(error) => Err(error),
    }
}

#[inline(never)]
pub fn tokenize_to_default_writer_no_args<W: crate::MessageWriter + Default>(
    token: u32,
) -> Result<()> {
    let mut writer = W::default();
    let result = writer.write(&token.to_le_bytes()[..]);

    match result {
        // Still finalize the writer even if the buffer
        // is full so as to avoid loosing the entire
        // log message.
        Ok(_) | Err(Error::OutOfRange) => writer.finalize(),
        Err(error) => Err(error),
    }
}

#[cfg(test)]
mod test {
    use pw_stream::Seek;

    use super::*;

    fn do_string_encode_test<const BUFFER_LEN: usize>(value: &str, expected: &[u8]) {
        let mut buffer = [0u8; BUFFER_LEN];
        let mut writer = CursorMessageWriter {
            cursor: Cursor::new(&mut buffer),
        };
        encode_string(&mut writer, value).unwrap();

        let len = writer.cursor.stream_position().unwrap().try_into().unwrap();
        let buffer = writer.cursor.into_inner();

        assert_eq!(len, expected.len());
        assert_eq!(&buffer[..len], expected);
    }

    #[test]
    fn test_string_encode() {
        do_string_encode_test::<64>("test", b"\x04test");
        do_string_encode_test::<4>("test", b"\x83tes");
        do_string_encode_test::<1>("test", b"\x80");

        // Truncates when the string does not fit.
        do_string_encode_test::<64>(
            "testtesttesttesttesttesttesttesttesttesttesttesttesttesttesttest",
            b"\xbftesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttes",
        );

        // Truncates when string is over 127 bytes.
        do_string_encode_test::<1024>(
            "testtesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttest",
            b"\xfftesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttesttes",
        );
    }
}
