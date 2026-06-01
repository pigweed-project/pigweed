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

use crate::*;

#[test]
fn test_parse() {
    assert_eq!(
        FormatString::parse_printf("long double %+ 4.2Lf is %-03hd%%."),
        Ok(FormatString {
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
        })
    );
}

//
// The following test cases are from //pw_tokenizer/py/decode_test.py
//

#[test]
fn test_percent() {
    assert_eq!(
        FormatString::parse_printf("%%"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Literal("%".to_string())],
        }),
    );
}

#[test]
fn test_percent_with_leading_plus_fails() {
    assert!(FormatString::parse_printf("%+%").is_err());
}

#[test]
fn test_percent_with_leading_negative_fails() {
    assert!(FormatString::parse_printf("%-%").is_err());
}

#[test]
fn test_percent_with_leading_space_fails() {
    assert!(FormatString::parse_printf("% %").is_err());
}

#[test]
fn test_percent_with_leading_hash_fails() {
    assert!(FormatString::parse_printf("%#%").is_err());
}

#[test]
fn test_percent_with_leading_zero_fails() {
    assert!(FormatString::parse_printf("%0%").is_err());
}

#[test]
fn test_percent_with_length_fails() {
    assert!(FormatString::parse_printf("%hh%").is_err());
    assert!(FormatString::parse_printf("%h%").is_err());
    assert!(FormatString::parse_printf("%l%").is_err());
    assert!(FormatString::parse_printf("%L%").is_err());
    assert!(FormatString::parse_printf("%j%").is_err());
    assert!(FormatString::parse_printf("%z%").is_err());
    assert!(FormatString::parse_printf("%t%").is_err());
}

#[test]
fn test_percent_with_width_fails() {
    assert!(FormatString::parse_printf("%9%").is_err());
}

#[test]
fn test_percent_with_multidigit_width_fails() {
    assert!(FormatString::parse_printf("%10%").is_err());
}

#[test]
fn test_percent_with_star_width_fails() {
    assert!(FormatString::parse_printf("%*%").is_err());
}

#[test]
fn test_percent_with_precision_fails() {
    assert!(FormatString::parse_printf("%.5%").is_err());
}

#[test]
fn test_percent_with_multidigit_precision_fails() {
    assert!(FormatString::parse_printf("%.10%").is_err());
}

#[test]
fn test_percent_with_star_precision_fails() {
    assert!(FormatString::parse_printf("%*%").is_err());
}

const INTEGERS: &[(&str, Primitive, Style)] = &[
    ("d", Primitive::Integer, Style::None),
    ("i", Primitive::Integer, Style::None),
    ("o", Primitive::Unsigned, Style::Octal),
    ("u", Primitive::Unsigned, Style::None),
    ("x", Primitive::Unsigned, Style::Hex),
    ("X", Primitive::Unsigned, Style::UpperHex),
    // While not strictly an integer pointers take the same args as integers.
    ("p", Primitive::Pointer, Style::Pointer),
];

#[test]
fn test_integer() {
    for (format_char, primitive, style) in INTEGERS {
        assert_eq!(
            FormatString::parse_printf(&format!("%{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: HashSet::new(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: None,
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );
    }
}

#[test]
fn test_integer_with_minus() {
    for (format_char, primitive, style) in INTEGERS {
        assert_eq!(
            FormatString::parse_printf(&format!("%-5{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::Left,
                    flags: [Flag::LeftJustify].into_iter().collect(),
                    min_field_width: MinFieldWidth::Fixed(5),
                    precision: Precision::None,
                    length: None,
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );
    }
}

#[test]
fn test_integer_with_plus() {
    for (format_char, primitive, style) in INTEGERS {
        assert_eq!(
            FormatString::parse_printf(&format!("%+{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: [Flag::ForceSign].into_iter().collect(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: None,
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );
    }
}

#[test]
fn test_integer_with_blank_space() {
    for (format_char, primitive, style) in INTEGERS {
        assert_eq!(
            FormatString::parse_printf(&format!("% {format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: [Flag::SpaceSign].into_iter().collect(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: None,
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );
    }
}

#[test]
fn test_integer_with_plus_and_blank_space_ignores_blank_space() {
    for (format_char, primitive, style) in INTEGERS {
        assert_eq!(
            FormatString::parse_printf(&format!("%+ {format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: [Flag::ForceSign, Flag::SpaceSign].into_iter().collect(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: None,
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );

        assert_eq!(
            FormatString::parse_printf(&format!("% +{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: [Flag::ForceSign, Flag::SpaceSign].into_iter().collect(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: None,
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );
    }
}

#[test]
fn test_integer_with_hash() {
    for (format_char, primitive, style) in INTEGERS {
        assert_eq!(
            FormatString::parse_printf(&format!("%#{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: [Flag::AlternateSyntax].into_iter().collect(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: None,
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );
    }
}

#[test]
fn test_integer_with_zero() {
    for (format_char, primitive, style) in INTEGERS {
        assert_eq!(
            FormatString::parse_printf(&format!("%0{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: [Flag::LeadingZeros].into_iter().collect(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: None,
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );
    }
}

#[test]
fn test_integer_with_length() {
    for (format_char, primitive, style) in INTEGERS {
        assert_eq!(
            FormatString::parse_printf(&format!("%hh{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: HashSet::new(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: Some(Length::Char),
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );

        assert_eq!(
            FormatString::parse_printf(&format!("%h{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: HashSet::new(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: Some(Length::Short),
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );

        assert_eq!(
            FormatString::parse_printf(&format!("%l{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: HashSet::new(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: Some(Length::Long),
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );

        assert_eq!(
            FormatString::parse_printf(&format!("%ll{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: HashSet::new(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: Some(Length::LongLong),
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );

        assert_eq!(
            FormatString::parse_printf(&format!("%j{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: HashSet::new(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: Some(Length::IntMax),
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );

        assert_eq!(
            FormatString::parse_printf(&format!("%z{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: HashSet::new(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: Some(Length::Size),
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );

        assert_eq!(
            FormatString::parse_printf(&format!("%t{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: HashSet::new(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: Some(Length::PointerDiff),
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );
    }
}

const FLOATS: &[(&str, Primitive, Style)] = &[
    ("f", Primitive::Float, Style::None),
    ("e", Primitive::Float, Style::Exponential),
    ("E", Primitive::Float, Style::UpperExponential),
];

#[test]
fn test_float() {
    for (format_char, primitive, style) in FLOATS {
        assert_eq!(
            FormatString::parse_printf(&format!("%{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: HashSet::new(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: None,
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );
    }
}

#[test]
fn test_float_with_minus() {
    for (format_char, primitive, style) in FLOATS {
        assert_eq!(
            FormatString::parse_printf(&format!("%-10{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::Left,
                    flags: [Flag::LeftJustify].into_iter().collect(),
                    min_field_width: MinFieldWidth::Fixed(10),
                    precision: Precision::None,
                    length: None,
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );
    }
}

#[test]
fn test_float_with_plus() {
    for (format_char, primitive, style) in FLOATS {
        assert_eq!(
            FormatString::parse_printf(&format!("%+{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: [Flag::ForceSign].into_iter().collect(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: None,
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );
    }
}

#[test]
fn test_float_with_blank_space() {
    for (format_char, primitive, style) in FLOATS {
        assert_eq!(
            FormatString::parse_printf(&format!("% {format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: [Flag::SpaceSign].into_iter().collect(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: None,
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );
    }
}

#[test]
fn test_float_with_plus_and_blank_space_ignores_blank_space() {
    for (format_char, primitive, style) in FLOATS {
        assert_eq!(
            FormatString::parse_printf(&format!("%+ {format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: [Flag::ForceSign, Flag::SpaceSign].into_iter().collect(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: None,
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );

        assert_eq!(
            FormatString::parse_printf(&format!("% +{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: [Flag::ForceSign, Flag::SpaceSign].into_iter().collect(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: None,
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );
    }
}

#[test]
fn test_float_with_hash() {
    for (format_char, primitive, style) in FLOATS {
        assert_eq!(
            FormatString::parse_printf(&format!("%.0{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: [].into_iter().collect(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::Fixed(0),
                    length: None,
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );

        assert_eq!(
            FormatString::parse_printf(&format!("%#.0{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: [Flag::AlternateSyntax].into_iter().collect(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::Fixed(0),
                    length: None,
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );
    }
}

#[test]
fn test_float_with_zero() {
    for (format_char, primitive, style) in FLOATS {
        assert_eq!(
            FormatString::parse_printf(&format!("%010{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: [Flag::LeadingZeros].into_iter().collect(),
                    min_field_width: MinFieldWidth::Fixed(10),
                    precision: Precision::None,
                    length: None,
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );
    }
}

#[test]
fn test_float_with_length() {
    for (format_char, primitive, style) in FLOATS {
        assert_eq!(
            FormatString::parse_printf(&format!("%hh{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: HashSet::new(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: Some(Length::Char),
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );

        assert_eq!(
            FormatString::parse_printf(&format!("%h{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: HashSet::new(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: Some(Length::Short),
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );

        assert_eq!(
            FormatString::parse_printf(&format!("%l{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: HashSet::new(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: Some(Length::Long),
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );

        assert_eq!(
            FormatString::parse_printf(&format!("%ll{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: HashSet::new(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: Some(Length::LongLong),
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );

        assert_eq!(
            FormatString::parse_printf(&format!("%j{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: HashSet::new(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: Some(Length::IntMax),
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );

        assert_eq!(
            FormatString::parse_printf(&format!("%z{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: HashSet::new(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: Some(Length::Size),
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );

        assert_eq!(
            FormatString::parse_printf(&format!("%t{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: HashSet::new(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: Some(Length::PointerDiff),
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );

        assert_eq!(
            FormatString::parse_printf(&format!("%L{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: HashSet::new(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::None,
                    length: Some(Length::LongDouble),
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );
    }
}

#[test]
fn test_float_with_width() {
    for (format_char, primitive, style) in FLOATS {
        assert_eq!(
            FormatString::parse_printf(&format!("%9{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: [].into_iter().collect(),
                    min_field_width: MinFieldWidth::Fixed(9),
                    precision: Precision::None,
                    length: None,
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );
    }
}

#[test]
fn test_float_with_multidigit_width() {
    for (format_char, primitive, style) in FLOATS {
        assert_eq!(
            FormatString::parse_printf(&format!("%10{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: [].into_iter().collect(),
                    min_field_width: MinFieldWidth::Fixed(10),
                    precision: Precision::None,
                    length: None,
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );
    }
}

#[test]
fn test_float_with_star_width() {
    for (format_char, primitive, style) in FLOATS {
        assert_eq!(
            FormatString::parse_printf(&format!("%*{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: [].into_iter().collect(),
                    min_field_width: MinFieldWidth::Variable,
                    precision: Precision::None,
                    length: None,
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );
    }
}

#[test]
fn test_float_with_precision() {
    for (format_char, primitive, style) in FLOATS {
        assert_eq!(
            FormatString::parse_printf(&format!("%.4{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: [].into_iter().collect(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::Fixed(4),
                    length: None,
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );
    }
}

#[test]
fn test_float_with_multidigit_precision() {
    for (format_char, primitive, style) in FLOATS {
        assert_eq!(
            FormatString::parse_printf(&format!("%.10{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: [].into_iter().collect(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::Fixed(10),
                    length: None,
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );
    }
}

#[test]
fn test_float_with_star_precision() {
    for (format_char, primitive, style) in FLOATS {
        assert_eq!(
            FormatString::parse_printf(&format!("%.*{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: [].into_iter().collect(),
                    min_field_width: MinFieldWidth::None,
                    precision: Precision::Variable,
                    length: None,
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );
    }
}

#[test]
fn test_float_with_star_width_and_star_precision() {
    for (format_char, primitive, style) in FLOATS {
        assert_eq!(
            FormatString::parse_printf(&format!("%*.*{format_char}")),
            Ok(FormatString {
                fragments: vec![FormatFragment::Conversion(ConversionSpec {
                    argument: Argument::None,
                    fill: ' ',
                    alignment: Alignment::None,
                    flags: [].into_iter().collect(),
                    min_field_width: MinFieldWidth::Variable,
                    precision: Precision::Variable,
                    length: None,
                    primitive: *primitive,
                    style: *style,
                })]
            })
        );
    }
}

#[test]
fn test_char() {
    assert_eq!(
        FormatString::parse_printf("%c"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: [].into_iter().collect(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::None,
                length: None,
                primitive: Primitive::Character,
                style: Style::None,
            })]
        })
    );
}

#[test]
fn test_char_with_minus() {
    assert_eq!(
        FormatString::parse_printf("%-5c"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::Left,
                flags: [Flag::LeftJustify].into_iter().collect(),
                min_field_width: MinFieldWidth::Fixed(5),
                precision: Precision::None,
                length: None,
                primitive: Primitive::Character,
                style: Style::None,
            })]
        })
    );
}

#[test]
fn test_char_with_plus() {
    // TODO: b/281750433 - This test should fail.
    assert!(FormatString::parse_printf("%+c").is_ok());
}

#[test]
fn test_char_with_blank_space() {
    // TODO: b/281750433 - This test should fail.
    assert!(FormatString::parse_printf("% c").is_ok());
}

#[test]
fn test_char_with_hash() {
    // TODO: b/281750433 - This test should fail.
    assert!(FormatString::parse_printf("%#c").is_ok());
}

#[test]
fn test_char_with_zero() {
    // TODO: b/281750433 - This test should fail.
    assert!(FormatString::parse_printf("%0c").is_ok());
}

#[test]
fn test_char_with_length() {
    // Length modifiers are ignored by %c but are still returned by the
    // parser.
    assert_eq!(
        FormatString::parse_printf("%hhc"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::None,
                length: Some(Length::Char),
                primitive: Primitive::Character,
                style: Style::None,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_printf("%hc"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::None,
                length: Some(Length::Short),
                primitive: Primitive::Character,
                style: Style::None,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_printf("%lc"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::None,
                length: Some(Length::Long),
                primitive: Primitive::Character,
                style: Style::None,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_printf("%llc"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::None,
                length: Some(Length::LongLong),
                primitive: Primitive::Character,
                style: Style::None,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_printf("%jc"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::None,
                length: Some(Length::IntMax),
                primitive: Primitive::Character,
                style: Style::None,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_printf("%zc"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::None,
                length: Some(Length::Size),
                primitive: Primitive::Character,
                style: Style::None,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_printf("%tc"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::None,
                length: Some(Length::PointerDiff),
                primitive: Primitive::Character,
                style: Style::None,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_printf("%Lc"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::None,
                length: Some(Length::LongDouble),
                primitive: Primitive::Character,
                style: Style::None,
            })]
        })
    );
}

#[test]
fn test_char_with_width() {
    assert_eq!(
        FormatString::parse_printf("%5c"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: [].into_iter().collect(),
                min_field_width: MinFieldWidth::Fixed(5),
                precision: Precision::None,
                length: None,
                primitive: Primitive::Character,
                style: Style::None,
            })]
        })
    );
}

#[test]
fn test_char_with_multidigit_width() {
    assert_eq!(
        FormatString::parse_printf("%10c"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: [].into_iter().collect(),
                min_field_width: MinFieldWidth::Fixed(10),
                precision: Precision::None,
                length: None,
                primitive: Primitive::Character,
                style: Style::None,
            })]
        })
    );
}

#[test]
fn test_char_with_star_width() {
    assert_eq!(
        FormatString::parse_printf("%*c"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: [].into_iter().collect(),
                min_field_width: MinFieldWidth::Variable,
                precision: Precision::None,
                length: None,
                primitive: Primitive::Character,
                style: Style::None,
            })]
        })
    );
}

#[test]
fn test_char_with_precision() {
    // TODO: b/281750433 - This test should fail.
    assert!(FormatString::parse_printf("%.4c").is_ok());
}

#[test]
fn test_long_char_with_hash() {
    // TODO: b/281750433 - This test should fail.
    assert!(FormatString::parse_printf("%#lc").is_ok());
}

#[test]
fn test_long_char_with_zero() {
    // TODO: b/281750433 - This test should fail.
    assert!(FormatString::parse_printf("%0lc").is_ok());
}

#[test]
fn test_string() {
    assert_eq!(
        FormatString::parse_printf("%s"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: [].into_iter().collect(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::None,
                length: None,
                primitive: Primitive::String,
                style: Style::None,
            })]
        })
    );
}

#[test]
fn test_string_with_minus() {
    assert_eq!(
        FormatString::parse_printf("%-6s"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::Left,
                flags: [Flag::LeftJustify].into_iter().collect(),
                min_field_width: MinFieldWidth::Fixed(6),
                precision: Precision::None,
                length: None,
                primitive: Primitive::String,
                style: Style::None,
            })]
        })
    );
}

#[test]
fn test_string_with_plus() {
    // TODO: b/281750433 - This test should fail.
    assert!(FormatString::parse_printf("%+s").is_ok());
}

#[test]
fn test_string_with_blank_space() {
    // TODO: b/281750433 - This test should fail.
    assert!(FormatString::parse_printf("% s").is_ok());
}

#[test]
fn test_string_with_hash() {
    // TODO: b/281750433 - This test should fail.
    assert!(FormatString::parse_printf("%#s").is_ok());
}

#[test]
fn test_string_with_zero() {
    // TODO: b/281750433 - This test should fail.
    assert!(FormatString::parse_printf("%0s").is_ok());
}

#[test]
fn test_string_with_length() {
    // Length modifiers are ignored by %s but are still returned by the
    // parser.
    assert_eq!(
        FormatString::parse_printf("%hhs"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::None,
                length: Some(Length::Char),
                primitive: Primitive::String,
                style: Style::None,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_printf("%hs"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::None,
                length: Some(Length::Short),
                primitive: Primitive::String,
                style: Style::None,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_printf("%ls"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::None,
                length: Some(Length::Long),
                primitive: Primitive::String,
                style: Style::None,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_printf("%lls"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::None,
                length: Some(Length::LongLong),
                primitive: Primitive::String,
                style: Style::None,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_printf("%js"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::None,
                length: Some(Length::IntMax),
                primitive: Primitive::String,
                style: Style::None,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_printf("%zs"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::None,
                length: Some(Length::Size),
                primitive: Primitive::String,
                style: Style::None,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_printf("%ts"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::None,
                length: Some(Length::PointerDiff),
                primitive: Primitive::String,
                style: Style::None,
            })]
        })
    );

    assert_eq!(
        FormatString::parse_printf("%Ls"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: HashSet::new(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::None,
                length: Some(Length::LongDouble),
                primitive: Primitive::String,
                style: Style::None,
            })]
        })
    );
}

#[test]
fn test_string_with_width() {
    assert_eq!(
        FormatString::parse_printf("%6s"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: [].into_iter().collect(),
                min_field_width: MinFieldWidth::Fixed(6),
                precision: Precision::None,
                length: None,
                primitive: Primitive::String,
                style: Style::None,
            })]
        })
    );
}

#[test]
fn test_string_with_multidigit_width() {
    assert_eq!(
        FormatString::parse_printf("%10s"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: [].into_iter().collect(),
                min_field_width: MinFieldWidth::Fixed(10),
                precision: Precision::None,
                length: None,
                primitive: Primitive::String,
                style: Style::None,
            })]
        })
    );
}

#[test]
fn test_string_with_star_width() {
    assert_eq!(
        FormatString::parse_printf("%*s"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: [].into_iter().collect(),
                min_field_width: MinFieldWidth::Variable,
                precision: Precision::None,
                length: None,
                primitive: Primitive::String,
                style: Style::None,
            })]
        })
    );
}

#[test]
fn test_string_with_star_precision() {
    assert_eq!(
        FormatString::parse_printf("%.3s"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: [].into_iter().collect(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::Fixed(3),
                length: None,
                primitive: Primitive::String,
                style: Style::None,
            })]
        })
    );
}

#[test]
fn test_string_with_multidigit_precision() {
    assert_eq!(
        FormatString::parse_printf("%.10s"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: [].into_iter().collect(),
                min_field_width: MinFieldWidth::None,
                precision: Precision::Fixed(10),
                length: None,
                primitive: Primitive::String,
                style: Style::None,
            })]
        })
    );
}

#[test]
fn test_string_with_width_and_precision() {
    assert_eq!(
        FormatString::parse_printf("%10.3s"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: [].into_iter().collect(),
                min_field_width: MinFieldWidth::Fixed(10),
                precision: Precision::Fixed(3),
                length: None,
                primitive: Primitive::String,
                style: Style::None,
            })]
        })
    );
}

#[test]
fn test_string_with_star_width_and_star_precision() {
    assert_eq!(
        FormatString::parse_printf("%*.*s"),
        Ok(FormatString {
            fragments: vec![FormatFragment::Conversion(ConversionSpec {
                argument: Argument::None,
                fill: ' ',
                alignment: Alignment::None,
                flags: [].into_iter().collect(),
                min_field_width: MinFieldWidth::Variable,
                precision: Precision::Variable,
                length: None,
                primitive: Primitive::String,
                style: Style::None,
            })]
        })
    );
}

#[test]
fn test_long_string_with_hash() {
    // TODO: b/281750433 - This test should fail.
    assert!(FormatString::parse_printf("%#ls").is_ok());
}

#[test]
fn test_long_string_with_zero() {
    // TODO: b/281750433 - This test should fail.
    assert!(FormatString::parse_printf("%0ls").is_ok());
}
