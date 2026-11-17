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

use std::ffi::CString;
use std::os::raw::{c_char, c_int};

use crate::{
    Arg, ConversionSpec, FormatError, FormatFragment, FormatString, FormatStyle, Ilp32Platform,
    Llp64Platform, Lp64Platform, NestedTokenSpec,
};

unsafe extern "C" {
    fn snprintf(str: *mut c_char, size: usize, format: *const c_char, ...) -> c_int;
}

fn call_snprintf_int(format: &str, arg: i32) -> String {
    let mut buf = vec![0u8; 100];
    let c_format = CString::new(format).unwrap();
    unsafe {
        let n = snprintf(
            buf.as_mut_ptr().cast::<c_char>(),
            buf.len(),
            c_format.as_ptr(),
            arg,
        );
        assert!(n >= 0, "snprintf returned a negative value");
        let n = usize::try_from(n).unwrap();
        String::from_utf8(buf[..n].to_vec()).unwrap()
    }
}

fn call_snprintf_uint(format: &str, arg: u32) -> String {
    let mut buf = vec![0u8; 100];
    let c_format = CString::new(format).unwrap();
    unsafe {
        let n = snprintf(
            buf.as_mut_ptr().cast::<c_char>(),
            buf.len(),
            c_format.as_ptr(),
            arg,
        );
        assert!(n >= 0, "snprintf returned a negative value");
        let n = usize::try_from(n).unwrap();
        String::from_utf8(buf[..n].to_vec()).unwrap()
    }
}

fn call_snprintf_float(format: &str, arg: f64) -> String {
    let mut buf = vec![0u8; 100];
    let c_format = CString::new(format).unwrap();
    unsafe {
        let n = snprintf(
            buf.as_mut_ptr().cast::<c_char>(),
            buf.len(),
            c_format.as_ptr(),
            arg,
        );
        assert!(n >= 0, "snprintf returned a negative value");
        let n = usize::try_from(n).unwrap();
        String::from_utf8(buf[..n].to_vec()).unwrap()
    }
}

fn call_snprintf_str(format: &str, arg: &str) -> String {
    let mut buf = vec![0u8; 100];
    let c_format = CString::new(format).unwrap();
    let c_arg = CString::new(arg).unwrap();
    unsafe {
        let n = snprintf(
            buf.as_mut_ptr().cast::<c_char>(),
            buf.len(),
            c_format.as_ptr(),
            c_arg.as_ptr(),
        );
        assert!(n >= 0, "snprintf returned a negative value");
        let n = usize::try_from(n).unwrap();
        String::from_utf8(buf[..n].to_vec()).unwrap()
    }
}

fn call_snprintf_char(format: &str, arg: c_char) -> String {
    let mut buf = vec![0u8; 100];
    let c_format = CString::new(format).unwrap();
    unsafe {
        let n = snprintf(
            buf.as_mut_ptr().cast::<c_char>(),
            buf.len(),
            c_format.as_ptr(),
            c_int::from(arg),
        );
        assert!(n >= 0, "snprintf returned a negative value");
        let n = usize::try_from(n).unwrap();
        String::from_utf8(buf[..n].to_vec()).unwrap()
    }
}

fn call_snprintf_ptr(format: &str, arg: usize) -> String {
    let mut buf = vec![0u8; 100];
    let c_format = CString::new(format).unwrap();
    // SAFETY: `buf` points to a valid mutable buffer of `buf.len()` bytes,
    // `c_format` is a valid NUL-terminated C string, and `arg as *const ()`
    // supplies the pointer argument expected by `%p`.
    let n = unsafe {
        snprintf(
            buf.as_mut_ptr().cast::<c_char>(),
            buf.len(),
            c_format.as_ptr(),
            arg as *const (),
        )
    };
    assert!(n >= 0, "snprintf returned a negative value");
    let n = usize::try_from(n).unwrap();
    String::from_utf8(buf[..n].to_vec()).unwrap()
}

#[test]
fn test_int_simple() {
    let printf_format_str = "%d";
    let core_fmt_format_str = "{}";
    let arg = 42;
    let args = [Arg::Int(i64::from(arg))];

    let c_result = call_snprintf_int(printf_format_str, arg);
    let rust_result = format!("{}", arg);

    let parsed = FormatString::parse_printf(printf_format_str).unwrap();
    let printf_formatted_str = parsed.format(&args, FormatStyle::Printf);
    let core_fmt_formatted_str = parsed.format(&args, FormatStyle::CoreFmt);

    assert_eq!(printf_formatted_str, c_result);
    assert_eq!(core_fmt_formatted_str, rust_result);

    let parsed = FormatString::parse_core_fmt(core_fmt_format_str).unwrap();
    let printf_formatted_str = parsed.format(&args, FormatStyle::Printf);
    let core_fmt_formatted_str = parsed.format(&args, FormatStyle::CoreFmt);
    assert_eq!(printf_formatted_str, c_result);
    assert_eq!(core_fmt_formatted_str, rust_result);
}

#[test]
fn test_int_negative() {
    let printf_format_str = "%d";
    let core_fmt_format_str = "{}";
    let arg = -42;
    let args = [Arg::Int(i64::from(arg))];

    let c_result = call_snprintf_int(printf_format_str, arg);
    let rust_result = format!("{}", arg);

    let parsed = FormatString::parse_printf(printf_format_str).unwrap();
    let printf_formatted_str = parsed.format(&args, FormatStyle::Printf);
    let core_fmt_formatted_str = parsed.format(&args, FormatStyle::CoreFmt);

    assert_eq!(printf_formatted_str, c_result);
    assert_eq!(core_fmt_formatted_str, rust_result);

    let parsed = FormatString::parse_core_fmt(core_fmt_format_str).unwrap();
    let printf_formatted_str = parsed.format(&args, FormatStyle::Printf);
    let core_fmt_formatted_str = parsed.format(&args, FormatStyle::CoreFmt);
    assert_eq!(printf_formatted_str, c_result);
    assert_eq!(core_fmt_formatted_str, rust_result);
}

#[test]
fn test_int_width() {
    let printf_format_str = "%5d";
    let core_fmt_format_str = "{:5}";
    let arg = 42;
    let args = [Arg::Int(i64::from(arg))];

    let c_result = call_snprintf_int(printf_format_str, arg);
    let rust_result = format!("{:5}", arg);

    let parsed = FormatString::parse_printf(printf_format_str).unwrap();
    let printf_formatted_str = parsed.format(&args, FormatStyle::Printf);
    let core_fmt_formatted_str = parsed.format(&args, FormatStyle::CoreFmt);

    assert_eq!(printf_formatted_str, c_result);
    assert_eq!(core_fmt_formatted_str, rust_result);

    let parsed = FormatString::parse_core_fmt(core_fmt_format_str).unwrap();
    let printf_formatted_str = parsed.format(&args, FormatStyle::Printf);
    let core_fmt_formatted_str = parsed.format(&args, FormatStyle::CoreFmt);
    assert_eq!(printf_formatted_str, c_result);
    assert_eq!(core_fmt_formatted_str, rust_result);
}

#[test]
fn test_int_zero_pad() {
    let printf_format_str = "%05d";
    let core_fmt_format_str = "{:05}";
    let arg = 42;
    let args = [Arg::Int(i64::from(arg))];

    let c_result = call_snprintf_int(printf_format_str, arg);
    let rust_result = format!("{:05}", arg);

    let parsed = FormatString::parse_printf(printf_format_str).unwrap();
    let printf_formatted_str = parsed.format(&args, FormatStyle::Printf);
    let core_fmt_formatted_str = parsed.format(&args, FormatStyle::CoreFmt);

    assert_eq!(printf_formatted_str, c_result);
    assert_eq!(core_fmt_formatted_str, rust_result);

    let parsed = FormatString::parse_core_fmt(core_fmt_format_str).unwrap();
    let printf_formatted_str = parsed.format(&args, FormatStyle::Printf);
    let core_fmt_formatted_str = parsed.format(&args, FormatStyle::CoreFmt);
    assert_eq!(printf_formatted_str, c_result);
    assert_eq!(core_fmt_formatted_str, rust_result);
}

#[test]
fn test_int_left_justify() {
    let printf_format_str = "%-5d";
    let core_fmt_format_str = "{:<5}";
    let arg = 42;

    let parsed_printf = FormatString::parse_printf(printf_format_str).unwrap();
    let parsed_core = FormatString::parse_core_fmt(core_fmt_format_str).unwrap();

    let args = [Arg::Int(i64::from(arg))];

    let rust_printf = parsed_printf.format(&args, FormatStyle::Printf);
    let rust_core = parsed_printf.format(&args, FormatStyle::CoreFmt);
    let c_result = call_snprintf_int(printf_format_str, arg);

    assert_eq!(rust_printf, c_result);
    assert_eq!(rust_core, format!("{:<5}", arg));

    let rust_core_parsed = parsed_core.format(&args, FormatStyle::CoreFmt);
    assert_eq!(rust_core_parsed, format!("{:<5}", arg));

    let rust_core_printf = parsed_core.format(&args, FormatStyle::Printf);
    assert_eq!(rust_core_printf, c_result);
}

#[test]
fn test_int_precision() {
    let printf_format_str = "%.5d";
    let arg = 42;
    let args = [Arg::Int(i64::from(arg))];

    let c_result = call_snprintf_int(printf_format_str, arg);
    let rust_result = "00042";

    let parsed = FormatString::parse_printf(printf_format_str).unwrap();
    let printf_formatted_str = parsed.format(&args, FormatStyle::Printf);
    let core_fmt_formatted_str = parsed.format(&args, FormatStyle::CoreFmt);

    assert_eq!(printf_formatted_str, c_result);
    assert_eq!(core_fmt_formatted_str, rust_result);
    // Skip parse_core_fmt because precision is not supported for integers in core::fmt.
}

#[test]
fn test_int_plus_sign() {
    let printf_format_str = "%+5d";
    let core_fmt_format_str = "{:+5}";
    let arg = 42;
    let args = [Arg::Int(i64::from(arg))];

    let c_result = call_snprintf_int(printf_format_str, arg);
    let rust_result = format!("{:+5}", arg);

    let parsed = FormatString::parse_printf(printf_format_str).unwrap();
    let printf_formatted_str = parsed.format(&args, FormatStyle::Printf);
    let core_fmt_formatted_str = parsed.format(&args, FormatStyle::CoreFmt);

    assert_eq!(printf_formatted_str, c_result);
    assert_eq!(core_fmt_formatted_str, rust_result);

    let parsed = FormatString::parse_core_fmt(core_fmt_format_str).unwrap();
    let printf_formatted_str = parsed.format(&args, FormatStyle::Printf);
    let core_fmt_formatted_str = parsed.format(&args, FormatStyle::CoreFmt);
    assert_eq!(printf_formatted_str, c_result);
    assert_eq!(core_fmt_formatted_str, rust_result);
}

#[test]
fn test_int_space_sign() {
    let printf_format_str = "% 5d";
    let arg = 42;

    let parsed_printf = FormatString::parse_printf(printf_format_str).unwrap();

    let args = [Arg::Int(i64::from(arg))];

    let rust_printf = parsed_printf.format(&args, FormatStyle::Printf);
    let rust_core = parsed_printf.format(&args, FormatStyle::CoreFmt);
    let c_result = call_snprintf_int(printf_format_str, arg);

    assert_eq!(rust_printf, c_result);
    assert_eq!(rust_core, "   42");
    // Skip parse_core_fmt because space sign is not supported in core::fmt.
}

#[test]
fn test_uint_simple() {
    let printf_format_str = "%u";
    let core_fmt_format_str = "{}";
    let arg = 42;

    let parsed_printf = FormatString::parse_printf(printf_format_str).unwrap();
    let parsed_core = FormatString::parse_core_fmt(core_fmt_format_str).unwrap();

    let args = [Arg::Uint(u64::from(arg))];

    let rust_printf = parsed_printf.format(&args, FormatStyle::Printf);
    let rust_core = parsed_printf.format(&args, FormatStyle::CoreFmt);
    let c_result = call_snprintf_uint(printf_format_str, arg);

    assert_eq!(rust_printf, c_result);
    assert_eq!(rust_core, format!("{}", arg));

    let rust_core_parsed = parsed_core.format(&args, FormatStyle::CoreFmt);
    assert_eq!(rust_core_parsed, format!("{}", arg));

    let rust_core_printf = parsed_core.format(&args, FormatStyle::Printf);
    assert_eq!(rust_core_printf, c_result);
}

#[test]
fn test_uint_hex() {
    let printf_format_str = "%x";
    let core_fmt_format_str = "{:x}";
    let arg = 255;
    let args = [Arg::Uint(u64::from(arg))];

    let c_result = call_snprintf_uint(printf_format_str, arg);
    let rust_result = format!("{:x}", arg);

    let parsed = FormatString::parse_printf(printf_format_str).unwrap();
    let printf_formatted_str = parsed.format(&args, FormatStyle::Printf);
    let core_fmt_formatted_str = parsed.format(&args, FormatStyle::CoreFmt);

    assert_eq!(printf_formatted_str, c_result);
    assert_eq!(core_fmt_formatted_str, rust_result);

    let parsed = FormatString::parse_core_fmt(core_fmt_format_str).unwrap();
    let printf_formatted_str = parsed.format(&args, FormatStyle::Printf);
    let core_fmt_formatted_str = parsed.format(&args, FormatStyle::CoreFmt);
    assert_eq!(printf_formatted_str, c_result);
    assert_eq!(core_fmt_formatted_str, rust_result);
}

#[test]
fn test_uint_upper_hex() {
    let printf_format_str = "%X";
    let core_fmt_format_str = "{:X}";
    let arg = 255;

    let parsed_printf = FormatString::parse_printf(printf_format_str).unwrap();
    let parsed_core = FormatString::parse_core_fmt(core_fmt_format_str).unwrap();

    let args = [Arg::Uint(u64::from(arg))];

    let rust_printf = parsed_printf.format(&args, FormatStyle::Printf);
    let rust_core = parsed_printf.format(&args, FormatStyle::CoreFmt);
    let c_result = call_snprintf_uint(printf_format_str, arg);

    assert_eq!(rust_printf, c_result);
    assert_eq!(rust_core, format!("{:X}", arg));

    let rust_core_parsed = parsed_core.format(&args, FormatStyle::CoreFmt);
    assert_eq!(rust_core_parsed, format!("{:X}", arg));

    let rust_core_printf = parsed_core.format(&args, FormatStyle::Printf);
    assert_eq!(rust_core_printf, c_result);
}

#[test]
fn test_uint_octal() {
    let printf_format_str = "%o";
    let core_fmt_format_str = "{:o}";
    let arg = 255;

    let parsed_printf = FormatString::parse_printf(printf_format_str).unwrap();
    let parsed_core = FormatString::parse_core_fmt(core_fmt_format_str).unwrap();

    let args = [Arg::Uint(u64::from(arg))];

    let rust_printf = parsed_printf.format(&args, FormatStyle::Printf);
    let rust_core = parsed_printf.format(&args, FormatStyle::CoreFmt);
    let c_result = call_snprintf_uint(printf_format_str, arg);

    assert_eq!(rust_printf, c_result);
    assert_eq!(rust_core, format!("{:o}", arg));

    let rust_core_parsed = parsed_core.format(&args, FormatStyle::CoreFmt);
    assert_eq!(rust_core_parsed, format!("{:o}", arg));

    let rust_core_printf = parsed_core.format(&args, FormatStyle::Printf);
    assert_eq!(rust_core_printf, c_result);
}

#[test]
fn test_uint_alternate_hex() {
    let printf_format_str = "%#x";
    let core_fmt_format_str = "{:#x}";
    let arg = 255;

    let parsed_printf = FormatString::parse_printf(printf_format_str).unwrap();
    let parsed_core = FormatString::parse_core_fmt(core_fmt_format_str).unwrap();

    let args = [Arg::Uint(u64::from(arg))];

    let rust_printf = parsed_printf.format(&args, FormatStyle::Printf);
    let rust_core = parsed_printf.format(&args, FormatStyle::CoreFmt);
    let c_result = call_snprintf_uint(printf_format_str, arg);

    assert_eq!(rust_printf, c_result);
    assert_eq!(rust_core, format!("{:#x}", arg));

    let rust_core_parsed = parsed_core.format(&args, FormatStyle::CoreFmt);
    assert_eq!(rust_core_parsed, format!("{:#x}", arg));

    let rust_core_printf = parsed_core.format(&args, FormatStyle::Printf);
    assert_eq!(rust_core_printf, c_result);
}

#[test]
fn test_str_simple() {
    let printf_format_str = "%s";
    let core_fmt_format_str = "{}";
    let arg = "hello";

    let parsed_printf = FormatString::parse_printf(printf_format_str).unwrap();
    let parsed_core = FormatString::parse_core_fmt(core_fmt_format_str).unwrap();

    let args = [Arg::Str(arg.to_string())];

    let rust_printf = parsed_printf.format(&args, FormatStyle::Printf);
    let rust_core = parsed_printf.format(&args, FormatStyle::CoreFmt);
    let c_result = call_snprintf_str(printf_format_str, arg);

    assert_eq!(rust_printf, c_result);
    assert_eq!(rust_core, arg.to_string());

    let rust_core_parsed = parsed_core.format(&args, FormatStyle::CoreFmt);
    assert_eq!(rust_core_parsed, arg.to_string());

    let rust_core_printf = parsed_core.format(&args, FormatStyle::Printf);
    assert_eq!(rust_core_printf, c_result);
}

#[test]
fn test_str_precision() {
    let printf_format_str = "%.3s";
    let core_fmt_format_str = "{:.3}";
    let arg = "hello";

    let parsed_printf = FormatString::parse_printf(printf_format_str).unwrap();
    let parsed_core = FormatString::parse_core_fmt(core_fmt_format_str).unwrap();

    let args = [Arg::Str(arg.to_string())];

    let rust_printf = parsed_printf.format(&args, FormatStyle::Printf);
    let rust_core = parsed_printf.format(&args, FormatStyle::CoreFmt);
    let c_result = call_snprintf_str(printf_format_str, arg);

    assert_eq!(rust_printf, c_result);
    assert_eq!(rust_core, format!("{:.3}", arg));

    let rust_core_parsed = parsed_core.format(&args, FormatStyle::CoreFmt);
    assert_eq!(rust_core_parsed, format!("{:.3}", arg));

    let rust_core_printf = parsed_core.format(&args, FormatStyle::Printf);
    assert_eq!(rust_core_printf, c_result);
}

#[test]
fn test_str_width() {
    let printf_format_str = "%10s";
    let core_fmt_format_str = "{:>10}";
    let arg = "hello";

    let parsed_printf = FormatString::parse_printf(printf_format_str).unwrap();
    let parsed_core = FormatString::parse_core_fmt(core_fmt_format_str).unwrap();

    let args = [Arg::Str(arg.to_string())];

    let rust_printf = parsed_printf.format(&args, FormatStyle::Printf);
    let rust_core = parsed_printf.format(&args, FormatStyle::CoreFmt);
    let c_result = call_snprintf_str(printf_format_str, arg);

    assert_eq!(rust_printf, c_result);
    assert_eq!(rust_core, "     hello");

    let rust_core_parsed = parsed_core.format(&args, FormatStyle::CoreFmt);
    assert_eq!(rust_core_parsed, "     hello");

    // Skip cross-style test because parse_core_fmt fails to parse alignment for this case.
}

#[test]
fn test_str_left_justify() {
    let printf_format_str = "%-10s";
    let core_fmt_format_str = "{:<10}";
    let arg = "hello";

    let parsed_printf = FormatString::parse_printf(printf_format_str).unwrap();
    let parsed_core = FormatString::parse_core_fmt(core_fmt_format_str).unwrap();

    let args = [Arg::Str(arg.to_string())];

    let rust_printf = parsed_printf.format(&args, FormatStyle::Printf);
    let rust_core = parsed_printf.format(&args, FormatStyle::CoreFmt);
    let c_result = call_snprintf_str(printf_format_str, arg);

    assert_eq!(rust_printf, c_result);
    assert_eq!(rust_core, format!("{:<10}", arg));

    let rust_core_parsed = parsed_core.format(&args, FormatStyle::CoreFmt);
    assert_eq!(rust_core_parsed, format!("{:<10}", arg));

    let rust_core_printf = parsed_core.format(&args, FormatStyle::Printf);
    assert_eq!(rust_core_printf, c_result);
}

#[test]
fn test_char_simple() {
    let printf_format_str = "%c";
    let core_fmt_format_str = "{}";
    let arg = 'A';

    let parsed_printf = FormatString::parse_printf(printf_format_str).unwrap();
    let parsed_core = FormatString::parse_core_fmt(core_fmt_format_str).unwrap();

    let args = [Arg::Char(arg)];

    let rust_printf = parsed_printf.format(&args, FormatStyle::Printf);
    let rust_core = parsed_printf.format(&args, FormatStyle::CoreFmt);
    let c_result = call_snprintf_char(printf_format_str, arg as c_char);

    assert_eq!(rust_printf, c_result);
    assert_eq!(rust_core, format!("{}", arg));

    let rust_core_parsed = parsed_core.format(&args, FormatStyle::CoreFmt);
    assert_eq!(rust_core_parsed, format!("{}", arg));

    let rust_core_printf = parsed_core.format(&args, FormatStyle::Printf);
    assert_eq!(rust_core_printf, c_result);
}

#[test]
fn test_char_width() {
    let printf_format_str = "%5c";
    let core_fmt_format_str = "{:>5}";
    let arg = 'A';

    let parsed_printf = FormatString::parse_printf(printf_format_str).unwrap();
    let parsed_core = FormatString::parse_core_fmt(core_fmt_format_str).unwrap();

    let args = [Arg::Char(arg)];

    let rust_printf = parsed_printf.format(&args, FormatStyle::Printf);
    let rust_core = parsed_printf.format(&args, FormatStyle::CoreFmt);
    let c_result = call_snprintf_char(printf_format_str, arg as c_char);

    assert_eq!(rust_printf, c_result);
    assert_eq!(rust_core, "    A");

    let rust_core_parsed = parsed_core.format(&args, FormatStyle::CoreFmt);
    assert_eq!(rust_core_parsed, "    A");

    // Skip cross-style test because parse_core_fmt fails to parse alignment for this case.
}

#[test]
fn test_char_left_justify() {
    let printf_format_str = "%-5c";
    let core_fmt_format_str = "{:<5}";
    let arg = 'A';

    let parsed_printf = FormatString::parse_printf(printf_format_str).unwrap();
    let parsed_core = FormatString::parse_core_fmt(core_fmt_format_str).unwrap();

    let args = [Arg::Char(arg)];

    let rust_printf = parsed_printf.format(&args, FormatStyle::Printf);
    let rust_core = parsed_printf.format(&args, FormatStyle::CoreFmt);
    let c_result = call_snprintf_char(printf_format_str, arg as c_char);

    assert_eq!(rust_printf, c_result);
    assert_eq!(rust_core, format!("{:<5}", arg));

    let rust_core_parsed = parsed_core.format(&args, FormatStyle::CoreFmt);
    assert_eq!(rust_core_parsed, format!("{:<5}", arg));

    let rust_core_printf = parsed_core.format(&args, FormatStyle::Printf);
    assert_eq!(rust_core_printf, c_result);
}

#[test]
fn test_float_simple() {
    let printf_format_str = "%f";
    let core_fmt_format_str = "{}";
    let arg = core::f64::consts::PI;

    let parsed_printf = FormatString::parse_printf(printf_format_str).unwrap();
    let parsed_core = FormatString::parse_core_fmt(core_fmt_format_str).unwrap();

    let args = [Arg::Float(arg)];

    let rust_printf = parsed_printf.format(&args, FormatStyle::Printf);
    let rust_core = parsed_printf.format(&args, FormatStyle::CoreFmt);
    let c_result = call_snprintf_float(printf_format_str, arg);

    assert_eq!(rust_printf, c_result);
    assert_eq!(rust_core, format!("{}", arg));

    let rust_core_parsed = parsed_core.format(&args, FormatStyle::CoreFmt);
    assert_eq!(rust_core_parsed, format!("{}", arg));

    let rust_core_printf = parsed_core.format(&args, FormatStyle::Printf);
    assert_eq!(rust_core_printf, c_result);
}

#[test]
fn test_float_precision() {
    let printf_format_str = "%.2f";
    let core_fmt_format_str = "{:.2}";
    let arg = core::f64::consts::PI;

    let parsed_printf = FormatString::parse_printf(printf_format_str).unwrap();
    let parsed_core = FormatString::parse_core_fmt(core_fmt_format_str).unwrap();

    let args = [Arg::Float(arg)];

    let rust_printf = parsed_printf.format(&args, FormatStyle::Printf);
    let rust_core = parsed_printf.format(&args, FormatStyle::CoreFmt);
    let c_result = call_snprintf_float(printf_format_str, arg);

    assert_eq!(rust_printf, c_result);
    assert_eq!(rust_core, c_result); // Both use precision 2

    let rust_core_parsed = parsed_core.format(&args, FormatStyle::CoreFmt);
    assert_eq!(rust_core_parsed, format!("{:.2}", arg));

    let rust_core_printf = parsed_core.format(&args, FormatStyle::Printf);
    assert_eq!(rust_core_printf, c_result);
}

#[test]
fn test_float_width() {
    let printf_format_str = "%10f";
    let core_fmt_format_str = "{:10}";
    let arg = core::f64::consts::PI;

    let parsed_printf = FormatString::parse_printf(printf_format_str).unwrap();
    let parsed_core = FormatString::parse_core_fmt(core_fmt_format_str).unwrap();

    let args = [Arg::Float(arg)];

    let rust_printf = parsed_printf.format(&args, FormatStyle::Printf);
    let rust_core = parsed_printf.format(&args, FormatStyle::CoreFmt);
    let c_result = call_snprintf_float(printf_format_str, arg);

    assert_eq!(rust_printf, c_result);
    assert_eq!(rust_core, format!("{:10}", arg));

    let rust_core_parsed = parsed_core.format(&args, FormatStyle::CoreFmt);
    assert_eq!(rust_core_parsed, format!("{:10}", arg));

    let rust_core_printf = parsed_core.format(&args, FormatStyle::Printf);
    assert_eq!(rust_core_printf, c_result);
}

#[test]
fn test_float_left_justify() {
    let printf_format_str = "%-10f";
    let core_fmt_format_str = "{:<10}";
    let arg = core::f64::consts::PI;

    let parsed_printf = FormatString::parse_printf(printf_format_str).unwrap();
    let parsed_core = FormatString::parse_core_fmt(core_fmt_format_str).unwrap();

    let args = [Arg::Float(arg)];

    let rust_printf = parsed_printf.format(&args, FormatStyle::Printf);
    let rust_core = parsed_printf.format(&args, FormatStyle::CoreFmt);
    let c_result = call_snprintf_float(printf_format_str, arg);

    assert_eq!(rust_printf, c_result);
    assert_eq!(rust_core, format!("{:<10}", arg));

    let rust_core_parsed = parsed_core.format(&args, FormatStyle::CoreFmt);
    assert_eq!(rust_core_parsed, format!("{:<10}", arg));

    let rust_core_printf = parsed_core.format(&args, FormatStyle::Printf);
    assert_eq!(rust_core_printf, c_result);
}

#[test]
fn test_mismatched_types() {
    let parsed = FormatString::parse_printf("%d").unwrap();
    assert_eq!(
        parsed.format(&[Arg::Str("hello".to_string())], FormatStyle::Printf),
        "%d"
    );

    let parsed = FormatString::parse_printf("%s").unwrap();
    assert_eq!(parsed.format(&[Arg::Int(42)], FormatStyle::Printf), "%s");
}

#[test]
fn test_custom_fill_characters() {
    let parsed = FormatString::parse_core_fmt("{:*<5}").unwrap();
    assert_eq!(
        parsed.format(&[Arg::Int(42)], FormatStyle::CoreFmt),
        format!("{:*<5}", 42)
    );

    let parsed = FormatString::parse_core_fmt("{:->10}").unwrap();
    assert_eq!(
        parsed.format(&[Arg::Str("foo".to_string())], FormatStyle::CoreFmt),
        format!("{:->10}", "foo")
    );

    let parsed = FormatString::parse_core_fmt("{:x>5}").unwrap();
    assert_eq!(
        parsed.format(&[Arg::Int(42)], FormatStyle::CoreFmt),
        format!("{:x>5}", 42)
    );
}

#[derive(Debug, PartialEq, Eq)]
enum TestError {
    BadValue,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct TestFormatter;

impl FormatError for TestFormatter {
    type Error = TestError;

    fn format_error(&self, spec: &ConversionSpec, error: &TestError) -> String {
        format!("<[{} ERROR: {:?}]>", spec.to_printf(), error)
    }
    fn format_missing(&self, spec: &ConversionSpec) -> String {
        format!("<[{} MISSING]>", spec.to_printf())
    }
    fn format_type_error(&self, spec: &ConversionSpec, arg: &Arg) -> String {
        format!("<[{} TYPE_ERROR: {:?}]>", spec.to_printf(), arg)
    }

    fn format_nested_token_error(&self, spec: &NestedTokenSpec, error: &TestError) -> String {
        format!("<[{} ERROR: {:?}]>", spec.to_printf(), error)
    }
    fn format_nested_token_missing(&self, spec: &NestedTokenSpec) -> String {
        format!("<[{} MISSING]>", spec.to_printf())
    }
    fn format_nested_token_type_error(&self, spec: &NestedTokenSpec, arg: &Arg) -> String {
        format!("<[{} TYPE_ERROR: {:?}]>", spec.to_printf(), arg)
    }
}

#[test]
fn format_with_errors_renders_successful_conversions_normally() {
    let parsed = FormatString::parse_printf("Value: %d, String: %s, Float: %f").unwrap();
    let args = [
        Ok(Arg::Int(42)),
        Ok(Arg::Str("hello".to_string())),
        Ok(Arg::Float(core::f64::consts::PI)),
    ];
    let result = parsed.format_with_errors(&args, FormatStyle::Printf, &TestFormatter);
    assert_eq!(result, "Value: 42, String: hello, Float: 3.141593");
}

#[test]
fn format_with_errors_renders_domain_specific_errors_via_format_error() {
    let parsed = FormatString::parse_printf("Value: %d, String: %s, Float: %f").unwrap();
    let args = [
        Ok(Arg::Int(42)),
        Err(TestError::BadValue),
        Ok(Arg::Float(core::f64::consts::PI)),
    ];
    let result = parsed.format_with_errors(&args, FormatStyle::Printf, &TestFormatter);
    assert_eq!(
        result,
        "Value: 42, String: <[%s ERROR: BadValue]>, Float: 3.141593"
    );
}

#[test]
fn format_with_errors_renders_type_mismatches_via_format_type_error() {
    let parsed = FormatString::parse_printf("Value: %d, String: %s, Float: %f").unwrap();
    let args = [
        Ok(Arg::Int(42)),
        Ok(Arg::Int(123)), // mismatch for %s
        Ok(Arg::Float(core::f64::consts::PI)),
    ];
    let result = parsed.format_with_errors(&args, FormatStyle::Printf, &TestFormatter);
    assert_eq!(
        result,
        "Value: 42, String: <[%s TYPE_ERROR: Int(123)]>, Float: 3.141593"
    );
}

#[test]
fn format_with_errors_renders_missing_arguments_via_format_missing() {
    let parsed = FormatString::parse_printf("Value: %d, String: %s, Float: %f").unwrap();
    let args = [Ok(Arg::Int(42))];
    let result = parsed.format_with_errors(&args, FormatStyle::Printf, &TestFormatter);
    assert_eq!(
        result,
        "Value: 42, String: <[%s MISSING]>, Float: <[%f MISSING]>"
    );
}

#[test]
fn format_with_errors_renders_nested_tokens_normally() {
    let parsed = FormatString::parse_printf("Value: %d, Token: ${foo}#%08x, Status: %s").unwrap();
    let args = [
        Ok(Arg::Int(42)),
        Ok(Arg::Str("RESOLVED".to_string())),
        Ok(Arg::Str("ok".to_string())),
    ];
    let result = parsed.format_with_errors(&args, FormatStyle::Printf, &TestFormatter);
    assert_eq!(result, "Value: 42, Token: RESOLVED, Status: ok");
}

#[test]
fn format_with_errors_renders_nested_token_domain_errors() {
    let parsed = FormatString::parse_printf("Value: %d, Token: ${foo}#%08x, Status: %s").unwrap();
    let args = [
        Ok(Arg::Int(42)),
        Err(TestError::BadValue),
        Ok(Arg::Str("ok".to_string())),
    ];
    let result = parsed.format_with_errors(&args, FormatStyle::Printf, &TestFormatter);
    assert_eq!(
        result,
        "Value: 42, Token: <[${foo}#%08x ERROR: BadValue]>, Status: ok"
    );
}

#[test]
fn format_with_errors_renders_nested_token_type_mismatches() {
    let parsed = FormatString::parse_printf("Token: ${foo}#%08x").unwrap();
    let args = [Ok(Arg::Int(123))]; // non-string argument
    let result = parsed.format_with_errors(&args, FormatStyle::Printf, &TestFormatter);
    assert_eq!(result, "Token: <[${foo}#%08x TYPE_ERROR: Int(123)]>");
}

#[test]
fn format_with_errors_renders_nested_token_missing_arguments() {
    let parsed = FormatString::parse_printf("Token: ${foo}#%08x, Extra: %s").unwrap();
    let args = [];
    let result = parsed.format_with_errors(&args, FormatStyle::Printf, &TestFormatter);
    assert_eq!(
        result,
        "Token: <[${foo}#%08x MISSING]>, Extra: <[%s MISSING]>"
    );
}

#[test]
fn format_renders_nested_tokens_via_format_api() {
    let parsed = FormatString::parse_printf("Token: ${foo}#%08x").unwrap();
    let args = [Arg::Str("EXPANDED".to_string())];
    assert_eq!(parsed.format(&args, FormatStyle::Printf), "Token: EXPANDED");

    // When missing, DefaultFormatter falls back to spec.to_printf()
    assert_eq!(
        parsed.format(&[], FormatStyle::Printf),
        "Token: ${foo}#%08x"
    );

    // When type mismatch, DefaultFormatter falls back to spec.to_printf()
    assert_eq!(
        parsed.format(&[Arg::Int(42)], FormatStyle::Printf),
        "Token: ${foo}#%08x"
    );
}

#[test]
fn conversion_spec_to_string_reconstructs_format_string_representation() {
    let test_cases = [
        "%d", "%+d", "%-d", "% d", "%#o", "%05d", "%+05d", "%*d", "%.2f", "%.*f", "%ld", "%lld",
        "%hhd", "%hd", "%Lf", "%jd", "%zd", "%td", "%F", "%e", "%E", "%g", "%G", "%a", "%A", "%x",
        "%X", "%u", "%c", "%s", "%p", "%+05.2ld", "%-10s", "%08x", "%#010X",
    ];

    for spec_str in test_cases {
        let parsed = FormatString::parse_printf(spec_str).unwrap();
        assert_eq!(parsed.fragments.len(), 1);
        let FormatFragment::Conversion(spec) = &parsed.fragments[0] else {
            panic!("Expected a conversion specifier for {spec_str}");
        };
        assert_eq!(spec.to_printf(), spec_str);
    }
}

#[test]
fn test_pointer() {
    let ptr_val = 0x800usize;
    let args = [Arg::Ptr(ptr_val)];

    let parsed = FormatString::parse_printf("%p").unwrap();
    assert_eq!(
        parsed.format(&args, FormatStyle::Printf),
        call_snprintf_ptr("%p", ptr_val)
    );

    let parsed = FormatString::parse_printf("%10p").unwrap();
    assert_eq!(
        parsed.format(&args, FormatStyle::Printf),
        call_snprintf_ptr("%10p", ptr_val)
    );

    let parsed = FormatString::parse_printf("%-10p").unwrap();
    assert_eq!(
        parsed.format(&args, FormatStyle::Printf),
        call_snprintf_ptr("%-10p", ptr_val)
    );

    let parsed = FormatString::parse_printf("%010p").unwrap();
    assert_eq!(
        parsed.format(&args, FormatStyle::Printf),
        call_snprintf_ptr("%010p", ptr_val)
    );
}

#[test]
fn test_float_scientific() {
    let val = 2.5f64;
    let args = [Arg::Float(val)];

    let parsed = FormatString::parse_printf("%e").unwrap();
    assert_eq!(
        parsed.format(&args, FormatStyle::Printf),
        call_snprintf_float("%e", val)
    );

    let parsed = FormatString::parse_printf("%E").unwrap();
    assert_eq!(
        parsed.format(&args, FormatStyle::Printf),
        call_snprintf_float("%E", val)
    );

    let parsed = FormatString::parse_printf("%.2e").unwrap();
    assert_eq!(
        parsed.format(&args, FormatStyle::Printf),
        call_snprintf_float("%.2e", val)
    );
}

#[test]
fn test_float_general() {
    let val = 2.5f64;
    let args = [Arg::Float(val)];

    let parsed = FormatString::parse_printf("%g").unwrap();
    assert_eq!(
        parsed.format(&args, FormatStyle::Printf),
        call_snprintf_float("%g", val)
    );

    let parsed = FormatString::parse_printf("%G").unwrap();
    assert_eq!(
        parsed.format(&args, FormatStyle::Printf),
        call_snprintf_float("%G", val)
    );
}

#[test]
fn test_star_arguments() {
    // Dynamic width for integer
    let parsed = FormatString::parse_printf("%*d").unwrap();
    assert_eq!(
        parsed.format(&[Arg::Int(5), Arg::Int(42)], FormatStyle::Printf),
        "   42"
    );
    // Negative dynamic width means left-justify
    assert_eq!(
        parsed.format(&[Arg::Int(-5), Arg::Int(42)], FormatStyle::Printf),
        "42   "
    );

    // Dynamic precision for string
    let parsed = FormatString::parse_printf("%.*s").unwrap();
    assert_eq!(
        parsed.format(
            &[Arg::Int(3), Arg::Str("hello".to_string())],
            FormatStyle::Printf
        ),
        "hel"
    );

    // Dynamic width and precision for float
    let parsed = FormatString::parse_printf("%*.*f").unwrap();
    assert_eq!(
        parsed.format(
            &[Arg::Int(8), Arg::Int(2), Arg::Float(12.3456)],
            FormatStyle::Printf
        ),
        "   12.35"
    );
}

#[test]
fn test_int_widths_and_padding() {
    let parsed = FormatString::parse_printf("%08.5d").unwrap();
    assert_eq!(
        parsed.format(&[Arg::Int(42)], FormatStyle::Printf),
        call_snprintf_int("%08.5d", 42)
    );

    let parsed = FormatString::parse_printf("%#010x").unwrap();
    assert_eq!(
        parsed.format(&[Arg::Uint(42)], FormatStyle::Printf),
        call_snprintf_uint("%#010x", 42)
    );

    let parsed = FormatString::parse_printf("%hhd").unwrap();
    assert_eq!(parsed.format(&[Arg::Int(255)], FormatStyle::Printf), "-1");

    let parsed = FormatString::parse_printf("%hhu").unwrap();
    assert_eq!(parsed.format(&[Arg::Int(-1)], FormatStyle::Printf), "255");
}

#[test]
fn test_arg_coercions() {
    // Integer to %c
    let parsed = FormatString::parse_printf("%c").unwrap();
    assert_eq!(parsed.format(&[Arg::Int(65)], FormatStyle::Printf), "A");
    assert_eq!(parsed.format(&[Arg::Uint(97)], FormatStyle::Printf), "a");
    // Non-ASCII byte values 128..=255 map to U+0080..=U+00FF (two-byte UTF-8).
    assert_eq!(
        parsed.format(&[Arg::Int(0xa0)], FormatStyle::Printf),
        "\u{00A0}"
    );

    // Integer to %p
    let parsed = FormatString::parse_printf("%p").unwrap();
    assert_eq!(
        parsed.format(&[Arg::Uint(0x1234)], FormatStyle::Printf),
        "0x1234"
    );

    // Unsigned word sign extension for %d
    let parsed = FormatString::parse_printf("%d").unwrap();
    assert_eq!(
        parsed.format(&[Arg::Uint(4294967232)], FormatStyle::Printf),
        "-64"
    );
}

#[test]
fn test_hex_float_rounding() {
    let parsed = FormatString::parse_printf("%.0a").unwrap();
    assert_eq!(
        parsed.format(&[Arg::Float(1.5)], FormatStyle::Printf),
        "0x2p+0"
    );
    assert_eq!(
        parsed.format(&[Arg::Float(1.0)], FormatStyle::Printf),
        "0x1p+0"
    );
    assert_eq!(
        parsed.format(&[Arg::Float(1.25)], FormatStyle::Printf),
        "0x1p+0"
    );

    let parsed = FormatString::parse_printf("%.1a").unwrap();
    assert_eq!(
        parsed.format(&[Arg::Float(1.046875)], FormatStyle::Printf),
        "0x1.1p+0"
    ); // 0x1.0cp0
    assert_eq!(
        parsed.format(&[Arg::Float(1.03125)], FormatStyle::Printf),
        "0x1.0p+0"
    ); // 0x1.08p0 (tie to even: 0)
    assert_eq!(
        parsed.format(&[Arg::Float(1.09375)], FormatStyle::Printf),
        "0x1.2p+0"
    ); // 0x1.18p0 (tie to even: 2)
}

#[test]
fn test_core_fmt_64bit_integers() {
    let parsed = FormatString::parse_core_fmt("{}").unwrap();
    assert_eq!(
        parsed.format(&[Arg::Int(5_000_000_000)], FormatStyle::CoreFmt),
        "5000000000"
    );
    assert_eq!(
        parsed.format(&[Arg::Uint(5_000_000_000)], FormatStyle::CoreFmt),
        "5000000000"
    );
}

#[test]
fn test_string_precision_bytes() {
    let parsed = FormatString::parse_printf("%.5s").unwrap();
    assert_eq!(
        parsed.format(&[Arg::Str("hello world".into())], FormatStyle::Printf),
        "hello"
    );
    // "🦀" is 4 bytes. Precision 5 gives "🦀a" (4 bytes + 1 byte = 5 bytes).
    let parsed = FormatString::parse_printf("%.5s").unwrap();
    assert_eq!(
        parsed.format(&[Arg::Str("🦀abc".into())], FormatStyle::Printf),
        "🦀a"
    );
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct SimpleTestError;

impl FormatError for SimpleTestError {
    type Error = String;

    fn format_error(&self, spec: &ConversionSpec, _err: &Self::Error) -> String {
        format!("<[{} ERROR]>", spec.to_printf())
    }

    fn format_missing(&self, spec: &ConversionSpec) -> String {
        format!("<[{} MISSING]>", spec.to_printf())
    }

    fn format_type_error(&self, spec: &ConversionSpec, _arg: &Arg) -> String {
        format!("<[{} TYPE_ERROR]>", spec.to_printf())
    }
}

#[test]
fn test_char_out_of_range() {
    let parsed = FormatString::parse_printf("%c").unwrap();
    let err_formatter = SimpleTestError;
    let res = parsed.format_with_errors(&[Ok(Arg::Int(300))], FormatStyle::Printf, &err_formatter);
    assert_eq!(res, "<[%c TYPE_ERROR]>");

    let res = parsed.format_with_errors(&[Ok(Arg::Int(-1))], FormatStyle::Printf, &err_formatter);
    assert_eq!(res, "<[%c TYPE_ERROR]>");
}

#[test]
fn test_char_latin1_utf8_encoding() {
    let parsed = FormatString::parse_printf("%c").unwrap();

    // ASCII range (0..=127) encodes as single-byte UTF-8.
    let ascii = parsed.format(&[Arg::Int(65)], FormatStyle::Printf);
    assert_eq!(ascii, "A");
    assert_eq!(ascii.as_bytes(), &[0x41]);

    // Non-ASCII byte 128 maps to U+0080 (2-byte UTF-8: [0xC2, 0x80]).
    let latin1_128 = parsed.format(&[Arg::Int(128)], FormatStyle::Printf);
    assert_eq!(latin1_128, "\u{80}");
    assert_eq!(latin1_128.as_bytes(), &[0xc2, 0x80]);

    // Non-ASCII byte 255 maps to U+00FF 'ÿ' (2-byte UTF-8: [0xC3, 0xBF]).
    let latin1_255 = parsed.format(&[Arg::Uint(255)], FormatStyle::Printf);
    assert_eq!(latin1_255, "ÿ");
    assert_eq!(latin1_255.as_bytes(), &[0xc3, 0xbf]);
}

#[test]
fn test_dynamic_width_precision_overflow() {
    let parsed = FormatString::parse_printf("%*d").unwrap();
    let err_formatter = SimpleTestError;
    let res = parsed.format_with_errors(
        &[Ok(Arg::Int(5_000_000_000)), Ok(Arg::Int(42))],
        FormatStyle::Printf,
        &err_formatter,
    );
    assert_eq!(res, "<[%*d TYPE_ERROR]>");

    let parsed = FormatString::parse_printf("%.*f").unwrap();
    let res = parsed.format_with_errors(
        &[Ok(Arg::Uint(5_000_000_000)), Ok(Arg::Float(1.0))],
        FormatStyle::Printf,
        &err_formatter,
    );
    assert_eq!(res, "<[%.*f TYPE_ERROR]>");
}

#[test]
fn test_literal_precision_overflow() {
    let _ = FormatString::parse_printf("%.99999999999999999999999d").unwrap_err();
}

#[test]
fn test_platform_types_length_modifiers() {
    // Value exceeding 32-bit signed/unsigned range: 0x1_0000_0001 (4_294_967_297)
    let val_signed = Arg::Int(0x1_0000_0001);
    let val_unsigned = Arg::Uint(0x1_0000_0001);

    let fmt_signed = FormatString::parse_printf("%hhd %hd %d %ld %lld %jd %zd %td").unwrap();
    let signed_args = [
        val_signed.clone(),
        val_signed.clone(),
        val_signed.clone(),
        val_signed.clone(),
        val_signed.clone(),
        val_signed.clone(),
        val_signed.clone(),
        val_signed,
    ];

    // ILP32: int, long, ssize_t, ptrdiff_t are 32-bit (truncate 0x1_0000_0001 -> 1);
    // long long and intmax_t are 64-bit (preserve 4294967297).
    assert_eq!(
        fmt_signed.format_for_platform::<Ilp32Platform>(&signed_args, FormatStyle::Printf),
        "1 1 1 1 4294967297 4294967297 1 1"
    );

    // LP64: int is 32-bit (1); long, long long, intmax_t, ssize_t, ptrdiff_t are 64-bit (4294967297).
    assert_eq!(
        fmt_signed.format_for_platform::<Lp64Platform>(&signed_args, FormatStyle::Printf),
        "1 1 1 4294967297 4294967297 4294967297 4294967297 4294967297"
    );

    // LLP64: int and long are 32-bit (1); long long, intmax_t, ssize_t, ptrdiff_t are 64-bit (4294967297).
    assert_eq!(
        fmt_signed.format_for_platform::<Llp64Platform>(&signed_args, FormatStyle::Printf),
        "1 1 1 1 4294967297 4294967297 4294967297 4294967297"
    );

    let fmt_unsigned = FormatString::parse_printf("%hhu %hu %u %lu %llu %ju %zu %tu %p").unwrap();
    let unsigned_args = [
        val_unsigned.clone(),
        val_unsigned.clone(),
        val_unsigned.clone(),
        val_unsigned.clone(),
        val_unsigned.clone(),
        val_unsigned.clone(),
        val_unsigned.clone(),
        val_unsigned.clone(),
        val_unsigned,
    ];

    assert_eq!(
        fmt_unsigned.format_for_platform::<Ilp32Platform>(&unsigned_args, FormatStyle::Printf),
        "1 1 1 1 4294967297 4294967297 1 1 0x1"
    );
    assert_eq!(
        fmt_unsigned.format_for_platform::<Lp64Platform>(&unsigned_args, FormatStyle::Printf),
        "1 1 1 4294967297 4294967297 4294967297 4294967297 4294967297 0x100000001"
    );
    assert_eq!(
        fmt_unsigned.format_for_platform::<Llp64Platform>(&unsigned_args, FormatStyle::Printf),
        "1 1 1 1 4294967297 4294967297 4294967297 4294967297 0x100000001"
    );

    #[cfg(target_pointer_width = "64")]
    {
        let fmt_ptr = FormatString::parse_printf("%p").unwrap();
        let ptr_arg = [Ok(Arg::Ptr(0x1_0000_0001))];
        assert_eq!(
            fmt_ptr.format_with_errors_for_platform::<Ilp32Platform, _>(
                &ptr_arg,
                FormatStyle::Printf,
                &SimpleTestError,
            ),
            "<[%p TYPE_ERROR]>"
        );
        assert_eq!(
            fmt_ptr.format_with_errors_for_platform::<Lp64Platform, _>(
                &ptr_arg,
                FormatStyle::Printf,
                &SimpleTestError,
            ),
            "0x100000001"
        );
    }
}

#[test]
fn test_parse_printf_lossy_and_strict_parity() {
    // Strict `parse_printf` rejects invalid specifiers (used by compile-time macros),
    // while `parse_printf_lossy` emits `%` literally and keeps parsing subsequent specs.
    FormatString::parse_printf("%q %d").unwrap_err();
    let lossy = FormatString::parse_printf_lossy("%q %d");
    assert_eq!(lossy.format(&[Arg::Int(5)], FormatStyle::Printf), "%q 5");

    FormatString::parse_printf("%**d %d").unwrap_err();
    let lossy_stars = FormatString::parse_printf_lossy("%**d %d");
    assert_eq!(
        lossy_stars.format(&[Arg::Int(5)], FormatStyle::Printf),
        "%**d 5"
    );

    FormatString::parse_printf("%+% %d").unwrap_err();
    let lossy_pct_flag = FormatString::parse_printf_lossy("%+% %d");
    assert_eq!(
        lossy_pct_flag.format(&[Arg::Int(5)], FormatStyle::Printf),
        "%+% 5"
    );

    for incomplete in ["%-5", "%.*", "%ll"] {
        FormatString::parse_printf(incomplete).unwrap_err();
        assert_eq!(
            FormatString::parse_printf_lossy(incomplete).format(&[], FormatStyle::Printf),
            incomplete
        );
    }

    FormatString::parse_printf("%%%").unwrap_err();
    assert_eq!(
        FormatString::parse_printf_lossy("%%%").format(&[], FormatStyle::Printf),
        "%%"
    );
}
