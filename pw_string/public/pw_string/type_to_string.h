// Copyright 2019 The Pigweed Authors
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
#pragma once

// This file provides functions for writing string representations of a few
// types to character buffers. Generally, the generic ToString function defined
// in "pw_string/to_string.h" should be used instead of these functions.

#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include "lib/stdcompat/bit.h"
#include "pw_span/span.h"
#include "pw_status/status_with_size.h"
#include "pw_string/util.h"

namespace pw::string {

// Returns the number of digits in the decimal representation of the provided
// non-negative integer. Returns 1 for 0 or 1 + log base 10 for other numbers.
constexpr uint_fast8_t DecimalDigitCount(uint64_t integer);

// Returns the number of digits in the hexadecimal representation of the
// provided non-negative integer.
constexpr uint_fast8_t HexDigitCount(uint64_t integer) {
  return static_cast<uint_fast8_t>((64 - cpp20::countl_zero(integer | 1u) + 3) /
                                   4);
}

// Writes an integer as a null-terminated string in base 10. Returns the number
// of characters written, excluding the null terminator, and the status.
//
// Numbers are never truncated; if the entire number does not fit, only a null
// terminator is written and the status is RESOURCE_EXHAUSTED.
//
// IntToString is templated, but a single 64-bit integer implementation is used
// for all integer types. The template is used for two reasons:
//
//   1. IntToString(int64_t) and IntToString(uint64_t) overloads are ambiguous
//      when called with types other than int64_t or uint64_t. Using the
//      template allows IntToString to be called with any integral type.
//
//   2. Templating IntToString allows the compiler to emit small functions like
//      IntToString<int> or IntToString<short> that perform casting / sign
//      extension on the various integer types. This saves code size, since call
//      sites pass their arguments directly and casting instructions are shared.
//
template <typename T>
constexpr StatusWithSize IntToString(T value, span<char> buffer) {
  if constexpr (std::is_signed_v<T>) {
    return IntToString<int64_t>(value, buffer);
  } else {
    return IntToString<uint64_t>(value, buffer);
  }
}

// Writes an integer as a hexadecimal string. Semantics match IntToString. The
// output is lowercase without a leading 0x. min_width adds leading zeroes such
// that the final string is at least the specified number of characters wide.
StatusWithSize IntToHexString(uint64_t value,
                              span<char> buffer,
                              uint_fast8_t min_width = 0);

// Rounds a floating point number to an integer and writes it as a
// null-terminated string. Returns the number of characters written, excluding
// the null terminator, and the status.
//
// Numbers are never truncated; if the entire number does not fit, only a null
// terminator is written and the status is RESOURCE_EXHAUSTED.
//
// WARNING: This is NOT a fully-functioning float-printing implementation! It
// simply outputs the closest integer, "inf", or "NaN". Floating point numbers
// too large to represent as a 64-bit int are treated as infinite.
//
// Examples:
//
//   FloatAsIntToString(1.25, buffer)     -> writes "1" to the buffer
//   FloatAsIntToString(-4.9, buffer)     -> writes "-5" to the buffer
//   FloatAsIntToString(3.5e20, buffer)   -> writes "inf" to the buffer
//   FloatAsIntToString(INFINITY, buffer) -> writes "-inf" to the buffer
//   FloatAsIntToString(-NAN, buffer)     -> writes "-NaN" to the buffer
//
StatusWithSize FloatAsIntToString(float value, span<char> buffer);

// Writes a bool as "true" or "false". Semantics match CopyEntireString.
StatusWithSize BoolToString(bool value, span<char> buffer);

// String used to represent null pointers.
inline constexpr std::string_view kNullPointerString("(null)");

// Writes the pointer's address or kNullPointerString. Semantics match
// CopyEntireString.
StatusWithSize PointerToString(const void* pointer, span<char> buffer);

// Specialized form of pw::string::Copy which supports nullptr values.
//
// Copies the string to the buffer, truncating if the full string does not fit.
// Always null terminates if buffer.size() > 0.
//
// If value is a nullptr, then "(null)" is used as a fallback.
//
// Returns the number of characters written, excluding the null terminator. If
// the string is truncated, the status is RESOURCE_EXHAUSTED.
inline StatusWithSize CopyStringOrNull(std::string_view value,
                                       span<char> buffer) {
  return Copy(value, buffer);
}
inline StatusWithSize CopyStringOrNull(const char* value, span<char> buffer) {
  if (value == nullptr) {
    return PointerToString(value, buffer);
  }
  return Copy(value, buffer);
}

// Copies the string to the buffer, if the entire string fits. Always null
// terminates if buffer.size() > 0.
//
// If value is a nullptr, then "(null)" is used as a fallback.
//
// Returns the number of characters written, excluding the null terminator. If
// the full string does not fit, only a null terminator is written and the
// status is RESOURCE_EXHAUSTED.
StatusWithSize CopyEntireStringOrNull(std::string_view value,
                                      span<char> buffer);

// Same as the string_view form of CopyEntireString, except that if value is a
// nullptr, then "(null)" is used as a fallback.
inline StatusWithSize CopyEntireStringOrNull(const char* value,
                                             span<char> buffer) {
  if (value == nullptr) {
    return PointerToString(value, buffer);
  }
  return CopyEntireStringOrNull(std::string_view(value), buffer);
}

// This function is a fallback that is called if by ToString if no overload
// matches. No definition is provided, so attempting to print an unsupported
// type causes a linker error.
//
// Applications may define pw::string::UnknownTypeToString to support generic
// printing for unknown types, if desired. Implementations must follow the
// ToString semantics.
template <typename T>
StatusWithSize UnknownTypeToString(const T& value, span<char> buffer);

// Implementations
namespace internal {

// Powers of 10 (except 0) as an array. This table is fairly large (160 B), but
// avoids having to recalculate these values for each DecimalDigitCount call.
inline constexpr std::array<uint64_t, 20> kPowersOf10{
    0ull,
    10ull,                    // 10^1
    100ull,                   // 10^2
    1000ull,                  // 10^3
    10000ull,                 // 10^4
    100000ull,                // 10^5
    1000000ull,               // 10^6
    10000000ull,              // 10^7
    100000000ull,             // 10^8
    1000000000ull,            // 10^9
    10000000000ull,           // 10^10
    100000000000ull,          // 10^11
    1000000000000ull,         // 10^12
    10000000000000ull,        // 10^13
    100000000000000ull,       // 10^14
    1000000000000000ull,      // 10^15
    10000000000000000ull,     // 10^16
    100000000000000000ull,    // 10^17
    1000000000000000000ull,   // 10^18
    10000000000000000000ull,  // 10^19
};

constexpr StatusWithSize HandleExhaustedBuffer(span<char> buffer) {
  if (!buffer.empty()) {
    buffer[0] = '\0';
  }
  return StatusWithSize::ResourceExhausted();
}

}  // namespace internal

/// @submodule{pw_string,util}

constexpr uint_fast8_t DecimalDigitCount(uint64_t integer) {
  // This fancy piece of code takes the log base 2, then approximates the
  // change-of-base formula by multiplying by 1233 / 4096.
  const uint_fast8_t log_10 = static_cast<uint_fast8_t>(
      (64 - cpp20::countl_zero(integer | 1)) * 1233 >> 12);

  // Adjust the estimated log base 10 by comparing against the power of 10.
  return static_cast<uint_fast8_t>(
      log_10 + (integer < internal::kPowersOf10[log_10] ? 0u : 1u));
}

// std::to_chars is available for integers in recent versions of GCC. I looked
// into switching to std::to_chars instead of this implementation. std::to_chars
// increased binary size by 160 B on an -Os build (even after removing
// DecimalDigitCount and its table). I didn't measure performance, but I don't
// think std::to_chars will be faster, so I kept this implementation for now.
template <>
constexpr StatusWithSize IntToString(uint64_t value, span<char> buffer) {
  constexpr uint32_t base = 10;
  constexpr uint32_t max_uint32_base_power = 1'000'000'000;
  constexpr uint_fast8_t max_uint32_base_power_exponent = 9;

  const uint_fast8_t total_digits = DecimalDigitCount(value);

  if (total_digits >= buffer.size()) {
    return internal::HandleExhaustedBuffer(buffer);
  }

  buffer[total_digits] = '\0';

  uint_fast8_t remaining = total_digits;
  while (remaining > 0u) {
    uint32_t lower_digits = 0;     // the value of the lower digits to write
    uint_fast8_t digit_count = 0;  // the number of lower digits to write

    // 64-bit division is slow on 32-bit platforms, so print large numbers in
    // 32-bit chunks to minimize the number of 64-bit divisions.
    if (value <= std::numeric_limits<uint32_t>::max()) {
      lower_digits = static_cast<uint32_t>(value);
      digit_count = remaining;
    } else {
      lower_digits = static_cast<uint32_t>(value % max_uint32_base_power);
      digit_count = max_uint32_base_power_exponent;
      value /= max_uint32_base_power;
    }

    // Write the specified number of digits, with leading 0s.
    for (uint_fast8_t i = 0; i < digit_count; ++i) {
      buffer[--remaining] = static_cast<char>(lower_digits % base + '0');
      lower_digits /= base;
    }
  }
  return StatusWithSize(total_digits);
}

template <>
constexpr StatusWithSize IntToString(int64_t value, span<char> buffer) {
  if (value >= 0) {
    return IntToString<uint64_t>(static_cast<uint64_t>(value), buffer);
  }

  // Write as an unsigned number, but leave room for the leading minus sign.
  // Do not use std::abs since it fails for the minimum value integer.
  const uint64_t absolute_value = -static_cast<uint64_t>(value);
  auto result = IntToString<uint64_t>(
      absolute_value, buffer.empty() ? buffer : buffer.subspan(1));

  if (result.ok()) {
    buffer[0] = '-';
    return StatusWithSize(result.size() + 1);
  }

  return internal::HandleExhaustedBuffer(buffer);
}

}  // namespace pw::string
