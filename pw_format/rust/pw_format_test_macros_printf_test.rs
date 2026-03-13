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

#[allow(unused_imports)]
use pw_format::{Style, macros::FormatParams};

// Used to record calls into the test generator from `generator_test_macro!`.
#[derive(Debug, PartialEq)]
pub enum TestGeneratorOps {
    Finalize,
    StringFragment(String),
    IntegerConversion {
        params: FormatParams,
        signed: bool,
        type_width: u8,
        arg: String,
    },
    StringConversion(String),
    CharConversion(String),
    UntypedConversion(String),
}

// Used to record calls into the test generator from `printf_generator_test_macro!` and friends.
#[derive(Clone, Debug, PartialEq)]
pub enum PrintfTestGeneratorOps {
    Finalize,
    StringFragment(String),
    IntegerConversion { ty: String, arg: String },
    StringConversion(String),
    CharConversion(String),
    UntypedConversion(String),
}

#[cfg(test)]
#[allow(clippy::literal_string_with_formatting_args)]
mod tests {
    use pw_format_test_macros::{
        char_sub_printf_format_core_fmt_generator_test_macro,
        char_sub_printf_format_printf_generator_test_macro,
        integer_sub_printf_format_core_fmt_generator_test_macro,
        integer_sub_printf_format_printf_generator_test_macro,
        printf_format_core_fmt_generator_test_macro, printf_format_generator_test_macro,
        printf_format_printf_generator_test_macro,
        string_sub_printf_format_core_fmt_generator_test_macro,
        string_sub_printf_format_printf_generator_test_macro,
    };

    use super::*;
    // Create an alias to ourselves so that the proc macro can name our crate.
    use crate as pw_format_test_macros_test;

    #[test]
    fn generate_calls_generator_correctly() {
        assert_eq!(
            printf_format_generator_test_macro!("test %ld %s %c %v", 5, "test", 'c', 1),
            vec![
                TestGeneratorOps::StringFragment("test ".to_string()),
                TestGeneratorOps::IntegerConversion {
                    params: FormatParams {
                        style: Style::None,
                        min_field_width: None,
                        zero_padding: false,
                        alternate_syntax: false,
                    },
                    signed: true,
                    type_width: 32,
                    arg: "5".to_string(),
                },
                TestGeneratorOps::StringFragment(" ".to_string()),
                TestGeneratorOps::StringConversion("\"test\"".to_string()),
                TestGeneratorOps::StringFragment(" ".to_string()),
                TestGeneratorOps::CharConversion("'c'".to_string()),
                TestGeneratorOps::StringFragment(" ".to_string()),
                TestGeneratorOps::UntypedConversion("1".to_string()),
                TestGeneratorOps::Finalize
            ]
        );
    }

    #[test]
    fn generate_printf_calls_generator_correctly() {
        assert_eq!(
            printf_format_printf_generator_test_macro!(
                "test %ld %#08x %s %c %v %v",
                5,
                0x42,
                "test",
                'c',
                1 as i32,
                "string" as &str
            ),
            (
                // %ld gets converted to %d because they are equivalent for 32 bit
                // systems.
                // %v gets converted to %d since we pass in a signed integer.
                "test %d %#08x %s %c %d %s",
                vec![
                    PrintfTestGeneratorOps::StringFragment("test ".to_string()),
                    PrintfTestGeneratorOps::IntegerConversion {
                        ty: "i32".to_string(),
                        arg: "5".to_string(),
                    },
                    PrintfTestGeneratorOps::StringFragment(" ".to_string()),
                    PrintfTestGeneratorOps::IntegerConversion {
                        ty: "u32".to_string(),
                        arg: "0x42".to_string(),
                    },
                    PrintfTestGeneratorOps::StringFragment(" ".to_string()),
                    PrintfTestGeneratorOps::StringConversion("\"test\"".to_string()),
                    PrintfTestGeneratorOps::StringFragment(" ".to_string()),
                    PrintfTestGeneratorOps::CharConversion("'c'".to_string()),
                    PrintfTestGeneratorOps::StringFragment(" ".to_string()),
                    PrintfTestGeneratorOps::UntypedConversion("1 as i32".to_string()),
                    PrintfTestGeneratorOps::StringFragment(" ".to_string()),
                    PrintfTestGeneratorOps::UntypedConversion("\"string\" as & str".to_string()),
                    PrintfTestGeneratorOps::Finalize
                ]
            )
        );
    }

    #[test]
    fn generate_printf_translates_field_width_and_leading_zeros_correctly() {
        let expected_fragments = vec![
            PrintfTestGeneratorOps::StringFragment("Test ".to_string()),
            PrintfTestGeneratorOps::IntegerConversion {
                ty: "u32".to_string(),
                arg: "0x42".to_string(),
            },
            PrintfTestGeneratorOps::StringFragment(" test".to_string()),
            PrintfTestGeneratorOps::Finalize,
        ];

        // No field width.
        assert_eq!(
            printf_format_printf_generator_test_macro!("Test %x test", 0x42),
            ("Test %x test", expected_fragments.clone())
        );

        // Field width without zero padding.
        assert_eq!(
            printf_format_printf_generator_test_macro!("Test %8x test", 0x42),
            ("Test %8x test", expected_fragments.clone())
        );

        // Field width with zero padding.
        assert_eq!(
            printf_format_printf_generator_test_macro!("Test %08x test", 0x42),
            ("Test %08x test", expected_fragments.clone())
        );

        // Test another format base.
        assert_eq!(
            printf_format_printf_generator_test_macro!("Test %08u test", 0x42),
            ("Test %08u test", expected_fragments.clone())
        );
    }

    #[test]
    fn generate_core_fmt_translates_field_width_and_leading_zeros_correctly() {
        let expected_fragments = vec![
            PrintfTestGeneratorOps::StringFragment("Test ".to_string()),
            PrintfTestGeneratorOps::IntegerConversion {
                ty: "u32".to_string(),
                arg: "0x42".to_string(),
            },
            PrintfTestGeneratorOps::StringFragment(" test".to_string()),
            PrintfTestGeneratorOps::Finalize,
        ];

        // No field width.
        assert_eq!(
            printf_format_core_fmt_generator_test_macro!("Test %x test", 0x42),
            ("Test {:x} test", expected_fragments.clone())
        );

        // Field width without zero padding.
        assert_eq!(
            printf_format_core_fmt_generator_test_macro!("Test %8x test", 0x42),
            ("Test {:8x} test", expected_fragments.clone())
        );

        // Field width with zero padding.
        assert_eq!(
            printf_format_core_fmt_generator_test_macro!("Test %08x test", 0x42),
            ("Test {:08x} test", expected_fragments.clone())
        );

        // Test another format base
        assert_eq!(
            printf_format_core_fmt_generator_test_macro!("Test %08u test", 0x42),
            ("Test {:08} test", expected_fragments.clone())
        );
    }

    // Test that a generator returning an overridden integer conversion specifier
    // changes that and only that conversion specifier in the format string.
    #[test]
    fn generate_printf_substitutes_integer_conversion() {
        assert_eq!(
            integer_sub_printf_format_printf_generator_test_macro!(
                "test %ld %s %c",
                5,
                "test",
                'c'
            ),
            (
                "test %K %s %c",
                vec![
                    PrintfTestGeneratorOps::StringFragment("test ".to_string()),
                    PrintfTestGeneratorOps::IntegerConversion {
                        ty: "i32".to_string(),
                        arg: "5".to_string(),
                    },
                    PrintfTestGeneratorOps::StringFragment(" ".to_string()),
                    PrintfTestGeneratorOps::StringConversion("\"test\"".to_string()),
                    PrintfTestGeneratorOps::StringFragment(" ".to_string()),
                    PrintfTestGeneratorOps::CharConversion("'c'".to_string()),
                    PrintfTestGeneratorOps::Finalize
                ]
            )
        );
    }

    // Test that a generator returning an overridden string conversion specifier
    // changes that and only that conversion specifier in the format string.
    #[test]
    fn generate_printf_substitutes_string_conversion() {
        assert_eq!(
            string_sub_printf_format_printf_generator_test_macro!("test %ld %s %c", 5, "test", 'c'),
            (
                // %ld gets converted to %d because they are equivalent for 32 bit
                // systems.
                "test %d %K %c",
                vec![
                    PrintfTestGeneratorOps::StringFragment("test ".to_string()),
                    PrintfTestGeneratorOps::IntegerConversion {
                        ty: "i32".to_string(),
                        arg: "5".to_string(),
                    },
                    PrintfTestGeneratorOps::StringFragment(" ".to_string()),
                    PrintfTestGeneratorOps::StringConversion("\"test\"".to_string()),
                    PrintfTestGeneratorOps::StringFragment(" ".to_string()),
                    PrintfTestGeneratorOps::CharConversion("'c'".to_string()),
                    PrintfTestGeneratorOps::Finalize
                ]
            )
        );
    }

    // Test that a generator returning an overridden character conversion specifier
    // changes that and only that conversion specifier in the format string.
    #[test]
    fn generate_printf_substitutes_char_conversion() {
        assert_eq!(
            char_sub_printf_format_printf_generator_test_macro!("test %ld %s %c", 5, "test", 'c'),
            (
                // %ld gets converted to %d because they are equivalent for 32 bit
                // systems.
                "test %d %s %K",
                vec![
                    PrintfTestGeneratorOps::StringFragment("test ".to_string()),
                    PrintfTestGeneratorOps::IntegerConversion {
                        ty: "i32".to_string(),
                        arg: "5".to_string()
                    },
                    PrintfTestGeneratorOps::StringFragment(" ".to_string()),
                    PrintfTestGeneratorOps::StringConversion("\"test\"".to_string()),
                    PrintfTestGeneratorOps::StringFragment(" ".to_string()),
                    PrintfTestGeneratorOps::CharConversion("'c'".to_string()),
                    PrintfTestGeneratorOps::Finalize
                ]
            )
        );
    }

    #[test]
    fn generate_core_fmt_calls_generator_correctly() {
        assert_eq!(
            printf_format_core_fmt_generator_test_macro!("test %ld %s %c %v", 5, "test", 'c', 1),
            (
                "test {} {} {} {}",
                vec![
                    PrintfTestGeneratorOps::StringFragment("test ".to_string()),
                    PrintfTestGeneratorOps::IntegerConversion {
                        ty: "i32".to_string(),
                        arg: "5".to_string()
                    },
                    PrintfTestGeneratorOps::StringFragment(" ".to_string()),
                    PrintfTestGeneratorOps::StringConversion("\"test\"".to_string()),
                    PrintfTestGeneratorOps::StringFragment(" ".to_string()),
                    PrintfTestGeneratorOps::CharConversion("'c'".to_string()),
                    PrintfTestGeneratorOps::StringFragment(" ".to_string()),
                    PrintfTestGeneratorOps::UntypedConversion("1".to_string()),
                    PrintfTestGeneratorOps::Finalize
                ]
            )
        );
    }

    // Test that a generator returning an overridden integer conversion specifier
    // changes that and only that conversion specifier in the format string.
    #[test]
    fn generate_core_fmt_substitutes_integer_conversion() {
        assert_eq!(
            integer_sub_printf_format_core_fmt_generator_test_macro!(
                "test %ld %s %c",
                5,
                "test",
                'c'
            ),
            (
                "test {:?} {} {}",
                vec![
                    PrintfTestGeneratorOps::StringFragment("test ".to_string()),
                    PrintfTestGeneratorOps::IntegerConversion {
                        ty: "i32".to_string(),
                        arg: "5".to_string()
                    },
                    PrintfTestGeneratorOps::StringFragment(" ".to_string()),
                    PrintfTestGeneratorOps::StringConversion("\"test\"".to_string()),
                    PrintfTestGeneratorOps::StringFragment(" ".to_string()),
                    PrintfTestGeneratorOps::CharConversion("'c'".to_string()),
                    PrintfTestGeneratorOps::Finalize
                ]
            )
        );
    }

    // Test that a generator returning an overridden string conversion specifier
    // changes that and only that conversion specifier in the format string.
    #[test]
    fn generate_core_fmt_substitutes_string_conversion() {
        assert_eq!(
            string_sub_printf_format_core_fmt_generator_test_macro!(
                "test %ld %s %c",
                5,
                "test",
                'c'
            ),
            (
                "test {} {:?} {}",
                vec![
                    PrintfTestGeneratorOps::StringFragment("test ".to_string()),
                    PrintfTestGeneratorOps::IntegerConversion {
                        ty: "i32".to_string(),
                        arg: "5".to_string(),
                    },
                    PrintfTestGeneratorOps::StringFragment(" ".to_string()),
                    PrintfTestGeneratorOps::StringConversion("\"test\"".to_string()),
                    PrintfTestGeneratorOps::StringFragment(" ".to_string()),
                    PrintfTestGeneratorOps::CharConversion("'c'".to_string()),
                    PrintfTestGeneratorOps::Finalize
                ]
            )
        );
    }

    // Test that a generator returning an overridden character conversion specifier
    // changes that and only that conversion specifier in the format string.
    #[test]
    fn generate_core_fmt_substitutes_char_conversion() {
        assert_eq!(
            char_sub_printf_format_core_fmt_generator_test_macro!("test %ld %s %c", 5, "test", 'c'),
            (
                "test {} {} {:?}",
                vec![
                    PrintfTestGeneratorOps::StringFragment("test ".to_string()),
                    PrintfTestGeneratorOps::IntegerConversion {
                        ty: "i32".to_string(),
                        arg: "5".to_string(),
                    },
                    PrintfTestGeneratorOps::StringFragment(" ".to_string()),
                    PrintfTestGeneratorOps::StringConversion("\"test\"".to_string()),
                    PrintfTestGeneratorOps::StringFragment(" ".to_string()),
                    PrintfTestGeneratorOps::CharConversion("'c'".to_string()),
                    PrintfTestGeneratorOps::Finalize
                ]
            )
        );
    }

    #[test]
    fn multiple_format_strings_are_concatenated() {
        assert_eq!(
            printf_format_generator_test_macro!("a" PW_FMT_CONCAT "b"),
            vec![
                TestGeneratorOps::StringFragment("ab".to_string()),
                TestGeneratorOps::Finalize
            ]
        );
    }
}
