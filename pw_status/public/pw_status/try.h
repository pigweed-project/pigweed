// Copyright 2020 The Pigweed Authors
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

#include <utility>

#include "pw_status/status.h"
#include "pw_status/status_base.h"
#include "pw_status/status_with_size.h"

/// @module{pw_status}

// Macros for cleanly working with Status or StatusWithSize objects in functions
// that return Status.

/// Returns early if `expr` is a non-OK `Status` or `Result`.
#define PW_TRY(expr) _PW_TRY(_PW_TRY_UNIQUE(__LINE__), expr, return)

/// @}

#define _PW_TRY(result, expr, return_stmt)                \
  do {                                                    \
    if (auto result = (expr); !result.ok()) {             \
      return_stmt::pw::internal::ConvertToStatus(result); \
    }                                                     \
  } while (0)

/// @module{pw_status}

/// Returns early if `expression` is a non-OK `Result`.
/// If `expression` is okay, assigns the inner value to `lhs`.
#define PW_TRY_ASSIGN(lhs, expression) \
  _PW_TRY_ASSIGN(_PW_TRY_UNIQUE(__LINE__), lhs, expression, return)

/// @}

#define _PW_TRY_ASSIGN(result, lhs, expr, return_stmt)  \
  auto result = (expr);                                 \
  if (!result.ok()) {                                   \
    return_stmt::pw::internal::ConvertToStatus(result); \
  }                                                     \
  lhs = ::pw::internal::ConvertToValue(result)

/// @module{pw_status}

/// Returns early if `expr` is a non-OK `Status` or `StatusWithSize`.
///
/// This is designed for use in functions that return a `StatusWithSize`.
#define PW_TRY_WITH_SIZE(expr) _PW_TRY_WITH_SIZE(_PW_TRY_UNIQUE(__LINE__), expr)

/// @}

#define _PW_TRY_WITH_SIZE(result, expr)                       \
  do {                                                        \
    if (auto result = (expr); !result.ok()) {                 \
      return ::pw::internal::ConvertToStatusWithSize(result); \
    }                                                         \
  } while (0)

#define _PW_TRY_UNIQUE(line) _PW_TRY_UNIQUE_EXPANDED(line)
#define _PW_TRY_UNIQUE_EXPANDED(line) _pw_try_unique_name_##line

/// @module{pw_status}

/// Like `PW_TRY`, but using `co_return` instead of early `return`.
///
/// This is necessary because only `co_return` can be used inside of a
/// coroutine, and there is no way to detect whether particular code is running
/// within a coroutine or not.
#define PW_CO_TRY(expr) _PW_TRY(_PW_TRY_UNIQUE(__LINE__), expr, co_return)

/// Like `PW_TRY_ASSIGN`, but using `co_return` instead of early `return`.
///
/// This is necessary because only `co_return` can be used inside of a
/// coroutine, and there is no way to detect whether particular code is running
/// within a coroutine or not.
#define PW_CO_TRY_ASSIGN(lhs, expression) \
  _PW_TRY_ASSIGN(_PW_TRY_UNIQUE(__LINE__), lhs, expression, co_return)

/// @}
