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

#if defined(__cpp_concepts) && __has_include(<concepts>)
#include <concepts>
#endif  // defined(__cpp_concepts) && __has_include(<concepts>)

#ifdef __cpp_lib_concepts
#define _PW_STATUS_CODE ::std::regular
#else
#define _PW_STATUS_CODE
#endif  // __cpp_lib_concepts

#include "pw_enum/to_string.h"

namespace pw {

/// @module{pw_status}

/// Base class providing standard status operations for status types using CRTP.
///
/// `StatusBase` provides common functionality for status types that wrap an
/// enum or integer status code, including checking for success (`ok()`),
/// retrieving the code (`code()`), string formatting (`str()`), updating,
/// and equality comparisons.
///
/// Custom status types should derive from `StatusBase<Derived, kOkCode>` or use
/// the `PW_STATUS_TYPE` macro. Derived classes should be declared
/// `[[nodiscard]]` unless created using `PW_STATUS_TYPE` (which applies
/// `[[nodiscard]]` automatically).
///
/// Users should not interact directly `StatusBase`, except to instantiate a
/// custom status class. Do not use `StatusBase` for variables or function
/// parameters.
///
/// @tparam Derived The derived status class (CRTP parameter).
/// @tparam kOkCode Code value that represents success (`ok() == true`).
template <typename Derived, _PW_STATUS_CODE auto kOkCode>
class StatusBase {
 public:
  /// The underlying status code type.
  using Code = decltype(kOkCode);

  /// Returns true if the status is the OK code.
  [[nodiscard]] constexpr bool ok() const { return code_ == kOkCode; }

  /// Returns the underlying status code.
  [[nodiscard]] constexpr Code code() const { return code_; }

  /// Updates this status to `other` if this status is currently `ok()`.
  ///
  /// This is useful for tracking the first encountered error, as calls to this
  /// helper will not change one error status to another error status.
  constexpr void Update(Derived other) {
    if (ok()) {
      code_ = other.code();
    }
  }

  /// Ignores any errors, suppressing unused-result warnings.
  constexpr void IgnoreError() const {}

  /// Returns a string representation of the status. Only supported for status
  /// types with enum codes.
  [[nodiscard]] constexpr const char* str() const {
    static_assert(std::is_enum_v<Code>,
                  "str() is only supported for status types with enum codes");
    return EnumToString(code_);
  }

  friend constexpr bool operator==(Derived lhs, Derived rhs) {
    return lhs.code_ == rhs.code_;
  }
  friend constexpr bool operator!=(Derived lhs, Derived rhs) {
    return lhs.code_ != rhs.code_;
  }
  friend constexpr bool operator==(Derived lhs, Code rhs) {
    return lhs.code_ == rhs;
  }
  friend constexpr bool operator==(Code lhs, Derived rhs) {
    return lhs == rhs.code_;
  }
  friend constexpr bool operator!=(Derived lhs, Code rhs) {
    return lhs.code_ != rhs;
  }
  friend constexpr bool operator!=(Code lhs, Derived rhs) {
    return lhs != rhs.code_;
  }

 protected:
  /// Defaults to `kOkCode` (`ok() == true`).
  constexpr StatusBase() : code_(kOkCode) {}

  /// Explicitly construct from the underlying code.
  explicit constexpr StatusBase(Code code) : code_(code) {}

  constexpr StatusBase(const StatusBase&) = default;
  constexpr StatusBase& operator=(const StatusBase&) = default;
  constexpr StatusBase(StatusBase&&) = default;
  constexpr StatusBase& operator=(StatusBase&&) = default;

  Code code_;
};

/// Defines a status type wrapping an existing status code enum.
///
/// The status type is default constructible (OK) and explicitly constructible
/// from the enum. Enumerators are accessed through the enum type or the `Code`
/// alias (e.g. `MyStatus::Code::kFailed`).
#define PW_STATUS_TYPE(name, ok_code)                                       \
  class [[nodiscard]] name final : public ::pw::StatusBase<name, ok_code> { \
   public:                                                                  \
    static_assert(::std::is_enum_v<Code>,                                   \
                  "PW_STATUS_TYPE requires an enum status code");           \
    constexpr name() = default;                                             \
    explicit constexpr name(Code code)                                      \
        : ::pw::StatusBase<name, ok_code>(code) {}                          \
  }

namespace internal {

template <typename D, auto kOkCode>
std::true_type IsStatusDerived(const StatusBase<D, kOkCode>*);
std::false_type IsStatusDerived(...);

/// Trait to detect whether `T` is or derives from any instantiation of
/// `StatusBase`.
template <typename T>
inline constexpr bool is_status_v = decltype(IsStatusDerived(
    std::declval<std::add_pointer_t<std::decay_t<T>>>()))::value;

/// Generic status pass-through for PW_TRY. Returns the exact status type `S`
/// without slicing it to StatusBase.
template <typename S, typename = std::enable_if_t<is_status_v<S>>>
constexpr S ConvertToStatus(S status) {
  return status;
}

/// True if the status type `S` supports `str()`.
template <typename S>
inline constexpr bool status_has_str_v = has_enum_to_string_v<typename S::Code>;

/// Returns a string for a status for use in `PW_CHECK_OK` failure messages.
/// Status types that do not support `str()` (e.g. those with integer codes)
/// use a placeholder string.
template <typename S>
constexpr const char* StatusToCheckString(const S& status) {
  static_assert(is_status_v<S>);
  if constexpr (status_has_str_v<S>) {
    return status.str();
  } else {
    return "NOT OK";
  }
}

}  // namespace internal

#undef _PW_STATUS_CODE

/// @endmodule

}  // namespace pw
