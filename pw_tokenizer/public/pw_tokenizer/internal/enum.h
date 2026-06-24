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

#include <cstdint>
#include <type_traits>

#include "pw_preprocessor/compiler.h"
#include "pw_tokenizer/hash.h"
#include "pw_tokenizer/tokenize.h"

namespace pw::tokenizer {

template <typename T>
constexpr bool ValidEnumerator(T) {
  static_assert(std::is_enum_v<T>, "Must be an enum");
  static_assert(sizeof(T) <= sizeof(Token), "Must fit in a token");
  return true;
}

}  // namespace pw::tokenizer

#define _PW_SEMICOLON(...) ;

// Core value tokenization macro (takes explicit domain)
#define _PW_ENUM_TOKENIZE_VALUE_IMPL(index, name, domain, enumerator, str) \
  static_assert(::pw::tokenizer::ValidEnumerator(name::enumerator));       \
  PW_TOKENIZER_DEFINE_TOKEN(                                               \
      static_cast<::pw::tokenizer::Token>(name::enumerator), domain, str)

// Enum ToString switch case macro
#define _PW_TOKENIZE_TO_STRING_CASE_IMPL(index, name, enumerator, str) \
  case name::enumerator:                                               \
    return str;

// Legacy tokenization macro (reuses core value macro with #name)
#define _PW_TOKENIZE_ENUMERATOR_IMPL(index, name, enumerator, str)         \
  static_assert(                                                           \
      #name[0] == ':' && #name[1] == ':',                                  \
      "Enum names must be fully qualified and start with ::, but \"" #name \
      "\" does not");                                                      \
  _PW_ENUM_TOKENIZE_VALUE_IMPL(index, name, #name, enumerator, str)

#define _PW_TOKENIZE_TO_STRING_CASE(index, name, enumerator) \
  _PW_TOKENIZE_TO_STRING_CASE_IMPL(index, name, enumerator, #enumerator)

// Declares an individual tokenized enum value.
#define _PW_TOKENIZE_ENUMERATOR(index, name, enumerator) \
  _PW_TOKENIZE_ENUMERATOR_IMPL(index, name, enumerator, #enumerator)

// Strip parenthesis around (enumerator, "custom string") tuples
#define _PW_CUSTOM_ENUMERATOR(enumerator, str) enumerator, str

#define _PW_TOKENIZE_TO_STRING_CASE_CUSTOM(index, name, arg) \
  _PW_TOKENIZE_TO_STRING_CASE_CUSTOM_EXPAND(                 \
      index, name, _PW_CUSTOM_ENUMERATOR arg)
#define _PW_TOKENIZE_TO_STRING_CASE_CUSTOM_EXPAND(index, name, ...) \
  _PW_TOKENIZE_TO_STRING_CASE_IMPL(index, name, __VA_ARGS__)

// Declares a tokenized custom string for an individual enum value.
#define _PW_TOKENIZE_ENUMERATOR_CUSTOM(index, name, arg) \
  _PW_TOKENIZE_ENUMERATOR_CUSTOM_EXPAND(index, name, _PW_CUSTOM_ENUMERATOR arg)
#define _PW_TOKENIZE_ENUMERATOR_CUSTOM_EXPAND(index, name, ...) \
  _PW_TOKENIZE_ENUMERATOR_IMPL(index, name, __VA_ARGS__)

// Helper macros to extract from (name, domain) tuple.
#define _PW_ENUM_EXTRACT_NAME(name, domain) name
#define _PW_ENUM_EXTRACT_DOMAIN(name, domain) domain

// Helper macros to tokenize an individual enumerator with a custom domain.
#define _PW_ENUM_TOKENIZE_VALUE_EXTRACT(index, names, arg)      \
  _PW_ENUM_TOKENIZE_VALUE_EXPAND(index,                         \
                                 _PW_ENUM_EXTRACT_NAME names,   \
                                 _PW_ENUM_EXTRACT_DOMAIN names, \
                                 _PW_CUSTOM_ENUMERATOR arg)

#define _PW_ENUM_TOKENIZE_VALUE_EXPAND(index, name, domain, ...) \
  _PW_ENUM_TOKENIZE_VALUE_IMPL(index, name, domain, __VA_ARGS__)

// Helper macros to generate a switch case for EnumToString with domain.
#define _PW_ENUM_TOKENIZE_CASE_EXTRACT(index, name, arg) \
  _PW_ENUM_TOKENIZE_CASE_EXPAND(index, name, _PW_CUSTOM_ENUMERATOR arg)

#define _PW_ENUM_TOKENIZE_CASE_EXPAND(index, name, ...) \
  _PW_TOKENIZE_TO_STRING_CASE_IMPL(index, name, __VA_ARGS__)

// Tokenizes a custom string for each given values within an enumerator, using
// a custom tokenization domain name.
// This macro can be used in the global namespace (unlike
// PW_TOKENIZE_ENUM_CUSTOM) because it specializes pw::EnumToString directly
// instead of relying on ADL.
//
// This macro is used by PW_ENUM, which needs to support versioning enums nested
// within a class definition. The standard macros would require adding a
// namespace for versioning purposes, which is not possible with nested enums.
// This macro allows PW_ENUM to version the domain directly.
#define _PW_TOKENIZE_ENUM_DOMAIN(fully_qualified_name, domain_name, ...)    \
  PW_APPLY(_PW_ENUM_TOKENIZE_VALUE_EXTRACT,                                 \
           _PW_SEMICOLON,                                                   \
           (fully_qualified_name, domain_name),                             \
           __VA_ARGS__);                                                    \
  PW_TOKENIZER_DEFINE_TOKEN(                                                \
      ::pw::tokenizer::Hash(domain_name), "enum_domain", domain_name);      \
  namespace pw::tokenizer {                                                 \
  template <>                                                               \
  constexpr uint32_t PwEnumDomainToken<fully_qualified_name>() {            \
    constexpr uint32_t kToken = ::pw::tokenizer::Hash(domain_name);         \
    return kToken;                                                          \
  }                                                                         \
  }                                                                         \
  namespace pw {                                                            \
  template <>                                                               \
  constexpr const char* EnumToString(fully_qualified_name _pw_enum_value) { \
    switch (_pw_enum_value) {                                               \
      PW_APPLY(_PW_ENUM_TOKENIZE_CASE_EXTRACT,                              \
               _PW_SEMICOLON,                                               \
               fully_qualified_name,                                        \
               __VA_ARGS__);                                                \
    }                                                                       \
    return "Unknown " #fully_qualified_name " value";                       \
  }                                                                         \
  }                                                                         \
  static_assert(true)

// Forward declaration of pw::EnumToString from pw_enum/to_string.h to allow
// specialization by the _PW_TOKENIZE_ENUM_DOMAIN macro.
namespace pw {

template <typename T>
constexpr const char* EnumToString(T value);

}  // namespace pw

// Forward declaration of pw::tokenizer::PwEnumDomainToken to allow
// specialization by the _PW_TOKENIZE_ENUM_DOMAIN macro.
namespace pw::tokenizer {

template <typename T>
constexpr uint32_t PwEnumDomainToken();

}  // namespace pw::tokenizer
