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
#![no_std]

//! # pw_assert_backend_std
//!
//! `pw_assert_backend_std` provides an implementation of `pw_assert`
//! backend macros that route to `std::panic`, `std::assert`,
//! `std::assert_eq` and `std::assert_ne`.

extern crate std;

/// Backend implementation of [`pw_assert::panic`].
#[macro_export]
macro_rules! panic_backend {
    ($format_string:literal $(,)?) => {{
        #[allow(clippy::unnecessary_cast)]
        {
            std::panic!($format_string)
        }
    }};

    ($format_string:literal, $($args:expr),* $(,)?) => {{
        #[allow(clippy::unnecessary_cast)]
        {
            std::panic!($format_string, $($args),*)
        }
    }};
}

/// Backend implementation of unary assertions (invoked by [`pw_assert::assert`]
/// and [`pw_assert::debug_assert`]).
#[macro_export]
macro_rules! assert_unary_backend {
    ($condition:expr $(,)?) => {{
        #[allow(clippy::unnecessary_cast)]
        {
            std::assert!($condition)
        }
    }};

    ($condition:expr, $($args:expr),* $(,)?) => {{
        #[allow(clippy::unnecessary_cast)]
        {
            std::assert!($condition, $($args),*)
        }
    }};
}

/// Backend implementation of binary assertions (invoked by [`pw_assert::eq`],
/// [`pw_assert::ne`], [`pw_assert::debug_eq`], and [`pw_assert::debug_ne`]).
#[macro_export]
macro_rules! assert_binary_backend {
    ($lhs:expr, ==, $rhs:expr $(,)?) => {{
        #[allow(clippy::unnecessary_cast)]
        {
            std::assert_eq!($lhs, $rhs)
        }
    }};

    ($lhs:expr, ==, $rhs:expr, $($args:expr),* $(,)?) => {{
        #[allow(clippy::unnecessary_cast)]
        {
            std::assert_eq!($lhs, $rhs, $($args),*)
        }
    }};

    ($lhs:expr, !=, $rhs:expr $(,)?) => {{
        #[allow(clippy::unnecessary_cast)]
        {
            std::assert_ne!($lhs, $rhs)
        }
    }};

    ($lhs:expr, !=, $rhs:expr, $($args:expr),* $(,)?) => {{
        #[allow(clippy::unnecessary_cast)]
        {
            std::assert_ne!($lhs, $rhs, $($args),*)
        }
    }};

    ($lhs:expr, $op:tt, $rhs:expr $(,)?) => {{
        #[allow(clippy::unnecessary_cast)]
        {
            std::assert!($lhs $op $rhs)
        }
    }};

    ($lhs:expr, $op:tt, $rhs:expr, $($args:expr),* $(,)?) => {{
        #[allow(clippy::unnecessary_cast)]
        {
            std::assert!($lhs $op $rhs, $($args),*)
        }
    }};
}
