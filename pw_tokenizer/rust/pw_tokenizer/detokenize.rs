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

#![cfg(feature = "std")]

use std::cmp;
use std::collections::HashMap;

use pw_format::{Arg, FormatString, FormatStyle};
use pw_status::Result;
use pw_varint::VarintDecode;

mod csv;

const DEFAULT_DOMAIN: &str = "";

// 4 passes supports detokenizing two layers of nested messages with tokenized
// domains (e.g. ${${bar}#ab12cd34}#00000012), without allowing a hypothetical
// detokenization cycle to continue for too long.
const MAX_DECODE_PASSES: usize = 4;

/// A string that has been detokenized. This struct tracks all possible results
/// if there are token collisions.
pub struct DetokenizedString {
    /// The token that was decoded.
    pub token: u32,
    /// The best match formatted string, or empty if decoding failed.
    pub best_string: String,
    /// All possible decoded formatting matches, sorted by likelihood/score.
    pub matches: Vec<DecodedFormatString>,
    /// True if the message decoded successfully and unambiguously.
    pub is_ok: bool,
    /// True if a token was present in the encoded data.
    pub has_token: bool,
}

/// A decoded format string, which may contain error messages if decoding failed.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DecodedFormatString {
    /// The result of decoding and formatting.
    pub result: Result<String>,
}

#[derive(Debug)]
struct MatchResult {
    decoded_format_string: DecodedFormatString,
    has_errors: bool,
    remaining_bytes: usize,
    decoding_errors: usize,
    argument_count: usize,
    date_removed: String,
    format_string: String,
}

fn compare_date_removed(lhs: &str, rhs: &str) -> std::cmp::Ordering {
    let lhs_never = lhs.is_empty();
    let rhs_never = rhs.is_empty();
    match (lhs_never, rhs_never) {
        (true, true) => std::cmp::Ordering::Equal,
        (true, false) => std::cmp::Ordering::Greater,
        (false, true) => std::cmp::Ordering::Less,
        (false, false) => lhs.cmp(rhs),
    }
}

impl MatchResult {
    // Determines if one result is better than the other if collisions occurred.
    // This logic should match the collision resolution logic in detokenize.py.
    fn cmp_priority(&self, other: &Self) -> std::cmp::Ordering {
        // Favor the result for which decoding succeeded.
        if self.has_errors != other.has_errors {
            if !self.has_errors {
                return std::cmp::Ordering::Greater;
            }
            return std::cmp::Ordering::Less;
        }

        // Favor the result for which all bytes were decoded.
        let self_all_bytes = self.remaining_bytes == 0;
        let other_all_bytes = other.remaining_bytes == 0;
        if self_all_bytes != other_all_bytes {
            if self_all_bytes {
                return std::cmp::Ordering::Greater;
            }
            return std::cmp::Ordering::Less;
        }

        // Favor the result with fewer decoding errors.
        if self.decoding_errors != other.decoding_errors {
            if self.decoding_errors < other.decoding_errors {
                return std::cmp::Ordering::Greater;
            }
            return std::cmp::Ordering::Less;
        }

        // Favor the result that successfully decoded the most arguments.
        if self.argument_count != other.argument_count {
            if self.argument_count > other.argument_count {
                return std::cmp::Ordering::Greater;
            }
            return std::cmp::Ordering::Less;
        }

        // Favor the result that was removed from the database most recently.
        compare_date_removed(&self.date_removed, &other.date_removed)
    }

    fn is_better_than(&self, other: &Self) -> bool {
        self.cmp_priority(other) == std::cmp::Ordering::Greater
    }
}

/// Decodes and detokenizes from a token database. This struct builds a hash
/// table of tokens to give `O(1)` token lookups.
pub struct Detokenizer {
    // domain -> token -> entries
    database: HashMap<String, HashMap<u32, Vec<TokenizedStringEntry>>>,
}

/// An entry in the token database.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TokenizedStringEntry {
    /// The format string of this entry.
    pub format_string: String,
    /// The date when this entry was removed, or empty if it is still active.
    pub date_removed: String,
}

impl Detokenizer {
    /// Constructs a detokenizer from a CSV database.
    pub fn from_csv(csv: &str) -> Result<Self> {
        let mut database: HashMap<String, HashMap<u32, Vec<TokenizedStringEntry>>> = HashMap::new();

        let parsed_csv = csv::parse_csv(csv);

        for row in parsed_csv {
            if row.len() != 4 {
                continue;
            }

            let token_str = row[0].trim();
            let date_str = row[1].trim();
            let domain = canonicalize_domain(&row[2]);
            let format_string = &row[3];

            let token = match u32::from_str_radix(token_str, 16) {
                Ok(t) => t,
                Err(_) => return Err(pw_status::Error::InvalidArgument),
            };

            if !is_valid_date(date_str) {
                return Err(pw_status::Error::InvalidArgument);
            }

            let entries = database
                .entry(domain)
                .or_default()
                .entry(token)
                .or_default();

            let mut found = false;
            for entry in entries.iter_mut() {
                if entry.format_string == *format_string {
                    found = true;
                    if date_str > &entry.date_removed {
                        entry.date_removed = date_str.to_string();
                    }
                    break;
                }
            }
            if !found {
                entries.push(TokenizedStringEntry {
                    format_string: format_string.to_string(),
                    date_removed: date_str.to_string(),
                });
            }
        }

        Ok(Self { database })
    }

    /// Looks up database entries for a given token and domain.
    pub fn database_lookup(&self, token: u32, domain: &str) -> &[TokenizedStringEntry] {
        let canonical_domain = canonicalize_domain(domain);
        self.database
            .get(&canonical_domain)
            .and_then(|domain_map| domain_map.get(&token))
            .map(|entries| entries.as_slice())
            .unwrap_or(&[])
    }

    /// Decodes and detokenizes the binary encoded message. Returns a
    /// `DetokenizedString` that stores all possible detokenized string results.
    pub fn detokenize(&self, encoded: &[u8]) -> DetokenizedString {
        self.detokenize_with_domain(encoded, DEFAULT_DOMAIN)
    }

    /// Overload of `detokenize` that takes a domain.
    pub fn detokenize_with_domain(&self, encoded: &[u8], domain: &str) -> DetokenizedString {
        // The token is missing from the encoded data; there is nothing to do.
        if encoded.is_empty() {
            return DetokenizedString {
                token: 0,
                best_string: String::new(),
                matches: Vec::new(),
                is_ok: false,
                has_token: false,
            };
        }

        // The token is implicitly zero-extended if there are not enough input
        // bytes.
        let mut token_bytes = [0u8; 4];
        let len = cmp::min(encoded.len(), 4);
        token_bytes[..len].copy_from_slice(&encoded[..len]);
        let token = u32::from_le_bytes(token_bytes);

        let arguments = &encoded[len..];

        let mut match_results = Vec::new();

        let entries = self.database_lookup(token, domain);
        for entry in entries {
            let Ok(fmt_str) = FormatString::parse_printf(&entry.format_string) else {
                match_results.push(MatchResult {
                    decoded_format_string: DecodedFormatString {
                        result: Err(pw_status::Error::InvalidArgument),
                    },
                    has_errors: true,
                    remaining_bytes: arguments.len(),
                    decoding_errors: 1,
                    argument_count: 0,
                    date_removed: entry.date_removed.clone(),
                    format_string: entry.format_string.clone(),
                });
                continue;
            };

            let res = self.attempt_detokenize(
                &entry.format_string,
                &fmt_str,
                arguments,
                &entry.date_removed,
            );
            match_results.push(res);
        }

        // Sort the match results by priority (best first).
        match_results.sort_by(|a, b| b.cmp_priority(a));

        let is_ok = if match_results.is_empty() || match_results[0].has_errors {
            false
        } else if match_results.len() == 1 {
            true
        } else {
            match_results[0].is_better_than(&match_results[1])
        };

        let mut best_string = String::new();
        if let Some(res) = match_results.first() {
            if let Ok(s) = &res.decoded_format_string.result {
                best_string = s.clone();
            } else {
                best_string = res.format_string.clone();
            }
        }

        let matches: Vec<DecodedFormatString> = match_results
            .into_iter()
            .map(|r| r.decoded_format_string)
            .collect();

        DetokenizedString {
            token,
            best_string,
            matches,
            is_ok,
            has_token: true,
        }
    }

    fn attempt_detokenize(
        &self,
        entry_format_string: &str,
        fmt_str: &FormatString,
        arguments: &[u8],
        date_removed: &str,
    ) -> MatchResult {
        let mut args = Vec::new();
        let mut cursor = 0;
        let mut decoding_errors = 0;
        let mut argument_count = 0;
        let mut error = None;

        for fragment in &fmt_str.fragments {
            let pw_format::FormatFragment::Conversion(spec) = fragment else {
                continue;
            };
            argument_count += 1;
            if error.is_some() {
                decoding_errors += 1;
                continue;
            }
            if cursor >= arguments.len() {
                error = Some(pw_status::Error::InvalidArgument);
                decoding_errors += 1;
                continue;
            }
            let decode_res = match spec.primitive {
                pw_format::Primitive::Integer => decode_int_arg(&arguments[cursor..]),
                pw_format::Primitive::Unsigned => decode_uint_arg(&arguments[cursor..]),
                pw_format::Primitive::Character => decode_char_arg(&arguments[cursor..]),
                pw_format::Primitive::Pointer => decode_ptr_arg(&arguments[cursor..]),
                pw_format::Primitive::Float => decode_float_arg(&arguments[cursor..]),
                pw_format::Primitive::String => decode_str_arg(&arguments[cursor..]),
                _ => Err(pw_status::Error::Unimplemented),
            };
            match decode_res {
                Ok((arg, len)) => {
                    args.push(arg);
                    cursor += len;
                }
                Err(e) => {
                    error = Some(e);
                    decoding_errors += 1;
                }
            }
        }

        let format_result = if let Some(e) = error {
            Err(e)
        } else if cursor < arguments.len() {
            Err(pw_status::Error::InvalidArgument)
        } else {
            fmt_str
                .format(&args, FormatStyle::Printf)
                .map_err(|_| pw_status::Error::InvalidArgument)
        };

        let has_errors = format_result.is_err();
        let remaining_bytes = arguments.len().saturating_sub(cursor);

        MatchResult {
            decoded_format_string: DecodedFormatString {
                result: format_result,
            },
            has_errors,
            remaining_bytes,
            decoding_errors,
            argument_count,
            date_removed: date_removed.to_string(),
            format_string: entry_format_string.to_string(),
        }
    }

    /// Detokenizes a parsed format string with arguments.
    pub fn detokenize_parsed(&self, fmt_str: &FormatString, arguments: &[u8]) -> Result<String> {
        self.attempt_detokenize("", fmt_str, arguments, "")
            .decoded_format_string
            .result
    }

    /// Decodes and detokenizes nested tokenized messages in a string.
    pub fn detokenize_text(&self, text: &str) -> String {
        let mut current = text.to_string();
        for _ in 0..MAX_DECODE_PASSES {
            let mut detok = NestedMessageDetokenizer::new(self);
            detok.detokenize(&current);
            let next = detok.flush();
            if next == current {
                break;
            }
            current = next;
        }
        current
    }
}

fn decode_int_arg(arguments: &[u8]) -> Result<(Arg, usize)> {
    let (len, val) =
        i64::varint_decode(arguments).map_err(|_| pw_status::Error::InvalidArgument)?;
    Ok((Arg::Int(val), len))
}

fn decode_uint_arg(arguments: &[u8]) -> Result<(Arg, usize)> {
    let (len, val) =
        i64::varint_decode(arguments).map_err(|_| pw_status::Error::InvalidArgument)?;
    Ok((Arg::Uint(val as u64), len))
}

fn decode_char_arg(arguments: &[u8]) -> Result<(Arg, usize)> {
    let (len, val) =
        i64::varint_decode(arguments).map_err(|_| pw_status::Error::InvalidArgument)?;
    let ch = char::from_u32(val as u32).unwrap_or(' ');
    Ok((Arg::Char(ch), len))
}

fn decode_ptr_arg(arguments: &[u8]) -> Result<(Arg, usize)> {
    let (len, val) =
        i64::varint_decode(arguments).map_err(|_| pw_status::Error::InvalidArgument)?;
    Ok((Arg::Ptr(val as usize), len))
}

fn decode_float_arg(arguments: &[u8]) -> Result<(Arg, usize)> {
    if arguments.len() < 4 {
        return Err(pw_status::Error::InvalidArgument);
    }
    let bytes = arguments[..4]
        .try_into()
        .map_err(|_| pw_status::Error::InvalidArgument)?;
    let val = f32::from_le_bytes(bytes);
    Ok((Arg::Float(val as f64), 4))
}

fn decode_str_arg(arguments: &[u8]) -> Result<(Arg, usize)> {
    if arguments.is_empty() {
        return Err(pw_status::Error::InvalidArgument);
    }
    let len_byte = arguments[0];
    let truncated = (len_byte & 0x80) != 0;
    let len = (len_byte & 0x7f) as usize;
    if arguments.len() < 1 + len {
        return Err(pw_status::Error::InvalidArgument);
    }
    let mut s = String::from_utf8_lossy(&arguments[1..1 + len]).to_string();
    if truncated {
        s.push_str("[...]");
    }
    Ok((Arg::Str(s), 1 + len))
}

#[allow(dead_code)]
#[derive(PartialEq, Eq, Clone, Copy)]
enum State {
    Passthrough,
    MessageStart,
    Domain,
    RadixOrData,
    Radix10Or16,
    Radix64,
    RadixEnd,
    Data10,
    Data16,
    Data64,
    Data64Padding,
}

fn is_valid_domain_char(c: char) -> bool {
    c.is_ascii_alphanumeric() || c == '_' || c == ':' || c == ' ' || ('\t'..='\r').contains(&c)
}

fn canonicalize_domain(domain: &str) -> String {
    domain.chars().filter(|c| !c.is_whitespace()).collect()
}

struct NestedMessageDetokenizer<'a> {
    detokenizer: &'a Detokenizer,
    output: String,
    state: State,
    message_start: usize,
    domain_size: usize,
    data_start: usize,
}

impl<'a> NestedMessageDetokenizer<'a> {
    fn new(detokenizer: &'a Detokenizer) -> Self {
        Self {
            detokenizer,
            output: String::new(),
            state: State::Passthrough,
            message_start: 0,
            domain_size: 0,
            data_start: 0,
        }
    }

    fn detokenize(&mut self, chunk: &str) {
        for next_char in chunk.chars() {
            self.detokenize_char(next_char);
        }
    }

    fn detokenize_char(&mut self, next_char: char) {
        if next_char == '$' {
            self.handle_end_of_message();

            self.message_start = self.output.len();
            self.state = State::MessageStart;
            self.output.push(next_char);
            return;
        }

        self.output.push(next_char);
        match self.state {
            State::Passthrough => {}
            State::MessageStart => {
                if next_char == '{' {
                    self.state = State::Domain;
                } else {
                    self.handle_radix_or_base64_data(next_char);
                }
            }
            State::Domain => {
                if next_char == '}' {
                    self.state = State::RadixOrData;
                } else if is_valid_domain_char(next_char) {
                    self.domain_size += 1;
                } else {
                    self.reset_message();
                }
            }
            State::RadixOrData => {
                self.handle_radix_or_base64_data(next_char);
            }
            State::Radix10Or16 => {
                if next_char == '0' || next_char == '6' {
                    self.state = State::RadixEnd;
                } else {
                    self.state = State::Data64;
                    self.handle_base64_char(next_char);
                }
            }
            State::Radix64 => {
                if next_char == '4' {
                    self.state = State::RadixEnd;
                } else {
                    self.state = State::Data64;
                    self.handle_base64_char(next_char);
                }
            }
            State::RadixEnd => {
                if next_char == '#' {
                    // Check if the radix was 10, 16, or 64.
                    if let Some(digit) = self.output.chars().rev().nth(1) {
                        self.state = if digit == '0' {
                            State::Data10
                        } else if digit == '6' {
                            State::Data16
                        } else {
                            State::Data64
                        };
                        self.data_start = self.output.len();
                    } else {
                        self.reset_message();
                    }
                } else {
                    self.state = State::Data64;
                    self.handle_base64_char(next_char);
                }
            }
            State::Data10 => {
                self.handle_base10_char(next_char);
            }
            State::Data16 => {
                self.handle_base16_char(next_char);
            }
            State::Data64 => {
                self.handle_base64_char(next_char);
            }
            State::Data64Padding => {
                if next_char == '=' {
                    self.handle_end_of_message_valid_base64();
                } else {
                    self.reset_message();
                }
            }
        }
    }

    fn flush(mut self) -> String {
        self.handle_end_of_message();
        self.output
    }

    fn domain(&self) -> String {
        let start = self.message_start + 2;
        let end = start + self.domain_size;
        let raw_domain = &self.output[start..end];
        canonicalize_domain(raw_domain)
    }

    fn handle_radix_or_base64_data(&mut self, next_char: char) {
        // The first few characters after $ could be either a radix specification or
        // Base64 data (e.g. $16dAw5== versus $16#00000001).
        if next_char == '#' {
            self.state = State::Data16;
            self.data_start = self.output.len();
            return;
        }

        self.data_start = self.output.len() - 1;
        if next_char == '1' {
            self.state = State::Radix10Or16;
        } else if next_char == '6' {
            self.state = State::Radix64;
        } else if pw_base64::is_valid_char(next_char) {
            // If this is Base64 data, it includes this character.
            self.state = State::Data64;
        } else {
            self.reset_message();
        }
    }

    fn handle_base10_char(&mut self, next_char: char) {
        if !next_char.is_ascii_digit() {
            self.reset_message();
            return;
        }

        let block_size = self.output.len() - self.data_start;
        if block_size == 10 {
            // Base10 data must be 10 chars long.
            self.handle_end_of_message_valid_base10_or_base16(10);
        }
    }

    fn handle_base16_char(&mut self, next_char: char) {
        if !next_char.is_ascii_hexdigit() {
            self.reset_message();
            return;
        }

        let block_size = self.output.len() - self.data_start;
        if block_size == 8 {
            // Base16 data must be 8 chars long.
            self.handle_end_of_message_valid_base10_or_base16(16);
        }
    }

    fn handle_base64_char(&mut self, next_char: char) {
        if pw_base64::is_valid_char(next_char) {
            return;
        }

        // Base64 data must be in 4 char blocks, ending with padding if needed.
        let block_size = (self.output.len() - self.data_start) % 4;
        if block_size == 1 {
            // Got invalid character after a 4-byte block. Pop that char and decode.
            self.output.pop();
            self.handle_end_of_message_valid_base64();
            self.output.push(next_char);
        } else if block_size == 2 || next_char != '=' {
            // Invalid character not on a 4-char block boundary. Could try decoding at
            // the block boundary instead of resetting.
            self.reset_message();
        } else if block_size == 3 {
            self.state = State::Data64Padding;
        } else {
            self.handle_end_of_message_valid_base64();
        }
    }

    fn handle_end_of_message(&mut self) {
        // Use integer values to check state order
        let state_val = self.state as u8;
        let data10_val = State::Data10 as u8;
        let data64_val = State::Data64 as u8;

        if state_val < data10_val {
            // It's not possible to have a complete token outside of the Data
            // states, even for the shortest possible messages ($10==).
            self.reset_message();
            return;
        }

        if state_val >= data64_val {
            // Base64 data must come in 4-byte blocks.
            if (self.output.len() - self.data_start).is_multiple_of(4) {
                self.handle_end_of_message_valid_base64();
            } else {
                self.reset_message();
            }
            return;
        }

        if self.state == State::Data10 {
            if self.output.len() - self.data_start == 10 {
                self.handle_end_of_message_valid_base10_or_base16(10);
            }
        } else if self.state == State::Data16 && self.output.len() - self.data_start == 8 {
            self.handle_end_of_message_valid_base10_or_base16(16);
        }
        self.reset_message();
    }

    fn handle_end_of_message_valid_base10_or_base16(&mut self, base: u32) {
        let data = &self.output[self.data_start..];
        if let Ok(token) = u32::from_str_radix(data, base) {
            self.detokenize_once(token);
        } else {
            self.reset_message();
        }
    }

    fn handle_end_of_message_valid_base64(&mut self) {
        let data = &self.output[self.data_start..];
        if let Ok(bytes) = base64_decode(data) {
            self.detokenize_once_base64(&bytes);
        } else {
            self.reset_message();
        }
    }

    fn detokenize_once(&mut self, token: u32) {
        let entries = self.detokenizer.database_lookup(token, &self.domain());
        let mut matching_entry = None;

        // Detokenize if there is only one match, or if there are multiple matches
        // but only one that is active.
        if entries.len() == 1 {
            matching_entry = Some(&entries[0]);
        } else if entries.len() > 1 {
            let mut active_entries = entries.iter().filter(|e| e.date_removed.is_empty());
            if let Some(first) = active_entries.next()
                && active_entries.next().is_none()
            {
                matching_entry = Some(first);
            }
        }

        if let Some(entry) = matching_entry
            && let Ok(fmt_str) = FormatString::parse_printf(&entry.format_string)
            && let Ok(replacement) = fmt_str.format(&[], FormatStyle::Printf)
        {
            self.output
                .replace_range(self.message_start..self.output.len(), &replacement);
        }
        self.reset_message();
    }

    fn detokenize_once_base64(&mut self, bytes: &[u8]) {
        // Detokenize if there is an unambiguous match, or if there is only one
        // match that failed to decode because no argument data was provided.
        let result = self
            .detokenizer
            .detokenize_with_domain(bytes, &self.domain());
        if result.is_ok || (result.matches.len() == 1 && bytes.len() == 4) {
            self.output
                .replace_range(self.message_start..self.output.len(), &result.best_string);
        }
        self.reset_message();
    }

    fn reset_message(&mut self) {
        self.message_start = 0;
        self.domain_size = 0;
        self.data_start = 0;
        self.state = State::Passthrough;
    }
}

fn is_valid_date(s: &str) -> bool {
    if s.is_empty() {
        return true;
    }
    if s.len() != 10 {
        return false;
    }
    s.chars().enumerate().all(|(i, c)| {
        if i == 4 || i == 7 {
            c == '-'
        } else {
            c.is_ascii_digit()
        }
    })
}

fn base64_decode(s: &str) -> std::result::Result<Vec<u8>, String> {
    let encoded_bytes = s.as_bytes();
    let mut output = vec![0u8; pw_base64::max_decoded_size(encoded_bytes.len())];
    match pw_base64::decode(encoded_bytes, &mut output) {
        Ok(len) => {
            output.truncate(len);
            Ok(output)
        }
        Err(e) => Err(format!("Base64 decode error: {:?}", e)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_from_csv() {
        let csv = "12345678,,,\"Hello World\"";
        let detok = Detokenizer::from_csv(csv).unwrap();
        assert!(detok.database.contains_key(""));
        let domain_map = detok.database.get("").unwrap();
        assert!(domain_map.contains_key(&0x12345678));
    }

    #[test]
    fn test_from_csv_different_domains() {
        let csv = "1,,domain1,Hello\n\
                   2,,domain2,\n\
                   3,,domain3,World!\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        assert_eq!(detok.database.len(), 3);
        assert!(detok.database.contains_key("domain1"));
        assert!(detok.database.contains_key("domain2"));
        assert!(detok.database.contains_key("domain3"));
    }

    #[test]
    fn test_from_csv_duplicate_entries_ignored() {
        let csv = "1,,,Hello World!\n\
                   2,,,another entry\n\
                   1,,,Hello World!\n\
                   3,,,Goodbye!\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        let domain_map = detok.database.get("").unwrap();
        let entries = domain_map.get(&1).unwrap();
        assert_eq!(entries.len(), 1);
    }

    #[test]
    fn test_from_csv_duplicate_entries_uses_newest_removal_date() {
        let csv = "1,2001-01-01,,Hello World!\n\
                   2,,,another entry\n\
                   1,2000-01-01,,Hello World!\n\
                   1,2002-01-01,,Hello World!\n\
                   3,,,Goodbye!\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        let domain_map = detok.database.get("").unwrap();
        let entries = domain_map.get(&1).unwrap();
        assert_eq!(entries.len(), 1);
        assert_eq!(entries[0].date_removed, "2002-01-01");
    }

    #[test]
    fn test_from_csv_count_domains() {
        let csv = "1,,domain1,Hello\n\
                   2,,domain2,\n\
                   3,,domain3,World!\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        assert_eq!(detok.database.len(), 3);
    }

    #[test]
    fn test_detokenize_with_domain() {
        let csv = "1,,domain1,Hello\n\
                   1,,domain2,World\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        let encoded = 1u32.to_le_bytes();
        let result = detok.detokenize_with_domain(&encoded, "domain1");
        assert_eq!(result.best_string, "Hello");

        let result = detok.detokenize_with_domain(&encoded, "domain2");
        assert_eq!(result.best_string, "World");
    }

    #[test]
    fn test_from_csv_domain_ignores_whitespace() {
        let csv = "1,, domain1 ,Hello\n\
                   2,,  domain2  ,\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        assert!(detok.database.contains_key("domain1"));
        assert!(detok.database.contains_key("domain2"));
    }

    #[test]
    fn test_detokenize_best_string_missing_token_is_empty() {
        let detok = Detokenizer::from_csv("").unwrap();
        let result = detok.detokenize(&[]);
        assert_eq!(result.best_string, "");
    }

    #[test]
    fn test_detokenize_best_string_unknown_token_is_empty() {
        let detok = Detokenizer::from_csv("").unwrap();
        let encoded = 1u32.to_le_bytes();
        let result = detok.detokenize(&encoded);
        assert_eq!(result.best_string, "");
    }

    #[test]
    fn test_detokenize_best_string_shorter_token_zero_extended() {
        let csv = "00000042,,,Hello World\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        let result = detok.detokenize(&[0x42, 0x00]);
        assert_eq!(result.best_string, "Hello World");
    }

    #[test]
    fn test_detokenize_with_args_no_matches() {
        let detok = Detokenizer::from_csv("").unwrap();
        let encoded = 1u32.to_le_bytes();
        let result = detok.detokenize(&encoded);
        assert!(result.matches.is_empty());
    }

    #[test]
    fn test_detokenize_with_args_single_match() {
        let csv = "00000001,,,Hello %d\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        let mut encoded = 1u32.to_le_bytes().to_vec();
        encoded.push(84); // Varint encoded 42
        let result = detok.detokenize(&encoded);
        assert_eq!(result.matches.len(), 1);
        assert_eq!(result.best_string, "Hello 42");
    }

    #[test]
    fn test_detokenize_with_args_empty() {
        let csv = "00000001,,,\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        let encoded = 1u32.to_le_bytes();
        let result = detok.detokenize(&encoded);
        assert_eq!(result.best_string, "");
    }

    #[test]
    fn test_detokenize_collisions_ok_if_exactly_one_success() {
        let csv = "00000001,,,crocodile %d\n\
                   00000001,,,alligator %s\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        let mut encoded = 1u32.to_le_bytes().to_vec();
        encoded.push(84); // Varint encoded 42
        let result = detok.detokenize(&encoded);
        assert!(result.is_ok);
        assert_eq!(result.best_string, "crocodile 42");
    }

    #[test]
    fn test_detokenize_collisions_not_ok_if_multiple_successful_decodes() {
        let csv = "00000001,,,crocodile %d\n\
                   00000001,,,alligator %d\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        let mut encoded = 1u32.to_le_bytes().to_vec();
        encoded.push(84); // Varint encoded 42
        let result = detok.detokenize(&encoded);
        assert!(!result.is_ok);
        assert_eq!(result.best_string, "crocodile 42");
    }

    #[test]
    fn test_detokenize_collisions_prefer_active() {
        let csv = "00000001,2001-01-01,,crocodile\n\
                   00000001,,,alligator\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        let encoded = 1u32.to_le_bytes();
        let result = detok.detokenize(&encoded);
        assert!(result.is_ok);
        assert_eq!(result.best_string, "alligator");
    }

    #[test]
    fn test_detokenize_collisions_prefer_newer_date() {
        let csv = "00000001,2001-01-01,,crocodile\n\
                   00000001,2002-01-01,,alligator\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        let encoded = 1u32.to_le_bytes();
        let result = detok.detokenize(&encoded);
        assert!(result.is_ok);
        assert_eq!(result.best_string, "alligator");
    }

    #[test]
    fn test_detokenize_collisions_prefer_more_arguments() {
        let csv = "00000001,,,crocodile %d %d\n\
                   00000001,,,alligator %d\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        let mut encoded = 1u32.to_le_bytes().to_vec();
        encoded.push(84); // Varint encoded 42
        encoded.push(84); // Varint encoded 42
        let result = detok.detokenize(&encoded);
        assert!(result.is_ok);
        assert_eq!(result.best_string, "crocodile 42 42");
    }

    #[test]
    fn test_detokenize() {
        let csv = "12345678,,,\"Hello World\"";
        let detok = Detokenizer::from_csv(csv).unwrap();
        let encoded = 0x12345678u32.to_le_bytes();
        let result = detok.detokenize(&encoded);
        assert_eq!(result.best_string, "Hello World");
    }

    #[test]
    fn test_detokenize_text_nested() {
        let csv = "00000001,,,This is a $#00000002\n\
                   00000002,,,nested argument!\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        let result = detok.detokenize_text("$#00000001");
        assert_eq!(result, "This is a nested argument!");
    }

    #[test]
    fn test_detokenize_text_base64() {
        let csv = "12345678,,,Hello World\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        let result = detok.detokenize_text("$eFY0Eg==");
        assert_eq!(result, "Hello World");
    }

    #[test]
    fn test_detokenize_collisions() {
        let csv = "12345678,,,crocodile!\n\
                   12345678,,,alligator!\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        let encoded = 0x12345678u32.to_le_bytes();
        let result = detok.detokenize(&encoded);
        assert_eq!(result.best_string, "crocodile!");
    }

    #[test]
    fn test_from_csv_bad_date() {
        let csv = "1,01-01-2001,,Hello\n";
        let detok = Detokenizer::from_csv(csv);
        assert!(detok.is_err());
    }

    #[test]
    fn test_from_csv_bad_token() {
        let csv = "g,,domain1,Hello\n";
        let detok = Detokenizer::from_csv(csv);
        assert!(detok.is_err());
    }

    #[test]
    fn test_detokenize_extra_data_is_unsuccessful() {
        let csv = "00000001,,,Hello\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        let mut encoded = 1u32.to_le_bytes().to_vec();
        encoded.push(84);
        let result = detok.detokenize(&encoded);
        assert!(!result.is_ok);
        assert_eq!(result.best_string, "Hello");
        assert_eq!(
            result.matches[0].result,
            Err(pw_status::Error::InvalidArgument)
        );
    }

    #[test]
    fn test_from_csv_bad_format() {
        let csv = "1,2001-01-01,D1,Hello\n\
                   2,, \n\
                   3,,D3,Goodbye!\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        assert_eq!(detok.database.len(), 2);
        assert!(detok.database.contains_key("D1"));
        assert!(detok.database.contains_key("D3"));
    }

    #[test]
    fn test_detokenize_text_deeply_nested() {
        let csv = "00000001,,,$10#0000000005\n\
                   00000002,,,This is a $#00000004\n\
                   00000003,,,deeply nested argument.\n\
                   00000004,,,$AQAAAA==\n\
                   00000005,,,$AwAAAA==\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        let result = detok.detokenize_text("This is a $#00000004");
        assert_eq!(result, "This is a deeply nested argument.");
    }
}
