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

//! `pw_log` backend that calls `libc`'s `printf` to emit log messages.  This
//! module is useful when you have a mixed C/C++ and Rust code base and want a
//! simple logging system that leverages an existing `printf` implementation.
//!
//! *Note*: This uses FFI to call `printf`.  This has two implications:
//! 1. C/C++ macro processing is not done.  If a system's `printf` relies on
//!    macros, this backend will likely need to be forked to make work.
//! 2. FFI calls use `unsafe`.  Attempts are made to use `printf` in sound ways
//!    such as bounding the length of strings explicitly but this is still an
//!    off-ramp from Rust's safety guarantees.
//!
//! Varargs marshaling for call to printf is handled through a type expansion
//! of a series of traits.  It is documented in the [`varargs`] module.
//!
//! TODO: <pwbug.dev/311232605> - Document how to configure facade backends.
#![deny(missing_docs)]

pub mod varargs;

// Re-export dependences of backend proc macro to be accessed via `$crate::__private`.
#[doc(hidden)]
pub mod __private {
    use core::ffi::{c_int, c_uchar};

    pub use pw_bytes::concat_static_strs;
    pub use pw_format_core::{PrintfHexFormatter, PrintfUpperHexFormatter};
    use pw_log_backend_api::LogLevel;
    pub use pw_log_backend_printf_macro::{_pw_log_backend, _pw_logf_backend};

    pub use crate::varargs::{Arguments, VarArgs};

    #[must_use]
    pub const fn log_level_tag(level: LogLevel) -> &'static str {
        match level {
            LogLevel::Debug => "DBG\0",
            LogLevel::Info => "INF\0",
            LogLevel::Warn => "WRN\0",
            LogLevel::Error => "ERR\0",
            LogLevel::Critical => "CRT\0",
            LogLevel::Fatal => "FTL\0",
        }
    }

    macro_rules! extend_args {
      ($head:ty; $next:ty $(,$rest:ty)* $(,)?) => {
          extend_args!(<$head as VarArgs>::OneMore<$next>; $($rest,)*)
      };
      ($head:ty;) => {
          $head
      };
    }

    /// The printf uses its own formatter trait because it needs strings to
    /// resolve to `%.*s` instead of `%s`.
    ///
    /// The default [`PrintfHexFormatter`] and [`PrintfUpperHexFormatter`] are
    /// used since they are not supported by strings.
    pub trait PrintfFormatter {
        /// The format specifier for this type.
        const FORMAT_ARG: &'static str;
    }

    /// A helper to declare a [`PrintfFormatter`] trait for a given type.
    macro_rules! declare_formatter {
        ($ty:ty, $specifier:literal) => {
            impl PrintfFormatter for $ty {
                const FORMAT_ARG: &'static str = $specifier;
            }
        };
    }

    declare_formatter!(i32, "d");
    declare_formatter!(u32, "u");
    declare_formatter!(&str, ".*s");

    /// A helper to declare an [`Argument<T>`] trait for a given type.
    ///
    /// Useful for cases where `Argument::push_args()` appends a single
    /// argument of type `T`.
    macro_rules! declare_simple_argument {
        ($ty:ty) => {
            impl Arguments<$ty> for $ty {
                type PushArg<Head: VarArgs> = Head::OneMore<$ty>;
                fn push_arg<Head: VarArgs>(head: Head, arg: &$ty) -> Self::PushArg<Head> {
                    // Try expanding `CHECK` which should fail if we've exceeded
                    // 12 arguments in our args tuple.
                    let _ = Self::PushArg::<Head>::CHECK;
                    head.append(*arg)
                }
            }
        };
    }

    declare_simple_argument!(u8);
    declare_simple_argument!(i32);
    declare_simple_argument!(u32);
    declare_simple_argument!(char);
    declare_simple_argument!(i64);
    declare_simple_argument!(u64);
    declare_simple_argument!(usize);

    // &str needs a more complex implementation of [`Argument<T>`] since it needs
    // to append two arguments.
    impl Arguments<&str> for &str {
        type PushArg<Head: VarArgs> = extend_args!(Head; c_int, *const c_uchar);
        fn push_arg<Head: VarArgs>(head: Head, arg: &&str) -> Self::PushArg<Head> {
            // Try expanding `CHECK` which should fail if we've exceeded 12
            // arguments in our args tuple.
            #[allow(clippy::let_unit_value)]
            let _ = Self::PushArg::<Head>::CHECK;
            let arg = *arg;
            head.append(arg.len() as c_int).append(arg.as_ptr().cast())
        }
    }
}

/// Implements the `pw_log` backend api.
#[macro_export]
macro_rules! pw_log_backend {
    ($log_level:expr, $($format_string:literal)PW_FMT_CONCAT+ $(, $args:expr)* $(,)?) => {{
        use $crate::__private as __pw_log_backend_crate;
        $crate::__private::_pw_log_backend!($log_level, $($format_string)PW_FMT_CONCAT+, $($args),*)
    }};
}

/// Implements the `pw_log` backend api.
#[macro_export]
macro_rules! pw_logf_backend {
    ($log_level:expr, $format_string:literal $(, $args:expr)* $(,)?) => {{
        use $crate::__private as __pw_log_backend_crate;
        $crate::__private::_pw_logf_backend!($log_level, $format_string,  $($args),*)
    }};
}

#[cfg(test)]
mod tests {
    use core::ffi::c_int;

    use super::__private::*;

    #[test]
    fn pushed_args_produce_correct_tuple() {
        let string = "test";
        let args = ();
        let args = <&str as Arguments<&str>>::push_arg(args, &(string as &str));
        let args = <u32 as Arguments<u32>>::push_arg(args, &2u32);
        assert_eq!(args, (string.len() as c_int, string.as_ptr().cast(), 2u32));
    }
}
