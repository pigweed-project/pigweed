// Copyright 2024 The Pigweed Authors
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

#include "pw_preprocessor/apply.h"
#include "pw_tokenizer/internal/enum.h"
#include "pw_tokenizer/tokenize.h"

namespace pw::tokenizer {

/// @submodule{pw_tokenizer,tokenize}

/// @brief Tokenizes a given enumerator value. Used in the vase of a tokenizing
/// log backend.
/// @param value enumerator value
/// @return The 32-bit token (`pw_tokenizer_Token`)
template <typename T>
constexpr auto EnumToToken(T value) {
  static_assert(std::is_enum_v<T>, "Must be an enum");
  static_assert(sizeof(T) <= sizeof(Token), "Must fit in a token");
  return static_cast<Token>(value);
}

/// @brief Returns a string representation of a given enumerator value name.
/// @deprecated Use `pw::EnumToString` from pw_enum/to_string.h instead.
template <typename T>
[[deprecated("Use pw::EnumToString from pw_enum/to_string.h instead")]]
constexpr const char* EnumToString(T value) {
  return PwEnumToString(value);
}

/// @}

}  // namespace pw::tokenizer

/// @submodule{pw_tokenizer,tokenize}

/// Tokenizes the given values within an enumerator. All values of the
/// enumerator must be present to compile and have the enumerator be tokenized
/// successfully.
/// This macro should be in the same namespace as the enum declaration to use
/// the `pw::EnumToString` function and avoid compilation errors.
#define PW_TOKENIZE_ENUM(fully_qualified_name, ...)      \
  PW_APPLY(_PW_TOKENIZE_ENUMERATOR,                      \
           _PW_SEMICOLON,                                \
           fully_qualified_name,                         \
           __VA_ARGS__);                                 \
  [[maybe_unused]] constexpr const char* PwEnumToString( \
      fully_qualified_name _pw_enum_value) {             \
    switch (_pw_enum_value) {                            \
      PW_APPLY(_PW_TOKENIZE_TO_STRING_CASE,              \
               _PW_SEMICOLON,                            \
               fully_qualified_name,                     \
               __VA_ARGS__);                             \
    }                                                    \
    return "Unknown " #fully_qualified_name " value";    \
  }                                                      \
  static_assert(true)

/// Tokenizes a custom string for each given values within an enumerator. All
/// values of the enumerator must be followed by a custom string as a tuple
/// (value, "string"). All values of the enumerator (and their associated
/// custom string) must be present to compile and have the custom strings be
/// tokenized successfully.
/// This macro should be in the same namespace as the enum declaration to use
/// the `pw::EnumToString` function and avoid compilation errors.
#define PW_TOKENIZE_ENUM_CUSTOM(fully_qualified_name, ...) \
  PW_APPLY(_PW_TOKENIZE_ENUMERATOR_CUSTOM,                 \
           _PW_SEMICOLON,                                  \
           fully_qualified_name,                           \
           __VA_ARGS__);                                   \
  [[maybe_unused]] constexpr const char* PwEnumToString(   \
      fully_qualified_name _pw_enum_value) {               \
    switch (_pw_enum_value) {                              \
      PW_APPLY(_PW_TOKENIZE_TO_STRING_CASE_CUSTOM,         \
               _PW_SEMICOLON,                              \
               fully_qualified_name,                       \
               __VA_ARGS__);                               \
    }                                                      \
    return "Unknown " #fully_qualified_name " value";      \
  }                                                        \
  static_assert(true)

/// @}
