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

use std::borrow::Cow;
use std::collections::HashSet;

use nom::branch::alt;
use nom::bytes::complete::{tag, take_till1, take_while, take_while_m_n};
use nom::character::complete::{anychar, char as nom_char};
use nom::combinator::{map, map_opt, map_res, not, opt, value};
use nom::multi::many0;
use nom::sequence::{delimited, preceded};
use nom::{IResult, Parser};

use crate::parser_util::{precision, width};
use crate::{
    Alignment, Argument, ConversionSpec, Flag, FormatFragment, FormatString, Length, NestedDomain,
    NestedTokenSpec, NestedTokenValue, Primitive, Style,
};

fn map_specifier(value: char) -> Result<(Primitive, Style), String> {
    match value {
        'd' | 'i' => Ok((Primitive::Integer, Style::None)),
        'o' => Ok((Primitive::Unsigned, Style::Octal)),
        'u' => Ok((Primitive::Unsigned, Style::None)),
        'x' => Ok((Primitive::Unsigned, Style::Hex)),
        'X' => Ok((Primitive::Unsigned, Style::UpperHex)),
        'f' => Ok((Primitive::Float, Style::None)),
        'F' => Ok((Primitive::Float, Style::Upper)),
        'e' => Ok((Primitive::Float, Style::Exponential)),
        'E' => Ok((Primitive::Float, Style::UpperExponential)),
        'g' => Ok((Primitive::Float, Style::General)),
        'G' => Ok((Primitive::Float, Style::UpperGeneral)),
        'a' => Ok((Primitive::Float, Style::HexFloat)),
        'A' => Ok((Primitive::Float, Style::UpperHexFloat)),
        'c' => Ok((Primitive::Character, Style::None)),
        's' => Ok((Primitive::String, Style::None)),
        'p' => Ok((Primitive::Pointer, Style::Pointer)),
        'v' => Ok((Primitive::Dynamic, Style::None)),
        _ => Err(format!("Unsupported format specifier '{value}'")),
    }
}

fn specifier(input: &str) -> IResult<&str, (Primitive, Style)> {
    map_res(anychar, map_specifier).parse(input)
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
    let (input, flags) = many0(map_res(anychar, map_flag)).parse(input)?;

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
    ))
    .parse(input)
}

// Note on lenient parsing of flags and modifiers:
// The printf parser intentionally accepts syntactically valid flags, widths,
// precisions, and length modifiers even when semantically inapplicable to the
// conversion specifier (such as %+c, %#s, %0s, or %.4c).
//
// This permissiveness is intentional for log detokenization and format string
// translation: malformed or redundant flags in source strings should not cause
// the entire format string or log entry to fail parsing. Downstream formatters
// and code generators ignore inapplicable modifiers rather than rejecting the
// format string during parse (see https://pwbug.dev/281750433).
fn conversion_spec(input: &str) -> IResult<&str, ConversionSpec> {
    let (input, _) = tag("%").parse(input)?;
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

fn dec_token_literal(input: &str) -> IResult<&str, u32> {
    map_res(
        take_while_m_n(10, 10, |c: char| c.is_ascii_digit()),
        |digits: &str| digits.parse::<u32>(),
    )
    .parse(input)
}

fn hex_token_literal(input: &str) -> IResult<&str, u32> {
    map_res(
        take_while_m_n(8, 8, |c: char| c.is_ascii_hexdigit()),
        |digits: &str| u32::from_str_radix(digits, 16),
    )
    .parse(input)
}

fn dec_token_value(input: &str) -> IResult<&str, NestedTokenValue> {
    alt((
        value(NestedTokenValue::Variable, tag("%010d")),
        map(dec_token_literal, NestedTokenValue::Literal),
    ))
    .parse(input)
}

fn hex_token_value(input: &str) -> IResult<&str, NestedTokenValue> {
    alt((
        value(NestedTokenValue::Variable, tag("%08x")),
        map(hex_token_literal, NestedTokenValue::Literal),
    ))
    .parse(input)
}

fn base64_string(input: &str) -> IResult<&str, &str> {
    let (rem, data) = take_while(pw_base64::is_valid_char)(input)?;
    let matched_len = match data.len() % 4 {
        2 if rem.starts_with("==") => data.len() + 2,
        3 if rem.starts_with('=') => data.len() + 1,
        rem => data.len() - rem,
    };
    if matched_len < 4 {
        return Err(nom::Err::Error(nom::error::Error::new(
            input,
            nom::error::ErrorKind::Tag,
        )));
    }
    Ok((&input[matched_len..], &input[..matched_len]))
}

const TOKEN_BYTES_LEN: usize = 4;

fn extract_token_and_data(data: &[u8]) -> Option<(u32, Vec<u8>)> {
    if data.is_empty() {
        return None;
    }
    let mut token_bytes = [0u8; TOKEN_BYTES_LEN];
    let token_len = core::cmp::min(data.len(), TOKEN_BYTES_LEN);
    token_bytes[..token_len].copy_from_slice(&data[..token_len]);
    let tok = u32::from_le_bytes(token_bytes);
    let token_data = data[token_len..].to_vec();
    Some((tok, token_data))
}

fn decode_base64_token(b64_str: &str) -> Option<(u32, Vec<u8>)> {
    let mut buf = vec![0u8; pw_base64::max_decoded_size(b64_str.len())];
    let len = pw_base64::decode(b64_str.as_bytes(), &mut buf).ok()?;
    buf.truncate(len);
    extract_token_and_data(&buf)
}

fn base64_token_payload(input: &str) -> IResult<&str, (NestedTokenValue, Option<Vec<u8>>)> {
    map_opt(base64_string, |b64_str| {
        let (tok, token_data) = decode_base64_token(b64_str)?;
        Some((NestedTokenValue::Literal(tok), Some(token_data)))
    })
    .parse(input)
}

fn token_value_and_payload(input: &str) -> IResult<&str, (NestedTokenValue, Option<Vec<u8>>)> {
    alt((
        map(preceded(tag("10#"), dec_token_value), |tok| (tok, None)),
        map(
            preceded(alt((tag("16#"), tag("#"))), hex_token_value),
            |tok| (tok, None),
        ),
        preceded(tag("64#"), base64_token_payload),
        base64_token_payload,
    ))
    .parse(input)
}

/// Canonicalizes a domain string by stripping all whitespace characters.
#[must_use]
pub fn canonicalize_domain(domain: &str) -> String {
    domain.chars().filter(|c| !c.is_whitespace()).collect()
}

/// Canonicalizes a domain string by stripping whitespace, returning a borrowed
/// slice if it contains no whitespace characters.
#[must_use]
pub fn canonicalize_domain_cow(domain: &str) -> Cow<'_, str> {
    if domain.chars().any(char::is_whitespace) {
        Cow::Owned(canonicalize_domain(domain))
    } else {
        Cow::Borrowed(domain)
    }
}

fn is_valid_domain_char(c: char) -> bool {
    c.is_ascii_alphanumeric() || matches!(c, '_' | ':' | ' ' | '\t'..='\r')
}

fn canonicalize_valid_domain(domain: &str) -> Option<String> {
    let canonical = canonicalize_domain(domain);
    if matches!(canonical.as_bytes().first(), Some(b) if b.is_ascii_digit()) {
        return None;
    }
    Some(canonical)
}

fn braced_domain_name(input: &str) -> IResult<&str, String> {
    map_opt(
        delimited(
            nom_char('{'),
            take_while(is_valid_domain_char),
            nom_char('}'),
        ),
        canonicalize_valid_domain,
    )
    .parse(input)
}

fn tokenized_nested_domain(input: &str, prefix: char) -> IResult<&str, NestedDomain> {
    let (input, _) = nom_char(prefix)(input)?;
    let (input, inner_domain) = opt(braced_domain_name).parse(input)?;
    let domain = inner_domain.unwrap_or_default();
    let (input, (token, _)) = token_value_and_payload(input)?;
    let nested_domain = match token {
        NestedTokenValue::Variable => NestedDomain::VariableToken { domain },
        NestedTokenValue::Literal(token) => NestedDomain::LiteralToken { domain, token },
    };
    Ok((input, nested_domain))
}

fn literal_nested_domain(input: &str, prefix: char) -> IResult<&str, NestedDomain> {
    preceded(
        not(nom_char(prefix)),
        map_opt(take_while(is_valid_domain_char), |domain_str| {
            let canonical = canonicalize_valid_domain(domain_str)?;
            Some(if canonical.is_empty() {
                NestedDomain::Default
            } else {
                NestedDomain::Literal(canonical)
            })
        }),
    )
    .parse(input)
}

fn nested_domain(input: &str, prefix: char) -> IResult<&str, NestedDomain> {
    delimited(
        nom_char('{'),
        alt((
            |i| tokenized_nested_domain(i, prefix),
            |i| literal_nested_domain(i, prefix),
        )),
        nom_char('}'),
    )
    .parse(input)
}

fn nested_token_spec(input: &str, prefix: char) -> IResult<&str, NestedTokenSpec> {
    let original_input = input;
    let (input, _) = nom_char(prefix)(input)?;
    let (input, domain) = opt(|i| nested_domain(i, prefix)).parse(input)?;
    let (rem, (token, token_data)) = token_value_and_payload(input)?;
    let raw = &original_input[..original_input.len() - rem.len()];
    Ok((
        rem,
        NestedTokenSpec {
            prefix,
            domain: domain.unwrap_or(NestedDomain::Default),
            token,
            token_data,
            raw: raw.to_string(),
        },
    ))
}

fn literal_fragment(input: &str) -> IResult<&str, FormatFragment> {
    map(take_till1(|c| c == '%'), |s: &str| {
        FormatFragment::Literal(s.to_string())
    })
    .parse(input)
}

fn percent_fragment(input: &str) -> IResult<&str, FormatFragment> {
    map(tag("%%"), |_| FormatFragment::Literal("%".to_string())).parse(input)
}

fn conversion_fragment(input: &str) -> IResult<&str, FormatFragment> {
    map(conversion_spec, FormatFragment::Conversion).parse(input)
}

fn lossy_percent_fragment(input: &str) -> IResult<&str, FormatFragment> {
    map(tag("%"), |_| FormatFragment::Literal("%".to_string())).parse(input)
}

fn fragment(input: &str, prefix: Option<char>) -> IResult<&str, FormatFragment> {
    if let Some(p) = prefix {
        alt((
            percent_fragment,
            conversion_fragment,
            |i| map(|inp| nested_token_spec(inp, p), FormatFragment::NestedToken).parse(i),
            |i| map(nom_char(p), |c| FormatFragment::Literal(c.to_string())).parse(i),
            |i| {
                map(take_till1(move |c| c == '%' || c == p), |s: &str| {
                    FormatFragment::Literal(s.to_string())
                })
                .parse(i)
            },
        ))
        .parse(input)
    } else {
        alt((percent_fragment, conversion_fragment, literal_fragment)).parse(input)
    }
}

fn lossy_fragment(input: &str, prefix: Option<char>) -> IResult<&str, FormatFragment> {
    if let Some(p) = prefix {
        alt((
            percent_fragment,
            conversion_fragment,
            lossy_percent_fragment,
            |i| map(|inp| nested_token_spec(inp, p), FormatFragment::NestedToken).parse(i),
            |i| map(nom_char(p), |c| FormatFragment::Literal(c.to_string())).parse(i),
            |i| {
                map(take_till1(move |c| c == '%' || c == p), |s: &str| {
                    FormatFragment::Literal(s.to_string())
                })
                .parse(i)
            },
        ))
        .parse(input)
    } else {
        alt((
            percent_fragment,
            conversion_fragment,
            lossy_percent_fragment,
            literal_fragment,
        ))
        .parse(input)
    }
}

pub(crate) fn format_string(input: &str, prefix: Option<char>) -> IResult<&str, FormatString> {
    let (input, fragments) = many0(|i| fragment(i, prefix)).parse(input)?;

    Ok((input, FormatString::from_fragments(&fragments)))
}

pub(crate) fn format_string_lossy(input: &str, prefix: Option<char>) -> FormatString {
    match many0(|i| lossy_fragment(i, prefix)).parse(input) {
        Ok((_, fragments)) => FormatString::from_fragments(&fragments),
        Err(_) => FormatString::from_fragments(&[FormatFragment::Literal(input.to_string())]),
    }
}

fn nested_token_only_fragment(input: &str, prefix: char) -> IResult<&str, FormatFragment> {
    alt((
        |i| {
            map(
                |inp| nested_token_spec(inp, prefix),
                FormatFragment::NestedToken,
            )
            .parse(i)
        },
        |i| map(nom_char(prefix), |c| FormatFragment::Literal(c.to_string())).parse(i),
        |i| {
            map(take_till1(move |c| c == prefix), |s: &str| {
                FormatFragment::Literal(s.to_string())
            })
            .parse(i)
        },
    ))
    .parse(input)
}

pub(crate) fn nested_tokens_string(input: &str, prefix: char) -> IResult<&str, FormatString> {
    let (input, fragments) = many0(|i| nested_token_only_fragment(i, prefix)).parse(input)?;

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
        assert_eq!(specifier("F"), Ok(("", (Primitive::Float, Style::Upper))));
        assert_eq!(
            specifier("e"),
            Ok(("", (Primitive::Float, Style::Exponential)))
        );
        assert_eq!(
            specifier("E"),
            Ok(("", (Primitive::Float, Style::UpperExponential)))
        );
        assert_eq!(specifier("g"), Ok(("", (Primitive::Float, Style::General))));
        assert_eq!(
            specifier("G"),
            Ok(("", (Primitive::Float, Style::UpperGeneral)))
        );
        assert_eq!(
            specifier("a"),
            Ok(("", (Primitive::Float, Style::HexFloat)))
        );
        assert_eq!(
            specifier("A"),
            Ok(("", (Primitive::Float, Style::UpperHexFloat)))
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
        assert_eq!(specifier("v"), Ok(("", (Primitive::Dynamic, Style::None))));

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
        assert_eq!(precision(".f"), Ok(("f", Precision::Fixed(0))));
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
            format_string("long double %+ 4.2Lf is %-03hd%%.", Some('$')),
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

    #[test]
    fn test_nested_token_specs() {
        // Default domain: $#%08x
        let parsed = FormatString::parse_printf("Token: $#%08x").unwrap();
        assert_eq!(parsed.fragments.len(), 2);
        assert_eq!(
            parsed.fragments[0],
            FormatFragment::Literal("Token: ".to_string())
        );
        let FormatFragment::NestedToken(spec) = &parsed.fragments[1] else {
            panic!("expected NestedToken");
        };
        assert_eq!(spec.prefix, '$');
        assert_eq!(spec.domain, NestedDomain::Default);
        assert_eq!(spec.token, NestedTokenValue::Variable);
        assert_eq!(spec.token_data, None);
        assert_eq!(spec.to_printf(), "$#%08x");

        // Explicit domain: ${my_domain}#%08x
        let parsed = FormatString::parse_printf("Msg: ${my_domain}#%08x").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[1] else {
            panic!("expected NestedToken");
        };
        assert_eq!(spec.domain, NestedDomain::Literal("my_domain".to_string()));
        assert_eq!(spec.token, NestedTokenValue::Variable);
        assert_eq!(spec.token_data, None);
        assert_eq!(spec.to_printf(), "${my_domain}#%08x");

        // Empty domain ${}#%08x is treated as Default domain
        let parsed = FormatString::parse_printf("${}#%08x").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(spec.domain, NestedDomain::Default);
        assert_eq!(spec.token, NestedTokenValue::Variable);
        assert_eq!(spec.token_data, None);

        // Whitespace-only domain ${   }#%08x is treated as Default domain
        let parsed = FormatString::parse_printf("${   }#%08x").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(spec.domain, NestedDomain::Default);

        // Domain with leading/trailing and interior whitespace is canonicalized (whitespace stripped)
        let parsed = FormatString::parse_printf("${  Domain 1  }#%08x").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(spec.domain, NestedDomain::Literal("Domain1".to_string()));

        // Inner domain with whitespace is canonicalized
        let parsed = FormatString::parse_printf("${${  Domain 1  }#01234567}#%08x").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(
            spec.domain,
            NestedDomain::LiteralToken {
                domain: "Domain1".to_string(),
                token: 0x01234567,
            }
        );

        // Inner domain with whitespace-only is treated as empty domain
        let parsed = FormatString::parse_printf("${${   }#01234567}#%08x").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(
            spec.domain,
            NestedDomain::LiteralToken {
                domain: String::new(),
                token: 0x01234567,
            }
        );

        // Doubly-nested: ${$#%08x}#%08x
        let parsed = FormatString::parse_printf("${$#%08x}#%08x").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(
            spec.domain,
            NestedDomain::VariableToken {
                domain: String::new()
            }
        );
        assert_eq!(spec.token, NestedTokenValue::Variable);
        assert_eq!(spec.token_data, None);
        assert_eq!(spec.to_printf(), "${$#%08x}#%08x");

        // Doubly-nested with domain: ${${my_domain}#%08x}#%08x
        let parsed = FormatString::parse_printf("${${my_domain}#%08x}#%08x").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(
            spec.domain,
            NestedDomain::VariableToken {
                domain: "my_domain".to_string()
            }
        );
        assert_eq!(spec.token_data, None);
        assert_eq!(spec.to_printf(), "${${my_domain}#%08x}#%08x");

        // Literal token: $#01234567
        let parsed = FormatString::parse_printf("$#01234567").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(spec.domain, NestedDomain::Default);
        assert_eq!(spec.token, NestedTokenValue::Literal(0x01234567));
        assert_eq!(spec.token_data, None);
        assert_eq!(spec.to_printf(), "$#01234567");

        // Literal token in domain: ${$#01234567}#%08x
        let parsed = FormatString::parse_printf("${$#01234567}#%08x").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(
            spec.domain,
            NestedDomain::LiteralToken {
                domain: String::new(),
                token: 0x01234567
            }
        );
        assert_eq!(spec.token, NestedTokenValue::Variable);
        assert_eq!(spec.token_data, None);

        // Radix specified: $16#%08x
        let parsed = FormatString::parse_printf("$16#%08x").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(spec.token, NestedTokenValue::Variable);
        assert_eq!(spec.token_data, None);
        assert_eq!(spec.to_printf(), "$16#%08x");

        // Direct Base64: $AQAAAA==
        let parsed = FormatString::parse_printf("$AQAAAA==").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(spec.domain, NestedDomain::Default);
        assert_eq!(spec.token, NestedTokenValue::Literal(1));
        assert_eq!(spec.token_data, Some(Vec::<u8>::new()));
        assert_eq!(spec.to_printf(), "$AQAAAA==");

        // Base64 with domain: ${custom}AQAAAA==
        let parsed = FormatString::parse_printf("${custom}AQAAAA==").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(spec.domain, NestedDomain::Literal("custom".to_string()));
        assert_eq!(spec.token, NestedTokenValue::Literal(1));
        assert_eq!(spec.token_data, Some(Vec::<u8>::new()));
        assert_eq!(spec.to_printf(), "${custom}AQAAAA==");

        // Explicit 64# radix: $64#AAAAAaAA
        let parsed = FormatString::parse_printf("$64#AAAAAaAA").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(spec.token, NestedTokenValue::Literal(0x01000000));
        assert_eq!(spec.token_data, Some(vec![160, 0]));
        assert_eq!(spec.to_printf(), "$64#AAAAAaAA");

        // Radix 10 decimal: $10#0000000010
        let parsed = FormatString::parse_printf("$10#0000000010").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(spec.token, NestedTokenValue::Literal(10));
        assert_eq!(spec.token_data, None);
        assert_eq!(spec.to_printf(), "$10#0000000010");

        // Radix 10 variable token: $10#%010d
        let parsed = FormatString::parse_printf("$10#%010d").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(spec.token, NestedTokenValue::Variable);
        assert_eq!(spec.token_data, None);
        assert_eq!(spec.to_printf(), "$10#%010d");

        // Radix 10 variable token with domain: ${custom}10#%010d
        let parsed = FormatString::parse_printf("${custom}10#%010d").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(spec.domain, NestedDomain::Literal("custom".to_string()));
        assert_eq!(spec.token, NestedTokenValue::Variable);
        assert_eq!(spec.token_data, None);
        assert_eq!(spec.to_printf(), "${custom}10#%010d");

        // URL-safe Base64: $-_AA
        let parsed = FormatString::parse_printf("$-_AA").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(spec.token, NestedTokenValue::Literal(0x00f0fb));
        assert_eq!(spec.to_printf(), "$-_AA");
    }

    #[test]
    fn test_nested_domain_radices() {
        // Explicit radix 16 in domain: ${$16#%08x}#%08x
        let parsed = FormatString::parse_printf("${$16#%08x}#%08x").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(
            spec.domain,
            NestedDomain::VariableToken {
                domain: String::new()
            }
        );

        // Explicit radix 16 literal token in domain: ${$16#01234567}#%08x
        let parsed = FormatString::parse_printf("${$16#01234567}#%08x").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(
            spec.domain,
            NestedDomain::LiteralToken {
                domain: String::new(),
                token: 0x01234567,
            }
        );

        // Explicit radix 10 literal token in domain: ${$10#0000000042}#%08x
        let parsed = FormatString::parse_printf("${$10#0000000042}#%08x").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(
            spec.domain,
            NestedDomain::LiteralToken {
                domain: String::new(),
                token: 42,
            }
        );

        // Explicit radix 10 with inner domain: ${${my_domain}10#0000000042}#%08x
        let parsed = FormatString::parse_printf("${${my_domain}10#0000000042}#%08x").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(
            spec.domain,
            NestedDomain::LiteralToken {
                domain: "my_domain".to_string(),
                token: 42,
            }
        );

        // Radix 10 variable token in domain: ${$10#%010d}#%08x
        let parsed = FormatString::parse_printf("${$10#%010d}#%08x").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(
            spec.domain,
            NestedDomain::VariableToken {
                domain: String::new(),
            }
        );

        // Radix 10 variable token with inner domain: ${${my_domain}10#%010d}#%08x
        let parsed = FormatString::parse_printf("${${my_domain}10#%010d}#%08x").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(
            spec.domain,
            NestedDomain::VariableToken {
                domain: "my_domain".to_string(),
            }
        );

        // Radix 64 literal token in domain: ${$64#AQAAAA==}#%08x
        let parsed = FormatString::parse_printf("${$64#AQAAAA==}#%08x").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(
            spec.domain,
            NestedDomain::LiteralToken {
                domain: String::new(),
                token: 1,
            }
        );

        // Radix 64 with inner domain: ${${my_domain}64#AQAAAA==}#%08x
        let parsed = FormatString::parse_printf("${${my_domain}64#AQAAAA==}#%08x").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(
            spec.domain,
            NestedDomain::LiteralToken {
                domain: "my_domain".to_string(),
                token: 1,
            }
        );

        // Direct Base64 in domain: ${$AQAAAA==}#%08x
        let parsed = FormatString::parse_printf("${$AQAAAA==}#%08x").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(
            spec.domain,
            NestedDomain::LiteralToken {
                domain: String::new(),
                token: 1,
            }
        );

        // Direct Base64 with inner domain: ${${my_domain}AQAAAA==}#%08x
        let parsed = FormatString::parse_printf("${${my_domain}AQAAAA==}#%08x").unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(
            spec.domain,
            NestedDomain::LiteralToken {
                domain: "my_domain".to_string(),
                token: 1,
            }
        );

        // Unsupported radix 8 in domain: falls back to literal + conversion
        let parsed = FormatString::parse_printf("${$8#01234567}#%08x").unwrap();
        assert_eq!(parsed.fragments.len(), 2);
        assert_eq!(
            parsed.fragments[0],
            FormatFragment::Literal("${$8#01234567}#".to_string())
        );
        assert!(matches!(parsed.fragments[1], FormatFragment::Conversion(_)));

        // Unsupported radix 8 with inner domain
        let parsed = FormatString::parse_printf("${${my_domain}8#01234567}#%08x").unwrap();
        assert_eq!(parsed.fragments.len(), 2);
        assert_eq!(
            parsed.fragments[0],
            FormatFragment::Literal("${${my_domain}8#01234567}#".to_string())
        );
        assert!(matches!(parsed.fragments[1], FormatFragment::Conversion(_)));

        // Radix 10 with %08x (hex format) in domain is invalid: %08x is parsed as conversion
        let parsed = FormatString::parse_printf("${$10#%08x}#%08x").unwrap();
        assert_eq!(parsed.fragments.len(), 4);
        assert_eq!(
            parsed.fragments[0],
            FormatFragment::Literal("${$10#".to_string())
        );
        assert!(matches!(parsed.fragments[1], FormatFragment::Conversion(_)));
        assert_eq!(
            parsed.fragments[2],
            FormatFragment::Literal("}#".to_string())
        );
        assert!(matches!(parsed.fragments[3], FormatFragment::Conversion(_)));
    }

    #[test]
    fn test_custom_prefix_and_literals() {
        // Custom prefix '~'
        let parsed = FormatString::parse_printf_with_prefix("Value: ~#%08x", '~').unwrap();
        let FormatFragment::NestedToken(spec) = &parsed.fragments[1] else {
            panic!("expected NestedToken");
        };
        assert_eq!(spec.prefix, '~');
        assert_eq!(spec.to_printf(), "~#%08x");

        // Custom prefix with domain: ~{custom}#%08x
        let parsed =
            FormatString::parse_printf_with_prefix("~{custom}#%08x and ~{~#%08x}#%08x", '~')
                .unwrap();
        assert_eq!(parsed.fragments.len(), 3);

        // Invalid nested token specifiers fall back to literal + normal conversion:
        // "${}#%d" -> Literal("${}#") followed by Conversion("%d")
        let parsed = FormatString::parse_printf("${}#%d").unwrap();
        assert_eq!(parsed.fragments.len(), 2);
        assert_eq!(
            parsed.fragments[0],
            FormatFragment::Literal("${}#".to_string())
        );
        let FormatFragment::Conversion(conv) = &parsed.fragments[1] else {
            panic!("expected Conversion");
        };
        assert_eq!(conv.to_printf(), "%d");

        // "${}#%06x" -> Literal("${}#") followed by Conversion("%06x")
        let parsed = FormatString::parse_printf("${}#%06x").unwrap();
        assert_eq!(parsed.fragments.len(), 2);
        assert_eq!(
            parsed.fragments[0],
            FormatFragment::Literal("${}#".to_string())
        );
        let FormatFragment::Conversion(conv) = &parsed.fragments[1] else {
            panic!("expected Conversion");
        };
        assert_eq!(conv.to_printf(), "%06x");

        // Plain dollar signs that are not nested tokens remain literals
        let parsed = FormatString::parse_printf("Cost is $100 or $5 per item").unwrap();
        assert_eq!(
            parsed.fragments,
            vec![FormatFragment::Literal(
                "Cost is $100 or $5 per item".to_string()
            )]
        );

        // Pure printf disables nested token parsing
        let parsed = FormatString::parse_pure_printf("$#%08x").unwrap();
        assert_eq!(parsed.fragments.len(), 2);
        assert_eq!(
            parsed.fragments[0],
            FormatFragment::Literal("$#".to_string())
        );
        assert!(matches!(parsed.fragments[1], FormatFragment::Conversion(_)));
    }

    #[test]
    fn test_malformed_utf8_does_not_panic() {
        // Multi-byte UTF-8 where byte length >= 10 or >= 8, but byte boundaries
        // do not align with character boundaries.
        assert_eq!(dec_token_literal("€€€€").ok(), None);
        assert_eq!(dec_token_literal("🚀🚀🚀").ok(), None);
        assert_eq!(dec_token_literal("123€567890").ok(), None);
        assert_eq!(dec_token_literal("").ok(), None);
        assert_eq!(dec_token_literal("123").ok(), None);

        // Valid 10-digit decimal tokens
        assert_eq!(dec_token_literal("0000000042").ok(), Some(("", 42)));
        assert_eq!(
            dec_token_literal("0000000042remainder").ok(),
            Some(("remainder", 42))
        );

        assert_eq!(hex_token_literal("€€€").ok(), None);
        assert_eq!(hex_token_literal("🚀🚀").ok(), None);
        assert_eq!(hex_token_literal("12€45678").ok(), None);
        assert_eq!(hex_token_literal("").ok(), None);
        assert_eq!(hex_token_literal("123").ok(), None);

        // Valid 8-digit hex tokens
        assert_eq!(hex_token_literal("01234567").ok(), Some(("", 0x01234567)));
        assert_eq!(
            hex_token_literal("deadbeef_tail").ok(),
            Some(("_tail", 0xdeadbeef))
        );

        // Parsing format strings with malformed multi-byte UTF-8 should not panic
        let parsed = FormatString::parse_printf("Malformed $#€€€ tail").unwrap();
        assert_eq!(
            parsed.fragments,
            vec![FormatFragment::Literal("Malformed $#€€€ tail".to_string())]
        );

        let parsed = FormatString::parse_printf("Malformed $16#€€€ tail").unwrap();
        assert_eq!(
            parsed.fragments,
            vec![FormatFragment::Literal(
                "Malformed $16#€€€ tail".to_string()
            )]
        );

        let parsed = FormatString::parse_printf("Malformed $10#€€€€ tail").unwrap();
        assert_eq!(
            parsed.fragments,
            vec![FormatFragment::Literal(
                "Malformed $10#€€€€ tail".to_string()
            )]
        );

        let parsed = FormatString::parse_printf("Emoji $10#🚀🚀🚀 tail").unwrap();
        assert_eq!(
            parsed.fragments,
            vec![FormatFragment::Literal("Emoji $10#🚀🚀🚀 tail".to_string())]
        );

        let parsed = FormatString::parse_printf("Domain ${$#€€€}#%08x tail").unwrap();
        assert_eq!(parsed.fragments.len(), 3);
        assert_eq!(
            parsed.fragments[0],
            FormatFragment::Literal("Domain ${$#€€€}#".to_string())
        );
        assert!(matches!(parsed.fragments[1], FormatFragment::Conversion(_)));
        assert_eq!(
            parsed.fragments[2],
            FormatFragment::Literal(" tail".to_string())
        );
    }

    #[test]
    fn test_nested_tokens_string_ignores_percent() {
        let parsed =
            FormatString::parse_nested_tokens("Progress: 100% complete $AgAAAA== My arg %s %d");
        assert_eq!(parsed.fragments.len(), 3);
        assert_eq!(
            parsed.fragments[0],
            FormatFragment::Literal("Progress: 100% complete ".to_string())
        );
        assert!(matches!(
            parsed.fragments[1],
            FormatFragment::NestedToken(_)
        ));
        assert_eq!(
            parsed.fragments[2],
            FormatFragment::Literal(" My arg %s %d".to_string())
        );

        let parsed_no_token = FormatString::parse_nested_tokens("Price is $5 and 50%");
        assert_eq!(
            parsed_no_token.fragments,
            vec![FormatFragment::Literal("Price is $5 and 50%".to_string())]
        );
    }

    #[test]
    fn test_base64_trailing_equals_and_invalid_domain_chars() {
        // Complete 4-char Base64 block followed immediately by '=' in surrounding text
        let parsed = FormatString::parse_printf("$AQAA=val").unwrap();
        assert_eq!(parsed.fragments.len(), 2);
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(spec.raw(), "$AQAA");
        assert_eq!(spec.token, NestedTokenValue::Literal(1));
        assert_eq!(
            parsed.fragments[1],
            FormatFragment::Literal("=val".to_string())
        );

        // Padded 8-char Base64 block followed immediately by extra '='
        let parsed = FormatString::parse_printf("$AQAAAA===42").unwrap();
        assert_eq!(parsed.fragments.len(), 2);
        let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
            panic!("expected NestedToken");
        };
        assert_eq!(spec.raw(), "$AQAAAA==");
        assert_eq!(spec.token, NestedTokenValue::Literal(1));
        assert_eq!(
            parsed.fragments[1],
            FormatFragment::Literal("=42".to_string())
        );

        // Unpadded trailing Base64 characters backtrack to last complete 4-char quad
        for (input, expected_tail) in [
            ("$AQAAx!", "x!"),
            ("$AQAAxy!", "xy!"),
            ("$AQAAxyz!", "xyz!"),
        ] {
            let parsed = FormatString::parse_printf(input).unwrap();
            assert_eq!(parsed.fragments.len(), 2, "input: {input}");
            let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
                panic!("expected NestedToken for {input}");
            };
            assert_eq!(spec.raw(), "$AQAA");
            assert_eq!(spec.token, NestedTokenValue::Literal(1));
            assert_eq!(
                parsed.fragments[1],
                FormatFragment::Literal(expected_tail.to_string())
            );
        }

        // Unclosed outer domain brace before a valid nested token must not swallow the valid token
        let parsed = FormatString::parse_printf("${foo#${bar}#%08x").unwrap();
        assert_eq!(parsed.fragments.len(), 2);
        assert_eq!(
            parsed.fragments[0],
            FormatFragment::Literal("${foo#".to_string())
        );
        let FormatFragment::NestedToken(spec) = &parsed.fragments[1] else {
            panic!("expected NestedToken");
        };
        assert_eq!(spec.domain, NestedDomain::Literal("bar".to_string()));
        assert_eq!(spec.token, NestedTokenValue::Variable);

        // Disallowed SEED-0105 domain characters fall back to literal + conversion
        for invalid in [
            "${bad!domain}#%08x",
            "${bad.domain}#%08x",
            "${123bad}#%08x",
            "${ 123bad}#%08x",
            "${${123bad}#01234567}#%08x",
        ] {
            let parsed = FormatString::parse_printf(invalid).unwrap();
            assert_eq!(parsed.fragments.len(), 2, "input: {invalid}");
            assert_eq!(
                parsed.fragments[0],
                FormatFragment::Literal(invalid.strip_suffix("%08x").unwrap().to_string()),
                "input: {invalid}"
            );
            assert!(
                matches!(parsed.fragments[1], FormatFragment::Conversion(_)),
                "input: {invalid}"
            );
        }

        // Valid non-digit first characters ('_', ':') and non-leading digits
        for (input, expected_domain) in [
            ("${_0}#%08x", "_0"),
            ("${:1}#%08x", ":1"),
            ("${  _domain_1  }#%08x", "_domain_1"),
            ("${a123}#%08x", "a123"),
        ] {
            let parsed = FormatString::parse_printf(input).unwrap();
            assert_eq!(parsed.fragments.len(), 1, "input: {input}");
            let FormatFragment::NestedToken(spec) = &parsed.fragments[0] else {
                panic!("expected NestedToken for {input}");
            };
            assert_eq!(
                spec.domain,
                NestedDomain::Literal(expected_domain.to_string())
            );
        }
    }
}
