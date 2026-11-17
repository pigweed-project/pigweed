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

use core::convert::Infallible;
use core::marker::PhantomData;
use std::borrow::Cow;
use std::collections::HashSet;

#[cfg(feature = "proc_macro")]
use quote::{quote, ToTokens};

mod float;
mod integer;
mod padding;
mod platform;
mod text;

use self::float::format_float;
use self::integer::{format_int, format_ptr, format_uint};
pub use self::platform::{
    Ilp32Platform, Llp64Platform, Lp64Platform, PlatformTypes, PointerInt, SignedInt, UnsignedInt,
};
use self::text::{format_char, format_str};
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

    /// Exponential rendering (i.e. "%e" or "{:e}").
    Exponential,

    /// Upper case exponential rendering (i.e. "%E" or "{:E}".
    UpperExponential,

    /// Compact shortest floating point representation (i.e. "%g" or "{:g}").
    General,

    /// Upper case compact shortest floating point representation (i.e. "%G" or "{:G}").
    UpperGeneral,

    /// Hexadecimal floating point representation (i.e. "%a" or "{:a}").
    HexFloat,

    /// Upper case hexadecimal floating point representation (i.e. "%A" or "{:A}").
    UpperHexFloat,

    /// Upper case floating point representation (i.e. "%F" or "{:F}").
    Upper,

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
#[cfg(feature = "proc_macro")]
impl ToTokens for Style {
    fn to_tokens(&self, tokens: &mut proc_macro2::TokenStream) {
        let new_tokens = match self {
            Style::None => quote!(pw_format::Style::None),
            Style::Octal => quote!(pw_format::Style::Octal),
            Style::Hex => quote!(pw_format::Style::Hex),
            Style::UpperHex => quote!(pw_format::Style::UpperHex),
            Style::Exponential => quote!(pw_format::Style::Exponential),
            Style::UpperExponential => quote!(pw_format::Style::UpperExponential),
            Style::General => quote!(pw_format::Style::General),
            Style::UpperGeneral => quote!(pw_format::Style::UpperGeneral),
            Style::HexFloat => quote!(pw_format::Style::HexFloat),
            Style::UpperHexFloat => quote!(pw_format::Style::UpperHexFloat),
            Style::Upper => quote!(pw_format::Style::Upper),
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
#[derive(Clone, Debug, Hash, PartialEq, Eq, PartialOrd, Ord)]
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
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MinFieldWidth {
    /// No field width specified.
    None,

    /// Fixed field width.
    Fixed(u32),

    /// Variable field width passed as an argument (i.e. %*d).
    Variable,
}

/// A printf precision (the .5 in %.5d).
///
/// For string conversions (%s) this is treated as the maximum number of
/// bytes of the string to output.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Precision {
    /// No precision specified.
    None,

    /// Fixed precision.
    Fixed(u32),

    /// Variable precision passed as an argument (i.e. %.*f).
    Variable,
}

impl Precision {
    /// Returns the fixed precision value if present, or None otherwise.
    #[must_use]
    pub const fn to_fixed(self) -> Option<u32> {
        match self {
            Self::Fixed(p) => Some(p),
            _ => None,
        }
    }
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

impl Length {
    /// Returns the length modifier string (e.g., `"hh"`, `"l"`).
    #[must_use]
    pub const fn as_str(&self) -> &'static str {
        match self {
            Self::Char => "hh",
            Self::Short => "h",
            Self::Long => "l",
            Self::LongLong => "ll",
            Self::LongDouble => "L",
            Self::IntMax => "j",
            Self::Size => "z",
            Self::PointerDiff => "t",
        }
    }
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

impl Default for ConversionSpec {
    fn default() -> Self {
        Self {
            argument: Argument::None,
            fill: ' ',
            alignment: Alignment::None,
            flags: HashSet::new(),
            min_field_width: MinFieldWidth::None,
            precision: Precision::None,
            length: None,
            primitive: Primitive::Untyped,
            style: Style::None,
        }
    }
}

impl ConversionSpec {
    /// Reconstructs the conversion specifier back to its printf format string representation (e.g., `%+05.2ld`).
    #[must_use]
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
            s.push_str(length.as_str());
        }

        let type_char = match (self.primitive, self.style) {
            (Primitive::Integer, _) => 'd',
            (Primitive::Unsigned, Style::Octal) => 'o',
            (Primitive::Unsigned, Style::Hex) => 'x',
            (Primitive::Unsigned, Style::UpperHex) => 'X',
            (Primitive::Unsigned, _) => 'u',
            (Primitive::Float, Style::Exponential) => 'e',
            (Primitive::Float, Style::UpperExponential) => 'E',
            (Primitive::Float, Style::General) => 'g',
            (Primitive::Float, Style::UpperGeneral) => 'G',
            (Primitive::Float, Style::HexFloat) => 'a',
            (Primitive::Float, Style::UpperHexFloat) => 'A',
            (Primitive::Float, Style::Upper) => 'F',
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
    type Error = core::convert::Infallible;

    fn format_error(&self, spec: &ConversionSpec, _error: &core::convert::Infallible) -> String {
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
    /// Formats a parsed format string with provided arguments using the specified
    /// producing platform's integer and pointer widths (`P: PlatformTypes`).
    #[must_use]
    pub fn format_for_platform<P: PlatformTypes>(
        &self,
        args: &[Arg],
        style: FormatStyle,
    ) -> String {
        let result_args: Vec<Result<Arg, Infallible>> =
            args.iter().map(|arg| Ok(arg.clone())).collect();
        self.format_with_errors_for_platform::<P, _>(&result_args, style, &DefaultFormatter)
    }

    /// Formats a parsed format string with provided arguments.
    #[must_use]
    pub fn format(&self, args: &[Arg], style: FormatStyle) -> String {
        self.format_for_platform::<Ilp32Platform>(args, style)
    }

    /// Formats a parsed format string with the provided argument states (`Result<Arg, FE::Error>`),
    /// delegating formatting of any failures, missing arguments, or type mismatches
    /// to the provided `FormatError` implementation.
    pub fn format_with_errors<FE: FormatError>(
        &self,
        args: &[Result<Arg, FE::Error>],
        style: FormatStyle,
        error_formatter: &FE,
    ) -> String {
        self.format_with_errors_for_platform::<Ilp32Platform, FE>(args, style, error_formatter)
    }

    /// Formats a parsed format string with the provided argument states (`Result<Arg, FE::Error>`)
    /// using the specified producing platform's integer and pointer widths (`P: PlatformTypes`).
    pub fn format_with_errors_for_platform<P: PlatformTypes, FE: FormatError>(
        &self,
        args: &[Result<Arg, FE::Error>],
        style: FormatStyle,
        error_formatter: &FE,
    ) -> String {
        let mut output = String::new();
        let mut session =
            FormatSession::<_, _, P>::new(args.iter(), style, error_formatter, &mut output);

        for fragment in &self.fragments {
            session.format_fragment(fragment);
        }

        output
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

    /// Parses a printf style format string infallibly for runtime formatting,
    /// treating any invalid or incomplete `%` sequence as a literal `'%'` and
    /// continuing to parse subsequent conversion specifiers.
    #[must_use]
    pub fn parse_printf_lossy(s: &str) -> Self {
        printf::format_string_lossy(s)
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
    /// The primary responsibility of this function is to merge literal
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
}

/// Resolved dynamic field width or precision (`*`) integer argument.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum DynamicInt {
    /// Non-negative width or precision magnitude.
    Positive(u32),
    /// Negative width or precision magnitude (indicating left-justification for width
    /// or omitted precision).
    Negative(u32),
}

/// State for formatting a single [`FormatString`] with a sequence of arguments.
///
/// # Lifetimes
///
/// * `'a`: The lifetime of the decoded [`Arg`] items (and any decoding errors)
///   yielded by the `args` iterator `I` (see the `impl` block).
/// * `'b`: The lifetime of the borrowed error formatter (`error_formatter`) and
///   the output string buffer (`output`) for the duration of the formatting
///   session.
struct FormatSession<'b, I, FE, P: PlatformTypes> {
    /// Iterator yielding references to decoded [`Arg`] results for each conversion.
    args: I,
    /// The formatting style ([`FormatStyle::Printf`] or [`FormatStyle::CoreFmt`]).
    style: FormatStyle,
    /// Formatter invoked to produce fallback text when an argument is missing,
    /// fails to decode, or has a mismatched type.
    error_formatter: &'b FE,
    /// Destination string buffer where formatted fragments are appended.
    output: &'b mut String,
    /// Marker for the producing platform's native integer/pointer types.
    _platform: PhantomData<P>,
}

impl<'a, 'b, I, FE, P: PlatformTypes> FormatSession<'b, I, FE, P>
where
    I: Iterator<Item = &'a Result<Arg, FE::Error>>,
    FE: FormatError,
    FE::Error: 'a,
{
    fn new(args: I, style: FormatStyle, error_formatter: &'b FE, output: &'b mut String) -> Self {
        Self {
            args,
            style,
            error_formatter,
            output,
            _platform: PhantomData,
        }
    }

    fn push_missing(&mut self, spec: &ConversionSpec) {
        self.output
            .push_str(&self.error_formatter.format_missing(spec));
    }

    fn push_error(&mut self, spec: &ConversionSpec, err: &FE::Error) {
        self.output
            .push_str(&self.error_formatter.format_error(spec, err));
    }

    fn push_type_error(&mut self, spec: &ConversionSpec, arg: &Arg) {
        self.output
            .push_str(&self.error_formatter.format_type_error(spec, arg));
    }

    fn format_fragment(&mut self, fragment: &FormatFragment) {
        match fragment {
            FormatFragment::Literal(s) => self.output.push_str(s),
            FormatFragment::Conversion(spec) => self.format_conversion(spec),
        }
    }

    fn format_conversion(&mut self, spec: &ConversionSpec) {
        let Some(effective_spec) = self.resolve_dynamic_spec(spec) else {
            return;
        };

        let Some(decoded) = self.args.next() else {
            self.push_missing(spec);
            return;
        };

        let arg = match decoded {
            Ok(arg) => arg,
            Err(err) => {
                self.push_error(spec, err);
                return;
            }
        };

        let initial_len = self.output.len();
        if format_value::<P>(&effective_spec, arg, self.style, self.output).is_err() {
            self.output.truncate(initial_len);
            self.push_type_error(spec, arg);
        }
    }

    fn resolve_dynamic_spec<'s>(
        &mut self,
        spec: &'s ConversionSpec,
    ) -> Option<Cow<'s, ConversionSpec>> {
        if spec.min_field_width != MinFieldWidth::Variable && spec.precision != Precision::Variable
        {
            return Some(Cow::Borrowed(spec));
        }

        let mut resolved = spec.clone();
        resolved.min_field_width = self.resolve_dynamic_width(spec, &mut resolved.flags)?;
        if resolved.flags.contains(&Flag::LeftJustify) {
            resolved.alignment = Alignment::Left;
        }
        resolved.precision = self.resolve_dynamic_precision(spec)?;
        Some(Cow::Owned(resolved))
    }

    fn consume_dynamic_int_arg(&mut self, spec: &ConversionSpec) -> Option<DynamicInt> {
        let Some(decoded) = self.args.next() else {
            self.push_missing(spec);
            return None;
        };
        let arg = match decoded {
            Ok(arg) => arg,
            Err(err) => {
                self.push_error(spec, err);
                return None;
            }
        };

        match arg {
            Arg::Int(val) => match u32::try_from(val.unsigned_abs()) {
                Ok(u) if *val < 0 => Some(DynamicInt::Negative(u)),
                Ok(u) => Some(DynamicInt::Positive(u)),
                Err(_) => {
                    self.push_type_error(spec, arg);
                    None
                }
            },
            Arg::Uint(val) => match u32::try_from(*val) {
                Ok(u) => Some(DynamicInt::Positive(u)),
                Err(_) => {
                    self.push_type_error(spec, arg);
                    None
                }
            },
            other => {
                self.push_type_error(spec, other);
                None
            }
        }
    }

    fn resolve_dynamic_width(
        &mut self,
        spec: &ConversionSpec,
        flags: &mut HashSet<Flag>,
    ) -> Option<MinFieldWidth> {
        match spec.min_field_width {
            MinFieldWidth::Fixed(w) => Some(MinFieldWidth::Fixed(w)),
            MinFieldWidth::None => Some(MinFieldWidth::None),
            MinFieldWidth::Variable => match self.consume_dynamic_int_arg(spec)? {
                DynamicInt::Positive(w) => Some(MinFieldWidth::Fixed(w)),
                DynamicInt::Negative(w) => {
                    flags.insert(Flag::LeftJustify);
                    Some(MinFieldWidth::Fixed(w))
                }
            },
        }
    }

    fn resolve_dynamic_precision(&mut self, spec: &ConversionSpec) -> Option<Precision> {
        match spec.precision {
            Precision::Fixed(p) => Some(Precision::Fixed(p)),
            Precision::None => Some(Precision::None),
            Precision::Variable => match self.consume_dynamic_int_arg(spec)? {
                DynamicInt::Positive(p) => Some(Precision::Fixed(p)),
                DynamicInt::Negative(_) => Some(Precision::None),
            },
        }
    }
}

/// Errors that can occur when formatting an [`Arg`] for a [`ConversionSpec`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FormatValueError {
    /// The conversion specifier and argument types are incompatible.
    MismatchedType,

    /// The argument value exceeds the valid range for the conversion specifier
    /// or target platform type.
    ValueOutOfRange,
}

fn format_value<P: PlatformTypes>(
    spec: &ConversionSpec,
    arg: &Arg,
    style: FormatStyle,
    output: &mut String,
) -> Result<(), FormatValueError> {
    match (spec.primitive, arg) {
        (Primitive::Integer, Arg::Int(v)) => format_int::<P>(*v, spec, style, output),
        (Primitive::Integer, Arg::Uint(v)) => format_int::<P>(v.cast_signed(), spec, style, output),
        (Primitive::Unsigned, Arg::Uint(v)) => format_uint::<P>(*v, spec, style, output),
        (Primitive::Unsigned, Arg::Int(v)) => {
            format_uint::<P>(v.cast_unsigned(), spec, style, output)
        }
        (Primitive::Float, Arg::Float(v)) => format_float(*v, spec, style, output),
        (Primitive::String, Arg::Str(v)) => format_str(v, spec, output),
        (Primitive::Character, Arg::Char(v)) => format_char(*v, spec, output),

        // Note: For integer arguments (`Arg::Int` / `Arg::Uint`) in `128..=255`,
        // `u8... as char` maps to Latin-1 Unicode code points (`U+0080..=U+00FF`),
        // which encode as two-byte UTF-8 sequences in a Rust string.
        (Primitive::Character, Arg::Int(v)) => {
            let c = u8::try_from(*v).map_err(|_| FormatValueError::ValueOutOfRange)? as char;
            format_char(c, spec, output);
        }
        (Primitive::Character, Arg::Uint(v)) => {
            let c = u8::try_from(*v).map_err(|_| FormatValueError::ValueOutOfRange)? as char;
            format_char(c, spec, output);
        }
        (Primitive::Pointer, Arg::Ptr(v)) => {
            format_ptr(P::Pointer::ptr_from_usize(*v)?, spec, output)
        }
        (Primitive::Pointer, Arg::Uint(v)) => {
            format_ptr(P::Pointer::ptr_from_u64(*v), spec, output)
        }
        (Primitive::Pointer, Arg::Int(v)) => {
            format_ptr(P::Pointer::ptr_from_u64(v.cast_unsigned()), spec, output)
        }
        (Primitive::Untyped, _) => format_untyped::<P>(spec, arg, style, output)?,
        _ => return Err(FormatValueError::MismatchedType),
    }
    Ok(())
}

fn format_untyped<P: PlatformTypes>(
    spec: &ConversionSpec,
    arg: &Arg,
    style: FormatStyle,
    output: &mut String,
) -> Result<(), FormatValueError> {
    match arg {
        Arg::Int(v) => format_int::<P>(*v, spec, style, output),
        Arg::Uint(v) => format_uint::<P>(*v, spec, style, output),
        Arg::Float(v) => format_float(*v, spec, style, output),
        Arg::Str(v) => format_str(v, spec, output),
        Arg::Char(v) => format_char(*v, spec, output),
        Arg::Ptr(v) => format_ptr(P::Pointer::ptr_from_usize(*v)?, spec, output),
    }
    Ok(())
}
