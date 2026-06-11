// Copyright 2026 The Pigweed Authors
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

use nom::branch::alt;
use nom::bytes::complete::tag;
use nom::character::complete::digit1;
use nom::combinator::{map, map_res};
use nom::IResult;

use crate::{MinFieldWidth, Precision};

fn variable_width(input: &str) -> IResult<&str, MinFieldWidth> {
    map(tag("*"), |_| MinFieldWidth::Variable)(input)
}

pub(crate) fn fixed_width(input: &str) -> IResult<&str, MinFieldWidth> {
    map_res(
        digit1,
        |value: &str| -> Result<MinFieldWidth, std::num::ParseIntError> {
            Ok(MinFieldWidth::Fixed(value.parse()?))
        },
    )(input)
}

fn no_width(input: &str) -> IResult<&str, MinFieldWidth> {
    Ok((input, MinFieldWidth::None))
}

pub(crate) fn width(input: &str) -> IResult<&str, MinFieldWidth> {
    alt((variable_width, fixed_width, no_width))(input)
}

fn variable_precision(input: &str) -> IResult<&str, Precision> {
    let (input, _) = tag(".")(input)?;
    map(tag("*"), |_| Precision::Variable)(input)
}

fn fixed_precision(input: &str) -> IResult<&str, Precision> {
    let (input, _) = tag(".")(input)?;
    map_res(
        digit1,
        |value: &str| -> Result<Precision, std::num::ParseIntError> {
            Ok(Precision::Fixed(value.parse()?))
        },
    )(input)
}

fn no_precision(input: &str) -> IResult<&str, Precision> {
    Ok((input, Precision::None))
}

pub(crate) fn precision(input: &str) -> IResult<&str, Precision> {
    alt((variable_precision, fixed_precision, no_precision))(input)
}
