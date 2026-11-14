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

use pw_format::{Arg, FormatString, FormatStyle};

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

fn check_case(tc: &TestCase) {
    let args: Vec<Arg> = tc.args.iter().map(Arg::from).collect();
    let parsed = FormatString::parse_printf(tc.format_string).unwrap_or_else(|err| {
        panic!(
            "Unexpected parse failure for test_id {:?} (format: {:?}): {err}",
            tc.test_id, tc.format_string,
        )
    });
    let actual = parsed.format(&args, FormatStyle::Printf);

    assert_eq!(
        actual, tc.expected_output,
        "Mismatch for test_id {:?} (format: {:?})",
        tc.test_id, tc.format_string,
    );
}
