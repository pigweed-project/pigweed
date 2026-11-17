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

//! Field width, alignment, and fill padding helpers.

use super::{Alignment, ConversionSpec, Flag, MinFieldWidth, Precision, Primitive};

/// Appends `prefix` and `value` to `output`, applying minimum field width,
/// left/right justification, and zero-fill or character-fill padding.
pub(super) fn apply_width_and_alignment(
    prefix: &str,
    value: &str,
    spec: &ConversionSpec,
    output: &mut String,
) {
    let MinFieldWidth::Fixed(w) = spec.min_field_width else {
        output.push_str(prefix);
        output.push_str(value);
        return;
    };

    let w = w as usize;
    // In printf, minimum field width specifies the minimum number of bytes,
    // not Unicode characters or terminal column width.
    let total_len = prefix.len() + value.len();

    if total_len >= w {
        output.push_str(prefix);
        output.push_str(value);
        return;
    }

    let pad_len = w - total_len;
    let is_left_aligned =
        spec.alignment == Alignment::Left || spec.flags.contains(&Flag::LeftJustify);
    let ignore_zero = is_left_aligned
        || matches!(spec.primitive, Primitive::String | Primitive::Character)
        || (matches!(spec.precision, Precision::Fixed(_))
            && matches!(spec.primitive, Primitive::Integer | Primitive::Unsigned));
    let do_zero_fill = spec.flags.contains(&Flag::LeadingZeros) && !ignore_zero;

    if is_left_aligned {
        // Left-justified values are padded on the right with the fill character.
        output.push_str(prefix);
        output.push_str(value);
        pad_fill(output, spec.fill, pad_len);
    } else if do_zero_fill {
        // Zero-fill padding is inserted after the sign/base prefix (e.g., `0x001` or `-0001`).
        output.push_str(prefix);
        pad_fill(output, '0', pad_len);
        output.push_str(value);
    } else {
        // Standard right-aligned fill padding precedes the sign/base prefix (e.g., `  0x1` or `  -1`).
        pad_fill(output, spec.fill, pad_len);
        output.push_str(prefix);
        output.push_str(value);
    }
}

/// Appends `count` copies of `fill_char` to `s`.
pub(super) fn pad_fill(s: &mut String, fill_char: char, count: usize) {
    s.extend(core::iter::repeat_n(fill_char, count));
}
