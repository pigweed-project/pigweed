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

//! # pw_assert_backend_pw_log
//!
//! `pw_assert_backend_pw_log` provides an implementation of `pw_assert` backend
//! macros that route to `pw_log` and [`pw_assert_HandleFailure`].

#[cfg(not(feature = "default_handler"))]
unsafe extern "C" {
    /// The crash handler called by asserts and panics when they fail.
    ///
    /// Since the `default_handler` feature is disabled, the application must
    /// provide an implementation of this function with the `#[no_mangle]` attribute.
    ///
    /// This function must not return.
    pub fn pw_assert_HandleFailure() -> !;
}

#[cfg(feature = "default_handler")]
#[allow(non_snake_case)]
/// Default implementation of the crash handler.
///
/// This implementation simply delegates to `core::panic!`.
///
/// # Safety
///
/// The default_handler panic handler is safe, but the unsafe keyword is
/// required to match the non-default panic handler signature.
pub unsafe extern "C-unwind" fn pw_assert_HandleFailure() -> ! {
    core::panic!("pw_assert panic")
}

// Re-export pw_log for use by panic/assert macros.
#[doc(hidden)]
pub mod __private {
    pub use pw_log::fatal;
}

#[cfg(feature = "color")]
#[macro_export]
macro_rules! __private_log_panic_banner {
    () => {
        // Colorized using https://glitchassassin.github.io/fk-ascii-editor/
        // and run through the following shell command to translate outputs:
        //   sed 's/{70}/\\x1b[0m/'g |
        //   sed 's/{B0}/\\x1b[1;33m/'g |
        //   sed 's/{90}/\\x1b[1;31m/'g |
        //   sed 's/{E0}/\\x1b[1;36m/'g |
        //   sed 's/{C0}/\\x1b[1;34m/'g |
        //   sed 's/{A0}/\\x1b[1;32m/'g |
        //   sed 's/{F0}/\\x1b[1;37m/'g |
        //   sed 's/{D0}/\\x1b[1;35m/'g
        $crate::__private::fatal!("

\x1b[0m.-------.    ____    ,---.   .--.\x1b[1;31m.-./`)\x1b[0m     _______
\x1b[0m\\  \x1b[1;33m_(`)_\x1b[0m \\ .'  __ `. |    \\  |  |\x1b[1;31m\\\x1b[0m \x1b[1;36m.-.\x1b[1;31m')\x1b[0m   /   __  \\
\x1b[0m| \x1b[1;33m(_\x1b[0m \x1b[1;32mo\x1b[1;33m._)\x1b[0m|/   '  \\  \\|  ,  \\ |  |\x1b[1;31m/\x1b[0m \x1b[1;36m`-'\x1b[0m \x1b[1;31m\\\x1b[0m  | \x1b[1;36m,_\x1b[0m/  \\__)
\x1b[0m|  \x1b[1;33m(_,_)\x1b[0m /|___|  /  ||  |\\\x1b[1;34m_\x1b[0m \\|  | \x1b[1;31m`-'`\"`\x1b[1;36m,-./  )
\x1b[0m|   '-.-'    _.-`   ||  \x1b[1;34m_( )_\x1b[0m\\  | .---. \x1b[1;36m\\  '\x1b[1;35m_\x1b[0m \x1b[1;36m'`)
\x1b[0m|   |     .'   \x1b[1;32m_\x1b[0m    || \x1b[1;34m(_\x1b[0m \x1b[1;35mo\x1b[0m \x1b[1;34m_)\x1b[0m  | |   |  \x1b[1;36m>\x1b[0m \x1b[1;35m(_)\x1b[0m  \x1b[1;36m)\x1b[0m  __
\x1b[0m|   |     |  \x1b[1;32m_( )_\x1b[0m  ||  \x1b[1;34m(_,_)\x1b[0m\\  | |   | \x1b[1;36m(  .  .-'\x1b[0m_/  )
\x1b[0m/   )     \\ \x1b[1;32m(_\x1b[0m \x1b[1;31mo\x1b[0m \x1b[1;32m_)\x1b[0m /|  |    |  | |   |  \x1b[1;36m`-'`-'\x1b[0m     /
\x1b[0m`---'      '.\x1b[1;32m(_,_)\x1b[0m.' '--'    '--' '---'    `._____.'
\x1b[0m
")
    };
}

#[cfg(not(feature = "color"))]
#[macro_export]
macro_rules! __private_log_panic_banner {
    () => {
        $crate::__private::fatal!(
            r#"

.-------.    ____    ,---.   .--..-./`)     _______
\  _(`)_ \ .'  __ `. |    \  |  |\ .-.')   /   __  \
| (_ o._)|/   '  \  \|  ,  \ |  |/ `-' \  | ,_/  \__)
|  (_,_) /|___|  /  ||  |\_ \|  | `-'`"`,-./  )
|   '-.-'    _.-`   ||  _( )_\  | .---. \  '_ '`)
|   |     .'   _    || (_ o _)  | |   |  > (_)  )  __
|   |     |  _( )_  ||  (_,_)\  | |   | (  .  .-'_/  )
/   )     \ (_ o _) /|  |    |  | |   |  `-'`-'     /
`---'      '.(_,_).' '--'    '--' '---'    `._____.'
"#
        )
    };
}

/// Backend implementation of [`pw_assert::panic`].
///
/// This macro logs a panic banner and the formatted message at `FATAL` level
/// using `pw_log`, and then calls the crash handler [`pw_assert_HandleFailure`].
#[macro_export]
macro_rules! panic_backend {
    ($format_string:literal $(,)?) => {{
        // Ideally we'd combine these two log statements.  However, the `pw_log` API
        // does not support passing through `PW_FMT_CONCAT` tokens to `pw_format`.
        $crate::__private_log_panic_banner!();
        $crate::__private::fatal!($format_string);
        unsafe{$crate::pw_assert_HandleFailure()}
    }};

    ($format_string:literal, $($args:expr),* $(,)?) => {{
        // Ideally we'd combine these two log statements.  However, the `pw_log` API
        // does not support passing through `PW_FMT_CONCAT` tokens to `pw_format`.
        $crate::__private_log_panic_banner!();
        $crate::__private::fatal!($format_string, $($args),*);
        unsafe{$crate::pw_assert_HandleFailure()}
    }};
}

/// Backend implementation of unary assertions (invoked by [`pw_assert::assert`]
/// and [`pw_assert::debug_assert`]).
///
/// This macro logs a panic banner, logs the failure (including condition, file
/// and line number and optional custom message) at `FATAL` level using `pw_log`,
/// and then calls [`pw_assert_HandleFailure`].
#[macro_export]
macro_rules! assert_unary_backend {
    ($condition:expr $(,)?) => {{
        #[allow(clippy::unnecessary_cast)]
        if !$condition {
            // Ideally we'd combine these two log statements.  However, the `pw_log` API
            // does not support passing through `PW_FMT_CONCAT` tokens to `pw_format`.
            $crate::__private_log_panic_banner!();
            $crate::__private::fatal!(
                "assertion failed: {} @ {}:{}",
                stringify!($condition) as &str,
                file!() as &str,
                line!() as u32
            );
            unsafe{$crate::pw_assert_HandleFailure()}
        }
    }};

    ($condition:expr, $($args:expr),* $(,)?) => {{
        #[allow(clippy::unnecessary_cast)]
        if !$condition {
            // Ideally we'd combine these two log statements.  However, the `pw_log` API
            // does not support passing through `PW_FMT_CONCAT` tokens to `pw_format`.
            $crate::__private_log_panic_banner!();
            $crate::__private::fatal!(
                "assertion failed: {} @ {}:{}",
                stringify!($condition) as &str,
                file!() as &str,
                line!() as u32
            );
            $crate::__private::fatal!($($args),*);
            unsafe{$crate::pw_assert_HandleFailure()}
        }
    }};
}

/// Backend implementation of binary assertions (invoked by [`pw_assert::eq`],
/// [`pw_assert::ne`], [`pw_assert::debug_eq`], and [`pw_assert::debug_ne`]).
///
/// This macro logs a panic banner, logs the failure (including values of both
/// operands, operation, file and line number, and optional custom message) at
/// `FATAL` level using `pw_log`, and then calls [`pw_assert_HandleFailure`].
#[macro_export]
macro_rules! assert_binary_backend {
    ($lhs:expr, $op:tt, $rhs:expr $(,)?) => {{
        #[allow(clippy::unnecessary_cast)]
        // This match statement ensures that each expression is only evaluated once.
        match (&$lhs, &$rhs) {
            (lhs, rhs) => {
                if !(*lhs $op *rhs) {
                    $crate::__private_log_panic_banner!();
                    $crate::__private::fatal!(
                        "assertion failed: {} {} {} @ {}:{}",
                        $lhs,
                        stringify!($op) as &str,
                        $rhs,
                        file!() as &str,
                        line!() as u32
                    );
                    unsafe{$crate::pw_assert_HandleFailure()}
                }
            }
        }
    }};

    ($lhs:expr, $op:tt, $rhs:expr, $($args:expr),* $(,)?) => {{
        #[allow(clippy::unnecessary_cast)]
        // This match statement ensures that each expression is only evaluated once.
        match (&$lhs, &$rhs) {
            (lhs, rhs) => {
                if !(*lhs $op *rhs) {
                    $crate::__private_log_panic_banner!();
                    // Ideally we'd combine these two log statements.  However, the `pw_log` API
                    // does not support passing through `PW_FMT_CONCAT` tokens to `pw_format`.
                    $crate::__private::fatal!(
                        "assertion failed: {} {} {} @ {}:{}",
                        $lhs,
                        stringify!($op) as &str,
                        $rhs,
                        file!() as &str,
                        line!() as u32
                    );
                    $crate::__private::fatal!($($args),*);
                    unsafe{$crate::pw_assert_HandleFailure()}
                }
            }
        }
    }};
}
