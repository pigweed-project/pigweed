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

pub mod __private {
    use core::fmt::Write;

    pub struct StackBuffer<const N: usize> {
        pub data: [u8; N],
        pub len: usize,
    }

    impl<const N: usize> Write for StackBuffer<N> {
        fn write_str(&mut self, s: &str) -> core::fmt::Result {
            let bytes = s.as_bytes();
            let space = N - self.len;
            if bytes.len() > space {
                return Err(core::fmt::Error);
            }
            self.data[self.len..self.len + bytes.len()].copy_from_slice(bytes);
            self.len += bytes.len();
            Ok(())
        }
    }

    extern "C" {
        fn pw_Log(
            level: i32,
            flags: u32,
            module: *const u8,
            file: *const u8,
            line: i32,
            func: *const u8,
            fmt: *const u8,
            ...
        );
    }

    /// Logs a message via `pw_Log`.
    ///
    /// # Safety
    ///
    /// The caller must ensure that `module`, `file`, and `msg` point to valid
    /// memory for the duration of the call, and that `module` and `file` are
    /// null-terminated.
    #[no_mangle]
    pub unsafe extern "C" fn rust_log(
        level: u8,
        module: *const u8,
        file: *const u8,
        line: i32,
        msg: *const u8,
        len: usize,
    ) {
        pw_Log(
            level as i32,
            0,
            module,
            file,
            line,
            c"".as_ptr() as *const u8,
            c"%.*s".as_ptr() as *const u8,
            len as core::ffi::c_int,
            msg,
        );
    }
}

// NOTE: This is not the canonical way to implement a backend. It creates an
// extra stack buffer and pulls in `core::fmt`. Ideally, we would have a
// backend similar to the printf backend, which generates the printf format
// string and calls `pw_Log` with a varargs.
#[macro_export]
macro_rules! pw_log_backend {
    ($log_level:expr, $format_string:literal $(, $args:expr)* $(,)?) => {{
        use core::fmt::Write;
        let mut buf = $crate::__private::StackBuffer::<128> { data: [0; 128], len: 0 };
        if core::write!(&mut buf, $format_string $(, $args)*).is_ok() {
            unsafe {
                $crate::__private::rust_log(
                    $log_level as u8,
                    core::concat!(core::module_path!(), "\0").as_ptr(),
                    core::concat!(core::file!(), "\0").as_ptr(),
                    core::line!() as i32,
                    buf.data.as_ptr(),
                    buf.len,
                );
            }
        }
    }};
}

#[macro_export]
macro_rules! pw_logf_backend {
    ($log_level:expr, $format_string:literal $(, $args:expr)* $(,)?) => {{
        use core::fmt::Write;
        let mut buf = $crate::__private::StackBuffer::<128> { data: [0; 128], len: 0 };
        if core::write!(&mut buf, $format_string $(, $args)*).is_ok() {
            unsafe {
                $crate::__private::rust_log(
                    $log_level as u8,
                    core::concat!(core::module_path!(), "\0").as_ptr(),
                    core::concat!(core::file!(), "\0").as_ptr(),
                    core::line!() as i32,
                    buf.data.as_ptr(),
                    buf.len,
                );
            }
        }
    }};
}
