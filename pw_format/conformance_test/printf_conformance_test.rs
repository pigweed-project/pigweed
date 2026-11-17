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

//! Cross-language conformance test runner for `pw_format` printf formatting.
//!
//! Test cases are authored in `printf_cases.yaml`, rendered into per-case
//! `#[test]` functions via `generate_printf_cases.py` and
//! `printf_cases.rs.jinja`, and evaluated against `pw_format` to verify
//! specifier parsing, missing/mismatched argument fallback, and cross-language
//! consistency with the C++ `std::snprintf` reference test
//! (`snprintf_reference_cc_test`).

use pw_format::{
    Arg, ConversionSpec, FormatError, FormatFragment, FormatString, FormatStyle, NestedDomain,
    NestedTokenSpec, NestedTokenValue,
};

include!("printf_cases.rs");

impl From<&TestArg> for Arg {
    fn from(arg: &TestArg) -> Self {
        match *arg {
            TestArg::SignedInt(val) => Self::Int(val),
            TestArg::UnsignedInt(val) => Self::Uint(val),
            TestArg::FloatingPoint(val) => Self::Float(val),
            TestArg::StringValue(val) => Self::Str(val.to_string()),
            TestArg::CharValue(ch) => {
                // Character arguments in `printf_cases.yaml` are single Unicode
                // scalar values (`char`).
                //
                // Note that Rust's `char` and `Arg::Char(char)` natively support
                // full UTF-8 Unicode scalar values, whereas C/C++ `snprintf("%c")`
                // and the C++ detokenizer currently only support single-byte
                // characters. In the future, we may want to improve the C++
                // formatting/detokenizer implementation to match Rust's UTF-8
                // character handling.
                Self::Char(ch)
            }
            TestArg::PointerValue(val) => {
                Self::Ptr(usize::try_from(val).expect("pointer_value fits in target usize"))
            }
        }
    }
}

fn escape_bytes_as_string(bytes: &[u8]) -> String {
    let mut s = String::new();
    s.push('"');
    for &b in bytes {
        match b {
            b'\\' => s.push_str("\\\\"),
            b'"' => s.push_str("\\\""),
            b'\n' => s.push_str("\\n"),
            b'\r' => s.push_str("\\r"),
            b'\t' => s.push_str("\\t"),
            0x20..=0x7e => s.push(b as char),
            _ => s.push_str(&format!("\\x{b:02x}")),
        }
    }
    s.push('"');
    s
}

fn serialize_next_ast_arg<'a, I>(arg_iter: &mut I) -> String
where
    I: Iterator<Item = &'a Arg>,
{
    match arg_iter.next() {
        Some(Arg::Int(val)) => format!("{val}"),
        Some(Arg::Uint(val)) => format!("{val}"),
        Some(Arg::Str(val)) => val.clone(),
        None => "<MISSING>".to_string(),
        Some(other) => format!("<TYPE_ERROR: {other:?}>"),
    }
}

fn push_domain_prefix(out: &mut String, domain: &str) {
    if !domain.is_empty() {
        out.push_str(domain);
    }
    out.push('#');
}

/// Serializes parsed `NestedTokenSpec` AST structures into a canonical `<NESTED ...>`
/// representation to validate parser AST output separately from runtime formatting.
///
/// # Why AST Serialization is Separate from Runtime Formatting
/// - `pw_format` parses nested token syntax (`${domain}#token`) into structured
///   `NestedTokenSpec` AST nodes (`NestedDomain`, `NestedTokenValue`, decoded Base64
///   payloads, etc.). Actual database lookups and recursive detokenization are performed
///   by `pw_tokenizer`.
/// - At runtime, `FormatString::format` / `FormatString::format_with_errors` expects
///   each `FormatFragment::NestedToken` to consume a pre-expanded `Arg::Str` supplied by
///   `pw_tokenizer` (exercised by `nested_token_format_*` test cases and
///   `test_nested_token_runtime_formatting`).
/// - For the parser conformance test cases (`nested_token_*`) in `printf_cases.yaml`,
///   this function inspects the parsed AST fragments directly and renders their structure
///   as `<NESTED domain#token "payload">` to verify parsing accuracy independently of
///   a token database.
fn serialize_parsed_nested_token_ast(fs: &FormatString, args: &[Arg]) -> String {
    let mut output = String::new();
    let mut arg_iter = args.iter();

    for fragment in &fs.fragments {
        match fragment {
            FormatFragment::Literal(s) => {
                output.push_str(s);
            }
            FormatFragment::Conversion(spec) => {
                let single_fs = FormatString {
                    fragments: vec![FormatFragment::Conversion(spec.clone())],
                };
                let current_arg = match arg_iter.next() {
                    Some(arg) => core::slice::from_ref(arg),
                    None => &[],
                };
                output.push_str(&single_fs.format(current_arg, FormatStyle::Printf));
            }
            FormatFragment::NestedToken(token_spec) => {
                output.push_str("<NESTED ");

                // Domain part
                match &token_spec.domain {
                    NestedDomain::Default => {}
                    NestedDomain::Literal(name) => {
                        output.push_str(name);
                    }
                    NestedDomain::LiteralToken { domain, token } => {
                        output.push('(');
                        push_domain_prefix(&mut output, domain);
                        output.push_str(&token.to_string());
                        output.push(')');
                    }
                    NestedDomain::VariableToken { domain } => {
                        output.push('(');
                        push_domain_prefix(&mut output, domain);
                        output.push_str(&serialize_next_ast_arg(&mut arg_iter));
                        output.push(')');
                    }
                }

                // Token part
                output.push('#');
                match &token_spec.token {
                    NestedTokenValue::Variable => {
                        output.push_str(&serialize_next_ast_arg(&mut arg_iter));
                    }
                    NestedTokenValue::Literal(tok) => {
                        output.push_str(&tok.to_string());
                        if let Some(data) = token_spec.token_data() {
                            if !data.is_empty() {
                                output.push(' ');
                                output.push_str(&escape_bytes_as_string(data));
                            }
                        }
                    }
                }

                output.push('>');
            }
        }
    }

    output
}

fn check_case(tc: &TestCase) {
    let args: Vec<Arg> = tc.args.iter().map(Arg::from).collect();

    let prefix = if tc.test_id.contains("prefix_amp") {
        '&'
    } else {
        FormatString::DEFAULT_NESTED_PREFIX
    };

    let parsed = FormatString::parse_printf_lossy_with_prefix(tc.format_string, prefix);
    let actual =
        if !tc.test_id.starts_with("nested_token_format_") && tc.test_id.contains("nested_token") {
            serialize_parsed_nested_token_ast(&parsed, &args)
        } else {
            parsed.format(&args, FormatStyle::Printf)
        };

    assert_eq!(
        actual, tc.expected_output,
        "Mismatch for test_id {:?} (format: {:?})",
        tc.test_id, tc.format_string,
    );
}

#[test]
fn test_custom_prefix_nested_tokens_and_errors() {
    // Valid nested tokens using custom prefix '&'
    let fs = FormatString::parse_printf_with_prefix("xxx&{foo}#%08xyyy", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[Arg::Int(123)]),
        "xxx<NESTED foo#123>yyy"
    );

    let fs = FormatString::parse_printf_with_prefix("xxx&{&#%08x}#%08xyyy", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[Arg::Int(123), Arg::Int(456)]),
        "xxx<NESTED (#123)#456>yyy"
    );

    let fs = FormatString::parse_printf_with_prefix("xxx&{&{foo}#%08x}#%08xyyy", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[Arg::Int(123), Arg::Int(456)]),
        "xxx<NESTED (foo#123)#456>yyy"
    );

    let fs = FormatString::parse_printf_with_prefix("&#%08x", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[Arg::Int(456)]),
        "<NESTED #456>"
    );

    let fs = FormatString::parse_printf_with_prefix("&#01234567", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[]),
        "<NESTED #19088743>"
    );

    let fs = FormatString::parse_printf_with_prefix("&IwEAAEFCQw==", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[]),
        "<NESTED #291 \"ABC\">"
    );

    let fs = FormatString::parse_printf_with_prefix("&AQAAAA==", '&').unwrap();
    assert_eq!(serialize_parsed_nested_token_ast(&fs, &[]), "<NESTED #1>");

    let fs = FormatString::parse_printf_with_prefix("&10#0000000010", '&').unwrap();
    assert_eq!(serialize_parsed_nested_token_ast(&fs, &[]), "<NESTED #10>");

    let fs = FormatString::parse_printf_with_prefix("&16#00000020", '&').unwrap();
    assert_eq!(serialize_parsed_nested_token_ast(&fs, &[]), "<NESTED #32>");

    let fs = FormatString::parse_printf_with_prefix("&16#%08x", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[Arg::Int(456)]),
        "<NESTED #456>"
    );

    // Error cases and corner cases with custom prefix '&'
    // Fallback to literal + conversion when token conversion is not %08x
    let fs = FormatString::parse_printf_with_prefix("&{}#%d", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[Arg::Int(42)]),
        "&{}#42"
    );

    let fs = FormatString::parse_printf_with_prefix("&{}#%06x", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[Arg::Uint(42)]),
        "&{}#00002a"
    );

    // Unclosed domain brace
    let fs = FormatString::parse_printf_with_prefix("&{unclosed#%08x", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[Arg::Uint(42)]),
        "&{unclosed#0000002a"
    );

    // Short digits
    let fs = FormatString::parse_printf_with_prefix("&#123", '&').unwrap();
    assert_eq!(serialize_parsed_nested_token_ast(&fs, &[]), "&#123");

    let fs = FormatString::parse_printf_with_prefix("&10#123", '&').unwrap();
    assert_eq!(serialize_parsed_nested_token_ast(&fs, &[]), "&10#123");

    let fs = FormatString::parse_printf_with_prefix("&16#123", '&').unwrap();
    assert_eq!(serialize_parsed_nested_token_ast(&fs, &[]), "&16#123");

    let fs = FormatString::parse_printf_with_prefix("&64#AAA", '&').unwrap();
    assert_eq!(serialize_parsed_nested_token_ast(&fs, &[]), "&64#AAA");

    // Plain text containing '&'
    let fs = FormatString::parse_printf_with_prefix("Cost is &100 or &5 per item", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[]),
        "Cost is &100 or &5 per item"
    );

    let fs = FormatString::parse_printf_with_prefix("&", '&').unwrap();
    assert_eq!(serialize_parsed_nested_token_ast(&fs, &[]), "&");

    let fs = FormatString::parse_printf_with_prefix("&&", '&').unwrap();
    assert_eq!(serialize_parsed_nested_token_ast(&fs, &[]), "&&");

    let fs = FormatString::parse_printf_with_prefix("&{}", '&').unwrap();
    assert_eq!(serialize_parsed_nested_token_ast(&fs, &[]), "&{}");

    let fs = FormatString::parse_printf_with_prefix("&#", '&').unwrap();
    assert_eq!(serialize_parsed_nested_token_ast(&fs, &[]), "&#");

    // Prefix isolation: '$' is treated as ordinary text when prefix is '&'
    let fs = FormatString::parse_printf_with_prefix("$#%08x", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[Arg::Uint(42)]),
        "$#0000002a"
    );

    // '&' is treated as ordinary text when prefix is '$'
    let fs = FormatString::parse_printf_with_prefix("&#%08x", '$').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[Arg::Uint(42)]),
        "&#0000002a"
    );

    // Missing argument handling
    let fs = FormatString::parse_printf_with_prefix("&#%08x", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[]),
        "<NESTED #<MISSING>>"
    );

    let fs = FormatString::parse_printf_with_prefix("&{&#%08x}#%08x", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[]),
        "<NESTED (#<MISSING>)#<MISSING>>"
    );
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[Arg::Int(123)]),
        "<NESTED (#123)#<MISSING>>"
    );

    // Domain whitespace stripping with custom prefix '&'
    let fs = FormatString::parse_printf_with_prefix("&{  Domain 1  }#%08x", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[Arg::Int(123)]),
        "<NESTED Domain1#123>"
    );

    let fs = FormatString::parse_printf_with_prefix("&{   }#%08x", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[Arg::Int(123)]),
        "<NESTED #123>"
    );

    let fs =
        FormatString::parse_printf_with_prefix("&{&{  Domain 1  }#01234567}#%08x", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[Arg::Int(456)]),
        "<NESTED (Domain1#19088743)#456>"
    );

    let fs = FormatString::parse_printf_with_prefix("&{&{   }#01234567}#%08x", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[Arg::Int(456)]),
        "<NESTED (#19088743)#456>"
    );

    // Supported radices with custom prefix '&'
    let fs = FormatString::parse_printf_with_prefix("&10#%010d", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[Arg::Int(123)]),
        "<NESTED #123>"
    );

    let fs = FormatString::parse_printf_with_prefix("&{foo}10#%010d", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[Arg::Int(123)]),
        "<NESTED foo#123>"
    );

    let fs = FormatString::parse_printf_with_prefix("&{&10#%010d}#%08x", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[Arg::Int(123), Arg::Int(456)]),
        "<NESTED (#123)#456>"
    );

    let fs = FormatString::parse_printf_with_prefix("&{&10#0000000010}#%08x", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[Arg::Int(456)]),
        "<NESTED (#10)#456>"
    );

    let fs = FormatString::parse_printf_with_prefix("&{&16#%08x}#%08x", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[Arg::Int(123), Arg::Int(456)]),
        "<NESTED (#123)#456>"
    );

    let fs = FormatString::parse_printf_with_prefix("&{&16#00000020}#%08x", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[Arg::Int(456)]),
        "<NESTED (#32)#456>"
    );

    let fs = FormatString::parse_printf_with_prefix("&64#AQAAAA==", '&').unwrap();
    assert_eq!(serialize_parsed_nested_token_ast(&fs, &[]), "<NESTED #1>");

    let fs = FormatString::parse_printf_with_prefix("&{&64#AQAAAA==}#%08x", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[Arg::Int(456)]),
        "<NESTED (#1)#456>"
    );

    let fs = FormatString::parse_printf_with_prefix("&{&AQAAAA==}#%08x", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[Arg::Int(456)]),
        "<NESTED (#1)#456>"
    );

    let fs = FormatString::parse_printf_with_prefix("&-_AA", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[]),
        "<NESTED #61691>"
    );

    let fs = FormatString::parse_printf_with_prefix("&64#-_AA", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[]),
        "<NESTED #61691>"
    );

    // Unsupported radices with custom prefix '&'
    let fs = FormatString::parse_printf_with_prefix("&8#00000001", '&').unwrap();
    assert_eq!(serialize_parsed_nested_token_ast(&fs, &[]), "&8#00000001");

    let fs = FormatString::parse_printf_with_prefix("&{&8#01234567}#%08x", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[Arg::Uint(42)]),
        "&{&8#01234567}#0000002a"
    );

    let fs = FormatString::parse_printf_with_prefix("&{&10#%08x}#%08x", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[Arg::Uint(10), Arg::Uint(42)]),
        "&{&10#0000000a}#0000002a"
    );

    // Multi-byte UTF-8 robustness with custom prefix '&'
    let fs = FormatString::parse_printf_with_prefix("Malformed &#€€€ tail", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[]),
        "Malformed &#€€€ tail"
    );

    let fs = FormatString::parse_printf_with_prefix("Domain &{&#€€€}#%08x tail", '&').unwrap();
    assert_eq!(
        serialize_parsed_nested_token_ast(&fs, &[Arg::Uint(42)]),
        "Domain &{&#€€€}#0000002a tail"
    );
}

#[test]
fn test_nested_token_runtime_formatting() {
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    enum TestDecodeError {
        DecodeError,
        Skipped,
    }

    struct TestErrorFormatter;

    impl FormatError for TestErrorFormatter {
        type Error = TestDecodeError;

        fn format_error(&self, spec: &ConversionSpec, error: &TestDecodeError) -> String {
            let spec_str = spec.to_printf();
            match error {
                TestDecodeError::DecodeError => format!("<[{spec_str} ERROR]>"),
                TestDecodeError::Skipped => format!("<[{spec_str} SKIPPED]>"),
            }
        }

        fn format_missing(&self, spec: &ConversionSpec) -> String {
            format!("<[{} MISSING]>", spec.to_printf())
        }

        fn format_type_error(&self, spec: &ConversionSpec, _arg: &Arg) -> String {
            format!("<[{} ERROR]>", spec.to_printf())
        }

        fn format_nested_token_error(
            &self,
            spec: &NestedTokenSpec,
            error: &TestDecodeError,
        ) -> String {
            let spec_str = spec.to_printf();
            match error {
                TestDecodeError::DecodeError => format!("<[{spec_str} ERROR]>"),
                TestDecodeError::Skipped => format!("<[{spec_str} SKIPPED]>"),
            }
        }

        fn format_nested_token_missing(&self, spec: &NestedTokenSpec) -> String {
            format!("<[{} MISSING]>", spec.to_printf())
        }

        fn format_nested_token_type_error(&self, spec: &NestedTokenSpec, _arg: &Arg) -> String {
            format!("<[{} ERROR]>", spec.to_printf())
        }
    }

    // Directly exercises the real FormatString::format_with_errors runtime path
    // across various nested token forms (custom prefix '&' and default prefix '$').
    let fs = FormatString::parse_printf_with_prefix("Status: &{foo}#%08x done", '&').unwrap();
    assert_eq!(
        fs.format_with_errors(
            &[Ok(Arg::Str("EXPANDED".to_string()))],
            FormatStyle::Printf,
            &TestErrorFormatter,
        ),
        "Status: EXPANDED done"
    );

    // Doubly-nested domain token + base64 literal token + printf %d
    let fs = FormatString::parse_printf_with_prefix(
        "Msg: &{&{domain}#%08x}#%08x | &AQAAAA== | count=%d",
        '&',
    )
    .unwrap();
    assert_eq!(
        fs.format_with_errors(
            &[
                Ok(Arg::Str("NESTED_VAR".to_string())),
                Ok(Arg::Str("LITERAL_BASE64".to_string())),
                Ok(Arg::Int(99)),
            ],
            FormatStyle::Printf,
            &TestErrorFormatter,
        ),
        "Msg: NESTED_VAR | LITERAL_BASE64 | count=99"
    );

    // Missing argument on nested token
    let fs = FormatString::parse_printf_with_prefix("Status: &{foo}#%08x", '&').unwrap();
    assert_eq!(
        fs.format_with_errors(&[], FormatStyle::Printf, &TestErrorFormatter),
        "Status: <[&{foo}#%08x MISSING]>"
    );

    // DecodeError on nested token
    let fs = FormatString::parse_printf_with_prefix("Status: &{foo}#%08x", '&').unwrap();
    assert_eq!(
        fs.format_with_errors(
            &[Err(TestDecodeError::DecodeError)],
            FormatStyle::Printf,
            &TestErrorFormatter,
        ),
        "Status: <[&{foo}#%08x ERROR]>"
    );

    // Skipped argument on nested token
    let fs = FormatString::parse_printf_with_prefix("Status: &{foo}#%08x", '&').unwrap();
    assert_eq!(
        fs.format_with_errors(
            &[Err(TestDecodeError::Skipped)],
            FormatStyle::Printf,
            &TestErrorFormatter,
        ),
        "Status: <[&{foo}#%08x SKIPPED]>"
    );

    // Type error when non-string Arg is passed to a NestedToken fragment
    let fs = FormatString::parse_printf_with_prefix("Status: &{foo}#%08x", '&').unwrap();
    assert_eq!(
        fs.format_with_errors(
            &[Ok(Arg::Int(42))],
            FormatStyle::Printf,
            &TestErrorFormatter
        ),
        "Status: <[&{foo}#%08x ERROR]>"
    );
}
