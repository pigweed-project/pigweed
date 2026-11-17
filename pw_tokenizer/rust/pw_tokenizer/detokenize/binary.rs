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

//! Binary token database (`TOKENS\0\0`) parser.

use pw_status::{Error, Result};

use crate::detokenize::database::Database;

const UNSET_DAY: u8 = 0xff;
const UNSET_MONTH: u8 = 0xff;
const UNSET_YEAR: u16 = 0xffff;
const MAX_DISPLAY_YEAR: u16 = 9999;

const BINARY_MAGIC: &[u8; 8] = b"TOKENS\0\0";
const BINARY_HEADER_META_SIZE: usize = 8;
const BINARY_ENTRY_SIZE: usize = 8;

fn is_valid_date_fields(year: u16, month: u8, day: u8) -> bool {
    if year > MAX_DISPLAY_YEAR || !(1..=12).contains(&month) || !(1..=31).contains(&day) {
        return false;
    }
    let is_leap = (year.is_multiple_of(4) && !year.is_multiple_of(100)) || year.is_multiple_of(400);
    let max_days = match month {
        2 if is_leap => 29,
        2 => 28,
        4 | 6 | 9 | 11 => 30,
        _ => 31,
    };
    day <= max_days
}

#[allow(clippy::cast_possible_truncation)] // year <= 9999 guarantees each decimal digit fits in u8.
fn date_removed_string(year: u16, month: u8, day: u8) -> String {
    if (day == UNSET_DAY && month == UNSET_MONTH && year == UNSET_YEAR)
        || !is_valid_date_fields(year, month, day)
    {
        return String::new();
    }

    let buf = [
        b'0' + (year / 1000) as u8,
        b'0' + ((year / 100) % 10) as u8,
        b'0' + ((year / 10) % 10) as u8,
        b'0' + (year % 10) as u8,
        b'-',
        b'0' + (month / 10),
        b'0' + (month % 10),
        b'-',
        b'0' + (day / 10),
        b'0' + (day % 10),
    ];
    core::str::from_utf8(&buf).unwrap_or("").to_string()
}

/// Parses a binary token database into a [`Database`].
///
/// # Errors
///
/// Returns [`Error::InvalidArgument`] if the header magic is invalid, the entry
/// table is truncated, or any entry lacks a valid UTF-8 NUL terminator.
pub fn parse_binary_database(input: &[u8]) -> Result<Database> {
    parse_binary_database_internal(input, None)
}

/// Parses a binary token database into a [`Database`] and pre-parses all format
/// strings with `prefix`.
///
/// # Errors
///
/// Returns [`Error::InvalidArgument`] if the header magic is invalid, the entry
/// table is truncated, or any entry lacks a valid UTF-8 NUL terminator.
pub fn parse_binary_database_preparsed(input: &[u8], prefix: char) -> Result<Database> {
    parse_binary_database_internal(input, Some(prefix))
}

fn parse_binary_database_internal(input: &[u8], prefix: Option<char>) -> Result<Database> {
    let Some(rest) = input.strip_prefix(BINARY_MAGIC) else {
        return Err(Error::InvalidArgument);
    };
    if rest.len() < BINARY_HEADER_META_SIZE {
        return Err(Error::InvalidArgument);
    }
    let entry_count = usize::try_from(u32::from_le_bytes([rest[0], rest[1], rest[2], rest[3]]))
        .map_err(|_| Error::InvalidArgument)?;
    let rest = &rest[BINARY_HEADER_META_SIZE..];

    let entries_len = entry_count
        .checked_mul(BINARY_ENTRY_SIZE)
        .ok_or(Error::InvalidArgument)?;
    if rest.len() < entries_len {
        return Err(Error::InvalidArgument);
    }
    let (entries_bytes, mut string_bytes) = rest.split_at(entries_len);

    let mut database = Database::new();
    for entry_chunk in entries_bytes.as_chunks::<BINARY_ENTRY_SIZE>().0 {
        let token = u32::from_le_bytes([
            entry_chunk[0],
            entry_chunk[1],
            entry_chunk[2],
            entry_chunk[3],
        ]);
        let day = entry_chunk[4];
        let month = entry_chunk[5];
        let year = u16::from_le_bytes([entry_chunk[6], entry_chunk[7]]);

        let nul_pos = string_bytes
            .iter()
            .position(|&b| b == 0)
            .ok_or(Error::InvalidArgument)?;
        let format_str =
            core::str::from_utf8(&string_bytes[..nul_pos]).map_err(|_| Error::InvalidArgument)?;
        string_bytes = &string_bytes[nul_pos + 1..];

        database.add_entry_with_prefix(
            "",
            token,
            format_str.to_string(),
            date_removed_string(year, month, day),
            prefix,
        );
    }

    Ok(database)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn append_binary_entry(buf: &mut Vec<u8>, token: u32, day: u8, month: u8, year: u16) {
        buf.extend_from_slice(&token.to_le_bytes());
        buf.push(day);
        buf.push(month);
        buf.extend_from_slice(&year.to_le_bytes());
    }

    #[test]
    fn test_parse_binary_database_tokens_header() {
        let mut binary_data = Vec::new();
        binary_data.extend_from_slice(b"TOKENS\0\0");
        binary_data.extend_from_slice(&1u32.to_le_bytes()); // entry_count = 1
        binary_data.extend_from_slice(&0u32.to_le_bytes()); // reserved

        append_binary_entry(&mut binary_data, 0x12345678, 0xff, 0xff, 0xffff);

        // Null-terminated string
        binary_data.extend_from_slice(b"hello world\0");

        let db = parse_binary_database(&binary_data).unwrap();
        let entries = db.lookup(0x12345678, "");
        assert_eq!(entries.len(), 1);
        assert_eq!(entries[0].format_string, "hello world");
        assert_eq!(entries[0].date_removed, "");
    }

    #[test]
    fn test_parse_binary_database_valid_and_invalid_dates() {
        let mut binary_data = Vec::new();
        binary_data.extend_from_slice(BINARY_MAGIC);
        binary_data.extend_from_slice(&4u32.to_le_bytes());
        binary_data.extend_from_slice(&0u32.to_le_bytes());

        // Valid date: 2026-06-12
        append_binary_entry(&mut binary_data, 1, 12, 6, 2026);
        // Valid leap day: 2024-02-29
        append_binary_entry(&mut binary_data, 2, 29, 2, 2024);
        // Invalid non-leap Feb 29: 2025-02-29 -> ""
        append_binary_entry(&mut binary_data, 3, 29, 2, 2025);
        // Out-of-range year > 9999 -> ""
        append_binary_entry(&mut binary_data, 4, 1, 1, 10000);

        binary_data.extend_from_slice(b"valid\0leap\0nonleap\0future\0");

        let db = parse_binary_database(&binary_data).unwrap();
        assert_eq!(db.lookup(1, "")[0].date_removed, "2026-06-12");
        assert_eq!(db.lookup(2, "")[0].date_removed, "2024-02-29");
        assert_eq!(db.lookup(3, "")[0].date_removed, "");
        assert_eq!(db.lookup(4, "")[0].date_removed, "");
    }

    #[test]
    fn test_parse_binary_database_invalid_inputs() {
        parse_binary_database(b"INVALID_HEADER").unwrap_err();
        parse_binary_database(b"TOKENS\0\0\x01\x00").unwrap_err();

        // Declared entry_count = 1, missing entry bytes
        let mut truncated = Vec::new();
        truncated.extend_from_slice(BINARY_MAGIC);
        truncated.extend_from_slice(&1u32.to_le_bytes());
        truncated.extend_from_slice(&0u32.to_le_bytes());
        parse_binary_database(&truncated).unwrap_err();

        // Missing null terminator on string table
        append_binary_entry(&mut truncated, 1, 0xff, 0xff, 0xffff);
        truncated.extend_from_slice(b"unterminated");
        parse_binary_database(&truncated).unwrap_err();
    }
}
