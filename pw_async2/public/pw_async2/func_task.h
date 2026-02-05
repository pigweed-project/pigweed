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

#include <optional>

#include "pw_async2/dispatcher.h"
#include "pw_function/function.h"

namespace pw::async2 {

/// @submodule{pw_async2,tasks}

/// A `Task` that delegates to a provided function `func`.
///
/// The provided `func` may be any callable object (function, lambda, or class
/// with `operator()`) that accepts a `Context&` and returns a `Poll<>`.
///
/// The resulting `Task` implements `Pend` by invoking `func`.
template <typename Func = Function<Poll<>(Context&)>>
class FuncTask final : public Task {
 public:
  /// Creates a new `Task` that delegates `Pend` to `func`.
  explicit constexpr FuncTask(Func&& func) : func_(std::forward<Func>(func)) {}

  FuncTask(const FuncTask&) = delete;
  FuncTask& operator=(const FuncTask&) = delete;

  FuncTask(FuncTask&&) = delete;
  FuncTask& operator=(FuncTask&&) = delete;

  ~FuncTask() override { Deregister(); }

 private:
  Poll<> DoPend(Context& cx) override { return func_(cx); }

  Func func_;
};

template <typename Func>
FuncTask(Func&&) -> FuncTask<Func>;

/// Whether to store or discard the function's return value in `RunOnceTask`.
enum class ReturnValuePolicy : bool { kKeep, kDiscard };

/// Runs a function once in a task. The task calls the function then returns
/// `Ready`. The task may be posted again after it is joined.
///
/// The function's return value is stored and can be accessed through the
/// `value` member after joining, or by calling `Wait`. A `void`-returning
/// function use a different @ref RunOnceTask<Func, ReturnValuePolicy::kDiscard>
/// "RunOnceTask specialization" that does not provide the return value. To
/// discard the return value for non-`void` functions, use the
/// `ReturnValuePolicy::kDiscard` template argument.
///
/// @warning `RunOnceTask` should be used rarely, such as in tests, truly
/// one-off cases, or as a last resort for sync-async interop. `pw_async2`
/// should not be used as a work queue. Overuse of `RunOnceTask` forfeits the
/// benefits of `pw_async2`, scattering logic across a mess of callbacks instead
/// of organizing it linearly in a task.
template <typename Func = Function<Poll<>()>,
          ReturnValuePolicy = std::is_void_v<std::invoke_result_t<Func>>
                                  ? ReturnValuePolicy::kDiscard
                                  : ReturnValuePolicy::kKeep>
class RunOnceTask final : public Task {
 public:
  /// The return value of the function.
  using value_type =
      std::conditional_t<std::is_void_v<std::invoke_result_t<Func>>,
                         ReadyType,
                         std::invoke_result_t<Func>>;

  /// Creates a new `Task` that runs the provided function once.
  explicit constexpr RunOnceTask(Func&& func)
      : func_(std::forward<Func>(func)) {}

  RunOnceTask(const RunOnceTask&) = delete;
  RunOnceTask& operator=(const RunOnceTask&) = delete;

  RunOnceTask(RunOnceTask&&) = delete;
  RunOnceTask& operator=(RunOnceTask&&) = delete;

  ~RunOnceTask() override { Deregister(); }

  /// Returns whether the task ran and set that `value` to the function's return
  /// value.
  bool has_value() const { return return_value_.has_value(); }

  /// The return value from the function.
  ///
  /// @pre The task must have completed. Call `Join` to ensure it finished.
  value_type& value() { return return_value_.value(); }

  /// @copydoc value
  const value_type& value() const { return return_value_.value(); }

  /// Blocks until the task completes and returns a reference its return value.
  value_type& Wait() {
    Task::Join();
    return *return_value_;
  }

 private:
  Poll<> DoPend(Context&) override {
    if constexpr (std::is_void_v<std::invoke_result_t<Func>>) {
      func_();
      return_value_.emplace();
    } else {
      return_value_ = func_();
    }
    return Ready();
  }

  Func func_;
  std::optional<value_type> return_value_;
};

/// Runs a function once in a task. The task calls the function then returns
/// `Ready`. The task may be posted again after it is joined.
///
/// This is a `RunOnceTask` specialization that discards the return value, which
/// is the default for `void`-returning functions.
template <typename Func>
class RunOnceTask<Func, ReturnValuePolicy::kDiscard> final : public Task {
 public:
  explicit constexpr RunOnceTask(Func&& func)
      : func_(std::forward<Func>(func)) {}

  RunOnceTask(const RunOnceTask&) = delete;
  RunOnceTask& operator=(const RunOnceTask&) = delete;

  RunOnceTask(RunOnceTask&&) = delete;
  RunOnceTask& operator=(RunOnceTask&&) = delete;

  ~RunOnceTask() override { Deregister(); }

 private:
  Poll<> DoPend(Context&) override {
    (void)func_();
    return Ready();
  }

  Func func_;
};

template <typename Func>
RunOnceTask(Func&&) -> RunOnceTask<Func>;

/// @endsubmodule

}  // namespace pw::async2
