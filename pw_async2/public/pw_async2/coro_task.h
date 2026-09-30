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

#include <concepts>
#include <type_traits>

#include "pw_async2/coro.h"
#include "pw_async2/future_task.h"
#include "pw_function/function.h"

namespace pw::async2 {

/// @submodule{pw_async2,coroutines}

/// A `Task` that runs a `Coro<T>` to completion and discards its return value
/// by default.
///
/// `CoroTask` is an alias of `FutureTask<Coro<T>, kPolicy>`. Pass
/// `ReturnValuePolicy::kKeep` (or use `FutureTask<Coro<T>>` directly) to store
/// the coroutine's return value for use with `Wait()` or `value()`.
///
/// Crashes if coroutine frame allocation fails. Use `FallibleCoroTask` to
/// handle allocation failures gracefully.
template <typename T, ReturnValuePolicy kPolicy = ReturnValuePolicy::kDiscard>
using CoroTask = FutureTask<Coro<T>, kPolicy>;

/// A `Task` that runs a `Coro<T>` to completion, discards its return value by
/// default, and invokes a handler if coroutine allocation fails.
///
/// `FallibleCoroTask` is an alias of
/// `FutureTask<FallibleCoro<T, Handler>, kPolicy>`. Pass
/// `ReturnValuePolicy::kKeep` (or use `FutureTask<FallibleCoro<T, Handler>>`
/// directly) to store the coroutine's return value.
template <typename T,
          typename Handler = Function<void()>,
          ReturnValuePolicy kPolicy = ReturnValuePolicy::kDiscard>
using FallibleCoroTask = FutureTask<FallibleCoro<T, Handler>, kPolicy>;

template <typename T, typename Handler>
FutureTask(Coro<T>&&, Handler&&)
    -> FutureTask<FallibleCoro<T, std::decay_t<Handler>>>;

template <typename Handler>
  requires(!Future<std::decay_t<Handler>> &&
           std::invocable<std::decay_t<Handler>> &&
           !std::is_void_v<std::invoke_result_t<std::decay_t<Handler>>>)
FutureTask(Handler&&)
    -> FutureTask<FallibleCoro<std::invoke_result_t<std::decay_t<Handler>>,
                               std::decay_t<Handler>>>;

/// @endsubmodule

}  // namespace pw::async2
