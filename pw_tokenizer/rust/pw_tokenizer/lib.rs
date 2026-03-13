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

//! `pw_tokenizer` - Efficient string handling and printf style encoding.
//!
//! Logging is critical, but developers are often forced to choose between
//! additional logging or saving crucial flash space. The `pw_tokenizer` crate
//! helps address this by replacing printf-style strings with binary tokens
//! during compilation. This enables extensive logging with substantially less
//! memory usage.
//!
//! For a more in depth explanation of the systems design and motivations,
//! see [Pigweed's pw_tokenizer module documentation](https://pigweed.dev/pw_tokenizer/).
//!
//! # Examples
//!
//! Pigweed's tokenization database uses `printf` style strings internally so
//! those are supported directly.
//!
//! ```
//! use pw_tokenizer::tokenize_printf_to_buffer;
//!
//! let mut buffer = [0u8; 1024];
//! let len = tokenize_printf_to_buffer!(&mut buffer, "The answer is %d", 42)?;
//!
//! // 4 bytes used to encode the token and one to encode the value 42.  This
//! // is a **3.5x** reduction in size compared to the raw string!
//! assert_eq!(len, 5);
//! # Ok::<(), pw_status::Error>(())
//! ```
//!
//! We also support Rust's `core::fmt` style syntax.  These format strings are
//! converted to `printf` style at compile time to maintain compatibly with the
//! rest of the Pigweed tokenizer ecosystem.  The below example produces the
//! same token and output as the above one.
//!
//! ```
//! use pw_tokenizer::tokenize_core_fmt_to_buffer;
//!
//! let mut buffer = [0u8; 1024];
//! let len = tokenize_core_fmt_to_buffer!(&mut buffer, "The answer is {}", 42 as i32)?;
//! assert_eq!(len, 5);
//! # Ok::<(), pw_status::Error>(())
//! ```
#![cfg_attr(not(feature = "std"), no_std)]
#![deny(missing_docs)]

use pw_status::Result;

#[doc(hidden)]
pub mod internal;

#[doc(hidden)]
// Creating a __private namespace allows us a way to get to the modules
// we need from macros by doing:
//     use $crate::__private as __pw_tokenizer_crate;
//
// This is how proc macro generated code can reliably reference back to
// `pw_tokenizer` while still allowing a user to import it under a different
// name.
pub mod __private {
    pub use pw_bytes::concat_static_strs;
    pub use pw_format_core::{PrintfFormatter, PrintfHexFormatter, PrintfUpperHexFormatter};
    pub use pw_status::Result;
    pub use pw_stream::{Cursor, Seek, WriteInteger, WriteVarint};
    pub use pw_tokenizer_core::hash_string;
    pub use pw_tokenizer_macro::{
        _token, _tokenize_core_fmt_to_buffer, _tokenize_core_fmt_to_writer,
        _tokenize_printf_to_buffer, _tokenize_printf_to_writer,
    };

    pub use crate::*;
}

/// Return the [`u32`] token for the specified string and add it to the token
/// database.
///
/// This is where the magic happens in `pw_tokenizer`!   ... and by magic
/// we mean hiding information in a special linker section that ends up in the
/// final elf binary but does not get flashed to the device.
///
/// Two things are accomplished here:
/// 1) The string is hashed into its stable `u32` token.  This is the value that
///    is returned from the macro.
/// 2) A [token database entry](https://pigweed.dev/pw_tokenizer/design.html#binary-database-format)
///    is generated, assigned to a unique static symbol, placed in a linker
///    section named `pw_tokenizer.entries.<TOKEN_HASH>`.  A
///    [linker script](https://pigweed.googlesource.com/pigweed/pigweed/+/refs/heads/main/pw_tokenizer/pw_tokenizer_linker_sections.ld)
///    is responsible for picking these symbols up and aggregating them into a
///    single `.pw_tokenizer.entries` section in the final binary.
///
/// # Example
/// ```
/// use pw_tokenizer::token;
///
/// let token = token!("hello, \"world\"");
/// assert_eq!(token, 3537412730);
/// ```
///
/// Currently there is no support for encoding tokens to specific domains
/// or with "fixed lengths" per [`pw_tokenizer_core::hash_bytes_fixed`].
#[macro_export]
macro_rules! token {
    ($string:literal) => {{
        use $crate::__private as __pw_tokenizer_crate;
        $crate::__private::_token!($string)
    }};
}

/// Tokenize a `core::fmt` style format string and arguments to an [`AsMut<u8>`]
/// buffer.  The format string is converted in to a `printf` and added token to
/// the token database.
///
/// See [`token`] for an explanation on how strings are tokenized and entries
/// are added to the token database.  The token's domain is set to `""`.
///
/// Returns a [`pw_status::Result<usize>`] the number of bytes written to the buffer.
///
/// `tokenize_to_buffer!` supports concatenation of format strings as described
/// in [`pw_format::macros::FormatAndArgs`].
///
/// # Errors
/// - [`pw_status::Error::OutOfRange`] - Buffer is not large enough to fit
///   tokenized data.
/// - [`pw_status::Error::InvalidArgument`] - Invalid buffer was provided.
///
/// # Example
///
/// ```
/// use pw_tokenizer::tokenize_core_fmt_to_buffer;
///
/// // Tokenize a format string and argument into a buffer.
/// let mut buffer = [0u8; 1024];
/// let len = tokenize_core_fmt_to_buffer!(&mut buffer, "The answer is {}", 42 as i32)?;
///
/// // 4 bytes used to encode the token and one to encode the value 42.
/// assert_eq!(len, 5);
///
/// // The format string can be composed of multiple strings literals using
/// // the custom`PW_FMT_CONCAT` operator.
/// let len = tokenize_core_fmt_to_buffer!(&mut buffer, "Hello " PW_FMT_CONCAT "Pigweed")?;
///
/// // Only a single 4 byte token is emitted after concatenation of the string
/// // literals above.
/// assert_eq!(len, 4);
/// # Ok::<(), pw_status::Error>(())
/// ```
#[macro_export]
macro_rules! tokenize_core_fmt_to_buffer {
    ($buffer:expr, $($format_string:literal)PW_FMT_CONCAT+ $(, $args:expr)* $(,)?) => {{
      use $crate::__private as __pw_tokenizer_crate;
      __pw_tokenizer_crate::_tokenize_core_fmt_to_buffer!($buffer, $($format_string)PW_FMT_CONCAT+, $($args),*)
    }};
}

/// Tokenize a printf format string and arguments to an [`AsMut<u8>`] buffer
/// and add the format string's token to the token database.
///
/// See [`token`] for an explanation on how strings are tokenized and entries
/// are added to the token database.  The token's domain is set to `""`.
///
/// Returns a [`pw_status::Result<usize>`] the number of bytes written to the buffer.
///
/// `tokenize_to_buffer!` supports concatenation of format strings as described
/// in [`pw_format::macros::FormatAndArgs`].
///
/// # Errors
/// - [`pw_status::Error::OutOfRange`] - Buffer is not large enough to fit
///   tokenized data.
/// - [`pw_status::Error::InvalidArgument`] - Invalid buffer was provided.
///
/// # Example
///
/// ```
/// use pw_tokenizer::tokenize_printf_to_buffer;
///
/// // Tokenize a format string and argument into a buffer.
/// let mut buffer = [0u8; 1024];
/// let len = tokenize_printf_to_buffer!(&mut buffer, "The answer is %d", 42)?;
///
/// // 4 bytes used to encode the token and one to encode the value 42.
/// assert_eq!(len, 5);
///
/// // The format string can be composed of multiple strings literals using
/// // the custom`PW_FMT_CONCAT` operator.
/// let len = tokenize_printf_to_buffer!(&mut buffer, "Hello " PW_FMT_CONCAT "Pigweed")?;
///
/// // Only a single 4 byte token is emitted after concatenation of the string
/// // literals above.
/// assert_eq!(len, 4);
/// # Ok::<(), pw_status::Error>(())
/// ```
#[macro_export]
macro_rules! tokenize_printf_to_buffer {
    ($buffer:expr, $($format_string:literal)PW_FMT_CONCAT+ $(, $args:expr)* $(,)?) => {{
      use $crate::__private as __pw_tokenizer_crate;
      __pw_tokenizer_crate::_tokenize_printf_to_buffer!($buffer, $($format_string)PW_FMT_CONCAT+, $($args),*)
    }};
}

/// Deprecated alias for [`tokenize_printf_to_buffer!`].
#[macro_export]
macro_rules! tokenize_to_buffer {
    ($buffer:expr, $($format_string:literal)PW_FMT_CONCAT+ $(, $args:expr)* $(,)?) => {{
      $crate::tokenize_printf_to_buffer!($buffer, $($format_string)PW_FMT_CONCAT+, $($args),*)
    }};
}

/// Tokenize a `core::fmt` format string and arguments to a [`MessageWriter`].
/// The format string is converted in to a `printf` and added token to the token
/// database.
///
/// `tokenize_core_fmt_to_writer!` and the accompanying [`MessageWriter`] trait
/// provide an optimized API for use cases like logging where the output of the
/// tokenization will be written to a shared/ambient resource like stdio, a
/// UART, or a shared buffer.
///
/// The `writer_type` should implement [`MessageWriter`] and [`Default`] traits.
/// The writer is instantiated with the [`Default`] allowing any intermediate
/// buffers to be declared on the stack of the internal writing engine instead
/// of the caller's stack.
///
/// See [`token`] for an explanation on how strings are tokenized and entries
/// are added to the token database.  The token's domain is set to `""`.
///
/// Returns a [`pw_status::Result<()>`].
///
/// `tokenize_core_fmt_to_writer!` supports concatenation of format strings as
///  described in [`pw_format::macros::FormatAndArgs`].
///
/// # Errors
/// - [`pw_status::Error::OutOfRange`] - [`MessageWriter`] does not have enough
///   space to fit tokenized data.
/// - others - `tokenize_core_fmt_to_writer!` will pass on any errors returned
///   by the [`MessageWriter`].
///
/// # Code Size
///
/// This data was collected by examining the disassembly of a test program
/// built for a Cortex M0.
///
/// | Tokenized Message   | Per Call-site Cost (bytes) |
/// | --------------------| -------------------------- |
/// | no arguments        | 10                         |
/// | one `i32` argument  | 18                         |
///
/// # Example
///
/// ```
/// use pw_status::Result;
/// use pw_stream::{Cursor, Write};
/// use pw_tokenizer::{MessageWriter, tokenize_core_fmt_to_writer};
///
/// const BUFFER_LEN: usize = 32;
///
/// // Declare a simple MessageWriter that uses a [`pw_status::Cursor`] to
/// // maintain an internal buffer.
/// struct TestMessageWriter {
///   cursor: Cursor<[u8; BUFFER_LEN]>,
/// }
///
/// impl Default for TestMessageWriter {
///   fn default() -> Self {
///       Self {
///           cursor: Cursor::new([0u8; BUFFER_LEN]),
///       }
///   }
/// }
///
/// impl MessageWriter for TestMessageWriter {
///   fn write(&mut self, data: &[u8]) -> Result<()> {
///       self.cursor.write_all(data)
///   }
///
///   fn remaining(&self) -> usize {
///       self.cursor.remaining()
///   }
///
///   fn finalize(self) -> Result<()> {
///       let len = self.cursor.position();
///       // 4 bytes used to encode the token and one to encode the value 42.
///       assert_eq!(len, 5);
///       Ok(())
///   }
/// }
///
/// // Tokenize a format string and argument into the writer.  Note how we
/// // pass in the message writer's type, not an instance of it.
/// let len = tokenize_core_fmt_to_writer!(TestMessageWriter, "The answer is {}", 42 as i32)?;
/// # Ok::<(), pw_status::Error>(())
/// ```
#[macro_export]
macro_rules! tokenize_core_fmt_to_writer {
    ($writer:expr, $($format_string:literal)PW_FMT_CONCAT+ $(, $args:expr)* $(,)?) => {{
      use $crate::__private as __pw_tokenizer_crate;
      __pw_tokenizer_crate::_tokenize_core_fmt_to_writer!($writer, $($format_string)PW_FMT_CONCAT+, $($args),*)
    }};
}

/// Tokenize a `printf` format string and arguments to a [`MessageWriter`] and
/// add the format string's token to the token database.
///
/// `tokenize_printf_fmt_to_writer!` and the accompanying [`MessageWriter`] trait
/// provide an optimized API for use cases like logging where the output of the
/// tokenization will be written to a shared/ambient resource like stdio, a
/// UART, or a shared buffer.
///
/// The `writer_type` should implement [`MessageWriter`] and [`Default`] traits.
/// The writer is instantiated with the [`Default`] allowing any intermediate
/// buffers to be declared on the stack of the internal writing engine instead
/// of the caller's stack.
///
/// See [`token`] for an explanation on how strings are tokenized and entries
/// are added to the token database.  The token's domain is set to `""`.
///
/// Returns a [`pw_status::Result<()>`].
///
/// `tokenize_core_fmt_to_writer!` supports concatenation of format strings as
///  described in [`pw_format::macros::FormatAndArgs`].
///
/// # Errors
/// - [`pw_status::Error::OutOfRange`] - [`MessageWriter`] does not have enough
///   space to fit tokenized data.
/// - others - `tokenize_printf_to_writer!` will pass on any errors returned
///   by the [`MessageWriter`].
///
/// # Code Size
///
/// This data was collected by examining the disassembly of a test program
/// built for a Cortex M0.
///
/// | Tokenized Message   | Per Call-site Cost (bytes) |
/// | --------------------| -------------------------- |
/// | no arguments        | 10                         |
/// | one `i32` argument  | 18                         |
///
/// # Example
///
/// ```
/// use pw_status::Result;
/// use pw_stream::{Cursor, Write};
/// use pw_tokenizer::{MessageWriter, tokenize_printf_to_writer};
///
/// const BUFFER_LEN: usize = 32;
///
/// // Declare a simple MessageWriter that uses a [`pw_status::Cursor`] to
/// // maintain an internal buffer.
/// struct TestMessageWriter {
///   cursor: Cursor<[u8; BUFFER_LEN]>,
/// }
///
/// impl Default for TestMessageWriter {
///   fn default() -> Self {
///       Self {
///           cursor: Cursor::new([0u8; BUFFER_LEN]),
///       }
///   }
/// }
///
/// impl MessageWriter for TestMessageWriter {
///   fn write(&mut self, data: &[u8]) -> Result<()> {
///       self.cursor.write_all(data)
///   }
///
///   fn remaining(&self) -> usize {
///       self.cursor.remaining()
///   }
///
///   fn finalize(self) -> Result<()> {
///       let len = self.cursor.position();
///       // 4 bytes used to encode the token and one to encode the value 42.
///       assert_eq!(len, 5);
///       Ok(())
///   }
/// }
///
/// // Tokenize a format string and argument into the writer.  Note how we
/// // pass in the message writer's type, not an instance of it.
/// let len = tokenize_printf_to_writer!(TestMessageWriter, "The answer is %d", 42)?;
/// # Ok::<(), pw_status::Error>(())
/// ```
#[macro_export]
macro_rules! tokenize_printf_to_writer {
    ($writer:expr, $($format_string:literal)PW_FMT_CONCAT+ $(, $args:expr)* $(,)?) => {{
      use $crate::__private as __pw_tokenizer_crate;
      __pw_tokenizer_crate::_tokenize_printf_to_writer!($writer, $($format_string)PW_FMT_CONCAT+, $($args),*)
    }};
}

/// Deprecated alias for [`tokenize_printf_to_writer!`].
#[macro_export]
macro_rules! tokenize_to_writer {
  ($writer:expr, $($format_string:literal)PW_FMT_CONCAT+ $(, $args:expr)* $(,)?) => {{
    $crate::tokenize_printf_to_writer!($writer, $($format_string)PW_FMT_CONCAT+, $($args),*)
  }};
}

/// A trait used by [`tokenize_to_writer!`] to output tokenized messages.
///
/// For more details on how this type is used, see the [`tokenize_to_writer!`]
/// documentation.
pub trait MessageWriter {
    /// Append `data` to the message.
    fn write(&mut self, data: &[u8]) -> Result<()>;

    /// Return the remaining space in this message instance.
    ///
    /// If there are no space constraints, return `usize::MAX`.
    fn remaining(&self) -> usize;

    /// Finalize message.
    ///
    /// `finalize()` is called when the tokenized message is complete.
    fn finalize(self) -> Result<()>;
}

#[cfg(test)]
// Untyped prints code rely on as casts to annotate type information.
#[allow(clippy::unnecessary_cast)]
#[allow(clippy::literal_string_with_formatting_args)]
mod tests {
    use super::*;
    extern crate self as pw_tokenizer;
    use std::cell::RefCell;

    use pw_stream::{Cursor, Write};

    // This is not meant to be an exhaustive test of tokenization which is
    // covered by `pw_tokenizer_core`'s unit tests.  Rather, this is testing
    // that the `tokenize!` macro connects to that correctly.
    #[test]
    fn test_token() {}

    macro_rules! tokenize_to_buffer_test {
      ($expected_data:expr, $buffer_len:expr, $printf_fmt:literal, $core_fmt:literal $(, $args:expr)* $(,)?) => {{
        if $printf_fmt != "" {
          let mut buffer = [0u8; $buffer_len];
          let len = tokenize_printf_to_buffer!(&mut buffer, $printf_fmt, $($args),*).unwrap();
          assert_eq!(
              &buffer[..len],
              $expected_data,
              "printf style input does not produce expected output",
          );
        }
        if $core_fmt != "" {
           let mut buffer = [0u8; $buffer_len];
           let len = tokenize_core_fmt_to_buffer!(&mut buffer, $core_fmt, $($args),*).unwrap();
           assert_eq!(
               &buffer[..len],
               $expected_data,
              "core::fmt style input does not produce expected output",
           );
        }
      }}
    }

    macro_rules! tokenize_to_writer_test {
      ($expected_data:expr, $buffer_len:expr, $printf_fmt:literal, $core_fmt:literal $(, $args:expr)* $(,)?) => {{
        // The `MessageWriter` API is used in places like logging where it
        // accesses an shared/ambient resource (like stdio or an UART).  To test
        // it in a hermetic way we declare test specific `MessageWriter` that
        // writes it's output to a scoped static variable that can be checked
        // after the test is run.

        // Since these tests are not multi-threaded, we can use a thread_local!
        // instead of a mutex.
        thread_local!(static TEST_OUTPUT: RefCell<Option<Vec<u8>>> = RefCell::new(None));

        struct TestMessageWriter {
            cursor: Cursor<[u8; $buffer_len]>,
        }

        impl Default for TestMessageWriter {
          fn default() -> Self {
              Self {
                  cursor: Cursor::new([0u8; $buffer_len]),
              }
          }
        }

        impl MessageWriter for TestMessageWriter {
          fn write(&mut self, data: &[u8]) -> Result<()> {
              self.cursor.write_all(data)
          }

          fn remaining(&self) -> usize {
              self.cursor.remaining()
          }

          fn finalize(self) -> Result<()> {
              let write_len = self.cursor.position();
              let data = self.cursor.into_inner();
              TEST_OUTPUT.with(|output| *output.borrow_mut() = Some(data[..write_len].to_vec()));

              Ok(())
          }
        }

        if $printf_fmt != "" {
          TEST_OUTPUT.with(|output| *output.borrow_mut() = None);
          tokenize_printf_to_writer!(TestMessageWriter, $printf_fmt, $($args),*).unwrap();
          TEST_OUTPUT.with(|output| {
              assert_eq!(
                  *output.borrow(),
                  Some($expected_data.to_vec()),
              )
          });
        }

        if $core_fmt != "" {
          TEST_OUTPUT.with(|output| *output.borrow_mut() = None);
          tokenize_core_fmt_to_writer!(TestMessageWriter, $core_fmt, $($args),*).unwrap();
          TEST_OUTPUT.with(|output| {
              assert_eq!(
                  *output.borrow(),
                  Some($expected_data.to_vec()),
              )
          });
        }
      }}
    }

    macro_rules! tokenize_test {
        ($expected_data:expr, $buffer_len:expr, $printf_fmt:literal, $core_fmt:literal $(, $args:expr)* $(,)?) => {{
            tokenize_to_buffer_test!($expected_data, $buffer_len, $printf_fmt, $core_fmt, $($args),*);
            tokenize_to_writer_test!($expected_data, $buffer_len, $printf_fmt, $core_fmt, $($args),*);
        }};
    }

    #[test]
    fn bare_string_encodes_correctly() {
        tokenize_test!(
            &[0xe0, 0x92, 0xe0, 0xa], // expected buffer
            64,                       // buffer size
            "Hello Pigweed",          // printf style
            "Hello Pigweed",          // core::fmt style
        );
    }

    #[test]
    fn test_decimal_format() {
        // "as casts" are used for the integer arguments below.  They are only
        // need for the core::fmt style arguments but are added so that we can
        // check that the printf and core::fmt style equivalents encode the same.
        tokenize_test!(
            &[0x52, 0x1c, 0xb0, 0x4c, 0x2], // expected buffer
            64,                             // buffer size
            "The answer is %d!",            // printf style
            "The answer is {}!",            // core::fmt style
            1 as i32
        );

        tokenize_test!(
            &[0x36, 0xd0, 0xfb, 0x69, 0x1], // expected buffer
            64,                             // buffer size
            "No! The answer is %d!",        // printf style
            "No! The answer is {}!",        // core::fmt style
            -1 as i32
        );

        tokenize_test!(
            &[0xa4, 0xad, 0x50, 0x54, 0x0],               // expected buffer
            64,                                           // buffer size
            "I think you'll find that the answer is %d!", // printf style
            "I think you'll find that the answer is {}!", // core::fmt style
            0 as i32
        );
    }

    #[test]
    fn test_misc_integer_format() {
        // %d, %i, %o, %u, %x, %X all encode integers the same.
        tokenize_test!(
            &[0x52, 0x1c, 0xb0, 0x4c, 0x2], // expected buffer
            64,                             // buffer size
            "The answer is %d!",            // printf style
            "",                             // no equivalent core::fmt style
            1
        );

        // Because %i is an alias for %d, it gets converted to a %d by the
        // `pw_format` macro infrastructure.
        tokenize_test!(
            &[0x52, 0x1c, 0xb0, 0x4c, 0x2], // expected buffer
            64,                             // buffer size
            "The answer is %i!",            // printf style
            "",                             // no equivalent core::fmt style
            1
        );

        tokenize_test!(
            &[0x5d, 0x70, 0x12, 0xb4, 0x2], // expected buffer
            64,                             // buffer size
            "The answer is %o!",            // printf style
            "",                             // no equivalent core::fmt style
            1u32
        );

        tokenize_test!(
            &[0x63, 0x58, 0x5f, 0x8f, 0x2], // expected buffer
            64,                             // buffer size
            "The answer is %u!",            // printf style
            "",                             // no equivalent core::fmt style
            1u32
        );

        tokenize_test!(
            &[0x66, 0xcc, 0x05, 0x7d, 0x2], // expected buffer
            64,                             // buffer size
            "The answer is %x!",            // printf style
            "",                             // no equivalent core::fmt style
            1u32
        );

        tokenize_test!(
            &[0x46, 0x4c, 0x16, 0x96, 0x2], // expected buffer
            64,                             // buffer size
            "The answer is %X!",            // printf style
            "",                             // no equivalent core::fmt style
            1u32
        );
    }

    #[test]
    fn test_string_format() {
        tokenize_test!(
            b"\x25\xf6\x2e\x66\x07Pigweed", // expected buffer
            64,                             // buffer size
            "Hello: %s!",                   // printf style
            "",                             // no equivalent core::fmt style
            "Pigweed"
        );
    }

    #[test]
    fn test_string_format_overflow() {
        tokenize_test!(
            b"\x25\xf6\x2e\x66\x83Pig", // expected buffer
            8,                          // buffer size
            "Hello: %s!",               // printf style
            "",                         // no equivalent core::fmt style
            "Pigweed"
        );
    }

    #[test]
    fn test_char_format() {
        tokenize_test!(
            &[0x2e, 0x52, 0xac, 0xe4, 0xa0, 0x1], // expected buffer
            64,                                   // buffer size
            "Hello: %cigweed",                    // printf style
            "",                                   // no equivalent core::fmt style
            "P".as_bytes()[0]
        );
    }

    #[test]
    fn test_untyped_format() {
        tokenize_test!(
            &[0x63, 0x58, 0x5f, 0x8f, 0x2], // expected buffer
            64,                             // buffer size
            "The answer is %u!",            // printf style
            "The answer is {}!",            // core::fmt style
            1 as u32
        );

        tokenize_test!(
            &[0x36, 0xd0, 0xfb, 0x69, 0x1], // expected buffer
            64,                             // buffer size
            "No! The answer is %v!",        // printf style
            "No! The answer is {}!",        // core::fmt style
            -1 as i32
        );

        tokenize_test!(
            b"\x25\xf6\x2e\x66\x07Pigweed", // expected buffer
            64,                             // buffer size
            "Hello: %v!",                   // printf style
            "Hello: {}!",                   // core::fmt style
            "Pigweed" as &str
        );
    }

    #[test]
    fn test_field_width_and_zero_pad_format() {
        tokenize_test!(
            &[0x3a, 0xc2, 0x1a, 0x05, 0xfc, 0xab, 0x06], // expected buffer
            64,                                          // buffer size
            "Lets go to the %x",                         // printf style
            "Lets go to the {:x}",                       // core::fmt style
            0xcafe as u32
        );

        tokenize_test!(
            &[0xf3, 0x16, 0x03, 0x99, 0xfc, 0xab, 0x06], // expected buffer
            64,                                          // buffer size
            "Lets go to the %8x",                        // printf style
            "Lets go to the {:8x}",                      // core::fmt style
            0xcafe as u32
        );

        tokenize_test!(
            &[0x44, 0xce, 0xa3, 0x7e, 0xfc, 0xab, 0x06], // expected buffer
            64,                                          // buffer size
            "Lets go to the %08x",                       // printf style
            "Lets go to the {:08x}",                     // core::fmt style
            0xcafe as u32
        );
    }

    #[test]
    fn tokenizer_supports_concatenated_printf_format_strings() {
        // Since the no argument and some arguments cases are handled differently
        // by `tokenize_to_buffer!` we need to test both.
        let mut buffer = [0u8; 64];
        let len =
            tokenize_printf_to_buffer!(&mut buffer, "Hello" PW_FMT_CONCAT " Pigweed").unwrap();
        assert_eq!(&buffer[..len], &[0xe0, 0x92, 0xe0, 0xa]);

        let len = tokenize_printf_to_buffer!(&mut buffer, "Hello: " PW_FMT_CONCAT "%cigweed",
          "P".as_bytes()[0])
        .unwrap();
        assert_eq!(&buffer[..len], &[0x2e, 0x52, 0xac, 0xe4, 0xa0, 0x1]);
    }

    #[test]
    fn tokenizer_supports_concatenated_core_fmt_format_strings() {
        // Since the no argument and some arguments cases are handled differently
        // by `tokenize_to_buffer!` we need to test both.
        let mut buffer = [0u8; 64];
        let len =
            tokenize_core_fmt_to_buffer!(&mut buffer, "Hello" PW_FMT_CONCAT " Pigweed").unwrap();
        assert_eq!(&buffer[..len], &[0xe0, 0x92, 0xe0, 0xa]);

        let len = tokenize_core_fmt_to_buffer!(&mut buffer, "The answer is " PW_FMT_CONCAT "{}!",
          1 as i32)
        .unwrap();
        assert_eq!(&buffer[..len], &[0x52, 0x1c, 0xb0, 0x4c, 0x2]);
    }
}
