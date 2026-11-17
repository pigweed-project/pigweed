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

//! Decoding metrics and decoded format string result types.

use core::cmp::Ordering;
use std::borrow::Cow;

use pw_format::{Arg, FormatFragment, FormatString, FormatStyle};
use pw_status::{Error, Result as StatusResult};

use super::error::{RawSpecErrorFormatter, TokenizerError, TokenizerErrorFormatter};

/// Decoding metrics for a detokenization attempt.
#[derive(Clone, Debug, Default, PartialEq, Eq, Hash)]
pub struct DecodeMetrics {
    /// Number of bytes consumed from the payload.
    pub bytes_consumed: usize,
    /// Number of unconsumed bytes remaining.
    pub remaining_bytes: usize,
    /// Number of arguments successfully decoded.
    pub arguments_decoded: usize,
    /// Number of decoding errors.
    pub decoding_errors: usize,
    /// Number of missing arguments.
    pub missing_args: usize,
    /// Number of skipped arguments.
    pub skipped_args: usize,
    /// True if the message decoded completely and unambiguously with zero errors.
    pub is_ok: bool,
}

impl DecodeMetrics {
    /// Returns total number of errors (decoding errors + missing arguments + skipped arguments).
    #[must_use]
    pub const fn total_errors(&self) -> usize {
        self.decoding_errors + self.missing_args + self.skipped_args
    }

    /// Finalizes `bytes_consumed`, `remaining_bytes`, and `is_ok` after decoding a payload.
    #[inline(always)]
    pub(crate) fn finalize(&mut self, payload_len: usize, remaining_bytes: usize) {
        self.bytes_consumed = payload_len - remaining_bytes;
        self.remaining_bytes = remaining_bytes;
        self.is_ok = self.decoding_errors == 0 && self.missing_args == 0 && remaining_bytes == 0;
    }
}

/// A decoded format string along with its decoded arguments and metrics.
///
/// # Lifetimes
///
/// * `'a` - The lifetime of the parsed format string and removal date borrowed
///   from the token database.
#[derive(Clone, Debug, PartialEq)]
pub struct DecodedFormatString<'a> {
    /// The parsed format string.
    pub fmt_str: Cow<'a, FormatString>,
    /// The decoded arguments, including any errors.
    pub decoded_args: Vec<core::result::Result<Arg, TokenizerError>>,
    /// Decoding metrics.
    pub metrics: DecodeMetrics,
    /// Date removed timestamp (for collision sorting).
    pub date_removed: Cow<'a, str>,
}

impl<'a> DecodedFormatString<'a> {
    /// Converts this decoded format string into an owned `'static` instance.
    #[must_use]
    pub fn into_owned(self) -> DecodedFormatString<'static> {
        DecodedFormatString {
            fmt_str: Cow::Owned(self.fmt_str.into_owned()),
            decoded_args: self.decoded_args,
            metrics: self.metrics,
            date_removed: Cow::Owned(self.date_removed.into_owned()),
        }
    }

    /// Returns true if decoding succeeded without errors or leftover unconsumed bytes.
    #[must_use]
    pub fn is_successful(&self) -> bool {
        self.metrics.is_ok
    }

    /// Returns the number of bytes that remained after decoding.
    #[must_use]
    pub fn remaining_bytes(&self) -> usize {
        self.metrics.remaining_bytes
    }

    /// Returns the number of arguments that failed to decode.
    #[must_use]
    pub fn decoding_errors(&self) -> usize {
        self.metrics.total_errors()
    }

    /// Returns the number of conversion specifiers in the format string.
    #[must_use]
    pub fn argument_count(&self) -> usize {
        self.fmt_str
            .fragments
            .iter()
            .filter(|f| {
                matches!(
                    f,
                    FormatFragment::Conversion(_) | FormatFragment::NestedToken(_)
                )
            })
            .count()
    }

    /// Returns the formatted string if successful, or `Error::InvalidArgument` on error.
    pub fn result(&self) -> StatusResult<String> {
        if !self.is_successful() {
            return Err(Error::InvalidArgument);
        }
        Ok(self.value())
    }

    /// Returns the decoded format string, keeping conversion specifiers for any arguments that failed to decode.
    #[must_use]
    pub fn value(&self) -> String {
        self.fmt_str.format_with_errors(
            &self.decoded_args,
            FormatStyle::Printf,
            &RawSpecErrorFormatter,
        )
    }

    /// Returns the decoded format string, with error messages for any arguments that failed to decode.
    #[must_use]
    pub fn value_with_errors(&self) -> String {
        self.fmt_str.format_with_errors(
            &self.decoded_args,
            FormatStyle::Printf,
            &TokenizerErrorFormatter,
        )
    }

    /// Compares two match results according to Pigweed collision resolution rules.
    #[must_use]
    pub fn cmp_priority(&self, other: &Self) -> Ordering {
        self.is_successful()
            .cmp(&other.is_successful())
            .then_with(|| {
                (self.metrics.remaining_bytes == 0).cmp(&(other.metrics.remaining_bytes == 0))
            })
            .then_with(|| {
                other
                    .metrics
                    .total_errors()
                    .cmp(&self.metrics.total_errors())
            })
            .then_with(|| self.argument_count().cmp(&other.argument_count()))
            .then_with(|| cmp_removal_date(&self.date_removed, &other.date_removed))
    }
}

fn cmp_removal_date(a: &str, b: &str) -> Ordering {
    match (a.is_empty(), b.is_empty()) {
        (true, false) => Ordering::Greater,
        (false, true) => Ordering::Less,
        _ => a.cmp(b),
    }
}
