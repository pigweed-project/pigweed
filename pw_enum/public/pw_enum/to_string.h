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
#include <utility>

#include "pw_enum/traits.h"

/// @module{pw_enum}

namespace pw {
namespace internal {

// Uncallable overload so unqualified `PwEnumToString` lookup succeeds in Phase
// 1 and defers to ADL in Phase 2.
void PwEnumToString() = delete;

template <typename T, typename = void>
inline constexpr bool has_adl_enum_to_string_v = false;

template <typename T>
inline constexpr bool has_adl_enum_to_string_v<
    T,
    std::void_t<decltype(PwEnumToString(std::declval<T>()))>> =
    std::is_enum_v<T>;

template <typename T>
constexpr const char* CallAdlPwEnumToString(T value) {
  return PwEnumToString(value);
}

}  // namespace internal

/// True if `pw::EnumToString` is supported for `T` (either via `PW_ENUM` or via
/// a `PwEnumToString(T)` function findable by ADL).
template <typename T>
inline constexpr bool has_enum_to_string_v =
    std::is_enum_v<T> &&
    (has_enum_traits_v<T> || internal::has_adl_enum_to_string_v<T>);

/// Returns a string representation of a given enumerator.
///
/// Enums registered with `PW_ENUM` or `PW_STATUS_ENUM` from
/// `pw_enum/generate.h` support `EnumToString` automatically via
/// `pw::EnumTraits<T>::ToString`.
///
/// Other enums can support `EnumToString` via the FTADLE pattern
/// (https://abseil.io/tips/218) by implementing a `PwEnumToString` function in
/// the same namespace as the enum (for example, using `PW_TOKENIZE_ENUM` or
/// `PW_TOKENIZE_ENUM_CUSTOM` from `pw_tokenizer/enum.h`).
template <typename T>
constexpr const char* EnumToString(T value) {
  static_assert(std::is_enum_v<T>, "Must be an enum");
  static_assert(
      has_enum_to_string_v<T>,
      "pw::EnumToString is not available for this enum. Register it with "
      "PW_ENUM or define PwEnumToString() in the enum's namespace.");
  if constexpr (has_enum_traits_v<T>) {
    return EnumTraits<T>::ToString(value);
  } else if constexpr (internal::has_adl_enum_to_string_v<T>) {
    return internal::CallAdlPwEnumToString(value);
  } else {
    return "";
  }
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
