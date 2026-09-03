// Copyright 2025 The Pigweed Authors
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

//! # pw_assert
//!
//! `pw_assert` provides crash-safe assert and panic macros that route to a
//! configured backend. This is designed for embedded systems where standard
//! library panics might not be suitable, or where specific logging/recovery
//! behavior is needed.
//!
//! The macros in this crate are designed to be drop-in replacements for
//! `core::panic!`, `core::assert!`, etc., and delegate to backend macros:
//! - [`panic!`] and [`debug_panic!`] -> `panic_backend!`
//! - [`assert!`] and [`debug_assert!`] -> `assert_unary_backend!`
//! - [`eq!`], [`ne!`], [`debug_eq!`], and [`debug_ne!`] -> `assert_binary_backend!`
//!
//! # Example
//!
//! ```no_run
//! pw_assert::assert!(42 == 16, "Stack start is not aligned");
//!
//! pw_assert::panic!("Unhandled interrupt: irq={}", 16 as u32);
//!
//! pw_assert::debug_assert!(1 == 0);
//!
//! pw_assert::debug_panic!("Next monotonic tick overflow");
//! ```

// Re-export backend macros for use by facade macros.
#[doc(hidden)]
pub mod __private {
    pub use pw_assert_backend::{assert_binary_backend, assert_unary_backend, panic_backend};
}

/// Panics unconditionally.
///
/// This macro delegates to the backend macro `panic_backend!`.
///
/// # Examples
///
/// ```no_run
/// pw_assert::panic!("Something went terribly wrong!");
///
/// pw_assert::panic!("Error code: {}", 42 as i32);
/// ```
#[macro_export]
macro_rules! panic {
    ($($arg:tt)*) => {
        $crate::__private::panic_backend!($($arg)*)
    };
}

/// Panics unconditionally when debug_assertions are enabled
///
/// If `debug_assertions` are enabled, this behaves exactly like [`panic!`]
/// (delegating to `panic_backend!`).
/// If `debug_assertions` are disabled, this macro is a no-op.
///
/// `debug_assertions` can be enabled by setting this bazel label to `True`
/// `@pigweed//pw_assert/rust:debug_assertions`.
///
/// # Examples
///
/// ```no_run
/// pw_assert::debug_panic!("This should never happen in debug mode.");
/// ```
#[macro_export]
#[cfg(feature = "debug_assertions")]
macro_rules! debug_panic {
    ($($arg:tt)*) => {
        $crate::__private::panic_backend!($($arg)*)
    };
}

/// Panics unconditionally when debug_assertions are enabled.
#[macro_export]
#[cfg(not(feature = "debug_assertions"))]
macro_rules! debug_panic {
    ($($arg:tt)*) => {};
}

/// Asserts that a condition is true.
///
/// If the condition evaluates to `false`, this macro delegates to the backend
/// macro `assert_unary_backend!`.
///
/// # Examples
///
/// ```no_run
/// let x = 5;
/// pw_assert::assert!(x > 0);
/// pw_assert::assert!(x == 5, "x should be 5, but was {}", x as i32);
/// ```
#[macro_export]
macro_rules! assert {
    ($condition:expr $(,)?) => {{
        $crate::__private::assert_unary_backend!($condition);
    }};

    ($condition:expr, $($args:expr),* $(,)?) => {{
        $crate::__private::assert_unary_backend!($condition, $($args),*);
    }};
}

/// Asserts that a condition is true when debug_assertions are enabled.
///
/// If `debug_assertions` are enabled, this behaves exactly like [`assert!`]
/// (delegating to `assert_unary_backend!` if the condition evaluates to `false`).
/// If `debug_assertions` are disabled, this macro is a no-op.
///
/// `debug_assertions` can be enabled by setting this bazel label to `True`
/// `@pigweed//pw_assert/rust:debug_assertions`.
///
/// # Examples
///
/// ```no_run
/// let x = 5;
/// pw_assert::debug_assert!(x == 5);
/// ```
#[macro_export]
#[cfg(feature = "debug_assertions")]
macro_rules! debug_assert {
    ($condition:expr $(,)?) => {{
        $crate::__private::assert_unary_backend!($condition);
    }};

    ($condition:expr, $($args:expr),* $(,)?) => {{
        $crate::__private::assert_unary_backend!($condition, $($args),*);
    }};
}

/// Asserts that a condition is true when debug_assertions are enabled.
#[macro_export]
#[cfg(not(feature = "debug_assertions"))]
macro_rules! debug_assert {
    ($($arg:tt)*) => {};
}

/// Asserts that two expressions are equal (equivalent to `assert_eq!`).
///
/// If the expressions are not equal, this macro delegates to the backend macro
/// `assert_binary_backend!`.
///
/// Note that depending on the backend, both expressions may need to be cast
/// expressions (e.g., `x as i32`).
///
/// # Examples
///
/// ```no_run
/// let x = 5;
/// pw_assert::eq!(x as i32, 5 as i32);
/// pw_assert::eq!(x as i32, 5 as i32, "x should be 5");
/// ```
#[macro_export]
macro_rules! eq {
    ($lhs:expr, $rhs:expr $(,)?) => {{
        $crate::__private::assert_binary_backend!($lhs, ==, $rhs);
    }};

    ($lhs:expr, $rhs:expr, $($args:expr),* $(,)?) => {{
        $crate::__private::assert_binary_backend!($lhs, ==, $rhs, $($args),*);
    }};
}

/// Asserts that two expressions are not equal (equivalent to `assert_ne!`).
///
/// If the expressions are equal, this macro delegates to the backend macro
/// `assert_binary_backend!`.
///
/// Note that depending on the backend, both expressions may need to be cast
/// expressions (e.g., `x as i32`).
///
/// # Examples
///
/// ```no_run
/// let x = 5;
/// pw_assert::ne!(x as i32, 6 as i32);
/// pw_assert::ne!(x as i32, 6 as i32, "x should not be 6");
/// ```
#[macro_export]
macro_rules! ne {
    ($lhs:expr, $rhs:expr $(,)?) => {{
        $crate::__private::assert_binary_backend!($lhs, !=, $rhs);
    }};

    ($lhs:expr, $rhs:expr, $($args:expr),* $(,)?) => {{
        $crate::__private::assert_binary_backend!($lhs, !=, $rhs, $($args),*);
    }};
}

/// Asserts that two expressions are equal when debug_assertions are enabled.
///
/// If `debug_assertions` are enabled, this behaves exactly like [`eq!`].
/// If `debug_assertions` are disabled, this macro is a no-op.
///
/// `debug_assertions` can be enabled by setting this bazel label to `True`
/// `@pigweed//pw_assert/rust:debug_assertions`.
///
/// # Examples
///
/// ```no_run
/// let x = 5;
/// pw_assert::debug_eq!(x as i32, 5 as i32);
/// ```
#[macro_export]
#[cfg(feature = "debug_assertions")]
macro_rules! debug_eq {
    ($lhs:expr, $rhs:expr $(,)?) => {{
        $crate::__private::assert_binary_backend!($lhs, ==, $rhs);
    }};

    ($lhs:expr, $rhs:expr, $($args:expr),* $(,)?) => {{
        $crate::__private::assert_binary_backend!($lhs, ==, $rhs, $($args),*);
    }};
}

/// Asserts that two expressions are equal when debug_assertions are enabled.
#[macro_export]
#[cfg(not(feature = "debug_assertions"))]
macro_rules! debug_eq {
    ($($arg:tt)*) => {};
}

/// Asserts that two expressions are not equal when debug_assertions are enabled.
///
/// If `debug_assertions` are enabled, this behaves exactly like [`ne!`].
/// If `debug_assertions` are disabled, this macro is a no-op.
///
/// `debug_assertions` can be enabled by setting this bazel label to `True`
/// `@pigweed//pw_assert/rust:debug_assertions`.
///
/// # Examples
///
/// ```no_run
/// let x = 5;
/// pw_assert::debug_ne!(x as i32, 6 as i32);
/// ```
#[macro_export]
#[cfg(feature = "debug_assertions")]
macro_rules! debug_ne {
    ($lhs:expr, $rhs:expr $(,)?) => {{
        $crate::__private::assert_binary_backend!($lhs, !=, $rhs);
    }};

    ($lhs:expr, $rhs:expr, $($args:expr),* $(,)?) => {{
        $crate::__private::assert_binary_backend!($lhs, !=, $rhs, $($args),*);
    }};
}

/// Asserts that two expressions are not equal when debug_assertions are enabled.
#[macro_export]
#[cfg(not(feature = "debug_assertions"))]
macro_rules! debug_ne {
    ($($arg:tt)*) => {};
}
