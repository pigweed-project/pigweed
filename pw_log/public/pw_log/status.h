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

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>

#include "pw_log/config.h"
#include "pw_log/tokenized_args.h"
#include "pw_preprocessor/compiler.h"
#include "pw_status/message.h"
#include "pw_status/status.h"
#include "pw_status/status_base.h"
#include "pw_tokenizer/enum.h"
#include "pw_tokenizer/nested_tokenization.h"
#include "pw_tokenizer/tokenize.h"

// Status messages are stored in the form the pw_log backend emits, so
// PW_STATUS_CFG_MESSAGES must match the backend.
#if PW_STATUS_CFG_MESSAGES != PW_STATUS_CFG_MESSAGES_NONE
#if __has_include("pw_log_backend/log_backend_uses_pw_tokenizer.h")
#if PW_STATUS_CFG_MESSAGES != PW_STATUS_CFG_MESSAGES_TOKENIZED
#error \
    "The pw_log backend tokenizes logs, but status messages are stored as \
strings. Set PW_STATUS_CFG_MESSAGES to PW_STATUS_CFG_MESSAGES_TOKENIZED in \
the pw_status config (in Bazel, --@pigweed//pw_status:messages=tokenized)."
#endif  // PW_STATUS_CFG_MESSAGES != PW_STATUS_CFG_MESSAGES_TOKENIZED
#elif PW_STATUS_CFG_MESSAGES != PW_STATUS_CFG_MESSAGES_STRING
#error \
    "Status messages are stored as tokens, but the pw_log backend does not \
tokenize logs. Set PW_STATUS_CFG_MESSAGES to PW_STATUS_CFG_MESSAGES_STRING in \
the pw_status config (in Bazel, --@pigweed//pw_status:messages=string)."
#endif  // __has_include("pw_log_backend/log_backend_uses_pw_tokenizer.h")
#endif  // PW_STATUS_CFG_MESSAGES != PW_STATUS_CFG_MESSAGES_NONE

/// @submodule{pw_log,tokenized_args}

/// Tokenizer domain for `pw_Status` and `pw::Status` codes.
#define PW_STATUS_DOMAIN "::pw::Status"

/// Format specifier for logging the code of a `pw::Status`, `pw_Status`,
/// `pw::StatusWithSize`, or `pw::Result<T>` with `PW_LOG_ENUM`.
#define PW_STATUS_CODE_FMT PW_LOG_TOKEN_FMT(PW_STATUS_DOMAIN)

/// Format specifier for logging a `pw::Status`, `pw::StatusWithSize`, or
/// `pw::Result<T>` with `PW_LOG_STATUS`.
#define PW_STATUS_FMT PW_LOG_STATUS_DOMAIN_FMT(PW_STATUS_DOMAIN, 5)

/// @endsubmodule

/// @cond
static_assert(PW_STATUS_LAST == PW_STATUS_UNAUTHENTICATED,
              "Update tokenized pw_Status entries when adding status codes");
static_assert(
    ::pw::EnumTraits<pw_Status>::kTokenDomain == PW_STATUS_DOMAIN,
    "PW_STATUS_DOMAIN must match EnumTraits<pw_Status>::kTokenDomain");

_PW_TOKENIZE_ENUM_DOMAIN(::pw_Status,
                         PW_STATUS_DOMAIN,
                         (PW_STATUS_OK, "OK"),
                         (PW_STATUS_CANCELLED, "CANCELLED"),
                         (PW_STATUS_UNKNOWN, "UNKNOWN"),
                         (PW_STATUS_INVALID_ARGUMENT, "INVALID_ARGUMENT"),
                         (PW_STATUS_DEADLINE_EXCEEDED, "DEADLINE_EXCEEDED"),
                         (PW_STATUS_NOT_FOUND, "NOT_FOUND"),
                         (PW_STATUS_ALREADY_EXISTS, "ALREADY_EXISTS"),
                         (PW_STATUS_PERMISSION_DENIED, "PERMISSION_DENIED"),
                         (PW_STATUS_RESOURCE_EXHAUSTED, "RESOURCE_EXHAUSTED"),
                         (PW_STATUS_FAILED_PRECONDITION, "FAILED_PRECONDITION"),
                         (PW_STATUS_ABORTED, "ABORTED"),
                         (PW_STATUS_OUT_OF_RANGE, "OUT_OF_RANGE"),
                         (PW_STATUS_UNIMPLEMENTED, "UNIMPLEMENTED"),
                         (PW_STATUS_INTERNAL, "INTERNAL"),
                         (PW_STATUS_UNAVAILABLE, "UNAVAILABLE"),
                         (PW_STATUS_DATA_LOSS, "DATA_LOSS"),
                         (PW_STATUS_UNAUTHENTICATED, "UNAUTHENTICATED"));

// PW_STATUS_FMT states how many low bits of a logged pw::Status hold its code.
static_assert(::pw::internal::kStatusCodeBits<pw_Status> == 5,
              "Update the code width in PW_STATUS_FMT");
static_assert(PW_LOG_STATUS_STRING_SIZE > 0,
              "PW_LOG_STATUS_STRING_SIZE must be at least 1");
/// @endcond

/// @submodule{pw_log,tokenized_args}

// The documented macros are in the first branch, which is also the one taken
// if PW_STATUS_CFG_MESSAGES is not defined, as when Doxygen reads this file.
#if PW_STATUS_CFG_MESSAGES == PW_STATUS_CFG_MESSAGES_NONE

/// Format specifier for logging a status with `PW_LOG_STATUS`. Generated
/// `pw_enum` statuses define `<STATUS>_FMT` with this macro.
///
/// Formats as `"CODE"`, or as `"CODE: message"` if the status has a message.
///
/// @param[in] domain String literal with the tokenizer domain of the status's
/// code enum.
///
/// @param[in] code_bits Number of low bits that the status's code occupies when
/// it shares a 32-bit word with a message token: the bit width of the code enum
/// (`pw::EnumTraits<Code>::kBitWidth`), or 32 if the code enum does not support
/// messages. Must be a decimal integer literal. If it is wrong, tokenized logs
/// of the status cannot be decoded.
#define PW_LOG_STATUS_DOMAIN_FMT(domain, code_bits) PW_LOG_TOKEN_FMT(domain)

/// Expands to the argument needed to log a status and its optional message
/// with `<STATUS>_FMT` or `PW_STATUS_FMT`. Accepts `pw::Status`, other statuses
/// with enum codes, `pw::StatusWithSize`, and `pw::Result`. Evaluates its
/// argument once.
///
/// With a non-tokenizing `pw_log` backend and status messages enabled, the
/// argument is a string in a temporary buffer that only exists until the end
/// of the log statement.
#define PW_LOG_STATUS(...) \
  PW_LOG_ENUM(::pw::log::internal::LoggedStatus(__VA_ARGS__))

#elif PW_STATUS_CFG_MESSAGES == PW_STATUS_CFG_MESSAGES_TOKENIZED

#define PW_LOG_STATUS_DOMAIN_FMT(domain, code_bits) \
  PW_TOKEN_BIT_FIELD_FMT(domain, code_bits, PW_STATUS_MSG_DOMAIN(domain))
#define PW_LOG_STATUS(...) ::pw::log::internal::StatusWord(__VA_ARGS__)

#else  // PW_STATUS_CFG_MESSAGES == PW_STATUS_CFG_MESSAGES_STRING

#define PW_LOG_STATUS_DOMAIN_FMT(domain, code_bits) "%s"
#define PW_LOG_STATUS(...) \
  ::pw::log::internal::StatusLogString().Format(__VA_ARGS__)

#endif  // PW_STATUS_CFG_MESSAGES

/// @}

namespace pw::log::internal {

// Returns the status to log for a status or an object with a status() function,
// such as pw::Result or pw::StatusWithSize.
template <typename T, std::enable_if_t<::pw::internal::is_status_v<T>, int> = 0>
constexpr const T& ToStatus(const T& value) {
  return value;
}

template <typename T,
          std::enable_if_t<!::pw::internal::is_status_v<T>, int> = 0>
constexpr auto ToStatus(const T& value) -> decltype(value.status()) {
  return value.status();
}

template <typename T, typename = void>
inline constexpr bool kHasEnumStatusCode = false;

template <typename T>
inline constexpr bool kHasEnumStatusCode<
    T,
    std::void_t<decltype(ToStatus(std::declval<const T&>()).code())>> =
    std::is_enum_v<decltype(ToStatus(std::declval<const T&>()).code())>;

template <typename T>
struct LoggedEnum<T, std::enable_if_t<!std::is_enum_v<T>>> {
  static constexpr auto Get(const T& value) {
    static_assert(kHasEnumStatusCode<T>,
                  "PW_LOG_ENUM and PW_LOG_STATUS require an enum, a status "
                  "with an enum code, StatusWithSize, or Result.");
    if constexpr (kHasEnumStatusCode<T>) {
      return ToStatus(value).code();
    } else {
      return PW_STATUS_INTERNAL;  // Unreachable; return to clean up errors
    }
  }
};

// Passes through the argument of PW_LOG_STATUS, which must not be a plain enum.
template <typename T>
constexpr const T& LoggedStatus(const T& status) {
  static_assert(!std::is_enum_v<T>,
                "PW_LOG_STATUS requires a status, StatusWithSize, or Result. "
                "Log enums with PW_LOG_ENUM.");
  return status;
}

constexpr uint32_t MessageToken(uint32_t token) { return token; }
constexpr uint32_t MessageToken(::pw::internal::EmptyStatusMessage) {
  return 0u;
}

// Tokenizing backends log a status as one 32-bit word: the code in the low bits
// and the message token in the rest. PW_TOKEN_BIT_FIELD_FMT tells the
// detokenizer to look up the two fields.
template <typename T>
constexpr uint32_t StatusWord(const T& value) {
  static_cast<void>(LoggedStatus(value));
  if constexpr (!std::is_enum_v<T> && kHasEnumStatusCode<T>) {
    const auto& status = ToStatus(value);
    const auto code = status.code();
    using Code = std::remove_const_t<decltype(code)>;
    // A status without a message has message token 0. Define token 0 as an
    // empty string in the message domain of the status code enum, so that such
    // a status detokenizes as only its code.
    if constexpr (::pw::internal::kStatusCodeBits<Code> != 0u) {
      PW_TOKENIZER_DEFINE_TOKEN_ALWAYS(
          0u, ::pw::internal::kStatusMessageDomain<Code>.chars, "");
    }
    return ::pw::tokenizer::EnumToToken(code) |
           MessageToken(
               ::pw::internal::StatusMessageAccess::Get(status.message()));
  } else {
    return ::pw::tokenizer::EnumToToken(LoggedEnum<T>::Get(value));
  }
}

// A status without a message has a null string.
constexpr const char* MessageString(const char* string) { return string; }
constexpr const char* MessageString(::pw::internal::EmptyStatusMessage) {
  return nullptr;
}

// Non-tokenizing backends log a status as one string, "CODE: message". When
// there is a message, it is formatted into this buffer, which is a temporary in
// the log statement, so the backend must use the string before the statement
// ends. When there is no message, the code string is used directly.
class StatusLogString {
 public:
  // User-provided so value-initializing a temporary does not zero text_.
  StatusLogString() noexcept {}

  template <typename T>
  const char* Format(const T& value) PW_ATTRIBUTE_LIFETIME_BOUND {
    static_cast<void>(LoggedStatus(value));
    if constexpr (!std::is_enum_v<T> && kHasEnumStatusCode<T>) {
      const auto& status = ToStatus(value);
      const char* code = ::pw::EnumToString(status.code());
      // The message's string starts with ": ".
      const char* message = MessageString(
          ::pw::internal::StatusMessageAccess::Get(status.message()));
      return message == nullptr ? code : FormatWithMessage(code, message);
    } else {
      return ::pw::EnumToString(LoggedEnum<T>::Get(value));
    }
  }

 private:
  PW_NO_INLINE const char* FormatWithMessage(const char* code,
                                             const char* message) noexcept {
    const std::string_view parts[] = {code, message};
    size_t size = 0;
    for (std::string_view part : parts) {
      size += part.copy(text_ + size, sizeof(text_) - 1 - size);
    }
    text_[size] = '\0';
    return text_;
  }

  char text_[PW_LOG_STATUS_STRING_SIZE];
};

}  // namespace pw::log::internal
