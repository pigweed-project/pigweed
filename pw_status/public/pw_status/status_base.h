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

#include <cstdint>
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
#include "pw_enum/traits.h"
#include "pw_status/internal/status_storage.h"

namespace pw {

/// @module{pw_status}

/// Whether a status type may carry an optional error message.
enum class StatusMessages {
  /// Messages are stored if `PW_STATUS_CFG_MESSAGES` enables them and the
  /// status code supports them.
  kDefault,
  /// The status never carries a message, so it is always only its code.
  kDisabled,
};

/// Base class providing standard status operations for status types using CRTP.
///
/// `StatusBase` provides common functionality for status types that wrap an
/// enum or integer status code, including checking for success (`ok()`),
/// retrieving the code (`code()`), optional error messages (`has_message()`,
/// `message()`), string formatting (`str()`), updating, and equality
/// comparisons.
///
/// Custom status types should derive from `StatusBase<Derived, kOkCode>` or
/// alias `StatusOf<kOkCode>`. Derived classes should be declared
/// `[[nodiscard]]` unless using `StatusOf` (which is declared `[[nodiscard]]`).
///
/// Equality comparisons consider only the status code; messages are ignored.
///
/// Users should not interact directly with `StatusBase`, except to instantiate
/// a custom status class. Do not use `StatusBase` for variables or function
/// parameters.
///
/// A status type supports messages if its code is an enum registered with
/// `PW_ENUM` whose values are all from 0 to 65535. Passing a `PW_STATUS_MSG` to
/// any other status type, or to one that uses `StatusMessages::kDisabled`, does
/// not compile.
///
/// @warning Status codes must be valid enumerators. They are not range checked,
/// and when messages are stored as tokens, the code shares a 32-bit word with
/// the message token. A value that needs more bits than the code's enumerators
/// do (`pw::EnumTraits<Code>::kBitWidth`) corrupts both the code and the
/// message. Check integers from outside the program, such as with
/// `pw::IsValidEnum`, before casting them to the code type.
///
/// @tparam Derived The derived status class (CRTP parameter).
/// @tparam kOkCode Code value that represents success (`ok() == true`).
/// @tparam kMessages Pass `StatusMessages::kDisabled` for a status that never
///   carries a message.
template <typename Derived,
          _PW_STATUS_CODE auto kOkCode,
          StatusMessages kMessages = StatusMessages::kDefault>
class StatusBase {
  static constexpr bool kSupportsMessages =
      kMessages == StatusMessages::kDefault &&
      internal::kStatusCodeBits<decltype(kOkCode)> != 0;

  static constexpr internal::StatusMessageType kMessageType =
      kSupportsMessages ? internal::kStatusMessageType
                        : internal::StatusMessageType::kNone;

  using Storage = internal::StatusStorage<kOkCode, kMessageType>;

 public:
  /// The underlying status code type.
  using Code = decltype(kOkCode);

  /// An optional error message. Create one with `PW_STATUS_MSG("...")`; a
  /// default-constructed `Message` is empty.
  ///
  /// How a message is stored depends on the `pw_status` configuration, so its
  /// contents are only accessible by logging the status with `PW_LOG_STATUS`.
  class Message {
   public:
    constexpr Message() noexcept = default;

    // Raw strings are not accepted; wrap string literals in PW_STATUS_MSG.
    constexpr Message(const char*) = delete;

    template <typename Fn>
    _PW_STATUS_MSG_CONSTEXPR Message(
        internal::DeferredStatusMessage<Fn> deferred) noexcept
        : value_(deferred.template Evaluate<Storage>()) {
      // This is an error in every configuration, so that code does not compile
      // when messages are disabled and break when they are enabled.
      static_assert(
          kSupportsMessages,
          "This status type cannot carry a message. Messages require a status "
          "code enum registered with PW_ENUM whose values are all from 0 to "
          "65535, and a status type that does not use "
          "StatusMessages::kDisabled.");
    }

    /// Returns true if there is no message. `PW_STATUS_MSG("")` is a message
    /// with no text, so it is not empty.
    [[nodiscard]] constexpr bool empty() const noexcept {
      return internal::IsEmpty(value_);
    }

   private:
    friend class StatusBase;
    friend struct internal::StatusMessageAccess;

    static constexpr Message FromStored(
        typename Storage::message_type value) noexcept {
      Message message;
      message.value_ = value;
      return message;
    }

    typename Storage::message_type value_{};
  };

  /// Returns true if the status is the OK code.
  [[nodiscard]] constexpr bool ok() const noexcept { return storage_.ok(); }

  /// Returns the underlying status code.
  [[nodiscard]] constexpr Code code() const noexcept { return storage_.code(); }

  /// Returns true if this status carries an error message.
  ///
  /// Always `false` for OK statuses and when messages are disabled.
  [[nodiscard]] constexpr bool has_message() const noexcept {
    return storage_.has_message();
  }

  /// Returns the error message, which is empty if `!has_message()`. Pass it to
  /// a constructor to create a status with a different code and the same
  /// message.
  [[nodiscard]] constexpr Message message() const noexcept {
    return Message::FromStored(storage_.message());
  }

  /// Updates this status to `other` if this status is currently `ok()`.
  ///
  /// This is useful for tracking the first encountered error, as calls to this
  /// helper will not change one error status to another error status.
  constexpr void Update(Derived other) noexcept {
    if (ok()) {
      *static_cast<Derived*>(this) = other;
    }
  }

  /// Ignores any errors, suppressing unused-result warnings.
  constexpr void IgnoreError() const noexcept {}

  /// Returns a string representation of the status code. Only supported for
  /// status types with enum codes.
  [[nodiscard]] constexpr const char* str() const {
    static_assert(std::is_enum_v<Code>,
                  "str() is only supported for status types with enum codes");
    return EnumToString(code());
  }

  /// Compares two statuses by code only; error messages are ignored.
  friend constexpr bool operator==(Derived lhs, Derived rhs) noexcept {
    return lhs.code() == rhs.code();
  }
  /// @copydoc operator==(Derived, Derived)
  friend constexpr bool operator!=(Derived lhs, Derived rhs) noexcept {
    return lhs.code() != rhs.code();
  }
  /// Compares a status with a status code; error messages are ignored.
  friend constexpr bool operator==(Derived lhs, Code rhs) noexcept {
    return lhs.code() == rhs;
  }
  /// @copydoc operator==(Derived, Code)
  friend constexpr bool operator==(Code lhs, Derived rhs) noexcept {
    return lhs == rhs.code();
  }
  /// @copydoc operator==(Derived, Code)
  friend constexpr bool operator!=(Derived lhs, Code rhs) noexcept {
    return lhs.code() != rhs;
  }
  /// @copydoc operator==(Derived, Code)
  friend constexpr bool operator!=(Code lhs, Derived rhs) noexcept {
    return lhs != rhs.code();
  }

 protected:
  /// Defaults to `kOkCode` (`ok() == true`).
  constexpr StatusBase() noexcept = default;

  /// Explicitly construct from the underlying code.
  explicit constexpr StatusBase(Code code) noexcept : storage_(code) {}

  /// Constructs from a status code and a `PW_STATUS_MSG`. The message is
  /// dropped if `code` is the OK code.
  constexpr StatusBase(Code code, Message message) noexcept
      : storage_(code, message.value_) {}

  constexpr StatusBase(const StatusBase&) = default;
  constexpr StatusBase& operator=(const StatusBase&) = default;
  constexpr StatusBase(StatusBase&&) = default;
  constexpr StatusBase& operator=(StatusBase&&) = default;

 private:
  Storage storage_;
};

/// Status type wrapping an existing status code enum.
///
/// `StatusOf` is default constructible (OK), explicitly constructible from the
/// enum, and constructible from `(Code, Message)` using `PW_STATUS_MSG("...")`.
/// Enumerators are accessed through the enum type or the `Code` alias (e.g.
/// `MyStatus::Code::kFailed`).
///
/// Users typically create a type alias of `StatusOf` for their status code enum
/// (e.g. `using MyStatus = pw::StatusOf<MyCode::kOk>;`).
///
/// @tparam kOkCode Enumerator that represents success (`ok() == true`).
/// @tparam kMessages Pass `StatusMessages::kDisabled` for a status that never
///   carries a message.
template <_PW_STATUS_CODE auto kOkCode,
          StatusMessages kMessages = StatusMessages::kDefault>
class [[nodiscard]] StatusOf final
    : public StatusBase<StatusOf<kOkCode, kMessages>, kOkCode, kMessages> {
  using Base = StatusBase<StatusOf<kOkCode, kMessages>, kOkCode, kMessages>;

 public:
  using typename Base::Code;
  using typename Base::Message;

  static_assert(std::is_enum_v<Code>, "StatusOf requires an enum status code");

  constexpr StatusOf() noexcept = default;

  explicit constexpr StatusOf(Code code) noexcept : Base(code) {}

  constexpr StatusOf(Code code, Message message) noexcept
      : Base(code, message) {}
};

/// @endmodule

namespace internal {

template <typename D, auto kOkCode, StatusMessages kMessages>
std::true_type IsStatusDerived(const StatusBase<D, kOkCode, kMessages>*);
std::false_type IsStatusDerived(...);

/// Trait to detect whether `T` is or derives from any instantiation of
/// `StatusBase`.
template <typename T>
inline constexpr bool is_status_v = decltype(IsStatusDerived(
    std::declval<std::add_pointer_t<std::decay_t<T>>>()))::value;

/// Generic status pass-through for PW_TRY. Returns the exact status type `S`
/// without slicing it to StatusBase.
template <typename S, typename = std::enable_if_t<is_status_v<S>>>
constexpr S ConvertToStatus(S status) noexcept {
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

}  // namespace pw
