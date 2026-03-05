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

#include "pw_async2/try.h"
#include "pw_preprocessor/arguments.h"

/// @submodule{pw_async2,poll}

/// @def PW_AWAIT
/// @brief Poll and resolve a future or return `Pending` if not ready.
///
/// `PW_AWAIT` polls the provided future, returning `Pending` from the current
/// function if the future is not yet ready. When the future completes,
/// `PW_AWAIT` evaluates to the result of the future. This macro is convenient
/// for writing `DoPend` implementations that need to wait on other futures.
///
/// @warning
/// `PW_AWAIT` does not extend the lifetime of the future. The future must
/// remain valid until it completes. Do not use `PW_AWAIT` with temporary
/// futures unless you are certain they will complete immediately.
///
/// @param variable_declaration
///   (Optional) The variable declaration to which the future's result will be
///   assigned. For example: `auto result` or `int value`.
///
/// @param future
///   The `pw::async2::Future` to poll.
///
/// @param context
///   The `pw::async2::Context&` with which to poll the future.
///
/// **Usage:**
///
/// `PW_AWAIT(future, context)`:
///   Waits for `future` to complete. Use this for futures that return `void`.
///
/// `PW_AWAIT(variable_declaration, future, context)`:
///   Waits for `future` to complete and assigns the result to
///   `variable_declaration`.
///
/// **Examples:**
///
/// @code{.cpp}
///   // Wait for a void future to complete:
///   PW_AWAIT(async_work_future_, cx);
///
///   // Wait for a value and assign it:
///   PW_AWAIT(auto result, async_value_future_, cx);
///
///   // Wait for a value and assign it to an existing variable:
///   int value;
///   PW_AWAIT(value, async_int_future_, cx);
///
///   // If the future needs to be created or has completed, restart it:
///   if (!my_future_.is_pendable()) {
///     my_future_ = StartAsyncWork();
///   }
///   PW_AWAIT(auto result, my_future_, cx);
/// @endcode
///
/// **Expansion:**
///
/// `PW_AWAIT` expands to code that pends the future, checks for readiness, and
/// returns `Pending` or unwraps the value.
///
/// For example, `PW_AWAIT(auto value, future, cx)` expands roughly to:
/// @code{.cpp}
///   auto&& _pw_poll_result = future.Pend(cx);
///   if (_pw_poll_result.IsPending()) {
///     return Pending();
///   }
///   auto value = std::move(*_pw_poll_result);
/// @endcode
// Doxygen/Doxylink struggle with correct parsing and linking when this is
// defined as a variadic macro. We use PW_EXCLUDE_FROM_DOXYGEN to provide a
// simplified argument-less definition that generates a clean entry in the tag
// file, ensuring reliable linking.
#ifdef PW_EXCLUDE_FROM_DOXYGEN
#define PW_AWAIT
#else
#define PW_AWAIT(...) PW_DELEGATE_BY_ARG_COUNT(_PW_AWAIT_, __VA_ARGS__)
#endif

/// @cond
#define _PW_AWAIT_2(future, cx) PW_TRY_READY((future).Pend(cx))
#define _PW_AWAIT_3(lhs, future, cx) PW_TRY_READY_ASSIGN(lhs, (future).Pend(cx))
/// @endcond

/// @endsubmodule
