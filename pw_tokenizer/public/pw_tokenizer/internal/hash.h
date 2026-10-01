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
#pragma once

// Note: This header intentionally avoids C++ standard library headers like
// <string_view> to prevent circular include issues when pw_tokenizer/tokenize.h
// is transitively included by <assert.h> (e.g. via pw_assert_tokenized or
// pw_assert_log with pw_log_tokenized) inside C library headers such as
// Newlib's <stdlib.h> / <wchar.h> -> <sys/reent.h>.

#include <stddef.h>
#include <stdint.h>

#include "pw_preprocessor/compiler.h"
#include "pw_tokenizer/config.h"

namespace pw::tokenizer {

// The constant to use when generating the hash. Changing this changes the value
// of all hashes, so do not change it randomly.
inline constexpr uint32_t k65599HashConstant = 65599u;

namespace internal {

// Calculates the hash of a string up to a maximum length.
constexpr uint32_t PwTokenizer65599FixedLengthHash(
    const char* string,
    size_t string_length,
    size_t hash_length = PW_TOKENIZER_CFG_C_HASH_LENGTH)
    PW_NO_SANITIZE("unsigned-integer-overflow") {
  // The length is hashed as if it were the first character.
  uint32_t hash = static_cast<uint32_t>(string_length);
  uint32_t coefficient = k65599HashConstant;

  const size_t length =
      string_length < hash_length ? string_length : hash_length;

  // Hash all of the characters in the string as unsigned ints.
  // The coefficient calculation is done modulo 0x100000000, so the unsigned
  // integer overflows are intentional.
  for (size_t i = 0; i < length; ++i) {
    hash += coefficient * static_cast<uint8_t>(string[i]);
    coefficient *= k65599HashConstant;
  }

  return hash;
}

// Calculates the hash of a string of any length.
constexpr uint32_t Hash(const char* string, size_t string_length)
    PW_NO_SANITIZE("unsigned-integer-overflow") {
  return PwTokenizer65599FixedLengthHash(string, string_length, string_length);
}

// Take the string as an array to support either literals or character arrays,
// but not const char*.
template <size_t kSize>
constexpr uint32_t Hash(const char (&string)[kSize]) {
  static_assert(kSize > 0u);
  return Hash(string, kSize - 1);
}

}  // namespace internal
}  // namespace pw::tokenizer
