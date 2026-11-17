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

//! Error types and `pw_format::FormatError` implementations for detokenization.

use pw_format::{Arg, ConversionSpec, FormatError, NestedTokenSpec};

/// Errors that can occur when decoding or formatting tokenized arguments.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum TokenizerError {
    /// The argument was missing from the encoded payload.
    Missing,
    /// The argument could not be decoded successfully.
    DecodeError,
    /// The argument could not be decoded successfully, with partial detail (e.g. truncated string).
    DecodeErrorWithDetail(String),
    /// The argument was skipped because a previous error occurred.
    Skipped,
    /// A nested format string whose decoding encountered errors or missing arguments.
    Nested {
        /// Raw representation preserving unparsed conversion specifiers.
        raw: String,
        /// Formatted representation with error placeholders.
        with_errors: String,
    },
}

/// Formats a standard Pigweed tokenizer error placeholder.
///
/// Example: `<[%d MISSING]>`, `<[%s ERROR]>`, `<[%s ERROR (bad utf8)]>`, `<[%s SKIPPED]>`.
#[must_use]
pub fn format_error_placeholder(spec: &str, status_text: &str, val_detail: Option<&str>) -> String {
    if let Some(detail) = val_detail.filter(|s| !s.is_empty()) {
        return format!("<[{spec} {status_text} ({detail})]>");
    }
    format!("<[{spec} {status_text}]>")
}

pub(crate) fn format_spec_error(spec_str: &str, error: &TokenizerError) -> String {
    match error {
        TokenizerError::Nested { with_errors, .. } => with_errors.clone(),
        TokenizerError::Missing => format_error_placeholder(spec_str, "MISSING", None),
        TokenizerError::DecodeError => format_error_placeholder(spec_str, "ERROR", None),
        TokenizerError::DecodeErrorWithDetail(detail) => {
            format_error_placeholder(spec_str, "ERROR", Some(detail))
        }
        TokenizerError::Skipped => format_error_placeholder(spec_str, "SKIPPED", None),
    }
}

/// Formatter for detokenizer formatting errors, producing `<[spec ERROR]>`, `<[spec MISSING]>`, or `<[spec SKIPPED]>`.
pub struct TokenizerErrorFormatter;

impl FormatError for TokenizerErrorFormatter {
    type Error = TokenizerError;

    fn format_error(&self, spec: &ConversionSpec, error: &TokenizerError) -> String {
        format_spec_error(&spec.to_printf(), error)
    }

    fn format_missing(&self, spec: &ConversionSpec) -> String {
        format_error_placeholder(&spec.to_printf(), "MISSING", None)
    }

    fn format_type_error(&self, spec: &ConversionSpec, _arg: &Arg) -> String {
        format_error_placeholder(&spec.to_printf(), "ERROR", None)
    }

    fn format_nested_token_error(&self, spec: &NestedTokenSpec, error: &TokenizerError) -> String {
        format_spec_error(&spec.to_printf(), error)
    }

    fn format_nested_token_missing(&self, spec: &NestedTokenSpec) -> String {
        format_error_placeholder(&spec.to_printf(), "MISSING", None)
    }

    fn format_nested_token_type_error(&self, spec: &NestedTokenSpec, _arg: &Arg) -> String {
        format_error_placeholder(&spec.to_printf(), "ERROR", None)
    }
}

fn format_raw_spec_error(spec_str: &str, error: &TokenizerError) -> String {
    match error {
        TokenizerError::Nested { raw, .. } => raw.clone(),
        _ => spec_str.to_string(),
    }
}

/// Formatter that preserves the raw conversion specifier (e.g. `%d`) for missing, skipped, or failed arguments.
pub struct RawSpecErrorFormatter;

impl FormatError for RawSpecErrorFormatter {
    type Error = TokenizerError;

    fn format_error(&self, spec: &ConversionSpec, error: &TokenizerError) -> String {
        format_raw_spec_error(&spec.to_printf(), error)
    }

    fn format_missing(&self, spec: &ConversionSpec) -> String {
        spec.to_printf()
    }

    fn format_type_error(&self, spec: &ConversionSpec, _arg: &Arg) -> String {
        spec.to_printf()
    }

    fn format_nested_token_error(&self, spec: &NestedTokenSpec, error: &TokenizerError) -> String {
        format_raw_spec_error(&spec.to_printf(), error)
    }

    fn format_nested_token_missing(&self, spec: &NestedTokenSpec) -> String {
        spec.to_printf()
    }

    fn format_nested_token_type_error(&self, spec: &NestedTokenSpec, _arg: &Arg) -> String {
        spec.to_printf()
    }
}
