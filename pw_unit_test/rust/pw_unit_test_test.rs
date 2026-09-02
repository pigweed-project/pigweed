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

pub fn add(left: u64, right: u64) -> u64 {
    left + right
}

#[derive(Debug, PartialEq, Eq)]
struct TestStruct {
    a: u32,
    b: &'static str,
}

#[cfg(test)]
mod tests {
    use core::ffi::CStr;
    use core::fmt::Write;

    use pw_unit_test::{assert, assert_eq, assert_ne, test, StringBuffer};

    use super::*;

    fn helper_assert_positive(x: i32) {
        assert!(x > 0, "expected {x} to be positive");
    }

    fn helper_assert_equal(a: u32, b: u32) {
        assert_eq!(a, b);
    }

    #[test]
    fn test_add() {
        let result = add(2, 2);
        assert_eq!(result, 4);
    }

    #[test]
    fn test_assert_basic() {
        assert!(true);
        assert!(1 + 1 == 2);
        assert!(!false);
    }

    #[test]
    fn test_assert_with_message() {
        let x = 42;
        assert!(x == 42, "x should be 42");
        assert!(x > 0, "x ({x}) should be positive");
    }

    #[test]
    fn test_assert_eq_integers() {
        assert_eq!(42, 42);
        assert_eq!(0u8, 0u8);
        assert_eq!(-10i32, -10i32);
    }

    #[test]
    fn test_assert_eq_strings_and_slices() {
        assert_eq!("hello", "hello");
        assert_eq!(&[1, 2, 3], &[1, 2, 3]);
    }

    #[test]
    fn test_assert_eq_structs() {
        let s1 = TestStruct {
            a: 10,
            b: "pigweed",
        };
        let s2 = TestStruct {
            a: 10,
            b: "pigweed",
        };
        assert_eq!(s1, s2);
    }

    #[test]
    fn test_assert_ne_basic() {
        assert_ne!(1, 2);
        assert_ne!("foo", "bar");
        let s1 = TestStruct {
            a: 10,
            b: "pigweed",
        };
        let s2 = TestStruct {
            a: 20,
            b: "pigweed",
        };
        assert_ne!(s1, s2);
    }

    #[test]
    fn test_helper_functions() {
        helper_assert_positive(100);
        helper_assert_equal(5, 5);
    }

    #[test]
    fn test_closure_assertions() {
        let values = [1, 2, 3, 4, 5];
        for &val in &values {
            assert!(val > 0);
            assert_eq!(val * 2, val + val);
        }
    }

    #[test]
    fn test_message_formatting_and_utf8_truncation() {
        let mut msg = StringBuffer::<16>::new();
        // 16 bytes total capacity means maximum 15 payload bytes + 1 null terminator.
        let _ = msg.write_str("1234567890");
        let c_str = unsafe { CStr::from_ptr(msg.as_c_str()) };
        assert_eq!(c_str.to_str(), Ok("1234567890"));

        // Test multi-byte character truncation at buffer boundary.
        // '€' is 3 bytes (0xE2, 0x82, 0xAC).
        // 8 bytes total capacity means maximum 7 payload bytes + 1 null terminator.
        let mut small_msg = StringBuffer::<8>::new();

        // "ab" (2 bytes) + "€" (3 bytes) = 5 bytes (fits).
        // Adding another "€" (3 bytes) would be 8 bytes, exceeding 7 payload bytes.
        // It should safely truncate and NOT split the second '€'.
        let _ = small_msg.write_str("ab€€");
        let small_c_str = unsafe { CStr::from_ptr(small_msg.as_c_str()) };

        // Validates that it decoded as valid UTF-8 AND truncated cleanly to "ab€" (5 bytes).
        assert_eq!(small_c_str.to_str(), Ok("ab€"));
        assert_eq!(small_c_str.to_bytes().len(), 5);
    }
}
