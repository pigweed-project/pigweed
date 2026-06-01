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
#![allow(clippy::literal_string_with_formatting_args)]

use std::collections::HashSet;

use crate::*;

#[test]
fn std_fmt_examples_parse_correctly() {
    // Examples from std::fmt.
    assert_eq!(
        FormatString::parse_core_fmt("Hello {:?}!"),
        Ok(FormatString {
            fragments: vec![
                FormatFragment::Literal("Hello ".to_string()),
                FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: HashSet::new(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: None,
                    primitive: Primitive::Untyped,
                    style: Style::Debug,
                }),
                FormatFragment::Literal("!".to_string()),
            ]
        })
    );

    assert_eq!(
        FormatString::parse_core_fmt("{:04}"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: vec![Flag::LeadingZeros].into_iter().collect(),
                min_field_width: MinFieldWidth::Fixed(4),
                precision: Precision::None,
                length: None,
                primitive: Primitive::Untyped,
                style: Style::None,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_core_fmt("{:#?}"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: vec![Flag::AlternateSyntax].into_iter().collect(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::None,
                length: None,
                primitive: Primitive::Untyped,
                style: Style::Debug,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_core_fmt("{0}"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::Positional(0),
                fill: ' ',
                alignment: Alignment::None,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::None,
                length: None,
                primitive: Primitive::Untyped,
                style: Style::None,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_core_fmt("{1}"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::Positional(1),
                fill: ' ',
                alignment: Alignment::None,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::None,
                length: None,
                primitive: Primitive::Untyped,
                style: Style::None,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_core_fmt("{argument}"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::Named("argument".to_string()),
                fill: ' ',
                alignment: Alignment::None,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::None,
                length: None,
                primitive: Primitive::Untyped,
                style: Style::None,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_core_fmt("{:<5}"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::Left,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::Fixed(5),
                precision: Precision::None,
                length: None,
                primitive: Primitive::Untyped,
                style: Style::None,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_core_fmt("{:-<5}"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: '-',
                alignment: Alignment::Left,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::Fixed(5),
                precision: Precision::None,
                length: None,
                primitive: Primitive::Untyped,
                style: Style::None,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_core_fmt("{:^5}"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::Center,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::Fixed(5),
                precision: Precision::None,
                length: None,
                primitive: Primitive::Untyped,
                style: Style::None,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_core_fmt("{:>5}"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::Right,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::Fixed(5),
                precision: Precision::None,
                length: None,
                primitive: Primitive::Untyped,
                style: Style::None,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_core_fmt("{:+}"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: vec![Flag::ForceSign].into_iter().collect(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::None,
                length: None,
                primitive: Primitive::Untyped,
                style: Style::None,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_core_fmt("{:#x}"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: vec![Flag::AlternateSyntax].into_iter().collect(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::None,
                length: None,
                primitive: Primitive::Untyped,
                style: Style::Hex,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_core_fmt("{:#010x}"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: vec![Flag::AlternateSyntax, Flag::LeadingZeros]
                    .into_iter()
                    .collect(),
                min_field_width: MinFieldWidth::Fixed(10),
                precision: Precision::None,
                length: None,
                primitive: Primitive::Untyped,
                style: Style::Hex,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_core_fmt("{1:.5}"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::Positional(1),
                fill: ' ',
                alignment: Alignment::None,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::Fixed(5),
                length: None,
                primitive: Primitive::Untyped,
                style: Style::None,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_core_fmt("{:.*}"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::Variable,
                length: None,
                primitive: Primitive::Untyped,
                style: Style::None,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_core_fmt("{2:.*}"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::Positional(2),
                fill: ' ',
                alignment: Alignment::None,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::Variable,
                length: None,
                primitive: Primitive::Untyped,
                style: Style::None,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_core_fmt("{name:.*}"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::Named("name".to_string()),
                fill: ' ',
                alignment: Alignment::None,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::Variable,
                length: None,
                primitive: Primitive::Untyped,
                style: Style::None,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_core_fmt("{name:>8.*}"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::Named("name".to_string()),
                fill: ' ',
                alignment: Alignment::Right,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::Fixed(8),
                precision: Precision::Variable,
                length: None,
                primitive: Primitive::Untyped,
                style: Style::None,
            })]
        })
    );
}

#[test]
fn conversions_allow_trailing_whitespace() {
    assert_eq!(
        FormatString::parse_core_fmt("{name:>8.* \t\r\n}"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::Named("name".to_string()),
                fill: ' ',
                alignment: Alignment::Right,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::Fixed(8),
                precision: Precision::Variable,
                length: None,
                primitive: Primitive::Untyped,
                style: Style::None,
            })]
        })
    );
}

#[test]
fn escaped_curly_braces_parse_correctly() {
    assert_eq!(
        FormatString::parse_core_fmt("Hello {{{:?}}}!"),
        Ok(FormatString {
            fragments: vec![
                FormatFragment::Literal("Hello {".to_string()),
                FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: HashSet::new(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: None,
                    primitive: Primitive::Untyped,
                    style: Style::Debug,
                }),
                FormatFragment::Literal("}!".to_string()),
            ]
        })
    );
}
