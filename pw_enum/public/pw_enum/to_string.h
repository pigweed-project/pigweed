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

#include <type_traits>

/// @module{pw_enum}

namespace pw {

/// Returns a string representation of a given enumerator.
///
/// This function relies on the FTADLE pattern (https://abseil.io/tips/218) to
/// define extension points. To add `EnumToString` support to an enum, implement
/// the `PwEnumToString` function in the same namespace as the enum. This is
/// typically done with the `PW_TOKENIZE_ENUM` or `PW_TOKENIZE_ENUM_CUSTOM`
/// macros from `pw_enum/tokenize.h`.
template <typename T>
constexpr const char* EnumToString(T value) {
  static_assert(std::is_enum_v<T>, "Must be an enum");
  return PwEnumToString(value);
}

/// Returns the domain name of the given enum type.
///
/// This serves as a fallback name (defaulting to `"Enum"`) for non-tokenizing
/// logging backends to prefix the enum's string representation.
///
/// By default, this function returns `"Enum"`. If an enum uses a custom
/// tokenization domain and you want non-tokenizing logging backends to prefix
/// the enum value with that custom domain name instead of `"Enum"`, you can
/// provide a custom template specialization:
///
/// @code
/// template <>
/// constexpr const char* pw::PwEnumDomainName<MyEnum>() {
///   return "CustomDomain";
/// }
/// @endcode
template <typename T>
constexpr const char* PwEnumDomainName() {
  static_assert(std::is_enum_v<T>, "Must be an enum");
  return "Enum";
}

}  // namespace pw

/// @}
