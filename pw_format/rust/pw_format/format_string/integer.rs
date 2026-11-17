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

//! Signed/unsigned integer (`%d`, `%i`, `%u`, `%o`, `%x`, `%X`) and pointer
//! (`%p`) formatting helpers.

use super::padding::{apply_width_and_alignment, pad_fill};
use super::platform::{PlatformTypes, SignedInt, UnsignedInt};
use super::{ConversionSpec, Flag, FormatStyle, Length, Style};

/// Returns the sign prefix (`"-"`, `"+"`, `" "`, or `""`) for a signed conversion.
pub(super) fn sign_prefix(is_negative: bool, spec: &ConversionSpec) -> &'static str {
    if is_negative {
        "-"
    } else if spec.flags.contains(&Flag::ForceSign) {
        "+"
    } else if spec.flags.contains(&Flag::SpaceSign) {
        " "
    } else {
        ""
    }
}

/// Formats a signed 64-bit integer according to `spec`, truncating to the
/// target platform or explicit length modifier width.
pub(super) fn format_int<P: PlatformTypes>(
    v: i64,
    spec: &ConversionSpec,
    style: FormatStyle,
    output: &mut String,
) {
    let sval = truncate_int::<P>(v, spec.length, style);
    let is_negative = sval < 0;
    let abs_val = sval.unsigned_abs();
    format_int_common(abs_val, is_negative, false, spec, output)
}

/// Formats an unsigned 64-bit integer according to `spec`, truncating to the
/// target platform or explicit length modifier width.
pub(super) fn format_uint<P: PlatformTypes>(
    v: u64,
    spec: &ConversionSpec,
    style: FormatStyle,
    output: &mut String,
) {
    let uval = truncate_uint::<P>(v, spec.length, style);
    format_int_common(uval, false, true, spec, output)
}

/// Formats a pointer address (`%p`) as a hexadecimal integer with `0x` prefix.
pub(super) fn format_ptr(v: u64, spec: &ConversionSpec, output: &mut String) {
    format_int_common(v, false, true, spec, output)
}

fn format_int_common(
    v: u64,
    is_negative: bool,
    is_unsigned: bool,
    spec: &ConversionSpec,
    output: &mut String,
) {
    let (base_prefix, mut value) = radix_and_base_prefix(v, spec);
    apply_int_precision(&mut value, v, spec);
    let prefix = assemble_int_prefix(base_prefix, is_negative, is_unsigned, &value, spec);
    apply_width_and_alignment(&prefix, &value, spec, output);
}

fn truncate_int<P: PlatformTypes>(v: i64, length: Option<Length>, style: FormatStyle) -> i64 {
    match (style, length) {
        (FormatStyle::CoreFmt, None) => v,
        (_, None | Some(Length::LongDouble)) => P::SignedInt::truncate_i64(v),
        (_, Some(Length::Char)) => P::SignedChar::truncate_i64(v),
        (_, Some(Length::Short)) => P::SignedShort::truncate_i64(v),
        (_, Some(Length::Long)) => P::SignedLong::truncate_i64(v),
        (_, Some(Length::LongLong)) => P::SignedLongLong::truncate_i64(v),
        (_, Some(Length::IntMax)) => P::IntMax::truncate_i64(v),
        (_, Some(Length::Size)) => P::SignedSize::truncate_i64(v),
        (_, Some(Length::PointerDiff)) => P::PointerDiff::truncate_i64(v),
    }
}

fn truncate_uint<P: PlatformTypes>(v: u64, length: Option<Length>, style: FormatStyle) -> u64 {
    match (style, length) {
        (FormatStyle::CoreFmt, None) => v,
        (_, None | Some(Length::LongDouble)) => P::UnsignedInt::truncate_u64(v),
        (_, Some(Length::Char)) => P::UnsignedChar::truncate_u64(v),
        (_, Some(Length::Short)) => P::UnsignedShort::truncate_u64(v),
        (_, Some(Length::Long)) => P::UnsignedLong::truncate_u64(v),
        (_, Some(Length::LongLong)) => P::UnsignedLongLong::truncate_u64(v),
        (_, Some(Length::IntMax)) => P::UIntMax::truncate_u64(v),
        (_, Some(Length::Size)) => P::Size::truncate_u64(v),
        (_, Some(Length::PointerDiff)) => P::UnsignedPointerDiff::truncate_u64(v),
    }
}

fn radix_and_base_prefix(v: u64, spec: &ConversionSpec) -> (&'static str, String) {
    let is_alt = spec.flags.contains(&Flag::AlternateSyntax) && v != 0;
    match spec.style {
        Style::Hex => (if is_alt { "0x" } else { "" }, format!("{v:x}")),
        Style::UpperHex => (if is_alt { "0X" } else { "" }, format!("{v:X}")),
        Style::Pointer => ("0x", format!("{v:x}")),
        Style::Octal => ("", format!("{v:o}")),
        _ => ("", format!("{v}")),
    }
}

fn apply_int_precision(value: &mut String, v: u64, spec: &ConversionSpec) {
    let Some(p) = spec.precision.to_fixed() else {
        return;
    };
    if p == 0 && v == 0 {
        value.clear();
        return;
    }
    let p = p as usize;
    if value.len() < p {
        let pad = p - value.len();
        let mut padded = String::with_capacity(p);
        pad_fill(&mut padded, '0', pad);
        padded.push_str(value);
        *value = padded;
    }
}

fn assemble_int_prefix(
    base_prefix: &str,
    is_negative: bool,
    is_unsigned: bool,
    value: &str,
    spec: &ConversionSpec,
) -> String {
    let mut prefix = String::new();
    if is_negative {
        prefix.push('-');
    } else if !is_unsigned {
        if spec.flags.contains(&Flag::ForceSign) {
            prefix.push('+');
        } else if spec.flags.contains(&Flag::SpaceSign) {
            prefix.push(' ');
        }
    }

    if spec.style == Style::Pointer {
        prefix.push_str(base_prefix);
    } else if spec.style == Style::Octal && spec.flags.contains(&Flag::AlternateSyntax) {
        if !value.starts_with('0') {
            prefix.push('0');
        }
    } else {
        prefix.push_str(base_prefix);
    }
    prefix
}
