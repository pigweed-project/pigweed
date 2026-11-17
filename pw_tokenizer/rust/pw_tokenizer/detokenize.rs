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

//! Detokenization decoding and message formatting support for `pw_tokenizer`.

#![cfg(feature = "std")]

use core::cmp::Ordering;
use std::borrow::Cow;

use pw_format::FormatString;
use pw_status::{Error, Result};

pub(crate) mod binary;
pub(crate) mod csv;
pub(crate) mod database;
pub(crate) mod elf;

pub mod arg_decoder;
pub mod decoded;
pub mod error;
pub(crate) mod primitives;
pub mod varint;

pub use arg_decoder::{
    canonicalize_domain, Decoder, TokenResolver, DEFAULT_RECURSION_PASSES, MAX_RECURSION_DEPTH,
};
pub use database::{Database, TokenizedStringEntry};
pub use decoded::{DecodeMetrics, DecodedFormatString};
pub use error::{
    format_error_placeholder, RawSpecErrorFormatter, TokenizerError, TokenizerErrorFormatter,
};
use primitives::extract_token_and_args;
pub use varint::{decode_unsigned_varint, decode_varint, zigzag_decode, ArgStatus};

const DEFAULT_DOMAIN: &str = "";

/// The result of an attempt to detokenize a string.
///
/// # Lifetimes
///
/// * `'a` - The lifetime of the token database entry (format string and removal
///   date) borrowed from the [`Detokenizer`].
#[derive(Clone, Debug, PartialEq)]
pub enum DetokenizeAttempt<'a> {
    /// Detokenization executed successfully.
    Success(DecodedFormatString<'a>),
    /// Detokenization failed because the format string could not be parsed.
    Failure {
        /// The original unparsed format string.
        format_string: Cow<'a, str>,
        /// The number of bytes that remained unparsed/unprocessed.
        remaining_bytes: usize,
    },
}

impl<'a> DetokenizeAttempt<'a> {
    /// Converts this detokenization attempt into an owned `'static` instance.
    #[must_use]
    pub fn into_owned(self) -> DetokenizeAttempt<'static> {
        match self {
            Self::Success(res) => DetokenizeAttempt::Success(res.into_owned()),
            Self::Failure {
                format_string,
                remaining_bytes,
            } => DetokenizeAttempt::Failure {
                format_string: Cow::Owned(format_string.into_owned()),
                remaining_bytes,
            },
        }
    }

    /// Returns the number of bytes that remained after decoding.
    #[must_use]
    pub fn remaining_bytes(&self) -> usize {
        match self {
            Self::Success(res) => res.remaining_bytes(),
            Self::Failure {
                remaining_bytes, ..
            } => *remaining_bytes,
        }
    }

    /// Returns the number of arguments that failed to decode.
    #[must_use]
    pub fn decoding_errors(&self) -> usize {
        match self {
            Self::Success(res) => res.decoding_errors(),
            Self::Failure { .. } => 1,
        }
    }

    /// Returns the result of decoding and formatting.
    ///
    /// # Errors
    ///
    /// Returns [`Error::InvalidArgument`] if the format string could not be parsed or any argument
    /// failed to decode.
    pub fn result(&self) -> Result<String> {
        match self {
            Self::Success(res) => res.result(),
            Self::Failure { .. } => Err(Error::InvalidArgument),
        }
    }

    /// Returns the decoded format string, with conversion specifiers kept for any arguments that failed to decode.
    #[must_use]
    pub fn value(&self) -> String {
        self.map_or_format_string(DecodedFormatString::value)
    }

    /// Returns the decoded format string, with error messages for any arguments that failed to decode.
    #[must_use]
    pub fn value_with_errors(&self) -> String {
        self.map_or_format_string(DecodedFormatString::value_with_errors)
    }

    fn map_or_format_string<F>(&self, f: F) -> String
    where
        F: FnOnce(&DecodedFormatString<'a>) -> String,
    {
        match self {
            Self::Success(res) => f(res),
            Self::Failure { format_string, .. } => format_string.to_string(),
        }
    }

    /// Compares two detokenization attempts according to collision resolution priority.
    #[must_use]
    pub fn cmp_priority(&self, other: &Self) -> Ordering {
        match (self, other) {
            (Self::Success(res_a), Self::Success(res_b)) => res_a.cmp_priority(res_b),
            (Self::Success(_), Self::Failure { .. }) => Ordering::Greater,
            (Self::Failure { .. }, Self::Success(_)) => Ordering::Less,
            (Self::Failure { .. }, Self::Failure { .. }) => Ordering::Equal,
        }
    }
}

/// A string that has been detokenized, containing all potential collision matches sorted by priority.
///
/// # Lifetimes
///
/// * `'a` - The lifetime of the token database entries borrowed by [`matches`](Self::matches).
#[derive(Clone, Debug, PartialEq)]
pub struct DetokenizedString<'a> {
    /// The token that was decoded.
    pub token: Option<u32>,
    /// All possible decoded matches, sorted by likelihood/score.
    pub matches: Vec<DetokenizeAttempt<'a>>,
    /// True if the message decoded successfully and unambiguously.
    pub is_ok: bool,
}

impl<'a> DetokenizedString<'a> {
    /// Creates an empty DetokenizedString with an optional token.
    #[must_use]
    pub const fn empty(token: Option<u32>) -> Self {
        Self {
            token,
            matches: Vec::new(),
            is_ok: false,
        }
    }

    /// Converts this detokenized string into an owned `'static` instance.
    #[must_use]
    pub fn into_owned(self) -> DetokenizedString<'static> {
        DetokenizedString {
            token: self.token,
            matches: self
                .matches
                .into_iter()
                .map(DetokenizeAttempt::into_owned)
                .collect(),
            is_ok: self.is_ok,
        }
    }

    /// Returns the best match formatted string, or empty if decoding failed.
    #[must_use]
    pub fn best_string(&self) -> String {
        self.matches.first().map(|m| m.value()).unwrap_or_default()
    }

    /// Returns the best match formatted string, with error messages for any arguments that failed to decode.
    #[must_use]
    pub fn best_string_with_errors(&self) -> String {
        if let Some(best_match) = self.matches.first() {
            return best_match.value_with_errors();
        }
        match self.token {
            Some(token) => format!("<[unknown token {token:08x}]>"),
            None => "<[missing token]>".to_string(),
        }
    }

    /// Returns the number of bytes that remained after decoding the best match, or 0 if there are no matches.
    #[must_use]
    pub fn remaining_bytes(&self) -> usize {
        self.matches
            .first()
            .map_or(0, DetokenizeAttempt::remaining_bytes)
    }

    /// Returns the number of arguments that failed to decode in the best match, or 0 if there are no matches.
    #[must_use]
    pub fn decoding_errors(&self) -> usize {
        self.matches
            .first()
            .map_or(0, DetokenizeAttempt::decoding_errors)
    }
}

fn is_unambiguously_successful(attempts: &[DetokenizeAttempt<'_>]) -> bool {
    match attempts {
        [DetokenizeAttempt::Success(first), DetokenizeAttempt::Success(second), ..] => {
            first.is_successful() && first.cmp_priority(second) == Ordering::Greater
        }
        [DetokenizeAttempt::Success(first), ..] => first.is_successful(),
        _ => false,
    }
}

/// Detokenizer for binary and text tokenized messages.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Detokenizer {
    /// Underlying token-to-format-string lookup database.
    database: Database,
    /// Prefix character identifying tokenized messages in text (`'$'` by default).
    prefix: char,
}

impl Default for Detokenizer {
    fn default() -> Self {
        Self::new(Database::new())
    }
}

impl Detokenizer {
    /// Creates a [`Detokenizer`] from an existing [`Database`].
    #[must_use]
    pub fn new(database: Database) -> Self {
        Self::with_prefix(database, '$')
    }

    /// Creates a [`Detokenizer`] with a [`Database`] and custom prefix.
    #[must_use]
    pub const fn with_prefix(database: Database, prefix: char) -> Self {
        Self { database, prefix }
    }

    /// Pre-parses all format strings in the detokenizer's database using its
    /// current prefix.
    pub fn preparse_format_strings(&mut self) {
        self.database.preparse_format_strings(self.prefix);
    }

    /// Consumes the detokenizer, pre-parses all format strings in its
    /// database, and returns it.
    #[must_use]
    pub fn with_preparsed_format_strings(mut self) -> Self {
        self.preparse_format_strings();
        self
    }

    /// Creates a [`Detokenizer`] by parsing a CSV token database (format
    /// strings parsed on demand).
    ///
    /// # Errors
    ///
    /// Returns [`Error::InvalidArgument`] if the CSV contains an invalid hex
    /// token or malformed ISO date string.
    pub fn from_csv(csv_data: &str) -> Result<Self> {
        Self::from_csv_with_prefix(csv_data, '$')
    }

    /// Creates a [`Detokenizer`] by parsing a CSV token database with a custom
    /// prefix (format strings parsed on demand).
    ///
    /// # Errors
    ///
    /// Returns [`Error::InvalidArgument`] if the CSV contains an invalid hex
    /// token or malformed ISO date string.
    pub fn from_csv_with_prefix(csv_data: &str, prefix: char) -> Result<Self> {
        let database = csv::parse_csv_database(csv_data)?;
        Ok(Self::with_prefix(database, prefix))
    }

    /// Creates a [`Detokenizer`] by parsing a CSV token database and
    /// pre-parsing all format strings.
    ///
    /// # Errors
    ///
    /// Returns [`Error::InvalidArgument`] if the CSV contains an invalid hex
    /// token or malformed ISO date string.
    pub fn from_csv_preparsed(csv_data: &str) -> Result<Self> {
        Self::from_csv_with_prefix_preparsed(csv_data, '$')
    }

    /// Creates a [`Detokenizer`] by parsing a CSV token database with a custom
    /// prefix and pre-parsing all format strings.
    ///
    /// # Errors
    ///
    /// Returns [`Error::InvalidArgument`] if the CSV contains an invalid hex
    /// token or malformed ISO date string.
    pub fn from_csv_with_prefix_preparsed(csv_data: &str, prefix: char) -> Result<Self> {
        let database = csv::parse_csv_database_preparsed(csv_data, prefix)?;
        Ok(Self::with_prefix(database, prefix))
    }

    /// Creates a [`Detokenizer`] by parsing a binary token database (format
    /// strings parsed on demand).
    ///
    /// # Errors
    ///
    /// Returns an error if the binary database header, entry table, or string
    /// table is truncated or invalid.
    pub fn from_binary(binary_data: &[u8]) -> Result<Self> {
        Self::from_binary_with_prefix(binary_data, '$')
    }

    /// Creates a [`Detokenizer`] by parsing a binary token database with a
    /// custom prefix (format strings parsed on demand).
    ///
    /// # Errors
    ///
    /// Returns an error if the binary database header, entry table, or string
    /// table is truncated or invalid.
    pub fn from_binary_with_prefix(binary_data: &[u8], prefix: char) -> Result<Self> {
        let database = binary::parse_binary_database(binary_data)?;
        Ok(Self::with_prefix(database, prefix))
    }

    /// Creates a [`Detokenizer`] by parsing a binary token database and
    /// pre-parsing all format strings.
    ///
    /// # Errors
    ///
    /// Returns an error if the binary database header, entry table, or string
    /// table is truncated or invalid.
    pub fn from_binary_preparsed(binary_data: &[u8]) -> Result<Self> {
        Self::from_binary_with_prefix_preparsed(binary_data, '$')
    }

    /// Creates a [`Detokenizer`] by parsing a binary token database with a
    /// custom prefix and pre-parsing all format strings.
    ///
    /// # Errors
    ///
    /// Returns an error if the binary database header, entry table, or string
    /// table is truncated or invalid.
    pub fn from_binary_with_prefix_preparsed(binary_data: &[u8], prefix: char) -> Result<Self> {
        let database = binary::parse_binary_database_preparsed(binary_data, prefix)?;
        Ok(Self::with_prefix(database, prefix))
    }

    /// Creates a [`Detokenizer`] by parsing tokens from an ELF section (format
    /// strings parsed on demand).
    ///
    /// # Errors
    ///
    /// Returns an error if any ELF token entry header is invalid, truncated,
    /// or contains non-UTF-8 strings.
    pub fn from_elf_section(elf_section: &[u8]) -> Result<Self> {
        Self::from_elf_section_with_prefix(elf_section, '$')
    }

    /// Creates a [`Detokenizer`] by parsing tokens from an ELF section with a
    /// custom prefix (format strings parsed on demand).
    ///
    /// # Errors
    ///
    /// Returns an error if any ELF token entry header is invalid, truncated,
    /// or contains non-UTF-8 strings.
    pub fn from_elf_section_with_prefix(elf_section: &[u8], prefix: char) -> Result<Self> {
        let database = elf::parse_elf_section_database(elf_section)?;
        Ok(Self::with_prefix(database, prefix))
    }

    /// Creates a [`Detokenizer`] by parsing tokens from an ELF section and
    /// pre-parsing all format strings.
    ///
    /// # Errors
    ///
    /// Returns an error if any ELF token entry header is invalid, truncated,
    /// or contains non-UTF-8 strings.
    pub fn from_elf_section_preparsed(elf_section: &[u8]) -> Result<Self> {
        Self::from_elf_section_with_prefix_preparsed(elf_section, '$')
    }

    /// Creates a [`Detokenizer`] by parsing tokens from an ELF section with a
    /// custom prefix and pre-parsing all format strings.
    ///
    /// # Errors
    ///
    /// Returns an error if any ELF token entry header is invalid, truncated,
    /// or contains non-UTF-8 strings.
    pub fn from_elf_section_with_prefix_preparsed(
        elf_section: &[u8],
        prefix: char,
    ) -> Result<Self> {
        let database = elf::parse_elf_section_database_preparsed(elf_section, prefix)?;
        Ok(Self::with_prefix(database, prefix))
    }

    /// Returns the detokenization prefix character (default `$`).
    #[must_use]
    pub const fn prefix(&self) -> char {
        self.prefix
    }

    /// Sets the detokenization prefix character.
    pub fn set_prefix(&mut self, prefix: char) {
        if self.prefix == prefix {
            return;
        }
        self.prefix = prefix;
        if self.database.has_preparsed_entries() {
            self.database.preparse_format_strings(prefix);
        }
    }

    /// Looks up entries for a token in the specified domain.
    #[must_use]
    pub fn database_lookup(&self, token: u32, domain: &str) -> &[TokenizedStringEntry] {
        self.database.lookup(token, domain)
    }

    /// Detokenizes a binary tokenized message using the default domain (without multi-pass literal token expansion).
    #[must_use]
    pub fn detokenize(&self, data: &[u8]) -> DetokenizedString<'_> {
        self.detokenize_with_domain(data, DEFAULT_DOMAIN)
    }

    /// Detokenizes a binary tokenized message within a specified domain (without multi-pass literal token expansion).
    #[must_use]
    pub fn detokenize_with_domain(&self, data: &[u8], domain: &str) -> DetokenizedString<'_> {
        self.detokenize_with_domain_and_recursion(data, domain, 1)
    }

    /// Detokenizes a binary tokenized message using the default domain with multi-pass literal token expansion enabled (offline decoding).
    #[must_use]
    pub fn detokenize_recursive(&self, data: &[u8]) -> DetokenizedString<'_> {
        self.detokenize_with_domain_and_recursion(data, DEFAULT_DOMAIN, DEFAULT_RECURSION_PASSES)
    }

    /// Detokenizes a binary tokenized message within a specified domain with a specified number of literal token expansion passes.
    #[must_use]
    pub fn detokenize_with_domain_and_recursion(
        &self,
        data: &[u8],
        domain: &str,
        max_literal_passes: usize,
    ) -> DetokenizedString<'_> {
        let Some((token, args)) = extract_token_and_args(data) else {
            return DetokenizedString::empty(None);
        };

        let entries = self.database.lookup(token, domain);
        if entries.is_empty() {
            return DetokenizedString::empty(Some(token));
        }

        let mut attempts: Vec<_> = entries
            .iter()
            .map(|entry| self.decode_entry(entry, args, domain, max_literal_passes))
            .collect();

        // Sort attempts according to Pigweed collision priority rules (best first).
        attempts.sort_by(|a, b| b.cmp_priority(a));

        let is_ok = is_unambiguously_successful(&attempts);

        DetokenizedString {
            token: Some(token),
            matches: attempts,
            is_ok,
        }
    }

    fn decode_entry<'a>(
        &'a self,
        entry: &'a TokenizedStringEntry,
        args: &[u8],
        domain: &str,
        max_literal_passes: usize,
    ) -> DetokenizeAttempt<'a> {
        let format_string = match &entry.parsed_format_string {
            Some(fmt_str) => Cow::Borrowed(fmt_str),
            None => Cow::Owned(FormatString::parse_printf_lossy_with_prefix(
                &entry.format_string,
                self.prefix,
            )),
        };

        let decoded = Decoder::new(self).decode_with_domain_and_recursion(
            format_string,
            args,
            &entry.date_removed,
            domain,
            max_literal_passes,
        );
        DetokenizeAttempt::Success(decoded)
    }

    /// Detokenizes a parsed format string with arguments.
    pub fn detokenize_parsed(&self, fmt_str: &FormatString, arguments: &[u8]) -> Result<String> {
        let decoder = Decoder::new(self);
        decoder.decode(fmt_str, arguments, "").result()
    }

    /// Detokenizes text containing tokenized strings in the default domain.
    #[must_use]
    pub fn detokenize_text(&self, text: &str) -> String {
        self.detokenize_text_with_domain(text, DEFAULT_DOMAIN)
    }

    /// Detokenizes text containing tokenized strings in a specified domain.
    #[must_use]
    pub fn detokenize_text_with_domain(&self, text: &str, domain: &str) -> String {
        self.detokenize_text_with_recursion(text, domain, DEFAULT_RECURSION_PASSES)
    }

    /// Detokenizes text containing tokenized strings in a specified domain with up to `max_passes` of recursive literal token expansion.
    #[must_use]
    pub fn detokenize_text_with_recursion(
        &self,
        text: &str,
        domain: &str,
        max_passes: usize,
    ) -> String {
        Decoder::new(self).expand_literal_tokens(text, domain, self.prefix, max_passes)
    }

    fn resolve_entry(&self, token: u32, domain: &str) -> Option<&TokenizedStringEntry> {
        let entries = self.database.lookup(token, domain);
        match entries {
            [] => None,
            [single] => Some(single),
            entries => {
                let mut active_entries =
                    entries.iter().filter(|entry| entry.date_removed.is_empty());
                if let Some(first_active) = active_entries.next() {
                    return active_entries.next().is_none().then_some(first_active);
                }

                // If all entries were removed, favor the one removed most recently,
                // provided there is no tie for the most recent removal date.
                let latest_date = entries.iter().map(|entry| &entry.date_removed).max()?;
                let mut latest_entries = entries
                    .iter()
                    .filter(|entry| &entry.date_removed == latest_date);
                let latest_entry = latest_entries.next()?;
                latest_entries.next().is_none().then_some(latest_entry)
            }
        }
    }
}

impl TokenResolver for Detokenizer {
    fn resolve_format_string(&self, token: u32, domain: &str) -> Option<&str> {
        self.resolve_entry(token, domain)
            .map(|e| e.format_string.as_str())
    }

    fn resolve_parsed_format_string(&self, token: u32, domain: &str) -> Option<&FormatString> {
        self.resolve_entry(token, domain)
            .and_then(|e| e.parsed_format_string.as_ref())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_from_csv_different_domains() {
        let csv = "1,,domain1,Hello\n\
                   2,,domain2,\n\
                   3,,domain3,World!\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        assert_eq!(detok.database_lookup(1, "domain1").len(), 1);
        assert_eq!(detok.database_lookup(2, "domain2").len(), 1);
        assert_eq!(detok.database_lookup(3, "domain3").len(), 1);
    }

    #[test]
    fn test_detokenize_with_domain() {
        let csv = "1,,domain1,Hello\n\
                   1,,domain2,World\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        let encoded = 1u32.to_le_bytes();
        let result = detok.detokenize_with_domain(&encoded, "domain1");
        assert_eq!(result.best_string(), "Hello");

        let result = detok.detokenize_with_domain(&encoded, "domain2");
        assert_eq!(result.best_string(), "World");
    }

    #[test]
    fn test_detokenize_best_string_missing_token_is_empty() {
        let detok = Detokenizer::from_csv("").unwrap();
        let result = detok.detokenize(&[]);
        assert_eq!(result.best_string(), "");
        assert_eq!(result.token, None);
    }

    #[test]
    fn test_detokenize_best_string_unknown_token_is_empty() {
        let detok = Detokenizer::from_csv("").unwrap();
        let encoded = 1u32.to_le_bytes();
        let result = detok.detokenize(&encoded);
        assert_eq!(result.best_string(), "");
        assert_eq!(result.token, Some(1));
    }

    #[test]
    fn test_detokenize_with_args_single_match() {
        let csv = "00000001,,,Hello %d\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        let mut encoded = 1u32.to_le_bytes().to_vec();
        encoded.push(84); // Varint encoded 42
        let result = detok.detokenize(&encoded);
        assert_eq!(result.matches.len(), 1);
        assert_eq!(result.best_string(), "Hello 42");
    }

    #[test]
    fn test_detokenize_parsed() {
        let detok = Detokenizer::from_csv("").unwrap();
        let fmt = FormatString::parse_printf("Parsed %d").unwrap();
        let args = [84]; // Varint encoded 42
        let result = detok.detokenize_parsed(&fmt, &args).unwrap();
        assert_eq!(result, "Parsed 42");
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
        assert_eq!(result.best_string(), "crocodile 42");
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
        assert_eq!(result.best_string(), "crocodile 42");
    }

    #[test]
    fn test_detokenize_collisions_prefer_active() {
        let csv = "00000001,2001-01-01,,crocodile\n\
                   00000001,,,alligator\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        let encoded = 1u32.to_le_bytes();
        let result = detok.detokenize(&encoded);
        assert!(result.is_ok);
        assert_eq!(result.best_string(), "alligator");
    }

    #[test]
    fn test_detokenize_collisions_prefer_newer_date() {
        let csv = "00000001,2001-01-01,,crocodile\n\
                   00000001,2002-01-01,,alligator\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        let encoded = 1u32.to_le_bytes();
        let result = detok.detokenize(&encoded);
        assert!(result.is_ok);
        assert_eq!(result.best_string(), "alligator");
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
        assert_eq!(result.best_string(), "crocodile 42 42");
    }

    #[test]
    fn test_detokenize_extra_data_is_unsuccessful() {
        let csv = "00000001,,,Hello\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        let mut encoded = 1u32.to_le_bytes().to_vec();
        encoded.push(84);
        let result = detok.detokenize(&encoded);
        assert!(!result.is_ok);
        assert_eq!(result.best_string(), "Hello");
        assert_eq!(
            result.matches[0].result(),
            Err(pw_status::Error::InvalidArgument)
        );
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
    fn test_custom_prefix() {
        let csv = "00000001,,,This is a ~#00000002\n\
                   00000002,,,nested argument!\n";
        let mut detok = Detokenizer::from_csv_with_prefix(csv, '~').unwrap();
        assert_eq!(detok.prefix(), '~');
        let result = detok.detokenize_text("~#00000001");
        assert_eq!(result, "This is a nested argument!");

        detok.set_prefix('$');
        assert_eq!(detok.prefix(), '$');
    }

    #[test]
    fn test_detokenize_from_elf_section() {
        let mut binary_data = Vec::new();
        let domain = b"\0";
        let string = b"hello %d\0";
        binary_data.extend_from_slice(&pw_tokenizer_core::TOKENIZER_ENTRY_MAGIC.to_le_bytes());
        binary_data.extend_from_slice(&0x12345678u32.to_le_bytes());
        binary_data.extend_from_slice(&u32::try_from(domain.len()).unwrap().to_le_bytes());
        binary_data.extend_from_slice(&u32::try_from(string.len()).unwrap().to_le_bytes());
        binary_data.extend_from_slice(domain);
        binary_data.extend_from_slice(string);

        let detok = Detokenizer::from_elf_section(&binary_data).unwrap();
        let mut encoded = 0x12345678u32.to_le_bytes().to_vec();
        encoded.extend_from_slice(&[0x08]); // 4 (zigzag encoded)
        let result = detok.detokenize(&encoded);
        assert_eq!(result.best_string(), "hello 4");
    }

    #[test]
    fn test_detokenizer_clone_and_debug() {
        let csv = "12345678,,,Hello\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        let cloned = detok.clone();
        assert_eq!(cloned.prefix(), '$');
        let debug_str = format!("{cloned:?}");
        assert!(debug_str.contains("Detokenizer"));
    }

    #[test]
    fn test_detokenize_text_deeply_nested_and_percent() {
        let csv = "00000001,,,$10#0000000005\n\
                   00000002,,,This is a $#00000004\n\
                   00000003,,,deeply nested 100% argument.\n\
                   00000004,,,$AQAAAA==\n\
                   00000005,,,$AwAAAA==\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        let result = detok.detokenize_text("50% start: This is a $#00000004");
        assert_eq!(result, "50% start: This is a deeply nested 100% argument.");
    }

    #[test]
    fn test_detokenize_binary_literal_recursion_optional() {
        let csv = "00000001,,,Message: $#00000002\n\
                   00000002,,,Pass1 -> $#00000003\n\
                   00000003,,,Pass2 -> $#00000004\n\
                   00000004,,,Pass3 -> $#00000005\n\
                   00000005,,,Pass4 -> $#00000006\n\
                   00000006,,,Pass5\n";
        let detok = Detokenizer::from_csv(csv).unwrap();
        let encoded = 1u32.to_le_bytes();

        // Default on-device detokenize only expands literal token in the format string once (1 pass).
        let default_res = detok.detokenize(&encoded);
        assert!(default_res.is_ok);
        assert_eq!(default_res.best_string(), "Message: Pass1 -> $#00000003");

        // Offline detokenize_recursive expands literal tokens up to DEFAULT_RECURSION_PASSES (4 passes).
        let recursive_res = detok.detokenize_recursive(&encoded);
        assert!(recursive_res.is_ok);
        assert_eq!(
            recursive_res.best_string(),
            "Message: Pass1 -> Pass2 -> Pass3 -> Pass4 -> $#00000006"
        );
    }

    #[test]
    fn test_detokenize_preparsed_vs_on_demand() {
        let csv = "00000001,,,Hello %s (%d)!\n\
                   00000002,,,Nested: $#%08x\n";
        let detok_on_demand = Detokenizer::from_csv(csv).unwrap();
        let detok_preparsed = Detokenizer::from_csv_preparsed(csv).unwrap();

        assert!(detok_on_demand.database_lookup(1, "")[0]
            .parsed_format_string
            .is_none());
        assert!(detok_preparsed.database_lookup(1, "")[0]
            .parsed_format_string
            .is_some());

        let mut msg = Vec::new();
        msg.extend_from_slice(&1u32.to_le_bytes());
        msg.push(5);
        msg.extend_from_slice(b"world");
        msg.push(84); // zigzag for 42

        let res_on_demand = detok_on_demand.detokenize(&msg);
        let res_preparsed = detok_preparsed.detokenize(&msg);

        assert_eq!(res_on_demand.best_string(), "Hello world (42)!");
        assert_eq!(res_preparsed.best_string(), "Hello world (42)!");
        assert!(matches!(
            res_on_demand.matches[0],
            DetokenizeAttempt::Success(ref d) if matches!(d.fmt_str, Cow::Owned(_))
        ));
        assert!(matches!(
            res_preparsed.matches[0],
            DetokenizeAttempt::Success(ref d) if matches!(d.fmt_str, Cow::Borrowed(_))
        ));
    }
}
