// Copyright 2023 The Pigweed Authors
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

use std::collections::HashSet;

use quote::{quote, ToTokens};

use crate::{core_fmt, printf};

/// Primitive type of a conversion (integer, float, string, etc.)
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Primitive {
    /// Signed integer primitive.
    Integer,

    /// Unsigned integer primitive.
    Unsigned,

    /// Floating point primitive.
    Float,

    /// String primitive.
    String,

    /// Character primitive.
    Character,

    /// Pointer primitive.
    Pointer,

    /// Untyped primitive.
    Untyped,
}

/// The abstract formatting style for a conversion.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Style {
    /// No style specified, use defaults.
    None,

    /// Octal rendering (i.e. "%o" or "{:o}").
    Octal,

    /// Hexadecimal rendering (i.e. "%x" or "{:x}").
    Hex,

    /// Upper case hexadecimal rendering (i.e. "%X" or "{:X}").
    UpperHex,

    /// Exponential rendering (i.e. "%e" or "{:e}".
    Exponential,

    /// Upper case exponential rendering (i.e. "%E" or "{:E}".
    UpperExponential,

    /// Pointer type rendering (i.e. "%p" or "{:p}").
    Pointer,

    /// `core::fmt`'s `{:?}`
    Debug,

    /// `core::fmt`'s `{:x?}`
    HexDebug,

    /// `core::fmt`'s `{:X?}`
    UpperHexDebug,

    /// Unsupported binary rendering
    ///
    /// This variant exists so that the proc macros can give useful error
    /// messages.
    Binary,
}

/// Implemented for testing through the pw_format_test_macros crate.
impl ToTokens for Style {
    fn to_tokens(&self, tokens: &mut proc_macro2::TokenStream) {
        let new_tokens = match self {
            Style::None => quote!(pw_format::Style::None),
            Style::Octal => quote!(pw_format::Style::Octal),
            Style::Hex => quote!(pw_format::Style::Hex),
            Style::UpperHex => quote!(pw_format::Style::UpperHex),
            Style::Exponential => quote!(pw_format::Style::Exponential),
            Style::UpperExponential => quote!(pw_format::Style::UpperExponential),
            Style::Debug => quote!(pw_format::Style::Debug),
            Style::HexDebug => quote!(pw_format::Style::HexDebug),
            Style::UpperHexDebug => quote!(pw_format::Style::UpperHexDebug),
            Style::Pointer => quote!(pw_format::Style::Pointer),
            Style::Binary => quote!(pw_format::Style::Binary),
        };
        new_tokens.to_tokens(tokens);
    }
}

/// A printf flag (the '+' in %+d).
#[derive(Clone, Debug, Hash, PartialEq, Eq)]
pub enum Flag {
    /// `-`
    LeftJustify,

    /// `+`
    ForceSign,

    /// ` `
    SpaceSign,

    /// `#`
    AlternateSyntax,

    /// `0`
    LeadingZeros,
}

/// A printf minimum field width (the 5 in %5d).
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MinFieldWidth {
    /// No field width specified.
    None,

    /// Fixed field with.
    Fixed(u32),

    /// Variable field width passed as an argument (i.e. %*d).
    Variable,
}

/// A printf precision (the .5 in %.5d).
///
/// For string conversions (%s) this is treated as the maximum number of
/// bytes of the string to output.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Precision {
    /// No precision specified.
    None,

    /// Fixed precision.
    Fixed(u32),

    /// Variable precision passed as an argument (i.e. %.*f).
    Variable,
}

/// A printf length (the l in %ld).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Length {
    /// `hh`
    Char,

    /// `h`
    Short,

    /// `l`
    Long,

    /// `ll`
    LongLong,

    /// `L`
    LongDouble,

    /// `j`
    IntMax,

    /// `z`
    Size,

    /// `t`
    PointerDiff,
}

/// A core::fmt alignment spec.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Alignment {
    /// No alignment
    None,

    /// Left alignment (`<`)
    Left,

    /// Center alignment (`^`)
    Center,

    /// Right alignment (`>`)
    Right,
}

/// An argument in a core::fmt style alignment spec.
///
/// i.e. the var_name in `{var_name:#0x}`
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Argument {
    /// No argument
    None,

    /// A positional argument (i.e. `{0}`).
    Positional(usize),

    /// A named argument (i.e. `{var_name}`).
    Named(String),
}

/// A printf conversion specification aka a % clause.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ConversionSpec {
    /// ConversionSpec's argument.
    pub argument: Argument,
    /// ConversionSpec's fill character.
    pub fill: char,
    /// ConversionSpec's field alignment.
    pub alignment: Alignment,
    /// ConversionSpec's set of [Flag]s.
    pub flags: HashSet<Flag>,
    /// ConversionSpec's minimum field width argument.
    pub min_field_width: MinFieldWidth,
    /// ConversionSpec's [Precision] argument.
    pub precision: Precision,
    /// ConversionSpec's [Length] argument.
    pub length: Option<Length>,
    /// ConversionSpec's [Primitive].
    pub primitive: Primitive,
    /// ConversionSpec's [Style].
    pub style: Style,
}

impl ConversionSpec {
    /// Reconstructs the conversion specifier back to its printf format string representation (e.g., `%+05.2ld`).
    pub fn to_printf(&self) -> String {
        let mut s = String::from("%");
        if self.flags.contains(&Flag::LeftJustify) {
            s.push('-');
        }
        if self.flags.contains(&Flag::ForceSign) {
            s.push('+');
        }
        if self.flags.contains(&Flag::SpaceSign) {
            s.push(' ');
        }
        if self.flags.contains(&Flag::AlternateSyntax) {
            s.push('#');
        }
        if self.flags.contains(&Flag::LeadingZeros) {
            s.push('0');
        }

        match self.min_field_width {
            MinFieldWidth::None => {}
            MinFieldWidth::Fixed(w) => s.push_str(&w.to_string()),
            MinFieldWidth::Variable => s.push('*'),
        }

        match self.precision {
            Precision::None => {}
            Precision::Fixed(p) => s.push_str(&format!(".{p}")),
            Precision::Variable => s.push_str(".*"),
        }

        if let Some(length) = self.length {
            s.push_str(match length {
                Length::Char => "hh",
                Length::Short => "h",
                Length::Long => "l",
                Length::LongLong => "ll",
                Length::LongDouble => "L",
                Length::IntMax => "j",
                Length::Size => "z",
                Length::PointerDiff => "t",
            });
        }

        let type_char = match (self.primitive, self.style) {
            (Primitive::Integer, _) => 'd',
            (Primitive::Unsigned, Style::Octal) => 'o',
            (Primitive::Unsigned, Style::Hex) => 'x',
            (Primitive::Unsigned, Style::UpperHex) => 'X',
            (Primitive::Unsigned, _) => 'u',
            (Primitive::Float, Style::Exponential) => 'e',
            (Primitive::Float, Style::UpperExponential) => 'E',
            (Primitive::Float, _) => 'f',
            (Primitive::Character, _) => 'c',
            (Primitive::String, _) => 's',
            (Primitive::Pointer, _) => 'p',
            (Primitive::Untyped, _) => 'v',
        };
        s.push(type_char);
        s
    }
}

/// A fragment of a printf format string.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum FormatFragment {
    /// A literal string value.
    Literal(String),

    /// A conversion specification (i.e. %d).
    Conversion(ConversionSpec),
}

impl FormatFragment {
    /// Try to append `fragment` to `self`.
    ///
    /// Returns `None` if the appending succeeds and `Some<fragment>` if it fails.
    fn try_append<'a>(&mut self, fragment: &'a FormatFragment) -> Option<&'a FormatFragment> {
        let Self::Literal(literal_fragment) = &fragment else {
            return Some(fragment);
        };

        let Self::Literal(literal_self) = self else {
            return Some(fragment);
        };

        literal_self.push_str(literal_fragment);

        None
    }
}

/// Representation of a decoded argument.
#[derive(Debug, Clone, PartialEq)]
pub enum Arg {
    /// Signed integer.
    Int(i64),
    /// Unsigned integer.
    Uint(u64),
    /// Floating point number.
    Float(f64),
    /// String.
    Str(String),
    /// Character.
    Char(char),
    /// Pointer.
    Ptr(usize),
}

/// The style of formatting to apply (influences defaults).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FormatStyle {
    /// Printf style defaults (e.g. %f defaults to precision 6).
    Printf,
    /// Core::fmt style defaults.
    CoreFmt,
}

/// A trait for formatting conversion specifiers that failed, were skipped, or were missing.
pub trait FormatError {
    /// The domain-specific error type.
    type Error;

    /// Renders a conversion specifier that failed with a domain-specific error.
    fn format_error(&self, spec: &ConversionSpec, error: &Self::Error) -> String;

    /// Renders a conversion specifier that was missing from the supplied arguments.
    fn format_missing(&self, spec: &ConversionSpec) -> String;

    /// Renders a conversion specifier that decoded successfully but failed to format (type mismatch).
    fn format_type_error(&self, spec: &ConversionSpec, arg: &Arg) -> String;
}

/// Formatter that retains the original conversion specifier when formatting fails,
/// parameterizing over `std::convert::Infallible` (which has no error state).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct DefaultFormatter;

impl FormatError for DefaultFormatter {
    type Error = std::convert::Infallible;

    fn format_error(&self, spec: &ConversionSpec, _error: &std::convert::Infallible) -> String {
        spec.to_printf()
    }
    fn format_missing(&self, spec: &ConversionSpec) -> String {
        spec.to_printf()
    }
    fn format_type_error(&self, spec: &ConversionSpec, _arg: &Arg) -> String {
        spec.to_printf()
    }
}

/// A parsed format string.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FormatString {
    /// The [FormatFragment]s that comprise the [FormatString].
    pub fragments: Vec<FormatFragment>,
}

impl FormatString {
    /// Formats a parsed format string with provided arguments.
    pub fn format(&self, args: &[Arg], style: FormatStyle) -> String {
        let result_args: Vec<Result<Arg, std::convert::Infallible>> =
            args.iter().map(|arg| Ok(arg.clone())).collect();

        self.format_with_errors(&result_args, style, &DefaultFormatter)
    }

    /// Formats a parsed format string with the provided argument states (Result<Arg, FE::Error>),
    /// delegating formatting of any failures, missing arguments, or type mismatches
    /// to the provided `FormatError` implementation.
    pub fn format_with_errors<FE: FormatError>(
        &self,
        args: &[Result<Arg, FE::Error>],
        style: FormatStyle,
        error_formatter: &FE,
    ) -> String {
        let mut output = String::new();
        let mut args_iter = args.iter();

        for fragment in &self.fragments {
            self.format_fragment(
                fragment,
                &mut args_iter,
                style,
                error_formatter,
                &mut output,
            );
        }

        output
    }

    fn format_fragment<'a, FE: FormatError>(
        &self,
        fragment: &FormatFragment,
        args: &mut impl Iterator<Item = &'a Result<Arg, FE::Error>>,
        style: FormatStyle,
        error_formatter: &FE,
        output: &mut String,
    ) where
        FE::Error: 'a,
    {
        let spec = match fragment {
            FormatFragment::Conversion(spec) => spec,
            FormatFragment::Literal(s) => {
                output.push_str(s);
                return;
            }
        };

        let Some(decoded) = args.next() else {
            output.push_str(&error_formatter.format_missing(spec));
            return;
        };

        let arg = match decoded {
            Ok(arg) => arg,
            Err(err) => {
                output.push_str(&error_formatter.format_error(spec, err));
                return;
            }
        };

        let mut formatted = String::new();
        match self.format_value(spec, arg, style, &mut formatted) {
            Ok(()) => output.push_str(&formatted),
            Err(_) => {
                output.push_str(&error_formatter.format_type_error(spec, arg));
            }
        }
    }

    /// Parses a printf style format string.
    pub fn parse_printf(s: &str) -> Result<Self, String> {
        // TODO: b/281858500 - Add better errors to failed parses.
        let (rest, result) = printf::format_string(s)
            .map_err(|e| format!("Failed to parse format string \"{s}\": {e}"))?;

        // If the parser did not consume all the input, return an error.
        if !rest.is_empty() {
            return Err(format!(
                "Failed to parse format string fragment: \"{rest}\""
            ));
        }

        Ok(result)
    }

    /// Parses a core::fmt style format string.
    pub fn parse_core_fmt(s: &str) -> Result<Self, String> {
        // TODO: b/281858500 - Add better errors to failed parses.
        let (rest, result) = core_fmt::format_string(s)
            .map_err(|e| format!("Failed to parse format string \"{s}\": {e}"))?;

        // If the parser did not consume all the input, return an error.
        if !rest.is_empty() {
            return Err(format!("Failed to parse format string: \"{rest}\""));
        }

        Ok(result)
    }

    /// Creates a `FormatString` from a slice of fragments.
    ///
    /// This primary responsibility of this function is to merge literal
    /// fragments.  Adjacent literal fragments occur when a parser parses
    /// escape sequences.  Merging them here allows a
    /// [`macros::FormatMacroGenerator`] to not worry about the escape codes.
    pub(crate) fn from_fragments(fragments: &[FormatFragment]) -> Self {
        Self {
            fragments: fragments
                .iter()
                .fold(Vec::<_>::new(), |mut fragments, fragment| {
                    // Collapse adjacent literal fragments.
                    let Some(last) = fragments.last_mut() else {
                        // If there are no accumulated fragments, add this one and return.
                        fragments.push((*fragment).clone());
                        return fragments;
                    };
                    if let Some(fragment) = last.try_append(fragment) {
                        // If the fragments were able to append, no more work to do
                        fragments.push((*fragment).clone());
                    };
                    fragments
                }),
        }
    }

    fn format_value(
        &self,
        spec: &ConversionSpec,
        arg: &Arg,
        style: FormatStyle,
        output: &mut String,
    ) -> Result<(), String> {
        match (spec.primitive, arg) {
            (Primitive::Integer, Arg::Int(v)) => self.format_int(*v, spec, style, output),
            (Primitive::Unsigned, Arg::Uint(v)) => self.format_uint(*v, spec, style, output),
            (Primitive::Float, Arg::Float(v)) => self.format_float(*v, spec, style, output),
            (Primitive::String, Arg::Str(v)) => self.format_str(v, spec, style, output),
            (Primitive::Character, Arg::Char(v)) => self.format_char(*v, spec, style, output),
            (Primitive::Pointer, Arg::Ptr(v)) => self.format_ptr(*v, spec, style, output),
            (Primitive::Untyped, _) => self.format_untyped(spec, arg, style, output),
            _ => Err(format!(
                "Mismatched type: expected {:?}, got {:?}",
                spec.primitive, arg
            )),
        }
    }

    fn format_untyped(
        &self,
        spec: &ConversionSpec,
        arg: &Arg,
        style: FormatStyle,
        output: &mut String,
    ) -> Result<(), String> {
        match arg {
            Arg::Int(v) => self.format_int(*v, spec, style, output),
            Arg::Uint(v) => self.format_uint(*v, spec, style, output),
            Arg::Float(v) => self.format_float(*v, spec, style, output),
            Arg::Str(v) => self.format_str(v, spec, style, output),
            Arg::Char(v) => self.format_char(*v, spec, style, output),
            Arg::Ptr(v) => self.format_ptr(*v, spec, style, output),
        }
    }

    fn format_int_common(
        &self,
        v: u64,
        sign: &str,
        spec: &ConversionSpec,
        output: &mut String,
    ) -> Result<(), String> {
        let (base_prefix, mut value) = match spec.style {
            Style::Hex | Style::Pointer => ("0x", format!("{:x}", v)),
            Style::UpperHex => ("0X", format!("{:X}", v)),
            Style::Octal => ("0", format!("{:o}", v)),
            _ => ("", format!("{}", v)),
        };

        if let Precision::Fixed(p) = spec.precision {
            while value.len() < p as usize {
                value.insert(0, '0');
            }
        }

        let mut prefix = sign.to_string();
        if spec.flags.contains(&Flag::AlternateSyntax) || spec.style == Style::Pointer {
            // For octal, it's possible that the value string already starts with the prefix.
            if !value.starts_with(base_prefix) {
                prefix.push_str(base_prefix);
            }
        }

        let s = self.apply_width_and_alignment(&prefix, &value, spec)?;
        output.push_str(&s);
        Ok(())
    }

    fn sign_prefix(&self, is_negative: bool, spec: &ConversionSpec) -> &'static str {
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

    fn format_int(
        &self,
        v: i64,
        spec: &ConversionSpec,
        _style: FormatStyle,
        output: &mut String,
    ) -> Result<(), String> {
        let sign = self.sign_prefix(v < 0, spec);
        self.format_int_common(v.unsigned_abs(), sign, spec, output)
    }

    fn format_uint(
        &self,
        v: u64,
        spec: &ConversionSpec,
        _style: FormatStyle,
        output: &mut String,
    ) -> Result<(), String> {
        self.format_int_common(v, "", spec, output)
    }

    fn format_float(
        &self,
        v: f64,
        spec: &ConversionSpec,
        style: FormatStyle,
        output: &mut String,
    ) -> Result<(), String> {
        let abs_v = v.abs();
        let value = match spec.precision {
            Precision::Fixed(p) => format!("{:.1$}", abs_v, p as usize),
            _ => match style {
                FormatStyle::Printf => format!("{:.6}", abs_v),
                FormatStyle::CoreFmt => format!("{}", abs_v),
            },
        };
        let prefix = self.sign_prefix(v < 0.0 || v.is_sign_negative(), spec);

        let s = self.apply_width_and_alignment(prefix, &value, spec)?;
        output.push_str(&s);
        Ok(())
    }

    fn format_str(
        &self,
        v: &str,
        spec: &ConversionSpec,
        _style: FormatStyle,
        output: &mut String,
    ) -> Result<(), String> {
        let mut value = v.to_string();
        if let Precision::Fixed(p) = spec.precision {
            value.truncate(p as usize);
        }
        let s = self.apply_width_and_alignment("", &value, spec)?;
        output.push_str(&s);
        Ok(())
    }

    fn format_char(
        &self,
        v: char,
        spec: &ConversionSpec,
        _style: FormatStyle,
        output: &mut String,
    ) -> Result<(), String> {
        let value = v.to_string();
        let s = self.apply_width_and_alignment("", &value, spec)?;
        output.push_str(&s);
        Ok(())
    }

    fn format_ptr(
        &self,
        v: usize,
        spec: &ConversionSpec,
        _style: FormatStyle,
        output: &mut String,
    ) -> Result<(), String> {
        self.format_int_common(v as u64, "", spec, output)
    }

    fn apply_width_and_alignment(
        &self,
        prefix: &str,
        value: &str,
        spec: &ConversionSpec,
    ) -> Result<String, String> {
        // If there is no fixed field width, format w/o padding.
        // Variable field width is unsupported for now.
        let MinFieldWidth::Fixed(w) = spec.min_field_width else {
            return Ok(format!("{}{}", prefix, value));
        };

        let w = w as usize;
        let total_len = prefix.len() + value.len();

        // If the value overflows the minimum field width, format w/o padding.
        if total_len >= w {
            return Ok(format!("{}{}", prefix, value));
        }

        let pad_len = w - total_len;
        let ignore_zero = spec.flags.contains(&Flag::LeftJustify)
            || (matches!(spec.precision, Precision::Fixed(_))
                && matches!(spec.primitive, Primitive::Integer | Primitive::Unsigned));
        let do_zero_fill = spec.flags.contains(&Flag::LeadingZeros) && !ignore_zero;
        let is_left_aligned =
            spec.alignment == Alignment::Left || spec.flags.contains(&Flag::LeftJustify);

        let mut s = String::with_capacity(w);
        if is_left_aligned {
            // Left justified values are never zero filled.
            s.push_str(prefix);
            s.push_str(value);
            for _ in 0..pad_len {
                s.push(spec.fill);
            }
        } else {
            if do_zero_fill {
                // Zero fill happens after the prefix like '0x001' or '-0001'.
                s.push_str(prefix);
                for _ in 0..pad_len {
                    s.push('0');
                }
            } else {
                // Normal fill happens after the prefix like '  0x1' or '  -01'.
                for _ in 0..pad_len {
                    s.push(spec.fill);
                }
                s.push_str(prefix);
            }
            s.push_str(value);
        }
        Ok(s)
    }
}
