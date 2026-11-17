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

//! ELF `.pw_tokenizer.entries` section token database parser.

use pw_status::{Error, Result};
use pw_tokenizer_core::TOKENIZER_ENTRY_MAGIC;

use crate::detokenize::database::Database;

fn parse_null_terminated_string_with_len(input: &[u8], len: usize) -> Result<(&[u8], &str)> {
    if len == 0 || input.len() < len {
        return Err(Error::InvalidArgument);
    }
    let (chunk, rest) = input.split_at(len);
    if chunk[len - 1] != 0 {
        return Err(Error::InvalidArgument);
    }
    let parsed_str = core::str::from_utf8(&chunk[..len - 1]).map_err(|_| Error::InvalidArgument)?;
    Ok((rest, parsed_str))
}

/// Parses an ELF token database section (`.pw_tokenizer.entries`) into a
/// [`Database`].
///
/// # Errors
///
/// Returns [`Error::InvalidArgument`] if an entry header is truncated, the
/// magic value is invalid, or any domain or format string is malformed.
pub fn parse_elf_section_database(input: &[u8]) -> Result<Database> {
    parse_elf_section_database_internal(input, None)
}

/// Parses an ELF token database section into a [`Database`] and pre-parses all
/// format strings with `prefix`.
///
/// # Errors
///
/// Returns [`Error::InvalidArgument`] if an entry header is truncated, the
/// magic value is invalid, or any domain or format string is malformed.
pub fn parse_elf_section_database_preparsed(input: &[u8], prefix: char) -> Result<Database> {
    parse_elf_section_database_internal(input, Some(prefix))
}

const ELF_ENTRY_HEADER_SIZE: usize = 16;

fn parse_elf_section_database_internal(mut input: &[u8], prefix: Option<char>) -> Result<Database> {
    let mut database = Database::new();
    while !input.is_empty() {
        if input.len() < ELF_ENTRY_HEADER_SIZE {
            return Err(Error::InvalidArgument);
        }
        let (&magic_bytes, rest) = input
            .split_first_chunk::<4>()
            .ok_or(Error::InvalidArgument)?;
        if u32::from_le_bytes(magic_bytes) != TOKENIZER_ENTRY_MAGIC {
            return Err(Error::InvalidArgument);
        }
        let (&token_bytes, rest) = rest
            .split_first_chunk::<4>()
            .ok_or(Error::InvalidArgument)?;
        let (&domain_len_bytes, rest) = rest
            .split_first_chunk::<4>()
            .ok_or(Error::InvalidArgument)?;
        let (&string_len_bytes, rest) = rest
            .split_first_chunk::<4>()
            .ok_or(Error::InvalidArgument)?;
        let token = u32::from_le_bytes(token_bytes);
        let domain_len = usize::try_from(u32::from_le_bytes(domain_len_bytes))
            .map_err(|_| Error::InvalidArgument)?;
        let string_len = usize::try_from(u32::from_le_bytes(string_len_bytes))
            .map_err(|_| Error::InvalidArgument)?;
        let (remaining, domain) = parse_null_terminated_string_with_len(rest, domain_len)?;
        let (remaining, format_string) =
            parse_null_terminated_string_with_len(remaining, string_len)?;
        database.add_entry_with_prefix(
            domain,
            token,
            format_string.to_string(),
            String::new(),
            prefix,
        );
        input = remaining;
    }
    Ok(database)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parse_elf_section_database() {
        let mut binary_data = Vec::new();

        // Entry 1 with domain "my_domain"
        let domain1 = b"my_domain\0";
        let string1 = b"hello %d\0";
        binary_data.extend_from_slice(&TOKENIZER_ENTRY_MAGIC.to_le_bytes());
        binary_data.extend_from_slice(&0x12345678u32.to_le_bytes());
        binary_data.extend_from_slice(&u32::try_from(domain1.len()).unwrap().to_le_bytes());
        binary_data.extend_from_slice(&u32::try_from(string1.len()).unwrap().to_le_bytes());
        binary_data.extend_from_slice(domain1);
        binary_data.extend_from_slice(string1);

        // Entry 2 with empty domain
        let domain2 = b"\0";
        let string2 = b"test message\0";
        binary_data.extend_from_slice(&TOKENIZER_ENTRY_MAGIC.to_le_bytes());
        binary_data.extend_from_slice(&0x87654321u32.to_le_bytes());
        binary_data.extend_from_slice(&u32::try_from(domain2.len()).unwrap().to_le_bytes());
        binary_data.extend_from_slice(&u32::try_from(string2.len()).unwrap().to_le_bytes());
        binary_data.extend_from_slice(domain2);
        binary_data.extend_from_slice(string2);

        let db = parse_elf_section_database(&binary_data).unwrap();

        let entries1 = db.lookup(0x12345678, "my_domain");
        assert_eq!(entries1.len(), 1);
        assert_eq!(entries1[0].format_string, "hello %d");

        let entries2 = db.lookup(0x87654321, "");
        assert_eq!(entries2.len(), 1);
        assert_eq!(entries2[0].format_string, "test message");
    }

    #[test]
    fn test_parse_elf_section_database_corrupt() {
        // Truncated header (< 16 bytes)
        parse_elf_section_database(&TOKENIZER_ENTRY_MAGIC.to_le_bytes()).unwrap_err();

        // Header valid, but payload truncated
        let mut truncated = Vec::new();
        truncated.extend_from_slice(&TOKENIZER_ENTRY_MAGIC.to_le_bytes());
        truncated.extend_from_slice(&0x12345678u32.to_le_bytes());
        truncated.extend_from_slice(&10u32.to_le_bytes()); // domain_len = 10
        truncated.extend_from_slice(&10u32.to_le_bytes()); // string_len = 10
        truncated.extend_from_slice(b"short"); // only 5 bytes
        parse_elf_section_database(&truncated).unwrap_err();

        // Zero domain_len
        let mut zero_domain = Vec::new();
        zero_domain.extend_from_slice(&TOKENIZER_ENTRY_MAGIC.to_le_bytes());
        zero_domain.extend_from_slice(&0x12345678u32.to_le_bytes());
        zero_domain.extend_from_slice(&0u32.to_le_bytes()); // domain_len = 0 (invalid)
        zero_domain.extend_from_slice(&5u32.to_le_bytes()); // string_len = 5
        zero_domain.extend_from_slice(b"test\0");
        parse_elf_section_database(&zero_domain).unwrap_err();

        // Missing null terminator in domain
        let mut no_null = Vec::new();
        no_null.extend_from_slice(&TOKENIZER_ENTRY_MAGIC.to_le_bytes());
        no_null.extend_from_slice(&0x12345678u32.to_le_bytes());
        no_null.extend_from_slice(&4u32.to_le_bytes());
        no_null.extend_from_slice(&5u32.to_le_bytes());
        no_null.extend_from_slice(b"doma"); // no trailing \0
        no_null.extend_from_slice(b"test\0");
        parse_elf_section_database(&no_null).unwrap_err();

        // Trailing garbage after valid entry
        let mut trailing_garbage = Vec::new();
        trailing_garbage.extend_from_slice(&TOKENIZER_ENTRY_MAGIC.to_le_bytes());
        trailing_garbage.extend_from_slice(&0x12345678u32.to_le_bytes());
        trailing_garbage.extend_from_slice(&1u32.to_le_bytes());
        trailing_garbage.extend_from_slice(&5u32.to_le_bytes());
        trailing_garbage.extend_from_slice(b"\0");
        trailing_garbage.extend_from_slice(b"test\0");
        trailing_garbage.extend_from_slice(b"extra_garbage");
        parse_elf_section_database(&trailing_garbage).unwrap_err();
    }
}
