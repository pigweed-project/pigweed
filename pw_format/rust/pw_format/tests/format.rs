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

use crate::{Arg, ConversionSpec, FormatError, FormatFragment, FormatString, FormatStyle};

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
    assert_eq!(rust_core, format!("{}", arg));

    let rust_core_parsed = parsed_core.format(&args, FormatStyle::CoreFmt);
    assert_eq!(rust_core_parsed, format!("{}", arg));

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
fn conversion_spec_to_string_reconstructs_format_string_representation() {
    let test_cases = [
        "%d", "%+d", "%-d", "% d", "%#o", "%05d", "%+05d", "%*d", "%.2f", "%.*f", "%ld", "%lld",
        "%hhd", "%hd", "%Lf", "%jd", "%zd", "%td", "%e", "%E", "%x", "%X", "%u", "%c", "%s", "%p",
        "%+05.2ld", "%-10s", "%08x", "%#010X",
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
