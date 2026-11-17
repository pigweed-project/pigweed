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

//! Recursive format string and nested token decoding engine for `pw_tokenizer`.

use std::borrow::Cow;

pub use pw_format::{canonicalize_domain, canonicalize_domain_cow};
use pw_format::{Arg, FormatFragment, FormatString, NestedDomain};

pub use super::decoded::{DecodeMetrics, DecodedFormatString};
pub use super::error::{
    format_error_placeholder, RawSpecErrorFormatter, TokenizerError, TokenizerErrorFormatter,
};
use super::primitives::{
    decode_char, decode_float, decode_pointer, decode_signed_int, decode_string,
    decode_unsigned_int,
};
use super::varint::{decode_unsigned_varint, decode_varint, ArgStatus};

/// Maximum nested recursion depth for nested format strings (variable tokens).
pub const MAX_RECURSION_DEPTH: usize = 16;

/// Default number of passes for recursive literal token text expansion.
pub const DEFAULT_RECURSION_PASSES: usize = 4;

/// Trait for resolving format strings for tokens in a domain.
pub trait TokenResolver {
    /// Resolves the format string for a token in a domain.
    fn resolve_format_string(&self, token: u32, domain: &str) -> Option<&str>;

    /// Resolves a pre-parsed format string for a token in a domain, if available.
    fn resolve_parsed_format_string(&self, _token: u32, _domain: &str) -> Option<&FormatString> {
        None
    }
}

/// An empty token resolver that resolves nothing.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct EmptyResolver;

impl TokenResolver for EmptyResolver {
    fn resolve_format_string(&self, _token: u32, _domain: &str) -> Option<&str> {
        None
    }
}

mod session;

use session::DecodingSession;

/// Binary argument decoder for tokenized format strings.
///
/// # Lifetimes
///
/// * `'a` - The lifetime of the [`TokenResolver`] used to look up nested token
///   format strings and domains.
#[derive(Clone, Copy)]
pub struct Decoder<'a> {
    /// Resolver used to look up nested token IDs and domain format strings.
    resolver: &'a dyn TokenResolver,
}

impl core::fmt::Debug for Decoder<'_> {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("Decoder").finish_non_exhaustive()
    }
}

/// Recursion state and domain configuration carried across nested token expansions.
///
/// # Lifetimes
///
/// * `'b` - The lifetime of the default domain name slice active for the current
///   format string evaluation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct RecursionContext<'b> {
    /// Default domain name used when a nested token omits an explicit domain.
    default_domain: &'b str,
    /// Current variable-token recursion depth (`0` at the root format string).
    depth: usize,
    /// Maximum allowed variable-token recursion depth before failing with a decode error.
    max_depth: usize,
    /// Maximum number of iterative literal-token text expansion passes.
    max_literal_passes: usize,
}

impl<'b> RecursionContext<'b> {
    const fn child<'c>(&'c self, domain: &'c str) -> RecursionContext<'c> {
        RecursionContext {
            default_domain: domain,
            depth: self.depth + 1,
            max_depth: self.max_depth,
            max_literal_passes: self.max_literal_passes,
        }
    }
}

impl<'a> Decoder<'a> {
    /// Creates a new decoder with the given token resolver.
    #[must_use]
    pub const fn new(resolver: &'a dyn TokenResolver) -> Self {
        Self { resolver }
    }

    /// Decodes binary arguments against the format string.
    #[must_use]
    pub fn decode<'b>(
        &self,
        format_string: &'b FormatString,
        payload: &[u8],
        date_removed: &'b str,
    ) -> DecodedFormatString<'b> {
        self.decode_with_domain_and_depth(
            format_string,
            payload,
            date_removed,
            "",
            MAX_RECURSION_DEPTH,
        )
    }

    /// Decodes binary arguments against the format string with a default domain.
    #[must_use]
    pub fn decode_with_domain<'b>(
        &self,
        format_string: &'b FormatString,
        payload: &[u8],
        date_removed: &'b str,
        default_domain: &str,
    ) -> DecodedFormatString<'b> {
        self.decode_with_domain_and_depth(
            format_string,
            payload,
            date_removed,
            default_domain,
            MAX_RECURSION_DEPTH,
        )
    }

    /// Decodes binary arguments against the format string with domain and max recursion depth.
    #[must_use]
    pub fn decode_with_domain_and_depth<'b>(
        &self,
        format_string: &'b FormatString,
        payload: &[u8],
        date_removed: &'b str,
        default_domain: &str,
        max_depth: usize,
    ) -> DecodedFormatString<'b> {
        self.decode_with_domain_depth_and_literal_passes(
            format_string,
            payload,
            date_removed,
            default_domain,
            max_depth,
            1,
        )
    }

    /// Decodes binary arguments against the format string with domain and max literal token recursion passes.
    #[must_use]
    pub fn decode_with_domain_and_recursion<'b>(
        &self,
        format_string: Cow<'b, FormatString>,
        payload: &[u8],
        date_removed: &'b str,
        default_domain: &str,
        max_literal_passes: usize,
    ) -> DecodedFormatString<'b> {
        let rec_ctx = RecursionContext {
            default_domain,
            depth: 0,
            max_depth: MAX_RECURSION_DEPTH,
            max_literal_passes,
        };
        self.decode_with_depth(format_string, payload, date_removed, rec_ctx)
    }

    /// Decodes binary arguments against the format string with domain, max format depth, and max literal token passes.
    #[must_use]
    pub fn decode_with_domain_depth_and_literal_passes<'b>(
        &self,
        format_string: &'b FormatString,
        payload: &[u8],
        date_removed: &'b str,
        default_domain: &str,
        max_depth: usize,
        max_literal_passes: usize,
    ) -> DecodedFormatString<'b> {
        let rec_ctx = RecursionContext {
            default_domain,
            depth: 0,
            max_depth,
            max_literal_passes,
        };
        self.decode_with_depth(Cow::Borrowed(format_string), payload, date_removed, rec_ctx)
    }

    /// Expands literal nested tokens in plain text up to `max_passes` times,
    /// treating `%` characters as literal text rather than conversion specifiers.
    #[must_use]
    pub fn expand_literal_tokens(
        &self,
        text: &str,
        default_domain: &str,
        prefix: char,
        max_passes: usize,
    ) -> String {
        if !text.contains(prefix) {
            return text.to_string();
        }
        let mut current = text.to_string();
        for _ in 0..max_passes {
            if !current.contains(prefix) {
                break;
            }
            let fmt_str = FormatString::parse_nested_tokens_with_prefix(&current, prefix);
            if !fmt_str
                .fragments
                .iter()
                .any(|f| matches!(f, FormatFragment::NestedToken(_)))
            {
                break;
            }
            let pass_ctx = RecursionContext {
                default_domain,
                depth: 0,
                max_depth: MAX_RECURSION_DEPTH,
                max_literal_passes: 1,
            };
            let next = self
                .decode_with_depth(Cow::Owned(fmt_str), &[], "", pass_ctx)
                .value();
            if next == current {
                break;
            }
            current = next;
        }
        current
    }

    fn decode_with_depth<'b>(
        &self,
        format_string: Cow<'b, FormatString>,
        payload: &[u8],
        date_removed: &'b str,
        rec_ctx: RecursionContext<'_>,
    ) -> DecodedFormatString<'b> {
        let mut cursor = payload;
        let mut decoded_args = Vec::with_capacity(format_string.fragments.len());
        let mut metrics = DecodeMetrics::default();

        self.decode_ctx(
            &format_string,
            &mut cursor,
            &mut decoded_args,
            &mut metrics,
            rec_ctx,
        );

        metrics.finalize(payload.len(), cursor.len());

        DecodedFormatString {
            fmt_str: format_string,
            decoded_args,
            metrics,
            date_removed: Cow::Borrowed(date_removed),
        }
    }

    fn resolve_domain_from_canonical_token(
        &self,
        token: u32,
        canonical_domain: &str,
    ) -> Result<String, TokenizerError> {
        let Some(resolved) = self.resolver.resolve_format_string(token, canonical_domain) else {
            return Err(TokenizerError::DecodeError);
        };
        Ok(canonicalize_domain(resolved))
    }

    fn resolve_domain_from_token(
        &self,
        token: u32,
        domain_str: &str,
        default_domain: &str,
    ) -> Result<String, TokenizerError> {
        let canonical = effective_domain(domain_str, default_domain);
        self.resolve_domain_from_canonical_token(token, &canonical)
    }

    fn resolve_nested_domain(
        &self,
        domain: &NestedDomain,
        cursor: &mut &[u8],
        default_domain: &str,
    ) -> Result<String, TokenizerError> {
        match domain {
            NestedDomain::Default => Ok(canonicalize_domain(default_domain)),
            NestedDomain::Literal(name) => Ok(canonicalize_domain(name)),
            NestedDomain::LiteralToken { domain, token } => {
                self.resolve_domain_from_token(*token, domain, default_domain)
            }
            NestedDomain::VariableToken { domain } => {
                let canonical = effective_domain(domain, default_domain);
                let token = self.decode_token_arg(cursor, &canonical)?;
                self.resolve_domain_from_canonical_token(token, &canonical)
            }
        }
    }

    fn decode_token_arg(&self, cursor: &mut &[u8], domain: &str) -> Result<u32, TokenizerError> {
        if cursor.is_empty() {
            return Err(TokenizerError::Missing);
        }

        // Tokenized arguments for `%08x` / `$#%08x` can be encoded in two ways:
        // - Zigzag-encoded varints (Pigweed's standard tokenized arg format for `%08x` / `%d`).
        // - Unsigned LEB128 varints (e.g. from `%u` or raw token values).
        //
        // A byte sequence such as `[0x02]` decodes to either zigzag 1 or unsigned 2.
        // To resolve this ambiguity deterministically, zigzag interpretation is
        // prioritized first: if the zigzag-decoded token exists in the database
        // for this domain, it is selected. If the zigzag lookup misses, unsigned
        // varint decoding and lookup is attempted as a fallback. If neither matches,
        // the zigzag token is preserved so error placeholders match standard `%08x`
        // tokenized argument expectations.
        let mut peek_cursor = *cursor;
        let (status, zigzag_val) = decode_varint(&mut peek_cursor);
        if status != ArgStatus::Ok {
            *cursor = peek_cursor;
            let err = match status {
                ArgStatus::Missing => TokenizerError::Missing,
                _ => TokenizerError::DecodeError,
            };
            return Err(err);
        }

        // Extract the lower 32 bits of the zigzag-decoded 64-bit integer.
        // 32-bit tokens with bit 31 set are sign-extended into `i32`/`i64` when
        // zigzag-encoded by C/C++/Rust tokenizers, so their lower 4 little-endian
        // bytes recover the original 32-bit token ID without truncation warnings.
        let [b0, b1, b2, b3, ..] = zigzag_val.to_le_bytes();
        let zigzag_token = u32::from_le_bytes([b0, b1, b2, b3]);
        if self
            .resolver
            .resolve_format_string(zigzag_token, domain)
            .is_some()
        {
            *cursor = peek_cursor;
            return Ok(zigzag_token);
        }

        // Fall back to checking raw unsigned varint if zigzag lookup missed.
        let mut raw_cursor = *cursor;
        let raw_token = match decode_unsigned_varint(&mut raw_cursor) {
            (ArgStatus::Ok, raw_val) => u32::try_from(raw_val).ok(),
            _ => None,
        };
        if let Some(token) =
            raw_token.filter(|&token| self.resolver.resolve_format_string(token, domain).is_some())
        {
            *cursor = raw_cursor;
            return Ok(token);
        }

        *cursor = peek_cursor;
        Ok(zigzag_token)
    }

    fn decode_ctx(
        &self,
        format_string: &FormatString,
        cursor: &mut &[u8],
        decoded_args: &mut Vec<Result<Arg, TokenizerError>>,
        metrics: &mut DecodeMetrics,
        rec_ctx: RecursionContext<'_>,
    ) {
        // The root format string begins at depth 0. Levels 0..=max_depth are valid
        // execution contexts. If recursion exceeds max_depth, abort this level.
        if rec_ctx.depth > rec_ctx.max_depth {
            metrics.decoding_errors += 1;
            decoded_args.push(Err(TokenizerError::DecodeError));
            return;
        }

        let mut session = DecodingSession {
            decoder: self,
            cursor,
            decoded_args,
            metrics,
            rec_ctx,
            has_error: false,
        };

        for fragment in &format_string.fragments {
            match fragment {
                FormatFragment::Literal(_) => continue,
                FormatFragment::NestedToken(token_spec) => {
                    session.decode_nested_token(token_spec);
                }
                FormatFragment::Conversion(spec) => {
                    session.decode_conversion(spec);
                }
            }
        }
    }
}

fn effective_domain(domain: &str, default_domain: &str) -> String {
    let target = if domain.is_empty() {
        default_domain
    } else {
        domain
    };
    canonicalize_domain(target)
}

#[cfg(test)]
mod tests {
    use std::collections::HashMap;

    use pw_varint::VarintEncode;

    use super::*;

    const TYPE_DOMAIN: &str = "type";

    struct MockTokenResolver {
        format_strings: HashMap<(u32, String), String>,
    }

    impl MockTokenResolver {
        fn new() -> Self {
            Self {
                format_strings: HashMap::new(),
            }
        }

        fn register(&mut self, token: u32, domain: &str, format_string: &str) {
            self.format_strings
                .insert((token, domain.to_string()), format_string.to_string());
        }
    }

    impl TokenResolver for MockTokenResolver {
        fn resolve_format_string(&self, token: u32, domain: &str) -> Option<&str> {
            self.format_strings
                .get(&(token, domain.to_string()))
                .map(|s| s.as_str())
        }
    }

    #[test]
    fn test_decoder_ambiguous_token_resolution() {
        let mut resolver = MockTokenResolver::new();
        // Token 100 (zigzag for 200) and Token 200 (unsigned varint for 200)
        resolver.register(100, "", "zigzag_match");
        resolver.register(200, "", "unsigned_match");
        // For fallback test: only token 202 is registered (token 101 is not registered)
        resolver.register(202, "", "unsigned_fallback");

        let decoder = Decoder::new(&resolver);
        let fmt = FormatString::parse_printf("Out: $#%08x").unwrap();

        // 200 encoded as varint: [0xc8, 0x01].
        // Both tokens exist: zigzag takes precedence.
        let ambiguous_payload = [0xc8, 0x01];
        let res = decoder.decode(&fmt, &ambiguous_payload, "");
        assert!(res.is_successful());
        assert_eq!(res.value(), "Out: zigzag_match");

        // When zigzag token (101 -> raw 202: [0xca, 0x01]) is NOT registered,
        // but unsigned token 202 is registered, fallback to unsigned succeeds.
        let fallback_payload = [0xca, 0x01];
        let res = decoder.decode(&fmt, &fallback_payload, "");
        assert!(res.is_successful());
        assert_eq!(res.value(), "Out: unsigned_fallback");
    }

    #[test]
    fn test_decoder_truncated_varint_eof() {
        let resolver = EmptyResolver;
        let decoder = Decoder::new(&resolver);
        let fmt = FormatString::parse_printf("Num: %d, Str: %s").unwrap();

        // 0x80 has continuation bit set, but slice ends immediately (EOF mid-varint).
        let payload = [0x80];
        let res = decoder.decode(&fmt, &payload, "");
        assert!(!res.is_successful());
        assert_eq!(res.value(), "Num: %d, Str: %s");
        assert_eq!(
            res.value_with_errors(),
            "Num: <[%d ERROR]>, Str: <[%s SKIPPED]>"
        );
    }

    #[test]
    fn test_decoder_truncated_string_eof() {
        let resolver = EmptyResolver;
        let decoder = Decoder::new(&resolver);
        let fmt = FormatString::parse_printf("Str: %s, Num: %d").unwrap();

        // Header 0x05 declares 5 bytes, but only 2 bytes follow before EOF.
        let payload = [0x05, b'h', b'i'];
        let res = decoder.decode(&fmt, &payload, "");
        assert!(!res.is_successful());
        assert_eq!(res.value(), "Str: %s, Num: %d");
        assert_eq!(
            res.value_with_errors(),
            "Str: <[%s ERROR (hi)]>, Num: <[%d SKIPPED]>"
        );
    }

    #[test]
    fn test_decoder_truncated_string_mid_utf8_preserves_valid_prefix() {
        let resolver = EmptyResolver;
        let decoder = Decoder::new(&resolver);
        let fmt = FormatString::parse_printf("Str: %s, Num: %d").unwrap();

        // Header 0x06 declares 6 bytes, but only 4 bytes follow ("hi " + incomplete '€' start byte 0xE2).
        let payload = [0x06, b'h', b'i', b' ', 0xe2];
        let res = decoder.decode(&fmt, &payload, "");
        assert!(!res.is_successful());
        assert_eq!(res.value(), "Str: %s, Num: %d");
        assert_eq!(
            res.value_with_errors(),
            "Str: <[%s ERROR (hi )]>, Num: <[%d SKIPPED]>"
        );
    }

    #[test]
    fn test_decoder_nested_type_primitives() {
        let mut resolver = MockTokenResolver::new();
        resolver.register(1, TYPE_DOMAIN, "%lld");
        resolver.register(2, TYPE_DOMAIN, "%s");

        let decoder = Decoder::new(&resolver);
        let fmt = FormatString::parse_printf("Value: ${type}#%08x").unwrap();

        // Signed int: type token 1 (zigzag 2 = 0x02), int -5 (zigzag 9 = 0x09)
        let payload = [0x02, 0x09];
        let res = decoder.decode(&fmt, &payload, "");
        assert!(res.is_successful());
        assert_eq!(res.value(), "Value: -5");

        // String: type token 2 (zigzag 4 = 0x04), string "hello" (len 5 = 0x05)
        let payload = [0x04, 0x05, b'h', b'e', b'l', b'l', b'o'];
        let res = decoder.decode(&fmt, &payload, "");
        assert!(res.is_successful());
        assert_eq!(res.value(), "Value: hello");
    }

    #[test]
    fn test_decoder_nested_type_recursive_struct() {
        let mut resolver = MockTokenResolver::new();
        resolver.register(1, TYPE_DOMAIN, "%d");
        resolver.register(10, TYPE_DOMAIN, "Point { x: %d, y: %d }");
        resolver.register(20, TYPE_DOMAIN, "Some(${type}#%08x)");
        resolver.register(21, TYPE_DOMAIN, "None");

        let decoder = Decoder::new(&resolver);
        let fmt = FormatString::parse_printf("Data: ${type}#%08x").unwrap();

        // Point { x: 10 (zigzag 20=0x14), y: 20 (zigzag 40=0x28) }
        let payload = [0x14, 0x14, 0x28];
        let res = decoder.decode(&fmt, &payload, "");
        assert!(res.is_successful());
        assert_eq!(res.value(), "Data: Point { x: 10, y: 20 }");

        // Some(100): token 20 (zigzag 40=0x28), then token 1 (zigzag 2=0x02), then 100 (zigzag 200 = 0xc8, 0x01)
        let payload = [0x28, 0x02, 0xc8, 0x01];
        let res = decoder.decode(&fmt, &payload, "");
        assert!(res.is_successful());
        assert_eq!(res.value(), "Data: Some(100)");

        // None: token 21 (zigzag 42=0x2a)
        let payload = [0x2a];
        let res = decoder.decode(&fmt, &payload, "");
        assert!(res.is_successful());
        assert_eq!(res.value(), "Data: None");
    }

    #[test]
    fn test_decoder_nested_type_unknown_token() {
        let resolver = MockTokenResolver::new();
        let decoder = Decoder::new(&resolver);
        let fmt = FormatString::parse_printf("Value: ${type}#%08x").unwrap();

        // Type token 99 is not registered
        let payload = [0x63, 0x01, 0x02];
        let res = decoder.decode(&fmt, &payload, "");
        assert!(!res.is_successful());
        assert_eq!(res.value_with_errors(), "Value: <[${type}#%08x ERROR]>");
        assert_eq!(res.value(), "Value: ${type}#%08x");
    }

    #[test]
    fn test_decoder_nested_type_recursion_limit() {
        let mut resolver = MockTokenResolver::new();
        resolver.register(1, TYPE_DOMAIN, "%d");
        resolver.register(20, TYPE_DOMAIN, "Some(${type}#%08x)");
        resolver.register(99, TYPE_DOMAIN, "${type}#%08x");

        let decoder = Decoder::new(&resolver);
        let fmt = FormatString::parse_printf("Data: ${type}#%08x").unwrap();

        // 16 levels: 15 'Some(${type}#%08x)' (token 20 = zigzag 40 = 0x28) followed by 1 '%d' (token 1 = zigzag 2 = 0x02) with int 42 (zigzag 84 = 0x54).
        // Depth 0: Data: ${type}#%08x
        // Depth 1..=15: Some(${type}#%08x)
        // Depth 16: %d (val 42)
        // Total depth reached is 16, which is <= MAX_RECURSION_DEPTH (16).
        let mut valid_payload = vec![0x28; 15];
        valid_payload.push(0x02);
        valid_payload.push(0x54);
        let res = decoder.decode(&fmt, &valid_payload, "");
        assert!(res.is_successful());
        let expected = format!("Data: {}42{}", "Some(".repeat(15), ")".repeat(15));
        assert_eq!(res.value(), expected);

        // 17 levels: 16 'Some(${type}#%08x)' followed by '%d' with int 42.
        // At depth 16, nested token encounters depth >= MAX_RECURSION_DEPTH and fails.
        let mut exceeded_payload = vec![0x28; 16];
        exceeded_payload.push(0x02);
        exceeded_payload.push(0x54);
        let res = decoder.decode(&fmt, &exceeded_payload, "");
        assert!(!res.is_successful());
        let expected_raw = format!(
            "Data: {}${{type}}#%08x{}",
            "Some(".repeat(16),
            ")".repeat(16)
        );
        let expected_err = format!(
            "Data: {}<[${{type}}#%08x ERROR]>{}",
            "Some(".repeat(16),
            ")".repeat(16)
        );
        assert_eq!(res.value(), expected_raw);
        assert_eq!(res.value_with_errors(), expected_err);

        // Infinite self-recursion: token 99 is "${type}#%08x" (zigzag 198 = [0xc6, 0x01]), payload has 30 copies of token 99.
        let cyclic_payload = [0xc6, 0x01].repeat(30);
        let res = decoder.decode(&fmt, &cyclic_payload, "");
        assert!(!res.is_successful());
        assert_eq!(res.value(), "Data: ${type}#%08x");
        assert_eq!(res.value_with_errors(), "Data: <[${type}#%08x ERROR]>");
    }

    #[test]
    fn test_decoder_error_skipping() {
        let resolver = MockTokenResolver::new();
        let decoder = Decoder::new(&resolver);
        let fmt = FormatString::parse_printf("A: %d, B: %s, C: %d").unwrap();

        // %d has corrupted 10-byte varint, so %s and second %d should be skipped
        let payload = vec![0x80; 10];
        let res = decoder.decode(&fmt, &payload, "");
        assert!(!res.is_successful());
        assert_eq!(
            res.value_with_errors(),
            "A: <[%d ERROR]>, B: <[%s SKIPPED]>, C: <[%d SKIPPED]>"
        );
        assert_eq!(res.value(), "A: %d, B: %s, C: %d");
    }

    #[test]
    fn test_decoder_star_width_and_precision() {
        let resolver = EmptyResolver;
        let decoder = Decoder::new(&resolver);

        // %*d: width 5 (zigzag 10 = 0x0a), val 42 (zigzag 84 = 0x54)
        let fmt = FormatString::parse_printf("Num: %*d").unwrap();
        let payload = [0x0a, 0x54];
        let res = decoder.decode(&fmt, &payload, "");
        assert!(res.is_successful());
        assert_eq!(res.value(), "Num:    42");

        // %*d with negative width: width -5 (zigzag 9 = 0x09), val 42
        let payload = [0x09, 0x54];
        let res = decoder.decode(&fmt, &payload, "");
        assert!(res.is_successful());
        assert_eq!(res.value(), "Num: 42   ");

        // %.*s: precision 3 (zigzag 6 = 0x06), string "hello" (len 5 = 0x05)
        let fmt = FormatString::parse_printf("Str: %.*s").unwrap();
        let payload = [0x06, 0x05, b'h', b'e', b'l', b'l', b'o'];
        let res = decoder.decode(&fmt, &payload, "");
        assert!(res.is_successful());
        assert_eq!(res.value(), "Str: hel");
    }

    #[test]
    fn test_decoder_nested_token_default_domain() {
        let mut resolver = MockTokenResolver::new();
        resolver.register(0x1234, "", "hello %d");

        let decoder = Decoder::new(&resolver);
        let fmt = FormatString::parse_printf("Token: $#%08x").unwrap();

        // 0x1234 as unsigned varint: [0xb4, 0x24]
        // int arg 42: zigzag 84 = 0x54
        let payload = [0xb4, 0x24, 0x54];
        let res = decoder.decode(&fmt, &payload, "");
        assert!(res.is_successful());
        assert_eq!(res.value(), "Token: hello 42");
    }

    #[test]
    fn test_decoder_nested_token_explicit_domain() {
        let mut resolver = MockTokenResolver::new();
        resolver.register(0x5678, "custom_domain", "Status: %s");

        let decoder = Decoder::new(&resolver);
        let fmt = FormatString::parse_printf("Msg: ${custom_domain}#%08x").unwrap();

        // Token 0x5678 unsigned varint: [0xf8, 0xac, 0x01]
        // string "OK": len 2 = 0x02, 'O', 'K'
        let payload = [0xf8, 0xac, 0x01, 0x02, b'O', b'K'];
        let res = decoder.decode(&fmt, &payload, "");
        assert!(res.is_successful());
        assert_eq!(res.value(), "Msg: Status: OK");
    }

    #[test]
    fn test_decoder_doubly_nested_token() {
        let mut resolver = MockTokenResolver::new();
        resolver.register(0x100, "", "my_domain");
        resolver.register(0x200, "my_domain", "NestedVal %d");

        let decoder = Decoder::new(&resolver);
        let fmt = FormatString::parse_printf("Double: ${$#%08x}#%08x").unwrap();

        // Domain token 0x100: [0x80, 0x02]
        // Value token 0x200: [0x80, 0x04]
        // arg 99: zigzag 198 = [0xc6, 0x01]
        let payload = [0x80, 0x02, 0x80, 0x04, 0xc6, 0x01];
        let res = decoder.decode(&fmt, &payload, "");
        assert!(res.is_successful());
        assert_eq!(res.value(), "Double: NestedVal 99");
    }

    #[test]
    fn test_decoder_doubly_nested_token_with_domain() {
        let mut resolver = MockTokenResolver::new();
        resolver.register(0x100, "root_domain", "leaf_domain");
        resolver.register(0x200, "leaf_domain", "LeafResult");

        let decoder = Decoder::new(&resolver);
        let fmt = FormatString::parse_printf("Double: ${${root_domain}#%08x}#%08x").unwrap();

        // Domain token 0x100: [0x80, 0x02]
        // Value token 0x200: [0x80, 0x04]
        let payload = [0x80, 0x02, 0x80, 0x04];
        let res = decoder.decode(&fmt, &payload, "");
        assert!(res.is_successful());
        assert_eq!(res.value(), "Double: LeafResult");
    }

    #[test]
    fn test_decoder_nested_token_custom_prefix() {
        let mut resolver = MockTokenResolver::new();
        resolver.register(0x300, "", "Custom: %s");

        let decoder = Decoder::new(&resolver);
        let fmt = FormatString::parse_printf_with_prefix("Value: ~#%08x", '~').unwrap();

        // Token 0x300: [0x80, 0x06]
        // string "works": len 5 = 0x05, "works"
        let payload = [0x80, 0x06, 0x05, b'w', b'o', b'r', b'k', b's'];
        let res = decoder.decode(&fmt, &payload, "");
        assert!(res.is_successful());
        assert_eq!(res.value(), "Value: Custom: works");
    }

    #[test]
    fn test_decoder_nested_token_type_domain() {
        let mut resolver = MockTokenResolver::new();
        resolver.register(0x400, TYPE_DOMAIN, "Point { x: %d, y: %d }");

        let decoder = Decoder::new(&resolver);
        let fmt = FormatString::parse_printf("Struct: ${type}#%08x").unwrap();

        // Token 0x400: [0x80, 0x08]
        // x: 10 (zigzag 20 = 0x14), y: 20 (zigzag 40 = 0x28)
        let payload = [0x80, 0x08, 0x14, 0x28];
        let res = decoder.decode(&fmt, &payload, "");
        assert!(res.is_successful());
        assert_eq!(res.value(), "Struct: Point { x: 10, y: 20 }");
    }

    #[test]
    fn test_decoder_nested_token_missing_and_error() {
        let mut resolver = MockTokenResolver::new();
        resolver.register(0x500, "", "val: %d");

        let decoder = Decoder::new(&resolver);
        let fmt = FormatString::parse_printf("A: $#%08x, B: %d").unwrap();

        // Missing payload: both missing / skipped
        let res = decoder.decode(&fmt, &[], "");
        assert!(!res.is_successful());
        assert_eq!(
            res.value_with_errors(),
            "A: <[$#%08x MISSING]>, B: <[%d SKIPPED]>"
        );

        // Unknown token: error placeholder
        let payload = [0x99, 0x99, 0x02];
        let res = decoder.decode(&fmt, &payload, "");
        assert!(!res.is_successful());
        assert_eq!(
            res.value_with_errors(),
            "A: <[$#%08x ERROR]>, B: <[%d SKIPPED]>"
        );
    }

    #[test]
    fn test_decoder_literal_token_and_domain() {
        let mut resolver = MockTokenResolver::new();
        resolver.register(0x100, "", "custom");
        resolver.register(0x200, "custom", "lit=%d");

        let decoder = Decoder::new(&resolver);

        // Literal token: resolved string is treated as literal plain text (%d preserved, 0 args consumed).
        let fmt_literal = FormatString::parse_printf("Out: ${$#00000100}#00000200").unwrap();
        let res_literal = decoder.decode(&fmt_literal, &[], "");
        assert!(res_literal.is_successful());
        assert_eq!(res_literal.value(), "Out: lit=%d");

        // Variable token: resolved string is treated as a format string consuming args from payload.
        let fmt_var = FormatString::parse_printf("Out: ${$#00000100}#%08x").unwrap();
        // 0x200 zigzag encoded is 0x400 -> LEB128 [0x80, 0x08]; 7 zigzag encoded is 0x0e.
        let payload = [0x80, 0x08, 0x0e];
        let res_var = decoder.decode(&fmt_var, &payload, "");
        assert!(res_var.is_successful());
        assert_eq!(res_var.value(), "Out: lit=7");
    }

    #[test]
    fn test_decoder_base64_nested_token() {
        let mut resolver = MockTokenResolver::new();
        // Token 1 in default domain: "One"
        resolver.register(1, "", "One");
        // Token 2 in "custom" domain: "Two"
        resolver.register(2, "custom", "Two");

        let decoder = Decoder::new(&resolver);

        // $AQAAAA== is token 1 in base64
        let fmt = FormatString::parse_printf("Value: $AQAAAA==").unwrap();
        let res = decoder.decode(&fmt, &[], "");
        assert!(res.is_successful());
        assert_eq!(res.value(), "Value: One");

        // With explicit domain ${custom}AgAAAA== (token 2)
        let fmt = FormatString::parse_printf("Value: ${custom}AgAAAA==").unwrap();
        let res = decoder.decode(&fmt, &[], "");
        assert!(res.is_successful());
        assert_eq!(res.value(), "Value: Two");
    }

    #[test]
    fn test_decoder_base64_nested_token_with_payload() {
        let mut resolver = MockTokenResolver::new();
        // Token 10: "count=%d"
        resolver.register(10, "", "count=%d");

        let decoder = Decoder::new(&resolver);

        // Token 10 in LE bytes: [0x0a, 0x00, 0x00, 0x00]
        // int arg 42: zigzag 84 = [0x54]
        // Payload: [0x0a, 0x00, 0x00, 0x00, 0x54]
        // Base64 encoded: "CgAAAFQ="
        let fmt = FormatString::parse_printf("Result: $CgAAAFQ=").unwrap();
        let res = decoder.decode(&fmt, &[], "");
        assert!(res.is_successful());
        assert_eq!(res.value(), "Result: count=42");
    }

    #[test]
    fn test_decoder_base64_nested_token_resilience() {
        let mut resolver = MockTokenResolver::new();
        resolver.register(1, "", "One");

        let decoder = Decoder::new(&resolver);

        // $123456== is unknown, but $AQAAAA== is token 1 ("One")
        let fmt = FormatString::parse_printf("$123456== and $AQAAAA==").unwrap();
        let res = decoder.decode(&fmt, &[], "");
        assert!(!res.is_successful());
        // value() preserves the unknown token without skipping the valid token
        assert_eq!(res.value(), "$123456== and One");
    }

    fn push_signed_varint(out: &mut Vec<u8>, val: i64) {
        let mut buf = [0u8; pw_varint::MAX_VARINT_SIZE_BYTES];
        let len = val.varint_encode(&mut buf).unwrap();
        out.extend_from_slice(&buf[..len]);
    }

    #[test]
    fn test_decoder_variable_nested_token_followed_by_parent_args_and_trailing_bytes() {
        let mut resolver = MockTokenResolver::new();
        resolver.register(10, "", "item=%d");
        let decoder = Decoder::new(&resolver);

        // Parent has a cursor-consuming variable nested token followed by another parent arg (`%d`).
        let fmt = FormatString::parse_printf("A: $#%08x, B: %d").unwrap();
        let mut payload = Vec::new();
        push_signed_varint(&mut payload, 10);
        push_signed_varint(&mut payload, 7);
        push_signed_varint(&mut payload, 99);

        let res = decoder.decode(&fmt, &payload, "");
        assert!(res.is_successful());
        assert_eq!(res.value(), "A: item=7, B: 99");

        // Embedded Base64 literal token payload with extra unconsumed trailing bytes reports error.
        // Token 10 ([0x0a, 0, 0, 0]) + zigzag 42 ([0x54]) + extra byte ([0x01]) -> "CgAAAFQB"
        let fmt_extra = FormatString::parse_printf("Result: $CgAAAFQB").unwrap();
        let res_extra = decoder.decode(&fmt_extra, &[], "");
        assert!(!res_extra.is_successful());
        assert_eq!(res_extra.decoding_errors(), 1);

        // Unresolved variable nested token marks subsequent parent `%d` as `<[%d SKIPPED]>`.
        let fmt_unexp = FormatString::parse_printf("Nested: $#%08x, parent: %d").unwrap();
        let mut unexp_payload = Vec::new();
        push_signed_varint(&mut unexp_payload, 0x9999);
        push_signed_varint(&mut unexp_payload, 42);
        let res_unexp = decoder.decode(&fmt_unexp, &unexp_payload, "");
        assert!(!res_unexp.is_successful());
        assert_eq!(res_unexp.value(), "Nested: $#%08x, parent: %d");
        assert_eq!(
            res_unexp.value_with_errors(),
            "Nested: <[$#%08x ERROR]>, parent: <[%d SKIPPED]>"
        );
    }

    #[test]
    fn test_decoder_sign_extended_32bit_pointer_and_dynamic_width_overflow() {
        let resolver = MockTokenResolver::new();
        let decoder = Decoder::new(&resolver);

        // Sign-extended 32-bit pointer (>= 0x8000_0000: 0x8011_e0d0 as i32 as i64)
        let fmt_ptr = FormatString::parse_printf("%hhd %hd %hhu %hu %u %p").unwrap();
        let mut payload = Vec::new();
        for arg in [
            i64::from(i8::MIN),
            i64::from(i16::MIN),
            i64::from(u8::MAX),
            i64::from(u16::MAX),
            100,
            i64::from(0x8011_e0d0u32.cast_signed()),
        ] {
            push_signed_varint(&mut payload, arg);
        }
        let res_ptr = decoder.decode(&fmt_ptr, &payload, "");
        assert!(res_ptr.is_successful());
        assert_eq!(res_ptr.value(), "-128 -32768 255 65535 100 0x8011e0d0");

        // Dynamic '*' width exceeding u32::MAX marks error and skips following specifiers cleanly
        let fmt_dyn = FormatString::parse_printf("%*d %d").unwrap();
        let mut dyn_payload = Vec::new();
        push_signed_varint(&mut dyn_payload, i64::from(u32::MAX) + 1);
        push_signed_varint(&mut dyn_payload, 10);
        push_signed_varint(&mut dyn_payload, 20);
        let res_dyn = decoder.decode(&fmt_dyn, &dyn_payload, "");
        assert!(!res_dyn.is_successful());
        assert!(res_dyn.decoding_errors() >= 1);
        assert_eq!(res_dyn.value_with_errors(), "<[%*d ERROR]> <[%d SKIPPED]>");
    }
}
