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

#include "pw_enum/traits.h"
#include "pw_status/status_base.h"
#include "pw_tokenizer/tokenize.h"

/// @module{pw_status}

/// Expands to the `pw_tokenizer` domain for the messages of statuses whose
/// code enum is tokenized in `code_domain`: the code's domain followed by
/// `"::Message"`. For example, the messages of `pw::Status` are in the
/// `"::pw::Status::Message"` domain.
///
/// Each status code enum has its own message domain because message tokens are
/// masked to fit the bits its code leaves free, so they are specific to it.
///
/// @param[in] code_domain String literal with the tokenizer domain of the
/// status's code enum (`pw::EnumTraits<Code>::kTokenDomain`).
#define PW_STATUS_MSG_DOMAIN(code_domain) code_domain "::Message"

/// @endmodule

// PW_STATUS_MSG must add its string to the token database without adding any
// code to the binary: creating a status with a message only loads a constant.
//
// If a status with a message may be created during constant evaluation (C++23),
// the PW_STATUS_MSG lambda may never be called at run time. A token entry that
// PW_TOKENIZER_DEFINE_TOKEN defines in the lambda would then be lost, so the
// entry is defined with PW_TOKENIZER_DEFINE_TOKEN_ALWAYS.
//
// In earlier standards, the lambda is only called at run time, so the entry is
// defined with PW_TOKENIZER_DEFINE_TOKEN. PW_TOKENIZER_DEFINE_TOKEN_ALWAYS
// would not compile for a status in the initializer of a static data member
// with GCC in C++17.
//
// Do NOT keep the entry by marking the lambda as used (PW_USED). That emits an
// out-of-line copy of every PW_STATUS_MSG lambda, which nothing calls.
#if defined(__cpp_constexpr) && __cpp_constexpr >= 202211L
#define _PW_STATUS_MSG_DEFINE_TOKEN PW_TOKENIZER_DEFINE_TOKEN_ALWAYS
#else
#define _PW_STATUS_MSG_DEFINE_TOKEN PW_TOKENIZER_DEFINE_TOKEN
#endif  // __cpp_constexpr >= 202211L

namespace pw::internal {

template <size_t kSize>
struct StatusMessageDomain {
  char chars[kSize];
};

template <typename Code>
constexpr auto MakeStatusMessageDomain() noexcept {
  constexpr std::string_view kCodeDomain = EnumTraits<Code>::kTokenDomain;
  constexpr std::string_view kSuffix = PW_STATUS_MSG_DOMAIN("");

  // The array is zero initialized, which null terminates the string.
  StatusMessageDomain<kCodeDomain.size() + kSuffix.size() + 1> domain{};
  size_t i = 0;
  for (char c : kCodeDomain) {
    domain.chars[i++] = c;
  }
  for (char c : kSuffix) {
    domain.chars[i++] = c;
  }
  return domain;
}

// PW_STATUS_MSG_DOMAIN for the status code enum Code, as a char array.
template <typename Code>
inline constexpr auto kStatusMessageDomain = MakeStatusMessageDomain<Code>();

// Token 0 means "no message", so strings that hash to 0 use the lowest bit of
// the mask as their token instead.
constexpr uint32_t NonZeroStatusMessageToken(uint32_t masked_hash,
                                             uint32_t mask) noexcept {
  return masked_hash == 0u ? (mask & (~mask + 1u)) : masked_hash;
}

}  // namespace pw::internal

/// @module{pw_status}

/// Creates an optional error message for `pw::Status` or another
/// `pw::StatusBase`-derived status.
///
/// Pass the result to a status constructor or factory function:
///
/// @code{.cpp}
///   return pw::Status::NotFound(PW_STATUS_MSG("No such key"));
///   return MyStatus(MyCode::kFailed, PW_STATUS_MSG("Sector CRC error"));
/// @endcode
///
/// The argument must be a string literal. What happens to it depends on the
/// `pw_status` configuration (`PW_STATUS_CFG_MESSAGES`):
/// - `PW_STATUS_CFG_MESSAGES_NONE` (the default): the string is discarded at
///   compile time.
/// - `PW_STATUS_CFG_MESSAGES_TOKENIZED`: the string is tokenized in the
///   status's `PW_STATUS_MSG_DOMAIN` and only its token is stored.
/// - `PW_STATUS_CFG_MESSAGES_STRING`: a pointer to the string literal is
///   stored.
///
/// A status with a message logs as `CODE (message)`, so that it reads well
/// after a description of what failed:
///
/// @code{.cpp}
///   // Logs "Read failed: NOT_FOUND (No such key)". If messages are disabled,
///   // logs "Read failed: NOT_FOUND".
///   PW_LOG_WARN("Read failed: " PW_STATUS_FMT, PW_LOG_STATUS(status));
/// @endcode
///
/// Messages are stored in the form they are logged in, `" (message)"`, so
/// logging a status does not have to format its message.
///
/// Status equality comparisons (`==` and `!=`) consider only the status code
/// and ignore messages.
///
/// Creating a status with a message is `constexpr` in C++23 and later, and
/// never `constexpr` in earlier standards. Each `PW_STATUS_MSG` has its own
/// type, so two of them cannot be the operands of a conditional expression.
#define PW_STATUS_MSG(string_literal) _PW_STATUS_MSG(string_literal)

/// @endmodule

// The token entry is defined in a branch that depends on the status type, so
// it is only instantiated for statuses that store tokens.
// The text logged after the code of a status that has a message. It is what
// PW_STATUS_MSG stores, as a string or as its token.
#define _PW_STATUS_MSG_LOGGED(string_literal) " (" string_literal ")"

#define _PW_STATUS_MSG_ENABLED(string_literal)                                 \
  ::pw::internal::DeferredStatusMessage([](auto _pw_status_tag) {              \
    using _PwStorage = typename decltype(_pw_status_tag)::storage_type;        \
    using _PwType = ::pw::internal::StatusMessageType;                         \
    if constexpr (_PwStorage::kMessageType == _PwType::kToken) {               \
      using _PwCode = typename _PwStorage::code_type;                          \
      constexpr uint32_t _pw_status_token =                                    \
          ::pw::internal::NonZeroStatusMessageToken(                           \
              _PW_TOKENIZER_MASK_TOKEN(_PwStorage::kTokenMask,                 \
                                       _PW_STATUS_MSG_LOGGED(string_literal)), \
              _PwStorage::kTokenMask);                                         \
      _PW_STATUS_MSG_DEFINE_TOKEN(                                             \
          _pw_status_token,                                                    \
          ::pw::internal::kStatusMessageDomain<_PwCode>.chars,                 \
          _PW_STATUS_MSG_LOGGED(string_literal));                              \
      return _pw_status_token;                                                 \
    } else if constexpr (_PwStorage::kMessageType ==                           \
                         _PwType::kStringLiteral) {                            \
      return static_cast<const char*>(_PW_STATUS_MSG_LOGGED(string_literal));  \
    } else {                                                                   \
      return ::pw::internal::EmptyStatusMessage{};                             \
    }                                                                          \
  })

#define _PW_STATUS_MSG_DISABLED(string_literal)                            \
  (static_cast<void>(sizeof(static_cast<const char*>("" string_literal))), \
   ::pw::internal::DeferredStatusMessage(                                  \
       ::pw::internal::DiscardedStatusMessage{}))

#if PW_STATUS_CFG_MESSAGES != PW_STATUS_CFG_MESSAGES_NONE
#define _PW_STATUS_MSG(string_literal) _PW_STATUS_MSG_ENABLED(string_literal)
#else
#define _PW_STATUS_MSG(string_literal) _PW_STATUS_MSG_DISABLED(string_literal)
#endif  // PW_STATUS_CFG_MESSAGES != PW_STATUS_CFG_MESSAGES_NONE
