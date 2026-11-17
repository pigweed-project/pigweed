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

//! Floating-point formatting helpers (`%f`, `%F`, `%e`, `%E`, `%g`, `%G`, `%a`, `%A`).

use std::borrow::Cow;

use super::integer::sign_prefix;
use super::padding::{apply_width_and_alignment, pad_fill};
use super::{ConversionSpec, Flag, FormatStyle, Precision, Style};

const HEX_LOWER: &[u8; 16] = b"0123456789abcdef";
const HEX_UPPER: &[u8; 16] = b"0123456789ABCDEF";

const DEFAULT_FLOAT_PRECISION: usize = 6;
const F64_FRAC_BITS: u32 = 52;
const F64_EXP_MASK: u64 = 0x7ff;
const F64_FRAC_MASK: u64 = 0x000f_ffff_ffff_ffff;
const F64_EXP_BIAS: i32 = 1023;
const F64_SUBNORMAL_EXP: i32 = -1022;
const F64_HEX_FRAC_NYBBLES: usize = 13;

/// Formats a 64-bit floating-point number according to `spec` and `style`.
pub(super) fn format_float(v: f64, spec: &ConversionSpec, style: FormatStyle, output: &mut String) {
    let is_uppercase = matches!(
        spec.style,
        Style::Upper | Style::UpperExponential | Style::UpperGeneral | Style::UpperHexFloat
    );
    let prec = spec
        .precision
        .to_fixed()
        .map_or(DEFAULT_FLOAT_PRECISION, |p| p as usize);

    let (sign, value_str) = if v.is_nan() {
        format_float_nan(spec, is_uppercase)
    } else if v.is_infinite() {
        format_float_inf(v, spec, is_uppercase)
    } else {
        format_float_finite(v, spec, style, prec, is_uppercase)
    };

    // Per C99 / IEEE 754 printf rules, `0` (zero-padding) is ignored for NaN and infinity.
    let effective_spec =
        if (v.is_nan() || v.is_infinite()) && spec.flags.contains(&Flag::LeadingZeros) {
            let mut s = spec.clone();
            s.flags.remove(&Flag::LeadingZeros);
            Cow::Owned(s)
        } else {
            Cow::Borrowed(spec)
        };

    apply_width_and_alignment(sign, &value_str, &effective_spec, output);
}

/// Formats a `NaN` floating-point value (`"nan"` or `"NAN"`).
fn format_float_nan(spec: &ConversionSpec, is_uppercase: bool) -> (&'static str, String) {
    let s = if is_uppercase { "NAN" } else { "nan" };
    (sign_prefix(false, spec), s.to_string())
}

/// Formats an infinite floating-point value (`"inf"` or `"INF"`) with its sign prefix.
fn format_float_inf(v: f64, spec: &ConversionSpec, is_uppercase: bool) -> (&'static str, String) {
    let s = if is_uppercase { "INF" } else { "inf" };
    (
        sign_prefix(v.is_sign_negative() || v < 0.0, spec),
        s.to_string(),
    )
}

fn hex_float_prefix(is_negative: bool, is_uppercase: bool, spec: &ConversionSpec) -> &'static str {
    match (
        is_negative,
        spec.flags.contains(&Flag::ForceSign),
        spec.flags.contains(&Flag::SpaceSign),
        is_uppercase,
    ) {
        (true, _, _, false) => "-0x",
        (true, _, _, true) => "-0X",
        (false, true, _, false) => "+0x",
        (false, true, _, true) => "+0X",
        (false, false, true, false) => " 0x",
        (false, false, true, true) => " 0X",
        (false, false, false, false) => "0x",
        (false, false, false, true) => "0X",
    }
}

/// Formats a finite floating-point number into `(prefix, magnitude_string)`.
fn format_float_finite(
    v: f64,
    spec: &ConversionSpec,
    style: FormatStyle,
    prec: usize,
    is_uppercase: bool,
) -> (&'static str, String) {
    let is_neg = v.is_sign_negative() || v < 0.0;
    let abs_val = v.abs();
    let alt_form = spec.flags.contains(&Flag::AlternateSyntax);

    match spec.style {
        Style::Exponential | Style::UpperExponential => (
            sign_prefix(is_neg, spec),
            format_exponential(abs_val, prec, is_uppercase, alt_form),
        ),
        Style::General | Style::UpperGeneral => (
            sign_prefix(is_neg, spec),
            format_general(abs_val, prec, is_uppercase, alt_form),
        ),
        Style::HexFloat | Style::UpperHexFloat => (
            hex_float_prefix(is_neg, is_uppercase, spec),
            format_hex_float(abs_val, is_uppercase, spec.precision.to_fixed(), alt_form),
        ),
        _ => (
            sign_prefix(is_neg, spec),
            format_standard_float(abs_val, spec.precision, alt_form, style),
        ),
    }
}

fn ensure_alt_form_decimal(s: &mut String) {
    if !s.contains('.') {
        if let Some(pos) = s.find(['e', 'E']) {
            s.insert(pos, '.');
        } else {
            s.push('.');
        }
    }
}

fn format_exponential(abs_val: f64, prec: usize, is_uppercase: bool, alt_form: bool) -> String {
    let exp_char = if is_uppercase { 'E' } else { 'e' };
    let raw_s = format!("{abs_val:.prec$e}");
    let mut s = if let Some((mantissa, exp)) = raw_s.split_once('e') {
        let exp_num = exp.parse::<i32>().unwrap_or(0);
        format!("{mantissa}{exp_char}{exp_num:+03}")
    } else {
        raw_s
    };
    if alt_form {
        ensure_alt_form_decimal(&mut s);
    }
    s
}

fn trim_trailing_zeros(s: &str) -> &str {
    if s.contains('.') {
        s.trim_end_matches('0').trim_end_matches('.')
    } else {
        s
    }
}

fn format_general(abs_val: f64, prec: usize, is_uppercase: bool, alt_form: bool) -> String {
    let sig_figs = core::cmp::max(1, prec);
    let sig_figs_i32 = i32::try_from(sig_figs).unwrap_or(i32::MAX);
    let exp_prec = sig_figs.saturating_sub(1);
    let raw_s = format!("{abs_val:.exp_prec$e}");
    let mut s = if let Ok(exp_num) = raw_s.split('e').next_back().unwrap_or("0").parse::<i32>() {
        if (-4..sig_figs_i32).contains(&exp_num) {
            let dec_prec =
                usize::try_from(core::cmp::max(0, sig_figs_i32 - 1 - exp_num)).unwrap_or(0);
            let mut dec_s = format!("{abs_val:.dec_prec$}");
            if !alt_form {
                dec_s.truncate(trim_trailing_zeros(&dec_s).len());
            }
            dec_s
        } else {
            let exp_char = if is_uppercase { 'E' } else { 'e' };
            if let Some((mantissa, exp)) = raw_s.split_once('e') {
                let m = if alt_form {
                    mantissa
                } else {
                    trim_trailing_zeros(mantissa)
                };
                let exp_val = exp.parse::<i32>().unwrap_or(0);
                format!("{m}{exp_char}{exp_val:+03}")
            } else {
                raw_s
            }
        }
    } else {
        raw_s
    };
    if alt_form {
        ensure_alt_form_decimal(&mut s);
    }
    s
}

fn format_standard_float(
    abs_val: f64,
    precision: Precision,
    alt_form: bool,
    style: FormatStyle,
) -> String {
    let mut s = match (precision, style) {
        (Precision::Fixed(p), _) => {
            let p = p as usize;
            format!("{abs_val:.p$}")
        }
        (_, FormatStyle::Printf) => format!("{abs_val:.DEFAULT_FLOAT_PRECISION$}"),
        (_, FormatStyle::CoreFmt) => format!("{abs_val}"),
    };
    if alt_form && !s.contains('.') {
        s.push('.');
    }
    s
}

fn extract_hex_nybbles(frac_val: u64) -> [u8; F64_HEX_FRAC_NYBBLES] {
    let mut nybbles = [0u8; F64_HEX_FRAC_NYBBLES];
    for (i, nybble) in nybbles.iter_mut().enumerate() {
        *nybble = ((frac_val >> (4 * ((F64_HEX_FRAC_NYBBLES - 1) - i))) & 0xf) as u8;
    }
    nybbles
}

fn round_hex_nybbles(
    nybbles: &mut [u8; F64_HEX_FRAC_NYBBLES],
    p: usize,
    leading: &mut u32,
    frac_val: u64,
) {
    let discarded = nybbles[p];
    let rem_mask = (1u64 << (4 * ((F64_HEX_FRAC_NYBBLES - 1) - p))) - 1;
    let rem = frac_val & rem_mask;
    let round_up = if discarded > 8 {
        true
    } else if discarded < 8 {
        false
    } else if rem > 0 {
        true
    } else {
        let last_digit = if p > 0 {
            nybbles[p - 1]
        } else {
            u8::try_from(*leading).unwrap_or(0)
        };
        (last_digit & 1) != 0
    };

    if !round_up {
        return;
    }

    if p == 0 {
        *leading += 1;
        return;
    }

    let mut carry = 1u8;
    for i in (0..p).rev() {
        let sum = nybbles[i] + carry;
        if sum >= 16 {
            nybbles[i] = sum - 16;
            carry = 1;
        } else {
            nybbles[i] = sum;
            carry = 0;
            break;
        }
    }
    if carry > 0 {
        *leading += 1;
    }
}

fn format_hex_digits(digits: &[u8], hex_table: &[u8; 16]) -> String {
    let mut s = String::with_capacity(digits.len() + 1);
    s.push('.');
    for &d in digits {
        let idx = usize::from(d);
        s.push(hex_table[idx] as char);
    }
    s
}

fn format_hex_fraction_string(
    mut nybbles: [u8; F64_HEX_FRAC_NYBBLES],
    precision: Option<u32>,
    alt_form: bool,
    leading: &mut u32,
    frac_val: u64,
    hex_table: &[u8; 16],
) -> String {
    match precision {
        Some(p) => {
            let p = p as usize;
            if p == 0 {
                round_hex_nybbles(&mut nybbles, 0, leading, frac_val);
                if alt_form {
                    ".".to_string()
                } else {
                    String::new()
                }
            } else if p < F64_HEX_FRAC_NYBBLES {
                round_hex_nybbles(&mut nybbles, p, leading, frac_val);
                format_hex_digits(&nybbles[..p], hex_table)
            } else {
                let mut s = format_hex_digits(&nybbles, hex_table);
                pad_fill(&mut s, '0', p - F64_HEX_FRAC_NYBBLES);
                s
            }
        }
        None => {
            let end = nybbles
                .iter()
                .rposition(|&d| d != 0)
                .map_or(0, |pos| pos + 1);
            if end == 0 {
                if alt_form {
                    ".".to_string()
                } else {
                    String::new()
                }
            } else {
                format_hex_digits(&nybbles[..end], hex_table)
            }
        }
    }
}

fn format_hex_float(
    abs_val: f64,
    is_uppercase: bool,
    precision: Option<u32>,
    alt_form: bool,
) -> String {
    let exp_char = if is_uppercase { 'P' } else { 'p' };

    if abs_val == 0.0 {
        return format_hex_float_zero(exp_char, precision, alt_form);
    }

    let (mut leading, exp, frac_val) = decompose_f64_bits(abs_val);
    let nybbles = extract_hex_nybbles(frac_val);
    let hex_table = if is_uppercase { HEX_UPPER } else { HEX_LOWER };

    let frac_str = format_hex_fraction_string(
        nybbles,
        precision,
        alt_form,
        &mut leading,
        frac_val,
        hex_table,
    );

    let lead_idx = usize::try_from(leading & 0xf).unwrap_or(0);
    let lead_char = hex_table[lead_idx] as char;
    format!("{lead_char}{frac_str}{exp_char}{exp:+}")
}

fn format_hex_float_zero(exp_char: char, precision: Option<u32>, alt_form: bool) -> String {
    let frac = match precision {
        Some(p) if p > 0 => {
            let mut s = String::from(".");
            pad_fill(&mut s, '0', p as usize);
            s
        }
        Some(_) | None if alt_form => ".".to_string(),
        _ => String::new(),
    };
    format!("0{frac}{exp_char}+0")
}

fn decompose_f64_bits(val: f64) -> (u32, i32, u64) {
    let bits = val.to_bits();
    let raw_exp = i32::try_from((bits >> F64_FRAC_BITS) & F64_EXP_MASK).unwrap_or(0);
    let fraction = bits & F64_FRAC_MASK;
    if raw_exp == 0 {
        (0, F64_SUBNORMAL_EXP, fraction)
    } else {
        (1, raw_exp - F64_EXP_BIAS, fraction)
    }
}
