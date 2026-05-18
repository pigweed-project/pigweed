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

#[doc(hidden)]
pub mod __private {

    pub use colors::log_level_tag;
    pub use pw_log_backend_basic_macro::{_pw_log_backend, _pw_logf_backend};
    use syscall_user::SysCallInterface;

    pub struct SysCallWriter;

    impl core::fmt::Write for SysCallWriter {
        fn write_str(&mut self, s: &str) -> core::fmt::Result {
            // Use the direct syscall interface to avoid a circular dependency in the
            // `userspace` crate.
            unsafe {
                syscall_user::SysCall::debug_log(s.as_ptr(), s.len()).map_err(|_| core::fmt::Error)
            }
        }
    }
}

// Implement the `pw_log` backend API.
#[macro_export]
macro_rules! pw_log_backend {
  ($log_level:expr, $format_string:literal $(, $args:expr)* $(,)?) => {{
    use $crate::__private as __pw_log_backend_crate;
    $crate::__private::_pw_log_backend!(
      &mut __pw_log_backend_crate::SysCallWriter,
      $log_level,
      $format_string,
      $($args),*);
  }};
}

#[macro_export]
macro_rules! pw_logf_backend {
  ($log_level:expr, $format_string:literal $(, $args:expr)* $(,)?) => {{
    use $crate::__private as __pw_log_backend_crate;
    $crate::__private::_pw_logf_backend!(
      &mut __pw_log_backend_crate::SysCallWriter,
      $log_level,
      $format_string,
      $($args),*);
  }};
}
