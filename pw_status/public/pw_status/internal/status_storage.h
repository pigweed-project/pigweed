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

// IWYU pragma: private, include "pw_status/status_base.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "pw_enum/traits.h"
#include "pw_status/internal/config.h"

// Creating a status with a message is constexpr in C++23 and later, where
// constexpr functions may declare the static variables that register tokens.
// Earlier standards are non-constexpr in every configuration so that code does
// not compile in one configuration and break in another.
#if defined(__cpp_constexpr) && __cpp_constexpr >= 202211L
#define _PW_STATUS_MSG_CONSTEXPR constexpr
#else
#define _PW_STATUS_MSG_CONSTEXPR
#endif  // __cpp_constexpr >= 202211L

namespace pw {
namespace internal {

// How a status stores its optional message.
enum class StatusMessageType {
  // No message; the status is only its code.
  kNone,
  // A pw_tokenizer token that shares a 32-bit word with the code.
  kToken,
  // A pointer to a string literal, stored next to the code.
  kStringLiteral,
};

// The message storage selected by the pw_status module configuration.
inline constexpr StatusMessageType kStatusMessageType =
#if PW_STATUS_CFG_MESSAGES == PW_STATUS_CFG_MESSAGES_TOKENIZED
    StatusMessageType::kToken;
#elif PW_STATUS_CFG_MESSAGES == PW_STATUS_CFG_MESSAGES_STRING
    StatusMessageType::kStringLiteral;
#else
    StatusMessageType::kNone;
#endif  // PW_STATUS_CFG_MESSAGES

struct EmptyStatusMessage {};

// "No message" is token 0 or a null string. PW_STATUS_MSG("") is a message.
constexpr bool IsEmpty(EmptyStatusMessage) noexcept { return true; }
constexpr bool IsEmpty(uint32_t token) noexcept { return token == 0u; }
constexpr bool IsEmpty(const char* string) noexcept {
  return string == nullptr;
}

// Gives pw_log access to the stored form of a StatusBase::Message: an
// EmptyStatusMessage, a uint32_t token, or a const char*.
struct StatusMessageAccess {
  template <typename Message>
  static constexpr auto Get(Message message) noexcept {
    return message.value_;
  }
};

// Tells a PW_STATUS_MSG which StatusStorage it is evaluated for. The storage
// determines the stored form of the message and, for tokens, its mask and
// domain.
template <typename Storage>
struct StatusMessageTag {
  using storage_type = Storage;
};

template <typename Fn>
class DeferredStatusMessage {
 public:
  explicit constexpr DeferredStatusMessage(Fn fn) noexcept : fn_(fn) {}

  template <typename Storage>
  constexpr auto Evaluate() const noexcept {
    return fn_(StatusMessageTag<Storage>{});
  }

 private:
  Fn fn_;
};

template <typename Fn>
DeferredStatusMessage(Fn) -> DeferredStatusMessage<Fn>;

// PW_STATUS_MSG evaluates to this when messages are disabled.
struct DiscardedStatusMessage {
  template <typename Tag>
  constexpr EmptyStatusMessage operator()(Tag) const noexcept {
    return {};
  }
};

// The number of low bits a status code occupies when it shares a 32-bit word
// with a message token, or 0 if the code's type does not support messages.
//
// Codes that support messages are enums registered with PW_ENUM, which
// provides their bit width and token domain, whose values are not negative and
// need at most 16 bits. That leaves at least 16 bits for the message token.
// Codes are stored as is in the low bits, which cannot represent negative
// values.
template <typename Code, typename = void>
inline constexpr size_t kStatusCodeBits = 0;

template <typename Code>
inline constexpr size_t
    kStatusCodeBits<Code, std::enable_if_t<has_enum_traits_v<Code>>> =
        ((std::is_signed_v<std::underlying_type_t<Code>> &&
          static_cast<int64_t>(EnumTraits<Code>::kMin) < 0) ||
         EnumTraits<Code>::kBitWidth > 16u)
            ? 0u
            : EnumTraits<Code>::kBitWidth;

template <auto kOkCode, StatusMessageType kType>
class StatusStorage;

template <auto kOkCode>
class StatusStorage<kOkCode, StatusMessageType::kNone> {
 public:
  using code_type = decltype(kOkCode);
  using message_type = EmptyStatusMessage;
  static constexpr StatusMessageType kMessageType = StatusMessageType::kNone;

  constexpr StatusStorage() noexcept = default;
  explicit constexpr StatusStorage(code_type code) noexcept : code_(code) {}
  constexpr StatusStorage(code_type code, EmptyStatusMessage) noexcept
      : code_(code) {}

  [[nodiscard]] constexpr bool ok() const noexcept { return code_ == kOkCode; }
  [[nodiscard]] constexpr code_type code() const noexcept { return code_; }
  [[nodiscard]] constexpr bool has_message() const noexcept { return false; }
  [[nodiscard]] constexpr EmptyStatusMessage message() const noexcept {
    return {};
  }

 private:
  code_type code_ = kOkCode;
};

template <auto kOkCode>
class StatusStorage<kOkCode, StatusMessageType::kStringLiteral> {
 public:
  using code_type = decltype(kOkCode);
  using message_type = const char*;
  static constexpr StatusMessageType kMessageType =
      StatusMessageType::kStringLiteral;

  constexpr StatusStorage() noexcept = default;
  explicit constexpr StatusStorage(code_type code) noexcept : code_(code) {}
  // OK statuses never carry a message.
  constexpr StatusStorage(code_type code, const char* message) noexcept
      : code_(code), message_(code == kOkCode ? nullptr : message) {}

  [[nodiscard]] constexpr bool ok() const noexcept { return code_ == kOkCode; }
  [[nodiscard]] constexpr code_type code() const noexcept { return code_; }
  [[nodiscard]] constexpr bool has_message() const noexcept {
    return message_ != nullptr;
  }
  [[nodiscard]] constexpr const char* message() const noexcept {
    return message_;
  }

 private:
  code_type code_ = kOkCode;
  const char* message_ = nullptr;
};

// Stores the code and a message token in one 32-bit word: the code in the low
// kStatusCodeBits bits and the token in the rest. The word is logged as a
// pw_tokenizer bit field token.
//
// Precondition: codes are valid enumerators and tokens have their code bits
// clear. Neither is checked.
template <auto kOkCode>
class StatusStorage<kOkCode, StatusMessageType::kToken> {
 public:
  using code_type = decltype(kOkCode);
  using message_type = uint32_t;
  static constexpr StatusMessageType kMessageType = StatusMessageType::kToken;

  static constexpr size_t kCodeBits = kStatusCodeBits<code_type>;
  static_assert(kCodeBits != 0u,
                "This status code type does not support messages");

  static constexpr uint32_t kCodeMask = (uint32_t{1} << kCodeBits) - 1u;
  static constexpr uint32_t kTokenMask = ~kCodeMask;

  constexpr StatusStorage() noexcept = default;
  explicit constexpr StatusStorage(code_type code) noexcept
      : word_(ToUint32(code)) {}
  // OK statuses never carry a message, so ok() is a single comparison.
  constexpr StatusStorage(code_type code, uint32_t token) noexcept
      : word_(code == kOkCode ? kOkWord : ToUint32(code) | token) {}

  [[nodiscard]] constexpr bool ok() const noexcept { return word_ == kOkWord; }
  [[nodiscard]] constexpr code_type code() const noexcept {
    return static_cast<code_type>(
        static_cast<std::underlying_type_t<code_type>>(word_ & kCodeMask));
  }
  [[nodiscard]] constexpr uint32_t message() const noexcept {
    return word_ & kTokenMask;
  }
  [[nodiscard]] constexpr bool has_message() const noexcept {
    return message() != 0u;
  }

 private:
  static constexpr uint32_t ToUint32(code_type code) noexcept {
    return static_cast<uint32_t>(
        static_cast<std::underlying_type_t<code_type>>(code));
  }

  static constexpr uint32_t kOkWord = ToUint32(kOkCode);

  uint32_t word_ = kOkWord;
};

}  // namespace internal
}  // namespace pw
