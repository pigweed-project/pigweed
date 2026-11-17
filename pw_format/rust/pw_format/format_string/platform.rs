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

//! Target platform integer and pointer width abstractions for `printf`
//! formatting.
//!
//! When formatting decoded arguments (such as 64-bit varint-decoded integers
//! from `pw_tokenizer`), `printf` conversion specifiers without an explicit
//! 64-bit length modifier (`ll` or `j`) interpret values using the native type
//! widths of the platform that produced the arguments.

use super::FormatValueError;

impl core::fmt::Display for FormatValueError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Self::MismatchedType => {
                f.write_str("conversion specifier and argument types are incompatible")
            }
            Self::ValueOutOfRange => f.write_str("format argument value out of target range"),
        }
    }
}

impl core::error::Error for FormatValueError {}

/// Trait implemented by target platform signed integer types (`i8`, `i16`,
/// `i32`, `i64`).
pub trait SignedInt: Copy + Into<i64> {
    /// Truncates a 64-bit signed value to this type's width following C
    /// integer conversion rules, and sign-extends the result back to `i64`.
    fn truncate_i64(v: i64) -> i64;
}

impl SignedInt for i8 {
    #[allow(clippy::cast_possible_truncation)]
    fn truncate_i64(v: i64) -> i64 {
        i64::from(v as i8)
    }
}

impl SignedInt for i16 {
    #[allow(clippy::cast_possible_truncation)]
    fn truncate_i64(v: i64) -> i64 {
        i64::from(v as i16)
    }
}

impl SignedInt for i32 {
    #[allow(clippy::cast_possible_truncation)]
    fn truncate_i64(v: i64) -> i64 {
        i64::from(v as i32)
    }
}

impl SignedInt for i64 {
    fn truncate_i64(v: i64) -> i64 {
        v
    }
}

/// Trait implemented by target platform unsigned integer types (`u8`, `u16`,
/// `u32`, `u64`).
pub trait UnsignedInt: Copy + Into<u64> {
    /// Truncates a 64-bit unsigned value to this type's width following C
    /// integer conversion rules, and zero-extends the result back to `u64`.
    fn truncate_u64(v: u64) -> u64;
}

impl UnsignedInt for u8 {
    #[allow(clippy::cast_possible_truncation)]
    fn truncate_u64(v: u64) -> u64 {
        u64::from(v as u8)
    }
}

impl UnsignedInt for u16 {
    #[allow(clippy::cast_possible_truncation)]
    fn truncate_u64(v: u64) -> u64 {
        u64::from(v as u16)
    }
}

impl UnsignedInt for u32 {
    #[allow(clippy::cast_possible_truncation)]
    fn truncate_u64(v: u64) -> u64 {
        u64::from(v as u32)
    }
}

impl UnsignedInt for u64 {
    fn truncate_u64(v: u64) -> u64 {
        v
    }
}

/// Trait implemented by target platform pointer-sized unsigned integer types
/// (e.g., `u32` on ILP32, `u64` on LP64/LLP64).
pub trait PointerInt: Copy + Into<u64> + TryFrom<u64> + TryFrom<usize> {
    /// Converts a host `usize` pointer address to a `u64` target pointer value,
    /// returning an error if the address exceeds the target platform's pointer
    /// range.
    ///
    /// # Errors
    ///
    /// Returns [`FormatValueError::ValueOutOfRange`] if `v` exceeds the target
    /// platform's pointer width.
    fn ptr_from_usize(v: usize) -> Result<u64, FormatValueError> {
        Self::try_from(v)
            .map(Into::into)
            .map_err(|_| FormatValueError::ValueOutOfRange)
    }

    /// Converts a 64-bit unsigned integer into the target platform's pointer
    /// width.
    fn ptr_from_u64(v: u64) -> u64;
}

impl PointerInt for u32 {
    #[allow(clippy::cast_possible_truncation)]
    fn ptr_from_u64(v: u64) -> u64 {
        u64::from(v as u32)
    }
}

impl PointerInt for u64 {
    fn ptr_from_u64(v: u64) -> u64 {
        v
    }
}

/// Describes the native C/C++ integer and pointer types of the producing
/// platform for all `printf` conversion specifiers and length modifiers.
pub trait PlatformTypes {
    /// Native `signed char` type (`hh` signed conversions, e.g., `i8`).
    type SignedChar: SignedInt;
    /// Native `unsigned char` type (`hh` unsigned conversions, e.g., `u8`).
    type UnsignedChar: UnsignedInt;

    /// Native `short` type (`h` signed conversions, e.g., `i16`).
    type SignedShort: SignedInt;
    /// Native `unsigned short` type (`h` unsigned conversions, e.g., `u16`).
    type UnsignedShort: UnsignedInt;

    /// Native `int` type (default signed conversions without length modifier,
    /// e.g., `i32`).
    type SignedInt: SignedInt;
    /// Native `unsigned int` type (default unsigned conversions without length
    /// modifier, e.g., `u32`).
    type UnsignedInt: UnsignedInt;

    /// Native `long` type (`l` signed conversions, e.g., `i32` on ILP32/LLP64,
    /// `i64` on LP64).
    type SignedLong: SignedInt;
    /// Native `unsigned long` type (`l` unsigned conversions, e.g., `u32` on
    /// ILP32/LLP64, `u64` on LP64).
    type UnsignedLong: UnsignedInt;

    /// Native `long long` type (`ll` signed conversions, e.g., `i64`).
    type SignedLongLong: SignedInt;
    /// Native `unsigned long long` type (`ll` unsigned conversions, e.g.,
    /// `u64`).
    type UnsignedLongLong: UnsignedInt;

    /// Native `intmax_t` type (`j` signed conversions, e.g., `i64`).
    type IntMax: SignedInt;
    /// Native `uintmax_t` type (`j` unsigned conversions, e.g., `u64`).
    type UIntMax: UnsignedInt;

    /// Native signed `size_t` / `ssize_t` type (`z` signed conversions, e.g.,
    /// `i32` on ILP32, `i64` on LP64/LLP64).
    type SignedSize: SignedInt;
    /// Native `size_t` type (`z` unsigned conversions, e.g., `u32` on ILP32,
    /// `u64` on LP64/LLP64).
    type Size: UnsignedInt;

    /// Native `ptrdiff_t` type (`t` signed conversions, e.g., `i32` on ILP32,
    /// `i64` on LP64/LLP64).
    type PointerDiff: SignedInt;
    /// Native unsigned `ptrdiff_t` type (`t` unsigned conversions, e.g., `u32`
    /// on ILP32, `u64` on LP64/LLP64).
    type UnsignedPointerDiff: UnsignedInt;

    /// Native pointer integer type (`%p` conversions / `uintptr_t`, e.g., `u32`
    /// on ILP32, `u64` on LP64/LLP64).
    type Pointer: PointerInt;
}

/// Standard ILP32 platform types (32-bit `int`, `long`, `size_t`, `ptrdiff_t`,
/// and 32-bit pointers; 64-bit `long long` and `intmax_t`), matching Pigweed's
/// 32-bit embedded targets and default detokenizer conventions.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Ilp32Platform;

impl PlatformTypes for Ilp32Platform {
    type SignedChar = i8;
    type UnsignedChar = u8;
    type SignedShort = i16;
    type UnsignedShort = u16;
    type SignedInt = i32;
    type UnsignedInt = u32;
    type SignedLong = i32;
    type UnsignedLong = u32;
    type SignedLongLong = i64;
    type UnsignedLongLong = u64;
    type IntMax = i64;
    type UIntMax = u64;
    type SignedSize = i32;
    type Size = u32;
    type PointerDiff = i32;
    type UnsignedPointerDiff = u32;
    type Pointer = u32;
}

/// Standard LP64 platform types (32-bit `int`; 64-bit `long`, `long long`,
/// `intmax_t`, `size_t`, `ptrdiff_t`, and 64-bit pointers), matching 64-bit
/// Unix/Linux/macOS targets.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Lp64Platform;

impl PlatformTypes for Lp64Platform {
    type SignedChar = i8;
    type UnsignedChar = u8;
    type SignedShort = i16;
    type UnsignedShort = u16;
    type SignedInt = i32;
    type UnsignedInt = u32;
    type SignedLong = i64;
    type UnsignedLong = u64;
    type SignedLongLong = i64;
    type UnsignedLongLong = u64;
    type IntMax = i64;
    type UIntMax = u64;
    type SignedSize = i64;
    type Size = u64;
    type PointerDiff = i64;
    type UnsignedPointerDiff = u64;
    type Pointer = u64;
}

/// Standard LLP64 platform types (32-bit `int` and `long`; 64-bit `long long`,
/// `intmax_t`, `size_t`, `ptrdiff_t`, and 64-bit pointers), matching 64-bit
/// Windows targets.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Llp64Platform;

impl PlatformTypes for Llp64Platform {
    type SignedChar = i8;
    type UnsignedChar = u8;
    type SignedShort = i16;
    type UnsignedShort = u16;
    type SignedInt = i32;
    type UnsignedInt = u32;
    type SignedLong = i32;
    type UnsignedLong = u32;
    type SignedLongLong = i64;
    type UnsignedLongLong = u64;
    type IntMax = i64;
    type UIntMax = u64;
    type SignedSize = i64;
    type Size = u64;
    type PointerDiff = i64;
    type UnsignedPointerDiff = u64;
    type Pointer = u64;
}
