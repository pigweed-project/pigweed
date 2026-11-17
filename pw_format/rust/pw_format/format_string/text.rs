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

//! String (`%s`) and character (`%c`) formatting helpers.

use super::padding::apply_width_and_alignment;
use super::ConversionSpec;

/// Finds the largest valid UTF-8 character boundary in `s` at or before `index`.
///
/// Mirrors `str::floor_char_boundary` (stabilized in Rust 1.91) so that
/// byte-based `printf` string precisions (`%.Ns`) never slice mid-codepoint.
pub(super) fn floor_char_boundary(s: &str, index: usize) -> usize {
    if index >= s.len() {
        return s.len();
    }
    let mut end = index;
    while !s.is_char_boundary(end) {
        end -= 1;
    }
    end
}

/// Formats a string argument (`%s` or `{}`), applying optional byte precision
/// truncation and minimum field width padding.
pub(super) fn format_str(v: &str, spec: &ConversionSpec, output: &mut String) {
    let value = match spec.precision.to_fixed() {
        Some(p) => &v[..floor_char_boundary(v, p as usize)],
        None => v,
    };
    apply_width_and_alignment("", value, spec, output);
}

/// Formats a single Unicode scalar character (`%c` or `{}`).
pub(super) fn format_char(v: char, spec: &ConversionSpec, output: &mut String) {
    const MAX_UTF8_CHAR_LEN: usize = 4;
    let mut buf = [0u8; MAX_UTF8_CHAR_LEN];
    let s = v.encode_utf8(&mut buf);
    apply_width_and_alignment("", s, spec, output);
}
