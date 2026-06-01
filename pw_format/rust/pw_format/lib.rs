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

//! The `pw_format` crate is a parser used to implement proc macros that:
//! * Understand format string argument types at compile time.
//! * Syntax check format strings.
//!
//! `pw_format` is written against `std` and is not intended to be
//! used in an embedded context.  Some efficiency and memory is traded for a
//! more expressive interface that exposes the format string's "syntax tree"
//! to the API client.
//!
//! # Proc Macros
//!
//! The [`macros`] module provides infrastructure for implementing proc macros
//! that take format strings as arguments.
//!
//! # Example
//!
//! ```
//! use pw_format::{
//!     Alignment, Argument, ConversionSpec, Flag, FormatFragment, FormatString,
//!     Length, MinFieldWidth, Precision, Primitive, Style,
//! };
//!
//! let format_string =
//!   FormatString::parse_printf("long double %+ 4.2Lf is %-03hd%%.").unwrap();
//!
//! assert_eq!(format_string, FormatString {
//!   fragments: vec![
//!       FormatFragment::Literal("long double ".to_string()),
//!       FormatFragment::Conversion(ConversionSpec {
//!           argument: Argument::None,
//!           fill: ' ',
//!           alignment: Alignment::None,
//!           flags: [Flag::ForceSign, Flag::SpaceSign].into_iter().collect(),
//!           min_field_width: MinFieldWidth::Fixed(4),
//!           precision: Precision::Fixed(2),
//!           length: Some(Length::LongDouble),
//!           primitive: Primitive::Float,
//!           style: Style::None,
//!       }),
//!       FormatFragment::Literal(" is ".to_string()),
//!       FormatFragment::Conversion(ConversionSpec {
//!           argument: Argument::None,
//!           fill: ' ',
//!           alignment: Alignment::Left,
//!           flags: [Flag::LeftJustify, Flag::LeadingZeros]
//!               .into_iter()
//!               .collect(),
//!           min_field_width: MinFieldWidth::Fixed(3),
//!           precision: Precision::None,
//!           length: Some(Length::Short),
//!           primitive: Primitive::Integer,
//!           style: Style::None,
//!       }),
//!       FormatFragment::Literal("%.".to_string()),
//!   ]
//! });
//! ```
#![deny(missing_docs)]

pub mod macros;

mod core_fmt;
mod format_string;
mod parser_util;
mod printf;

pub use format_string::{
    Alignment, Arg, Argument, ConversionSpec, Flag, FormatFragment, FormatString, FormatStyle,
    Length, MinFieldWidth, Precision, Primitive, Style,
};

#[cfg(test)]
mod tests;
