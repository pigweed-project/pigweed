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

#include "pw_log/tokenized_args.h"
#include "pw_status/status.h"
#include "pw_status/status_base.h"

/// @submodule{pw_log,tokenized_args}

/// Format specifier for logging a status with `PW_LOG_STATUS`. Generated
/// `pw_enum` statuses define `<STATUS>_FMT` with this macro.
///
/// Formats as the name of the status code.
///
/// @param[in] domain String literal with the tokenizer domain of the status's
/// code enum.
#define PW_LOG_STATUS_DOMAIN_FMT(domain) PW_LOG_TOKEN_FMT(domain)

/// Expands to the argument needed to log a status with `<STATUS>_FMT`.
/// Accepts statuses with enum codes and `pw::Result`. Evaluates its argument
/// once.
#define PW_LOG_STATUS(...) \
  PW_LOG_ENUM(::pw::log::internal::LoggedStatus(__VA_ARGS__))

/// @}

namespace pw::log::internal {

// Returns the status to log for a status or an object with a status() function,
// such as pw::Result.
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
                  "with an enum code, or Result.");
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
                "PW_LOG_STATUS requires a status or Result. Log enums with "
                "PW_LOG_ENUM.");
  return status;
}

}  // namespace pw::log::internal
