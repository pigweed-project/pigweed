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

use nom::bytes::complete::{tag, take_till};
use nom::multi::count;
use nom::number::complete::{le_u16, le_u32, le_u8};
use nom::IResult;
use pw_status::Result;

use crate::detokenize::database::Database;

#[derive(Debug)]
struct BinaryEntry {
    token: u32,
    day: u8,
    month: u8,
    year: u16,
}

fn parse_header(input: &[u8]) -> IResult<&[u8], usize> {
    let (input, _) = tag(b"TOKENS\0\0")(input)?;
    let (input, entry_count) = le_u32(input)?;
    let (input, _reserved) = le_u32(input)?;
    Ok((input, entry_count as usize))
}

fn parse_entry(input: &[u8]) -> IResult<&[u8], BinaryEntry> {
    let (input, token) = le_u32(input)?;
    let (input, day) = le_u8(input)?;
    let (input, month) = le_u8(input)?;
    let (input, year) = le_u16(input)?;
    Ok((
        input,
        BinaryEntry {
            token,
            day,
            month,
            year,
        },
    ))
}

fn parse_null_terminated_string(input: &[u8]) -> IResult<&[u8], &str> {
    let (input, s_bytes) = take_till(|b| b == 0)(input)?;
    let (input, _) = tag(b"\0")(input)?;
    let s = core::str::from_utf8(s_bytes).map_err(|_| {
        nom::Err::Error(nom::error::Error::new(
            s_bytes,
            nom::error::ErrorKind::Verify,
        ))
    })?;
    Ok((input, s))
}

fn is_valid_date_fields(year: u16, month: u8, day: u8) -> bool {
    if !(1..=12).contains(&month) {
        return false;
    }
    if !(1..=31).contains(&day) {
        return false;
    }
    let max_days = match month {
        2 => {
            let is_leap =
                (year.is_multiple_of(4) && !year.is_multiple_of(100)) || year.is_multiple_of(400);
            if is_leap {
                29
            } else {
                28
            }
        }
        4 | 6 | 9 | 11 => 30,
        _ => 31,
    };
    day <= max_days
}

fn date_remove_string(entry: &BinaryEntry) -> String {
    if entry.day == 0xff && entry.month == 0xff && entry.year == 0xffff {
        return String::new();
    }

    if !is_valid_date_fields(entry.year, entry.month, entry.day) {
        return String::new();
    }

    format!("{:04}-{:02}-{:02}", entry.year, entry.month, entry.day)
}

/// Parses a binary token database.
pub fn parse_binary_database(input: &[u8]) -> Result<Database> {
    let (input, entry_count) =
        parse_header(input).map_err(|_| pw_status::Error::InvalidArgument)?;
    let (input, raw_entries) =
        count(parse_entry, entry_count)(input).map_err(|_| pw_status::Error::InvalidArgument)?;
    let (_input, strings) = count(parse_null_terminated_string, entry_count)(input)
        .map_err(|_| pw_status::Error::InvalidArgument)?;

    let mut database = Database::new();

    for (entry, s) in raw_entries.into_iter().zip(strings) {
        database.add_entry("", entry.token, s.to_string(), date_remove_string(&entry));
    }

    Ok(database)
}
