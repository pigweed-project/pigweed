// Copyright 2024 The Pigweed Authors
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

//! # Unsupported core::fmt features
//! * Argument widths or precisions: `{:0$}` or `{:varname$}`

use std::collections::HashSet;

use nom::branch::alt;
use nom::bytes::complete::{tag, take_till1, take_while};
use nom::character::complete::{alpha1, alphanumeric1, anychar, digit1};
use nom::combinator::{map, map_res, opt, recognize, value};
use nom::multi::{many0, many0_count};
use nom::sequence::pair;
use nom::IResult;

use crate::parser_util::{fixed_width, precision};
use crate::{
    Alignment, Argument, ConversionSpec, Flag, FormatFragment, FormatString, MinFieldWidth,
    Precision, Primitive, Style,
};

/// The `name` in a `{name}` format string.  Matches a Rust identifier.
fn named_argument(input: &str) -> IResult<&str, Argument> {
    let (input, ident) = recognize(pair(
        alt((alpha1, tag("_"))),
        many0_count(alt((alphanumeric1, tag("_")))),
    ))(input)?;

    Ok((input, Argument::Named(ident.to_string())))
}

/// The decimal value a `{0}` format string.  Matches a decimal value.
fn positional_argument(input: &str) -> IResult<&str, Argument> {
    let (input, index) = map_res(digit1, |val: &str| val.parse::<usize>())(input)?;

    Ok((input, Argument::Positional(index)))
}

/// No argument value.
///
/// Fallback and does not consume any data.
fn none_argument(input: &str) -> IResult<&str, Argument> {
    Ok((input, Argument::None))
}

/// An optional named or positional argument.
///
/// ie. `{name:...}` or `{0:...}` of `{:...}
fn argument(input: &str) -> IResult<&str, Argument> {
    alt((named_argument, positional_argument, none_argument))(input)
}

/// An explicit formatting type
///
/// i.e. the `x?` in `{:x?}
fn explicit_type(input: &str) -> IResult<&str, Style> {
    alt((
        value(Style::Debug, tag("?")),
        value(Style::HexDebug, tag("x?")),
        value(Style::UpperHexDebug, tag("X?")),
        value(Style::Octal, tag("o")),
        value(Style::Hex, tag("x")),
        value(Style::UpperHex, tag("X")),
        value(Style::Pointer, tag("p")),
        value(Style::Binary, tag("b")),
        value(Style::Exponential, tag("e")),
        value(Style::UpperExponential, tag("E")),
    ))(input)
}

/// An optional explicit formatting type
///
/// i.e. the `x?` in `{:x?} or no type as in `{:}`
fn style(input: &str) -> IResult<&str, Style> {
    let (input, spec) = explicit_type(input).unwrap_or((input, Style::None));

    Ok((input, spec))
}

/// A formatting flag.  One of `-`, `+`, `#`, or `0`.
fn map_flag(value: char) -> Result<Flag, String> {
    match value {
        '-' => Ok(Flag::LeftJustify),
        '+' => Ok(Flag::ForceSign),
        '#' => Ok(Flag::AlternateSyntax),
        '0' => Ok(Flag::LeadingZeros),
        _ => Err(format!("Unsupported flag '{value}'")),
    }
}

/// A collection of one or more formatting flags (`-`, `+`, `#`, or `0`).
fn flags(input: &str) -> IResult<&str, HashSet<Flag>> {
    let (input, flags) = many0(map_res(anychar, map_flag))(input)?;

    Ok((input, flags.into_iter().collect()))
}

fn map_alignment(value: char) -> Result<Alignment, String> {
    match value {
        '<' => Ok(Alignment::Left),
        '^' => Ok(Alignment::Center),
        '>' => Ok(Alignment::Right),
        _ => Err(format!("Unsupported alignment '{value}'")),
    }
}

/// An alignment flag (`<`, `^`, or `>`).
fn bare_alignment(input: &str) -> IResult<&str, Alignment> {
    map_res(anychar, map_alignment)(input)
}

/// A combined fill character and alignment flag (`<`, `^`, or `>`).
fn fill_and_alignment(input: &str) -> IResult<&str, (char, Alignment)> {
    let (input, fill) = anychar(input)?;
    let (input, alignment) = bare_alignment(input)?;

    Ok((input, (fill, alignment)))
}

/// An optional fill character plus and alignment flag, or none.
fn alignment(input: &str) -> IResult<&str, (char, Alignment)> {
    // First try to match alignment spec preceded with a fill character.  This
    // is to match cases where the fill character is the same as one of the
    // alignment spec characters.
    if let Ok((input, (fill, alignment))) = fill_and_alignment(input) {
        return Ok((input, (fill, alignment)));
    }

    // If the above fails, fall back on looking for the alignment spec without
    // a fill character and default to ' ' as the fill character.
    if let Ok((input, alignment)) = bare_alignment(input) {
        return Ok((input, (' ', alignment)));
    }

    // Of all else false return none alignment with ' ' fill character.
    Ok((input, (' ', Alignment::None)))
}

/// A complete format specifier (i.e. the part between the `{}`s).
fn format_spec(input: &str) -> IResult<&str, ConversionSpec> {
    let (input, _) = tag(":")(input)?;
    let (input, (fill, alignment)) = alignment(input)?;
    let (input, flags) = flags(input)?;
    let (input, width) = opt(fixed_width)(input)?;
    let (input, precision) = precision(input)?;
    let (input, style) = style(input)?;

    Ok((
        input,
        ConversionSpec {
            argument: Argument::None, // This will get filled in by calling function.
            fill,
            alignment,
            flags,
            min_field_width: width.unwrap_or(MinFieldWidth::None),
            precision,
            length: None,
            primitive: Primitive::Untyped, // All core::fmt primitives are untyped.
            style,
        },
    ))
}

/// A complete conversion specifier (i.e. a `{}` expression).
fn conversion(input: &str) -> IResult<&str, ConversionSpec> {
    let (input, _) = tag("{")(input)?;
    let (input, argument) = argument(input)?;
    let (input, spec) = opt(format_spec)(input)?;
    // Allow trailing whitespace.  Here we specifically match against Rust's
    // idea of whitespace (specified in the Unicode Character Database) as it
    // differs from nom's space0 combinator (just spaces and tabs).
    let (input, _) = take_while(|c: char| c.is_whitespace())(input)?;
    let (input, _) = tag("}")(input)?;

    let mut spec = spec.unwrap_or_else(|| ConversionSpec {
        argument: Argument::None,
        fill: ' ',
        alignment: Alignment::None,
        flags: HashSet::new(),
        min_field_width: MinFieldWidth::None,
        precision: Precision::None,
        length: None,
        primitive: Primitive::Untyped,
        style: Style::None,
    });

    spec.argument = argument;

    Ok((input, spec))
}

/// A string literal (i.e. the non-`{}` part).
fn literal_fragment(input: &str) -> IResult<&str, FormatFragment> {
    map(take_till1(|c| c == '{' || c == '}'), |s: &str| {
        FormatFragment::Literal(s.to_string())
    })(input)
}

/// An escaped `{` or `}`.
fn escape_fragment(input: &str) -> IResult<&str, FormatFragment> {
    alt((
        map(tag("{{"), |_| FormatFragment::Literal("{".to_string())),
        map(tag("}}"), |_| FormatFragment::Literal("}".to_string())),
    ))(input)
}

/// A complete conversion specifier (i.e. a `{}` expression).
fn conversion_fragment(input: &str) -> IResult<&str, FormatFragment> {
    map(conversion, FormatFragment::Conversion)(input)
}

/// An escape, literal, or conversion fragment.
fn fragment(input: &str) -> IResult<&str, FormatFragment> {
    alt((escape_fragment, conversion_fragment, literal_fragment))(input)
}

/// Parse a complete `core::fmt` style format string.
pub(crate) fn format_string(input: &str) -> IResult<&str, FormatString> {
    let (input, fragments) = many0(fragment)(input)?;

    Ok((input, FormatString::from_fragments(&fragments)))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn type_parses_correctly() {
        assert_eq!(style(""), Ok(("", Style::None)));
        assert_eq!(style("?"), Ok(("", Style::Debug)));
        assert_eq!(style("x?"), Ok(("", Style::HexDebug)));
        assert_eq!(style("X?"), Ok(("", Style::UpperHexDebug)));
        assert_eq!(style("o"), Ok(("", Style::Octal)));
    }

    #[test]
    fn flags_prase_correctly() {
        assert_eq!(
            flags("0"),
            Ok(("", vec![Flag::LeadingZeros].into_iter().collect()))
        );
        assert_eq!(
            flags("-"),
            Ok(("", vec![Flag::LeftJustify].into_iter().collect()))
        );
        assert_eq!(
            flags("+"),
            Ok(("", vec![Flag::ForceSign].into_iter().collect()))
        );
        assert_eq!(
            flags("#"),
            Ok(("", vec![Flag::AlternateSyntax].into_iter().collect()))
        );

        assert_eq!(
            flags("+#0"),
            Ok((
                "",
                vec![Flag::ForceSign, Flag::AlternateSyntax, Flag::LeadingZeros]
                    .into_iter()
                    .collect()
            ))
        );

        // Unlike printf ` ` is not a valid flag char.
        assert_eq!(flags(" "), Ok((" ", HashSet::new())));
    }

    #[test]
    fn alignment_parses_correctly() {
        // Defaults to no alignment.
        assert_eq!(alignment(""), Ok(("", (' ', Alignment::None))));

        // Alignments w/o fill characters default to space fill.
        assert_eq!(alignment("<"), Ok(("", (' ', Alignment::Left))));
        assert_eq!(alignment("^"), Ok(("", (' ', Alignment::Center))));
        assert_eq!(alignment(">"), Ok(("", (' ', Alignment::Right))));

        // Alignments with fill characters.
        assert_eq!(alignment("-<"), Ok(("", ('-', Alignment::Left))));
        assert_eq!(alignment("-^"), Ok(("", ('-', Alignment::Center))));
        assert_eq!(alignment("->"), Ok(("", ('-', Alignment::Right))));

        // Alignments with alignment characters as fill characters.
        assert_eq!(alignment("><"), Ok(("", ('>', Alignment::Left))));
        assert_eq!(alignment("^^"), Ok(("", ('^', Alignment::Center))));
        assert_eq!(alignment("<>"), Ok(("", ('<', Alignment::Right))));

        // Non-alignment characters are not parsed and defaults to no alignment.
        assert_eq!(alignment("1234"), Ok(("1234", (' ', Alignment::None))));
    }

    #[test]
    fn empty_conversion_spec_has_sensible_defaults() {
        assert_eq!(
            conversion("{}"),
            Ok((
                "",
                ConversionSpec {
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
            ))
        );
    }
}
