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

//! Per-invocation decoding state machine for format string fragments.

use std::borrow::Cow;

use pw_format::{
    Arg, ConversionSpec, FormatString, MinFieldWidth, NestedTokenSpec, NestedTokenValue, Precision,
    Primitive, MAX_FIELD_LEN,
};

use super::{
    decode_char, decode_float, decode_pointer, decode_signed_int, decode_string,
    decode_unsigned_int, DecodeMetrics, DecodedFormatString, Decoder, RecursionContext,
    TokenizerError,
};

/// Ephemeral state machine tracking cursor position, decoded arguments, and error
/// state while decoding a single format string's fragments.
///
/// # Lifetimes
///
/// * `'sess` - The duration of the active decoding call borrowing the decoder,
///   cursor, argument vector, and metrics.
/// * `'data` - The lifetime of the binary argument payload slice being consumed.
/// * `'dec` - The lifetime of the [`TokenResolver`](super::TokenResolver) referenced by the [`Decoder`].
pub(super) struct DecodingSession<'sess, 'data, 'dec> {
    /// Reference to the parent argument decoder and token resolver.
    pub(super) decoder: &'sess Decoder<'dec>,
    /// Mutable cursor into the remaining binary payload bytes.
    pub(super) cursor: &'sess mut &'data [u8],
    /// Accumulated decoded arguments or per-specifier decoding errors.
    pub(super) decoded_args: &'sess mut Vec<Result<Arg, TokenizerError>>,
    /// Accumulated decoding metrics for the current format string.
    pub(super) metrics: &'sess mut DecodeMetrics,
    /// Current recursion depth and default domain context.
    pub(super) rec_ctx: RecursionContext<'sess>,
    /// True once a fatal argument decoding error has occurred, causing subsequent
    /// specifiers in this format string to be marked as [`TokenizerError::Skipped`].
    pub(super) has_error: bool,
}

impl<'sess, 'data, 'dec> DecodingSession<'sess, 'data, 'dec> {
    fn record_success(&mut self, arg: Arg) {
        self.metrics.arguments_decoded += 1;
        self.decoded_args.push(Ok(arg));
    }

    fn record_error(&mut self, err: TokenizerError) {
        self.record_error_with_flag(err, true);
    }

    fn record_error_without_skipping(&mut self, err: TokenizerError) {
        self.record_error_with_flag(err, false);
    }

    fn record_error_with_flag(&mut self, err: TokenizerError, set_error: bool) {
        match &err {
            TokenizerError::Missing => self.metrics.missing_args += 1,
            TokenizerError::Skipped => self.metrics.skipped_args += 1,
            _ => self.metrics.decoding_errors += 1,
        }
        self.decoded_args.push(Err(err));
        if set_error {
            self.has_error = true;
        }
    }

    fn record_decoded_arg(&mut self, result: Result<Arg, TokenizerError>) {
        match result {
            Ok(arg) => self.record_success(arg),
            Err(err) => self.record_error(err),
        }
    }

    fn record_skipped(&mut self) {
        self.record_error_with_flag(TokenizerError::Skipped, false);
    }

    fn resolve_and_parse_child(
        &self,
        token: u32,
        domain: &str,
        prefix: char,
    ) -> Option<Cow<'dec, FormatString>> {
        if let Some(parsed) = self
            .decoder
            .resolver
            .resolve_parsed_format_string(token, domain)
        {
            return Some(Cow::Borrowed(parsed));
        }
        let child_fmt_str = self.decoder.resolver.resolve_format_string(token, domain)?;
        Some(Cow::Owned(FormatString::parse_printf_lossy_with_prefix(
            child_fmt_str,
            prefix,
        )))
    }

    fn handle_child_decoded(
        &mut self,
        child: DecodedFormatString<'_>,
        consumes_parent_cursor: bool,
    ) {
        self.metrics.arguments_decoded += child.metrics.arguments_decoded;
        self.metrics.decoding_errors += child.metrics.decoding_errors;
        self.metrics.missing_args += child.metrics.missing_args;
        self.metrics.skipped_args += child.metrics.skipped_args;

        let succeeded = if consumes_parent_cursor {
            child.metrics.total_errors() == 0
        } else {
            child.is_successful()
        };

        if succeeded {
            self.record_success(Arg::Str(child.value()));
            return;
        }

        if child.metrics.total_errors() == 0 {
            self.metrics.decoding_errors += 1;
        }
        if consumes_parent_cursor {
            self.has_error = true;
        }
        let child_raw = child.value();
        let child_with_errors = child.value_with_errors();
        self.decoded_args.push(Err(TokenizerError::Nested {
            raw: child_raw,
            with_errors: child_with_errors,
        }));
    }

    pub(super) fn decode_nested_token(&mut self, token_spec: &NestedTokenSpec) {
        if self.has_error {
            self.record_skipped();
            return;
        }

        let target_domain = match self.decoder.resolve_nested_domain(
            &token_spec.domain,
            self.cursor,
            self.rec_ctx.default_domain,
        ) {
            Ok(domain) => domain,
            Err(err) => {
                self.record_error(err);
                return;
            }
        };

        match &token_spec.token {
            NestedTokenValue::Literal(token) => {
                self.decode_nested_literal_token(
                    *token,
                    token_spec.prefix,
                    &target_domain,
                    token_spec.token_data.as_deref(),
                );
            }
            NestedTokenValue::Variable => {
                // Spawning a child format string executes at depth + 1. Prevent descending
                // into a child if the current depth is already at or above max_depth.
                if self.rec_ctx.depth >= self.rec_ctx.max_depth {
                    self.record_error(TokenizerError::DecodeError);
                    return;
                }
                self.decode_nested_variable(token_spec.prefix, &target_domain);
            }
        }
    }

    fn expand_literal_if_needed(&self, text: String, domain: &str, prefix: char) -> String {
        if self.rec_ctx.max_literal_passes > 1 {
            self.decoder.expand_literal_tokens(
                &text,
                domain,
                prefix,
                self.rec_ctx.max_literal_passes - 1,
            )
        } else {
            text
        }
    }

    fn decode_nested_literal_with_payload(
        &mut self,
        token: u32,
        prefix: char,
        domain: &str,
        payload: &[u8],
    ) {
        if self.rec_ctx.depth >= self.rec_ctx.max_depth {
            self.record_error_without_skipping(TokenizerError::DecodeError);
            return;
        }
        let Some(child_decoded) = self.decode_child_format(token, domain, prefix, payload) else {
            self.record_error_without_skipping(TokenizerError::DecodeError);
            return;
        };
        if child_decoded.is_successful() && self.rec_ctx.max_literal_passes > 1 {
            let expanded = self.expand_literal_if_needed(child_decoded.value(), domain, prefix);
            self.metrics.arguments_decoded += child_decoded.metrics.arguments_decoded;
            self.record_success(Arg::Str(expanded));
            return;
        }
        self.handle_child_decoded(child_decoded, false);
    }

    fn decode_nested_literal_token(
        &mut self,
        token: u32,
        prefix: char,
        domain: &str,
        token_data: Option<&[u8]>,
    ) {
        if let Some(payload) = token_data.filter(|b| !b.is_empty()) {
            self.decode_nested_literal_with_payload(token, prefix, domain, payload);
            return;
        }

        // Literal token with no argument payload (Base10/Base16 or 4-byte Base64):
        // Lookup raw string from database without treating it as a printf format string.
        let Some(resolved) = self.decoder.resolver.resolve_format_string(token, domain) else {
            self.record_error_without_skipping(TokenizerError::DecodeError);
            return;
        };

        let expanded = self.expand_literal_if_needed(resolved.to_string(), domain, prefix);
        self.record_success(Arg::Str(expanded));
    }

    fn decode_child_format(
        &self,
        token: u32,
        domain: &str,
        prefix: char,
        payload: &[u8],
    ) -> Option<DecodedFormatString<'dec>> {
        let child_fmt = self.resolve_and_parse_child(token, domain, prefix)?;
        Some(
            self.decoder
                .decode_with_depth(child_fmt, payload, "", self.rec_ctx.child(domain)),
        )
    }

    fn decode_child_from_cursor(&mut self, token: u32, domain: &str, prefix: char) {
        let Some(child_decoded) = self.decode_child_format(token, domain, prefix, self.cursor)
        else {
            self.record_error(TokenizerError::DecodeError);
            return;
        };

        *self.cursor = &(*self.cursor)[child_decoded.metrics.bytes_consumed..];
        self.handle_child_decoded(child_decoded, true);
    }

    fn decode_nested_variable(&mut self, prefix: char, domain: &str) {
        match self.decoder.decode_token_arg(self.cursor, domain) {
            Ok(token) => self.decode_child_from_cursor(token, domain, prefix),
            Err(err) => self.record_error(err),
        }
    }

    fn is_valid_dynamic_width(val: i64) -> bool {
        val.unsigned_abs() <= u64::from(MAX_FIELD_LEN)
    }

    fn is_valid_dynamic_precision(val: i64) -> bool {
        // Per C99 §7.19.6.1p5, a negative `int` precision is treated as omitted.
        // Require `i32` range so values below `i32::MIN` are rejected as corrupted.
        if val < 0 {
            i32::try_from(val).is_ok()
        } else {
            val.cast_unsigned() <= u64::from(MAX_FIELD_LEN)
        }
    }

    fn decode_dynamic_field(
        &mut self,
        is_dynamic: bool,
        is_valid: impl FnOnce(i64) -> bool,
    ) -> bool {
        if !is_dynamic {
            return true;
        }
        let val = match decode_signed_int(self.cursor) {
            Ok(val) => val,
            Err(err) => {
                self.record_error(err);
                return false;
            }
        };
        if !is_valid(val) {
            self.record_error(TokenizerError::DecodeError);
            return false;
        }
        self.record_success(Arg::Int(val));
        true
    }

    pub(super) fn decode_conversion(&mut self, spec: &ConversionSpec) {
        if self.has_error {
            self.record_skipped();
            return;
        }

        if !spec.has_valid_field_lengths() {
            self.record_error(TokenizerError::DecodeError);
            return;
        }

        if !self.decode_dynamic_field(
            spec.min_field_width == MinFieldWidth::Variable,
            Self::is_valid_dynamic_width,
        ) || !self.decode_dynamic_field(
            spec.precision == Precision::Variable,
            Self::is_valid_dynamic_precision,
        ) {
            return;
        }

        let decoded = match spec.primitive {
            Primitive::Integer => decode_signed_int(self.cursor).map(Arg::Int),
            Primitive::Unsigned => decode_unsigned_int(self.cursor).map(Arg::Uint),
            Primitive::Float => decode_float(self.cursor).map(Arg::Float),
            Primitive::String => decode_string(self.cursor).map(Arg::Str),
            Primitive::Character => decode_char(self.cursor).map(Arg::Char),
            Primitive::Pointer => decode_pointer(self.cursor).map(Arg::Ptr),
            Primitive::Dynamic => Err(TokenizerError::DecodeError),
        };
        self.record_decoded_arg(decoded);
    }
}
