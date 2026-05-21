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

//! `pw_log` backend that calls [`std::println!`] to emit log messages.  This
//! module is useful when you have an application or test running on the
//! development host and you want messages to be emitted to `stdout`.
//!
//! *Note*: This modules requires `std`.
//!
//! TODO: <pwbug.dev/311232605> - Document how to configure facade backends.

#[doc(hidden)]
pub mod __private {
    use pw_log_backend_api::LogLevel;
    pub use pw_log_backend_println_macro::{_pw_log_backend, _pw_logf_backend};

    pub const fn log_level_tag(level: LogLevel) -> &'static str {
        match level {
            LogLevel::Debug => "DBG",
            LogLevel::Info => "INF",
            LogLevel::Warn => "WRN",
            LogLevel::Error => "ERR",
            LogLevel::Critical => "CRT",
            LogLevel::Fatal => "FTL",
        }
    }
}

// Implement the `pw_log` backend API.
#[macro_export]
macro_rules! pw_log_backend {
  ($log_level:expr, $($format_string:literal)PW_FMT_CONCAT+ $(, $args:expr)* $(,)?) => {{
    use $crate::__private as __pw_log_backend_crate;
    $crate::__private::_pw_log_backend!($log_level, $($format_string)PW_FMT_CONCAT+, $($args),*);
  }};

  ($log_level:expr, $format_string:literal $(, $args:expr)* $(,)?) => {{
    use $crate::__private as __pw_log_backend_crate;
    $crate::__private::_pw_log_backend!($log_level, $format_string, $($args),*);
  }};
}

#[macro_export]
macro_rules! pw_logf_backend {
  ($log_level:expr, $format_string:literal $(, $args:expr)* $(,)?) => {{
    use $crate::__private as __pw_log_backend_crate;
    $crate::__private::_pw_logf_backend!($log_level, $format_string, $($args),*);
  }};
}
