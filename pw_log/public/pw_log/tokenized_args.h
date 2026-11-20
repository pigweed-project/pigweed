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
#pragma once

#include <stddef.h>

#include "pw_preprocessor/arguments.h"

#ifdef __cplusplus
#include <type_traits>

#include "pw_enum/to_string.h"
#include "pw_tokenizer/enum.h"
#endif  // __cplusplus

/// @submodule{pw_log,tokenized_args}

/// Format specifier for logging a `PW_TOKENIZE_ENUM` enum with `PW_LOG_ENUM`.
///
/// If nested tokenization is supported by the logging backend, this expands to
/// a nested token format specifier using `#enum` as the domain. For
/// non-tokenizing backends, defaults to `%s`.
///
/// @note `pw_enum` types use versioned tokenizer domains. Log them with the
/// generated `<ENUM>_FMT` macro or `PW_LOG_ENUM_VERSIONED_FMT()` instead.
#define PW_LOG_ENUM_FMT(enum) PW_LOG_TOKEN_FMT(#enum)

// pw_log backends that use pw_tokenizer and want to support nested tokenization
// define this file under their public_overrides/ directory to activate the
// PW_LOG_TOKEN aliases. If this file does not exist in the log backend,
// arguments behave as basic strings (const char*).
#if __has_include("pw_log_backend/log_backend_uses_pw_tokenizer.h")

#include "pw_tokenizer/nested_tokenization.h"
#include "pw_tokenizer/tokenize.h"

#define PW_LOG_TOKEN_TYPE pw_tokenizer_Token
#define PW_LOG_TOKEN_DEFAULT_VALUE ((pw_tokenizer_Token)0)
#define PW_LOG_TOKEN PW_TOKENIZE_STRING
#define PW_LOG_TOKEN_EXPR PW_TOKENIZE_STRING_EXPR
#define PW_LOG_TOKEN_FMT PW_TOKEN_FMT
#define PW_LOG_ENUM(...) \
  ::pw::tokenizer::EnumToToken(::pw::log::internal::ToEnum(__VA_ARGS__))
#define PW_LOG_NESTED_TOKEN_FMT PW_NESTED_TOKEN_FMT
#define PW_LOG_ENUM_VERSIONED_FMT(...) \
  PW_TOKENIZER_ENUM_DOMAIN_AND_VALUE_FMT(__VA_ARGS__)
#define PW_LOG_ENUM_VERSIONED(...)                                         \
  ::pw::tokenizer::EnumDomainToken<                                        \
      std::decay_t<decltype(::pw::log::internal::ToEnum(__VA_ARGS__))>>(), \
      PW_LOG_ENUM(__VA_ARGS__)

#else

/// If nested tokenization is supported by the logging backend, this is an
/// alias for `pw_tokenizer_Token`.
///
/// For non-tokenizing backends, defaults to `const char*`.
#define PW_LOG_TOKEN_TYPE const char*

/// An "empty" value for a token.
///
/// If nested tokenization is supported by the logging backend, this is `0`.
/// Otherwise, it is `nullptr`.
#ifdef __cplusplus
#define PW_LOG_TOKEN_DEFAULT_VALUE nullptr
#else
#define PW_LOG_TOKEN_DEFAULT_VALUE NULL
#endif  // __cplusplus

/// If nested tokenization is supported by the logging backend, this is an
/// alias for `PW_TOKENIZE_STRING`. No-op otherwise.
#define PW_LOG_TOKEN(...) \
  PW_DELEGATE_BY_ARG_COUNT(_PW_STRING_OPTIONAL_DOMAIN_, __VA_ARGS__)

#define _PW_STRING_OPTIONAL_DOMAIN_1(string_literal) string_literal
#define _PW_STRING_OPTIONAL_DOMAIN_2(domain, string_literal) string_literal

/// If nested tokenization is supported by the logging backend, this is an
/// alias for `PW_TOKENIZE_STRING_EXPR`. No-op otherwise.
#define PW_LOG_TOKEN_EXPR(...) \
  PW_DELEGATE_BY_ARG_COUNT(_PW_STRING_OPTIONAL_DOMAIN_, __VA_ARGS__)

/// If nested tokenization is supported by the logging backend, this is an
/// alias for `PW_TOKEN_FMT`.
///
/// For non-tokenizing backends, defaults to the string specifier `%s`.
#define PW_LOG_TOKEN_FMT(...) "%s"

/// Converts an enum to the log argument for a static-domain format specifier
/// (such as a generated `<ENUM>_FMT` macro). Evaluates its argument once.
///
/// If nested tokenization is supported by the logging backend, returns the
/// 32-bit enum token. For non-tokenizing backends, returns the string name of
/// the enumerator (`const char*`).
///
/// With `pw_log/status.h` included, also accepts statuses with enum codes,
/// `pw::StatusWithSize`, and `pw::Result`, and logs their status code.
#define PW_LOG_ENUM(...) \
  ::pw::EnumToString(::pw::log::internal::ToEnum(__VA_ARGS__))

/// If nested tokenization is supported by the logging backend, this is an
/// alias for `PW_NESTED_TOKEN_FMT`.
///
/// For non-tokenizing backends, defaults to `%s::%s`.
#define PW_LOG_NESTED_TOKEN_FMT(...) "%s::%s"

/// Type-agnostic format specifier for logging a versioned enum or status code
/// with `PW_LOG_ENUM_VERSIONED`.
///
/// On both tokenizing and non-tokenizing backends, formats as the enumerator
/// string (for example, `"SECOND"` or `"NOT_FOUND"`).
#define PW_LOG_ENUM_VERSIONED_FMT(...) "%s"

/// Converts an enum to the arguments needed to log its enumerator with
/// `PW_LOG_ENUM_VERSIONED_FMT()`. Accepts the same types as `PW_LOG_ENUM` and
/// evaluates its argument once.
///
/// If nested tokenization is supported by the logging backend, expands to two
/// arguments: the 32-bit versioned domain token and the enum value token. For
/// non-tokenizing backends, expands to the enumerator string (`const char*`).
#define PW_LOG_ENUM_VERSIONED(...) PW_LOG_ENUM(__VA_ARGS__)

#endif  // __has_include("pw_log_backend/log_backend_uses_pw_tokenizer.h")

#ifdef __cplusplus

namespace pw::log {

/// Type of a log token. Either a `const char*` or a `pw::tokenizer::Token`.
/// Use in C++ instead of `PW_LOG_TOKEN_TYPE`.
using Token = PW_LOG_TOKEN_TYPE;

/// Default value of a log token. This token should not be logged.
inline constexpr Token kDefaultToken = PW_LOG_TOKEN_DEFAULT_VALUE;

}  // namespace pw::log

#endif  // __cplusplus

/// @}

#ifdef __cplusplus

namespace pw::log::internal {

// Provides the enum that PW_LOG_ENUM logs for an argument of type T.
// pw_log/status.h specializes this for statuses.
template <typename T, typename = void>
struct LoggedEnum {
  static constexpr T Get(T value) {
    static_assert(std::is_enum_v<T>,
                  "PW_LOG_ENUM requires an enum. To log the code of a status, "
                  "StatusWithSize, or Result, include pw_log/status.h.");
    return value;
  }
};

// Enums are passed by value, so PW_LOG_ENUM does not odr-use its argument and
// accepts volatile enums.
template <typename T, std::enable_if_t<std::is_enum_v<T>, int> = 0>
constexpr T ToEnum(T value) {
  return LoggedEnum<T>::Get(value);
}

template <typename T, std::enable_if_t<!std::is_enum_v<T>, int> = 0>
constexpr auto ToEnum(const T& value) {
  return LoggedEnum<T>::Get(value);
}

}  // namespace pw::log::internal

#endif  // __cplusplus
