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

use std::collections::HashSet;

use nom::IResult;
use nom::branch::alt;
use nom::bytes::complete::{tag, take_till1};
use nom::character::complete::anychar;
use nom::combinator::{map, map_res};
use nom::multi::many0;

use crate::parser_util::{precision, width};
use crate::{
    Alignment, Argument, ConversionSpec, Flag, FormatFragment, FormatString, Length, Primitive,
    Style,
};

fn map_specifier(value: char) -> Result<(Primitive, Style), String> {
    match value {
        'd' | 'i' => Ok((Primitive::Integer, Style::None)),
        'o' => Ok((Primitive::Unsigned, Style::Octal)),
        'u' => Ok((Primitive::Unsigned, Style::None)),
        'x' => Ok((Primitive::Unsigned, Style::Hex)),
        'X' => Ok((Primitive::Unsigned, Style::UpperHex)),
        'f' => Ok((Primitive::Float, Style::None)),
        'e' => Ok((Primitive::Float, Style::Exponential)),
        'E' => Ok((Primitive::Float, Style::UpperExponential)),
        'c' => Ok((Primitive::Character, Style::None)),
        's' => Ok((Primitive::String, Style::None)),
        'p' => Ok((Primitive::Pointer, Style::Pointer)),
        'v' => Ok((Primitive::Untyped, Style::None)),
        'F' => Err("%F is not supported because it does not have a core::fmt analog".to_string()),
        'g' => Err("%g is not supported because it does not have a core::fmt analog".to_string()),
        'G' => Err("%G is not supported because it does not have a core::fmt analog".to_string()),
        _ => Err(format!("Unsupported format specifier '{value}'")),
    }
}

fn specifier(input: &str) -> IResult<&str, (Primitive, Style)> {
    map_res(anychar, map_specifier)(input)
}

fn map_flag(value: char) -> Result<Flag, String> {
    match value {
        '-' => Ok(Flag::LeftJustify),
        '+' => Ok(Flag::ForceSign),
        ' ' => Ok(Flag::SpaceSign),
        '#' => Ok(Flag::AlternateSyntax),
        '0' => Ok(Flag::LeadingZeros),
        _ => Err(format!("Unsupported flag '{value}'")),
    }
}

fn flags(input: &str) -> IResult<&str, HashSet<Flag>> {
    let (input, flags) = many0(map_res(anychar, map_flag))(input)?;

    Ok((input, flags.into_iter().collect()))
}

fn length(input: &str) -> IResult<&str, Option<Length>> {
    alt((
        map(tag("hh"), |_| Some(Length::Char)),
        map(tag("h"), |_| Some(Length::Short)),
        map(tag("ll"), |_| Some(Length::LongLong)), // ll must precede l
        map(tag("l"), |_| Some(Length::Long)),
        map(tag("L"), |_| Some(Length::LongDouble)),
        map(tag("j"), |_| Some(Length::IntMax)),
        map(tag("z"), |_| Some(Length::Size)),
        map(tag("t"), |_| Some(Length::PointerDiff)),
        map(tag(""), |_| None),
    ))(input)
}

fn conversion_spec(input: &str) -> IResult<&str, ConversionSpec> {
    let (input, _) = tag("%")(input)?;
    let (input, flags) = flags(input)?;
    let (input, width) = width(input)?;
    let (input, precision) = precision(input)?;
    let (input, length) = length(input)?;
    let (input, (primitive, style)) = specifier(input)?;

    Ok((
        input,
        ConversionSpec {
            argument: Argument::None, // Printf style strings do not support arguments.
            fill: ' ',
            alignment: if flags.contains(&Flag::LeftJustify) {
                Alignment::Left
            } else {
                Alignment::None
            },
            flags,
            min_field_width: width,
            precision,
            length,
            primitive,
            style,
        },
    ))
}

fn literal_fragment(input: &str) -> IResult<&str, FormatFragment> {
    map(take_till1(|c| c == '%'), |s: &str| {
        FormatFragment::Literal(s.to_string())
    })(input)
}

fn percent_fragment(input: &str) -> IResult<&str, FormatFragment> {
    map(tag("%%"), |_| FormatFragment::Literal("%".to_string()))(input)
}

fn conversion_fragment(input: &str) -> IResult<&str, FormatFragment> {
    map(conversion_spec, FormatFragment::Conversion)(input)
}

fn fragment(input: &str) -> IResult<&str, FormatFragment> {
    alt((percent_fragment, conversion_fragment, literal_fragment))(input)
}

pub(crate) fn format_string(input: &str) -> IResult<&str, FormatString> {
    let (input, fragments) = many0(fragment)(input)?;

    Ok((input, FormatString::from_fragments(&fragments)))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{MinFieldWidth, Precision};

    #[test]
    fn test_specifier() {
        assert_eq!(specifier("d"), Ok(("", (Primitive::Integer, Style::None))));
        assert_eq!(specifier("i"), Ok(("", (Primitive::Integer, Style::None))));
        assert_eq!(
            specifier("o"),
            Ok(("", (Primitive::Unsigned, Style::Octal)))
        );
        assert_eq!(specifier("u"), Ok(("", (Primitive::Unsigned, Style::None))));
        assert_eq!(specifier("x"), Ok(("", (Primitive::Unsigned, Style::Hex))));
        assert_eq!(
            specifier("X"),
            Ok(("", (Primitive::Unsigned, Style::UpperHex)))
        );
        assert_eq!(specifier("f"), Ok(("", (Primitive::Float, Style::None))));
        assert_eq!(
            specifier("e"),
            Ok(("", (Primitive::Float, Style::Exponential)))
        );
        assert_eq!(
            specifier("E"),
            Ok(("", (Primitive::Float, Style::UpperExponential)))
        );
        assert_eq!(
            specifier("c"),
            Ok(("", (Primitive::Character, Style::None)))
        );
        assert_eq!(specifier("s"), Ok(("", (Primitive::String, Style::None))));
        assert_eq!(
            specifier("p"),
            Ok(("", (Primitive::Pointer, Style::Pointer)))
        );
        assert_eq!(specifier("v"), Ok(("", (Primitive::Untyped, Style::None))));

        assert_eq!(
            specifier("z"),
            Err(nom::Err::Error(nom::error::Error {
                input: "z",
                code: nom::error::ErrorKind::MapRes
            }))
        );
    }

    #[test]
    fn test_flags() {
        // Parse all the flags
        assert_eq!(
            flags("-+ #0"),
            Ok((
                "",
                vec![
                    Flag::LeftJustify,
                    Flag::ForceSign,
                    Flag::SpaceSign,
                    Flag::AlternateSyntax,
                    Flag::LeadingZeros,
                ]
                .into_iter()
                .collect()
            ))
        );

        // Parse all the flags but reversed.  Should produce the same set.
        assert_eq!(
            flags("0# +-"),
            Ok((
                "",
                vec![
                    Flag::LeftJustify,
                    Flag::ForceSign,
                    Flag::SpaceSign,
                    Flag::AlternateSyntax,
                    Flag::LeadingZeros,
                ]
                .into_iter()
                .collect()
            ))
        );

        // Non-flag characters after flags are not parsed.
        assert_eq!(
            flags("0d"),
            Ok(("d", vec![Flag::LeadingZeros,].into_iter().collect()))
        );

        // No flag characters returns empty set.
        assert_eq!(flags("d"), Ok(("d", vec![].into_iter().collect())));
    }

    #[test]
    fn test_width() {
        assert_eq!(
            width("1234567890d"),
            Ok(("d", MinFieldWidth::Fixed(1234567890)))
        );
        assert_eq!(width("*d"), Ok(("d", MinFieldWidth::Variable)));
        assert_eq!(width("d"), Ok(("d", MinFieldWidth::None)));
    }

    #[test]
    fn test_precision() {
        assert_eq!(
            precision(".1234567890f"),
            Ok(("f", Precision::Fixed(1234567890)))
        );
        assert_eq!(precision(".*f"), Ok(("f", Precision::Variable)));
        assert_eq!(precision("f"), Ok(("f", Precision::None)));
    }
    #[test]
    fn test_length() {
        assert_eq!(length("hhd"), Ok(("d", Some(Length::Char))));
        assert_eq!(length("hd"), Ok(("d", Some(Length::Short))));
        assert_eq!(length("ld"), Ok(("d", Some(Length::Long))));
        assert_eq!(length("lld"), Ok(("d", Some(Length::LongLong))));
        assert_eq!(length("Lf"), Ok(("f", Some(Length::LongDouble))));
        assert_eq!(length("jd"), Ok(("d", Some(Length::IntMax))));
        assert_eq!(length("zd"), Ok(("d", Some(Length::Size))));
        assert_eq!(length("td"), Ok(("d", Some(Length::PointerDiff))));
        assert_eq!(length("d"), Ok(("d", None)));
    }

    #[test]
    fn test_conversion_spec() {
        assert_eq!(
            conversion_spec("%d"),
            Ok((
                "",
                ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: [].into_iter().collect(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: None,
                    primitive: Primitive::Integer,
                    style: Style::None,
                }
            ))
        );

        assert_eq!(
            conversion_spec("%04ld"),
            Ok((
                "",
                ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: [Flag::LeadingZeros].into_iter().collect(),
                    min_field_width: MinFieldWidth::Fixed(4),
                    precision: Precision::None,
                    length: Some(Length::Long),
                    primitive: Primitive::Integer,
                    style: Style::None,
                }
            ))
        );
    }

    #[test]
    fn test_format_string() {
        assert_eq!(
            format_string("long double %+ 4.2Lf is %-03hd%%."),
            Ok((
                "",
                FormatString {
                    fragments: vec![
                        FormatFragment::Literal("long double ".to_string()),
                        FormatFragment::Conversion(ConversionSpec {
                            argument: Argument::None,
                            fill: ' ',
                            alignment: Alignment::None,
                            flags: [Flag::ForceSign, Flag::SpaceSign].into_iter().collect(),
                            min_field_width: MinFieldWidth::Fixed(4),
                            precision: Precision::Fixed(2),
                            length: Some(Length::LongDouble),
                            primitive: Primitive::Float,
                            style: Style::None,
                        }),
                        FormatFragment::Literal(" is ".to_string()),
                        FormatFragment::Conversion(ConversionSpec {
                            argument: Argument::None,
                            fill: ' ',
                            alignment: Alignment::Left,
                            flags: [Flag::LeftJustify, Flag::LeadingZeros]
                                .into_iter()
                                .collect(),
                            min_field_width: MinFieldWidth::Fixed(3),
                            precision: Precision::None,
                            length: Some(Length::Short),
                            primitive: Primitive::Integer,
                            style: Style::None,
                        }),
                        FormatFragment::Literal("%.".to_string()),
                    ]
                }
            ))
        );
    }
}
