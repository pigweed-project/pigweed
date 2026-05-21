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
    use pw_status::Result;
    pub use pw_tokenizer::{MessageWriter, tokenize_core_fmt_to_writer, tokenize_printf_to_writer};
    pub use tokenized_writer::Base64TokenizedMessageWriter;

    pub fn write(buffer: &[u8]) -> Result<()> {
        let mut console = console::Console::new();
        console.write_all(buffer)
    }

    pub type WriterCallback = fn(&[u8]) -> Result<()>;
    pub struct TokenizedWriter(Base64TokenizedMessageWriter<WriterCallback>);

    impl Default for TokenizedWriter {
        fn default() -> Self {
            Self(Base64TokenizedMessageWriter::new(write))
        }
    }

    impl MessageWriter for TokenizedWriter {
        fn write(&mut self, data: &[u8]) -> Result<()> {
            self.0.write(data)
        }

        fn remaining(&self) -> usize {
            self.0.remaining()
        }

        fn finalize(self) -> Result<()> {
            self.0.finalize()
        }
    }
}

// Implement the `pw_log` backend API.
//
// Since we're logging to a shared/ambient resource we can use
// tokenize_*_to_writer!` instead of `tokenize_*_to_buffer!` and avoid the
// overhead of initializing any intermediate buffers or objects.
//
// Uses `pw_format` special `PW_FMT_CONCAT` operator to prepend a place to
// print the log level.
#[macro_export]
macro_rules! pw_log_backend {
  ($log_level:expr, $($format_string:literal)PW_FMT_CONCAT+ $(, $args:expr)* $(,)?) => {{
    let _ = $crate::__private::tokenize_core_fmt_to_writer!(
      $crate::__private::TokenizedWriter,
      "[{}] " PW_FMT_CONCAT $($format_string)PW_FMT_CONCAT+,
      $crate::__private::log_level_tag($log_level) as &str,
      $($args),*);
  }};
}

#[macro_export]
macro_rules! pw_logf_backend {
  ($log_level:expr, $format_string:literal $(, $args:expr)* $(,)?) => {{
    let _ = $crate::__private::tokenize_printf_to_writer!(
      $crate::__private::TokenizedWriter,
      "[%s] " PW_FMT_CONCAT $format_string,
      $crate::__private::log_level_tag($log_level),
      $($args),*);
  }};
}
