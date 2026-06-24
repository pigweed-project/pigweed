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

#include <type_traits>

#include "pw_enum/to_string.h"
#include "pw_tokenizer/enum.h"

/// @submodule{pw_log,tokenized_args}

/// If nested tokenization is supported by the logging backend, this is a
/// format specifier to declare a nested token with a specific domain value.
///
/// For non-tokenizing backends, defaults to the string specifier `%s`.
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
#define PW_LOG_ENUM(enumerator) ::pw::tokenizer::EnumToToken(enumerator)
#define PW_LOG_NESTED_TOKEN_FMT PW_NESTED_TOKEN_FMT
#define PW_LOG_ENUM_VERSIONED_FMT PW_LOG_NESTED_TOKEN_FMT
#define PW_LOG_ENUM_VERSIONED(enumerator) \
  PW_TOKENIZER_ENUM_DOMAIN_AND_VALUE(enumerator)

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
/// alias for `PW_TOKEN_FORMAT`.
///
/// For non-tokenizing backends, defaults to the string specifier `%s`.
#define PW_LOG_TOKEN_FMT(...) "%s"

/// If nested tokenization is supported by the logging backend, this will
/// return a token representation of the enum.
///
/// For non-tokenizing backends, defaults to the string representation of the
/// enum.
#define PW_LOG_ENUM(enumerator) ::pw::EnumToString(enumerator)

/// If nested tokenization is supported by the logging backend, this is an
/// alias for `PW_NESTED_TOKEN_FMT`.
///
/// For non-tokenizing backends, defaults to the string specifier `%s`.
#define PW_LOG_NESTED_TOKEN_FMT(...) "%s::%s"

/// Format specifier for PW_LOG_ENUM_VERSIONED.
///
/// Aliases PW_LOG_NESTED_TOKEN_FMT.
#define PW_LOG_ENUM_VERSIONED_FMT PW_LOG_NESTED_TOKEN_FMT

/// Returns the arguments needed to log a versioned enum.
///
/// These arguments are intended to be formatted using
/// `PW_LOG_ENUM_VERSIONED_FMT`.
///
/// If nested tokenization is supported by the logging backend, this returns
/// the versioned domain token along with the token representation of the enum.
///
/// For non-tokenizing backends, defaults to the domain name along with the
/// string representation of the enum.
#define PW_LOG_ENUM_VERSIONED(enumerator)                       \
  ::pw::PwEnumDomainName<std::decay_t<decltype(enumerator)>>(), \
      PW_LOG_ENUM(enumerator)

#endif  //__has_include("log_backend/log_backend_uses_pw_tokenizer.h")

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
