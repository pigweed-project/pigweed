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
use core::iter::Peekable;
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

/// Maximum allowed minimum field width or precision (`65,536`) when
/// formatting values at runtime.
///
/// Bounding width and precision specifier counts prevents corrupted or
/// untrusted arguments (such as wire varints in `pw_tokenizer`) or malformed
/// format strings from triggering multi-GiB heap allocations.
pub const MAX_FIELD_LEN: u32 = 0x0001_0000;

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
    /// Returns `true` if fixed width and precision do not exceed
    /// [`MAX_FIELD_LEN`].
    #[must_use]
    pub const fn has_valid_field_lengths(&self) -> bool {
        !matches!(self.min_field_width, MinFieldWidth::Fixed(w) if w > MAX_FIELD_LEN)
            && !matches!(self.precision, Precision::Fixed(p) if p > MAX_FIELD_LEN)
    }

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

/// Domain specification for a nested token.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum NestedDomain {
    /// Default / not-specified domain (e.g. `$#%08x` or `${}#%08x`).
    Default,

    /// A literal domain string (e.g. `${domain}#%08x`).
    Literal(String),

    /// A literal token that gives the domain string (e.g. `${$#01234567}#%08x` or `${${domain}#01234567}#%08x`).
    LiteralToken {
        /// Optional domain for looking up the token (empty for default domain).
        domain: String,
        /// The literal token value.
        token: u32,
    },

    /// A variable domain token requiring a read from the argument data (e.g. `${$#%08x}#%08x` or `${${domain}#%08x}#%08x`).
    VariableToken {
        /// Optional domain for looking up the variable token (empty for default domain).
        domain: String,
    },
}

/// Token specification for a nested token.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NestedTokenValue {
    /// A literal token value (e.g. `$#01234567` or `$10#0000000010`).
    Literal(u32),

    /// A variable token requiring a read from the argument data (`%08x` or `%010d`).
    Variable,
}

/// A nested token specification (e.g. `$#%08x`, `${domain}#%08x`, `${$#%08x}#%08x`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NestedTokenSpec {
    /// Prefix character, typically `'$'`.
    pub prefix: char,

    /// Domain specification.
    pub domain: NestedDomain,

    /// The token value specification.
    pub token: NestedTokenValue,

    /// Additional token argument data decoded from base64 (`None` if not base64).
    pub token_data: Option<Vec<u8>>,

    /// The raw format string slice representing this nested token.
    ///
    /// Preserved verbatim because parsing SEED-0105 nested tokens into
    /// `(prefix, domain, token)` is intentionally lossy across:
    /// - stripped whitespace inside `{ ... }` domain braces (e.g. `${ my domain }#...`
    ///   and `${}#%08x` vs `$#%08x`),
    /// - explicit radix specifiers (`$10#...`, `$16#...`, `$64#...`),
    /// - hex letter casing (`$#DEADBEEF` vs `$#deadbeef`), and
    /// - Base64 nested tokens with embedded binary argument payloads (`$AQIDBA==`).
    ///
    /// Retaining the verbatim source slice ensures fallback formatting when a
    /// nested token cannot be decoded reproduces the exact original text.
    pub raw: String,
}

impl Default for NestedTokenSpec {
    fn default() -> Self {
        Self {
            prefix: FormatString::DEFAULT_NESTED_PREFIX,
            domain: NestedDomain::Default,
            token: NestedTokenValue::Variable,
            token_data: None,
            raw: String::new(),
        }
    }
}

impl NestedTokenSpec {
    /// Returns the additional token argument data as an optional byte slice.
    #[must_use]
    pub fn token_data(&self) -> Option<&[u8]> {
        self.token_data.as_deref()
    }

    /// Returns the raw format string slice for this nested token.
    #[must_use]
    pub fn raw(&self) -> &str {
        &self.raw
    }

    /// Reconstructs the nested token specification back to its printf format string representation.
    #[must_use]
    pub fn to_printf(&self) -> String {
        self.raw.clone()
    }
}

/// A fragment of a printf format string.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum FormatFragment {
    /// A literal string value.
    Literal(String),

    /// A conversion specification (i.e. %d).
    Conversion(ConversionSpec),

    /// A nested token specification (i.e. $#%08x or ${domain}#%08x).
    NestedToken(NestedTokenSpec),
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

    /// Renders a nested token specification that failed with a domain-specific error.
    fn format_nested_token_error(&self, spec: &NestedTokenSpec, _error: &Self::Error) -> String {
        spec.to_printf()
    }

    /// Renders a nested token specification that was missing from the supplied arguments.
    fn format_nested_token_missing(&self, spec: &NestedTokenSpec) -> String {
        spec.to_printf()
    }

    /// Renders a nested token specification that decoded successfully but failed to format (type mismatch).
    fn format_nested_token_type_error(&self, spec: &NestedTokenSpec, _arg: &Arg) -> String {
        spec.to_printf()
    }
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

    /// Default prefix character for nested token specifiers.
    pub const DEFAULT_NESTED_PREFIX: char = '$';

    /// Parses a printf style format string using the default nested token prefix ('$').
    pub fn parse_printf(s: &str) -> Result<Self, String> {
        Self::parse_printf_with_prefix(s, Self::DEFAULT_NESTED_PREFIX)
    }

    /// Parses a printf style format string using a specified nested token prefix.
    pub fn parse_printf_with_prefix(s: &str, prefix: char) -> Result<Self, String> {
        Self::parse_printf_opt_prefix(s, Some(prefix))
    }

    /// Parses a pure printf style format string without nested token parsing.
    pub fn parse_pure_printf(s: &str) -> Result<Self, String> {
        Self::parse_printf_opt_prefix(s, None)
    }

    fn parse_printf_opt_prefix(s: &str, prefix: Option<char>) -> Result<Self, String> {
        // TODO: b/281858500 - Add better errors to failed parses.
        let (rest, result) = printf::format_string(s, prefix)
            .map_err(|e| format!("Failed to parse format string \"{s}\": {e}"))?;

        // If the parser did not consume all the input, return an error.
        if !rest.is_empty() {
            return Err(format!(
                "Failed to parse format string fragment \"{rest}\" in \"{s}\""
            ));
        }

        Ok(result)
    }

    /// Parses a printf style format string infallibly using the default nested
    /// token prefix (`'$'`), treating any invalid or incomplete `%` sequence as
    /// a literal `'%'` and continuing to parse subsequent conversion specifiers
    /// and nested tokens.
    #[must_use]
    pub fn parse_printf_lossy(s: &str) -> Self {
        Self::parse_printf_lossy_with_prefix(s, Self::DEFAULT_NESTED_PREFIX)
    }

    /// Parses a printf style format string infallibly using a specified nested
    /// token prefix, treating any invalid or incomplete `%` sequence as a
    /// literal `'%'` and continuing to parse subsequent conversion specifiers
    /// and nested tokens.
    #[must_use]
    pub fn parse_printf_lossy_with_prefix(s: &str, prefix: char) -> Self {
        printf::format_string_lossy(s, Some(prefix))
    }

    /// Parses plain text containing nested token specifiers using the default prefix (`'$'`),
    /// treating `%` characters as literal text rather than conversion specifiers.
    #[must_use]
    pub fn parse_nested_tokens(s: &str) -> Self {
        Self::parse_nested_tokens_with_prefix(s, Self::DEFAULT_NESTED_PREFIX)
    }

    /// Parses plain text containing nested token specifiers using a specified prefix,
    /// treating `%` characters as literal text rather than conversion specifiers.
    #[must_use]
    pub fn parse_nested_tokens_with_prefix(s: &str, prefix: char) -> Self {
        let (_, result) = printf::nested_tokens_string(s, prefix)
            .expect("nested_tokens_string is infallible on all UTF-8 inputs");
        result
    }

    /// Parses a core::fmt style format string.
    pub fn parse_core_fmt(s: &str) -> Result<Self, String> {
        // TODO: b/281858500 - Add better errors to failed parses.
        let (rest, result) = core_fmt::format_string(s)
            .map_err(|e| format!("Failed to parse format string \"{s}\": {e}"))?;

        // If the parser did not consume all the input, return an error.
        if !rest.is_empty() {
            return Err(format!(
                "Failed to parse format string fragment \"{rest}\" in \"{s}\""
            ));
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

/// State for formatting a single [`FormatString`] with a sequence of arguments.
///
/// # Lifetimes
///
/// * `'a`: The lifetime of the decoded [`Arg`] items (and any decoding errors)
///   yielded by the `args` iterator `I`.
/// * `'b`: The lifetime of the borrowed error formatter (`error_formatter`) and
///   the output string buffer (`output`) for the duration of the formatting
///   session.
struct FormatSession<'a, 'b, I, FE, P: PlatformTypes>
where
    I: Iterator<Item = &'a Result<Arg, FE::Error>>,
    FE: FormatError,
    FE::Error: 'a,
{
    /// Peekable iterator yielding references to decoded [`Arg`] results for each conversion.
    args: Peekable<I>,
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

impl<'a, 'b, I, FE, P: PlatformTypes> FormatSession<'a, 'b, I, FE, P>
where
    I: Iterator<Item = &'a Result<Arg, FE::Error>>,
    FE: FormatError,
    FE::Error: 'a,
{
    fn new(args: I, style: FormatStyle, error_formatter: &'b FE, output: &'b mut String) -> Self {
        Self {
            args: args.peekable(),
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

    // Only consume `Ok` slots so downstream `Err` markers (such as
    // `TokenizerError::Skipped` after an aborted decode) stay intact.
    fn drain_remaining_ok_args(&mut self, count: usize) {
        for _ in 0..count {
            if matches!(self.args.peek(), Some(Ok(_))) {
                self.args.next();
            } else {
                break;
            }
        }
    }

    fn fail_dynamic_arg(&mut self, spec: &ConversionSpec, arg: &Arg, remaining_ok_slots: usize) {
        self.push_type_error(spec, arg);
        self.drain_remaining_ok_args(remaining_ok_slots);
    }

    fn next_arg(&mut self, spec: &ConversionSpec, remaining_ok_slots: usize) -> Option<&'a Arg> {
        let Some(decoded) = self.args.next() else {
            self.push_missing(spec);
            return None;
        };
        match decoded {
            Ok(arg) => Some(arg),
            Err(err) => {
                self.push_error(spec, err);
                self.drain_remaining_ok_args(remaining_ok_slots);
                None
            }
        }
    }

    fn format_fragment(&mut self, fragment: &FormatFragment) {
        match fragment {
            FormatFragment::Literal(s) => self.output.push_str(s),
            FormatFragment::NestedToken(token_spec) => self.format_nested_token(token_spec),
            FormatFragment::Conversion(spec) => self.format_conversion(spec),
        }
    }

    fn format_nested_token(&mut self, token_spec: &NestedTokenSpec) {
        let Some(decoded) = self.args.next() else {
            self.output
                .push_str(&self.error_formatter.format_nested_token_missing(token_spec));
            return;
        };

        let arg = match decoded {
            Ok(arg) => arg,
            Err(err) => {
                self.output.push_str(
                    &self
                        .error_formatter
                        .format_nested_token_error(token_spec, err),
                );
                return;
            }
        };

        if let Arg::Str(s) = arg {
            self.output.push_str(s);
        } else {
            self.output.push_str(
                &self
                    .error_formatter
                    .format_nested_token_type_error(token_spec, arg),
            );
        }
    }

    fn format_conversion(&mut self, spec: &ConversionSpec) {
        let Some(effective_spec) = self.resolve_dynamic_spec(spec) else {
            return;
        };

        let Some(arg) = self.next_arg(spec, 0) else {
            return;
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

    fn resolve_dynamic_width(
        &mut self,
        spec: &ConversionSpec,
        flags: &mut HashSet<Flag>,
    ) -> Option<MinFieldWidth> {
        let MinFieldWidth::Variable = spec.min_field_width else {
            return Some(spec.min_field_width);
        };
        let remaining_slots = usize::from(spec.precision == Precision::Variable) + 1;
        let arg = self.next_arg(spec, remaining_slots)?;
        let (width, is_negative) = match arg {
            Arg::Int(val) => (validate_field_len(val.unsigned_abs()), *val < 0),
            Arg::Uint(val) => (validate_field_len(*val), false),
            _ => (None, false),
        };
        let Some(w) = width else {
            self.fail_dynamic_arg(spec, arg, remaining_slots);
            return None;
        };
        if is_negative {
            flags.insert(Flag::LeftJustify);
        }
        Some(MinFieldWidth::Fixed(w))
    }

    fn resolve_dynamic_precision(&mut self, spec: &ConversionSpec) -> Option<Precision> {
        let Precision::Variable = spec.precision else {
            return Some(spec.precision);
        };
        let arg = self.next_arg(spec, 1)?;
        let resolved = match arg {
            // Per C99 §7.19.6.1p5, a negative 32-bit C `int` precision is treated
            // as omitted; values below `i32::MIN` are rejected as out of range.
            Arg::Int(val) if *val < 0 => i32::try_from(*val).ok().map(|_| Precision::None),
            Arg::Int(val) => validate_field_len(val.cast_unsigned()).map(Precision::Fixed),
            Arg::Uint(val) => validate_field_len(*val).map(Precision::Fixed),
            _ => None,
        };
        let Some(precision) = resolved else {
            self.fail_dynamic_arg(spec, arg, 1);
            return None;
        };
        Some(precision)
    }
}

fn validate_field_len(val: u64) -> Option<u32> {
    u32::try_from(val).ok().filter(|&v| v <= MAX_FIELD_LEN)
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
    if !spec.has_valid_field_lengths() {
        return Err(FormatValueError::ValueOutOfRange);
    }

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
