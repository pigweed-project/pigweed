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

#include "pw_async2/context.h"
#include "pw_async2/future.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"

namespace pw::async2 {

/// @submodule{pw_async2,futures}

/// Creates a task that pends a future until it completes.
///
/// `FutureTask` can be initialized in a few ways:
///
/// - Move a future: `FutureTask task(std::move(future))
/// - Construct a future in place: `FutureTask<MyFuture> task(arg1, arg2)`
/// - Refer to an existing future: `FutureTask<MyFuture&> task(future)`
///
/// @warning `FutureTask` is intended for test and occasional production use. A
/// `FutureTask` does not contain logic, and relying too much on `FutureTasks`
/// could push logic out of async tasks, which nullifies the benefits of
/// `pw_async2`. Creating a task for each future is also less efficient than
/// having one task work with multiple futures.
template <typename T>
class FutureTask final : public Task {
 public:
  /// The type of the future that is pended by this task.
  using future_type = std::remove_reference_t<T>;

  /// The type produced by this tasks's future when it completes.
  using value_type = typename future_type::value_type;

  /// Constructs a `FutureTask`. Forwards arguments to the future's constructor
  /// for `FutureTask`s that own their future. Reference `FutureTask`s take a
  /// mutable reference to their future.
  ///
  /// Requires at least one argument. Default constructed futures are not
  /// permitted since since they cannot be pended.
  template <typename Arg, typename... Args>
  explicit constexpr FutureTask(Arg&& arg, Args&&... args)
      : Task(PW_ASYNC_TASK_NAME("FutureTask")),
        future_(std::forward<Arg>(arg), std::forward<Args>(args)...),
        output_(Pending()) {}

  ~FutureTask() override { Deregister(); }

  /// Takes the `Poll` result from the most recent task run. This function is
  /// NOT thread safe. It cannot be called when the task may run on another
  /// thread.
  Poll<value_type> TakePoll() { return std::move(output_); }

  /// Returns the value produced by the future.
  ///
  /// @pre The task MUST have been @ref Task::Join "joined" first.
  FutureValue<future_type>& value() & { return output_.value(); }
  /// @copydoc value
  const FutureValue<future_type>& value() const& { return output_.value(); }
  /// @copydoc value
  FutureValue<future_type>&& value() && { return std::move(output_).value(); }
  /// @copydoc value
  const FutureValue<future_type>&& value() const&& {
    return std::move(output_).value();
  }

  /// Joins that task, blocking until the future completes, then returns a
  /// reference its value.
  FutureValue<future_type>& Wait() {
    Task::Join();
    return *output_;
  }

 private:
  static_assert(Future<future_type>);

  Poll<> DoPend(Context& cx) override {
    output_ = future_.Pend(cx);
    return output_.Readiness();
  }

  T future_;
  Poll<value_type> output_;
};

template <typename T>
FutureTask(T&& value) -> FutureTask<T>;

/// @endsubmodule

}  // namespace pw::async2
