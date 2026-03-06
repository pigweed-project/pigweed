// Copyright 2023 The Pigweed Authors
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

#include "pw_async2/coro.h"
#include "pw_async2/func_task.h"
#include "pw_function/function.h"

namespace pw::async2 {

/// @submodule{pw_async2,coroutines}

/// A `Task` that delegates to a provided `Coro<T>` and executes an
/// error handler function if coroutine allocation fails.
template <typename T,
          typename AllocationErrorHandler = pw::Function<void()>,
          ReturnValuePolicy policy = std::is_void_v<T>
                                         ? ReturnValuePolicy::kDiscard
                                         : ReturnValuePolicy::kKeep>
  requires std::invocable<AllocationErrorHandler>
class FallibleCoroTask final : public Task {
 public:
  using value_type = std::conditional_t<std::is_void_v<T>, ReadyType, T>;

  /// Create a new `Task` that runs `coro`, invoking `or_else` if allocation
  /// fails.
  template <typename ErrorHandler>
  FallibleCoroTask(Coro<T>&& coro, ErrorHandler&& error_handler)
      : Task(PW_ASYNC_TASK_NAME("FallibleCoroTask<T>")),
        coro_(std::move(coro)),
        error_handler_(std::forward<ErrorHandler>(error_handler)) {}

  FallibleCoroTask(const FallibleCoroTask&) = delete;
  FallibleCoroTask& operator=(const FallibleCoroTask&) = delete;
  FallibleCoroTask(FallibleCoroTask&&) = delete;
  FallibleCoroTask& operator=(FallibleCoroTask&&) = delete;

  ~FallibleCoroTask() override { Deregister(); }

  /// Returns whether this `FallibleCoroTask` wraps a valid `Coro` and can be
  /// pended. Pending a `!ok()` `FallibleCoroTask` calls the error handler.
  ///
  /// This will be `false` if `Coro` allocation failed.
  [[nodiscard]] bool ok() const { return coro_.ok(); }

  /// Returns whether the task ran and set that `value` to the function's return
  /// value.
  bool has_value() const { return return_value_.has_value(); }

  /// The return value from the coroutine.
  ///
  /// @pre The task must have completed. Call `Join` to ensure it finished.
  value_type& value() { return return_value_.value(); }

  /// @copydoc value
  const value_type& value() const { return return_value_.value(); }

  /// Blocks until the task completes and returns the return value in a
  /// `std::optional`. The `std::optional` is empty if coroutine allocation
  /// failed.
  std::optional<value_type>& Wait() {
    Task::Join();
    return return_value_;
  }

 private:
  Poll<> DoPend(Context& cx) final {
    if (!coro_.ok()) {
      error_handler_();
      return Ready();
    }

    auto result = coro_.Pend(cx);
    switch (result.state()) {
      case internal::CoroPollState::kPending:
        return Pending();
      case internal::CoroPollState::kAborted:
        error_handler_();
        return Ready();
      case internal::CoroPollState::kReady:
        return_value_ = std::move(*result);
        return Ready();
    }
  }

  Coro<T> coro_;
  AllocationErrorHandler error_handler_;
  std::optional<T> return_value_;
};

/// `FallibleCoroTask` specialization that discards the coroutine's return
/// value.
template <typename T, typename AllocationErrorHandler>
class FallibleCoroTask<T, AllocationErrorHandler, ReturnValuePolicy::kDiscard>
    final : public Task {
 public:
  FallibleCoroTask(Coro<T>&& coro, AllocationErrorHandler&& error_handler)
      : Task(PW_ASYNC_TASK_NAME("FallibleCoroTask")),
        coro_(std::move(coro)),
        error_handler_(std::move(error_handler)) {}

  FallibleCoroTask(const FallibleCoroTask&) = delete;
  FallibleCoroTask& operator=(const FallibleCoroTask&) = delete;
  FallibleCoroTask(FallibleCoroTask&&) = delete;
  FallibleCoroTask& operator=(FallibleCoroTask&&) = delete;

  ~FallibleCoroTask() override { Deregister(); }

  /// Returns whether this `FallibleCoroTask` wraps a valid `Coro` and can be
  /// pended. Pending a `!ok()` `FallibleCoroTask` calls the error handler.
  ///
  /// This will be `false` if `Coro` allocation failed.
  [[nodiscard]] bool ok() const { return coro_.ok(); }

 private:
  Poll<> DoPend(Context& cx) final {
    if (!coro_.ok()) {
      error_handler_();
      return Ready();
    }
    switch (coro_.Pend(cx).state()) {
      case internal::CoroPollState::kPending:
        return Pending();
      case internal::CoroPollState::kAborted:
        error_handler_();
        return Ready();
      case internal::CoroPollState::kReady:
        return Ready();
    }
  }

  Coro<T> coro_;
  AllocationErrorHandler error_handler_;
};

template <typename T, typename AllocationErrorHandler>
FallibleCoroTask(Coro<T>&&, AllocationErrorHandler&&)
    -> FallibleCoroTask<T, std::decay_t<AllocationErrorHandler>>;

/// @endsubmodule

}  // namespace pw::async2
