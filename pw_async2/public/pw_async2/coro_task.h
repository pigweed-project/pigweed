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

#include <optional>

#include "pw_async2/coro.h"
#include "pw_async2/func_task.h"
#include "pw_async2/task.h"

namespace pw::async2 {
namespace internal {

[[noreturn]] void CrashDueToCoroutineAllocationFailure();

}  // namespace internal

/// @submodule{pw_async2,coroutines}

template <typename T,
          ReturnValuePolicy policy = std::is_void_v<T>
                                         ? ReturnValuePolicy::kDiscard
                                         : ReturnValuePolicy::kKeep>
class CoroTask final : public Task {
 public:
  using value_type = T;

  CoroTask(Coro<T>&& coro)
      : Task(PW_ASYNC_TASK_NAME("CoroTask<T>")),
        coro_(std::move(coro)),
        return_value_(internal::CoroPollState::kPending) {
    if (!coro_.IsValid()) {
      internal::CrashDueToCoroutineAllocationFailure();
    }
  }

  CoroTask(const CoroTask&) = delete;
  CoroTask& operator=(const CoroTask&) = delete;
  CoroTask(CoroTask&&) = delete;
  CoroTask& operator=(CoroTask&&) = delete;

  ~CoroTask() override { Deregister(); }

  /// Returns whether the task ran and set that `value` to the function's return
  /// value.
  bool has_value() const { return return_value_.has_value(); }

  /// The return value from the coroutine.
  ///
  /// @pre The task must have completed. Call `Join` to ensure it has completed.
  value_type& value() { return return_value_.value(); }

  /// @copydoc value
  const value_type& value() const { return return_value_.value(); }

  /// Blocks until the task completes and returns a reference its return value.
  value_type& Wait() {
    Task::Join();
    return *return_value_;
  }

 private:
  Poll<> DoPend(Context& cx) final {
    // Coro::Pend() asserts if allocation failed (!coro_.IsValid()).
    return_value_ = coro_.Pend(cx);
    switch (return_value_.state()) {
      case internal::CoroPollState::kPending:
        return Pending();
      case internal::CoroPollState::kAborted:
        internal::CrashDueToCoroutineAllocationFailure();
      case internal::CoroPollState::kReady:
        return Ready();
    }
  }

  Coro<T> coro_;
  internal::CoroPoll<value_type> return_value_;
};

template <typename T>
class CoroTask<T, ReturnValuePolicy::kDiscard> final : public Task {
 public:
  CoroTask(Coro<T>&& coro)
      : Task(PW_ASYNC_TASK_NAME("CoroTask")), coro_(std::move(coro)) {
    if (!coro_.IsValid()) {
      internal::CrashDueToCoroutineAllocationFailure();
    }
  }

  CoroTask(const CoroTask&) = delete;
  CoroTask& operator=(const CoroTask&) = delete;
  CoroTask(CoroTask&&) = delete;
  CoroTask& operator=(CoroTask&&) = delete;

  ~CoroTask() override { Deregister(); }

 private:
  Poll<> DoPend(Context& cx) final {
    PW_ASSERT(coro_.IsValid());

    switch (coro_.Pend(cx).state()) {
      case internal::CoroPollState::kPending:
        return Pending();
      case internal::CoroPollState::kAborted:
        internal::CrashDueToCoroutineAllocationFailure();
      case internal::CoroPollState::kReady:
        return Ready();
    }
  }

  Coro<T> coro_;
};

template <typename T>
CoroTask(Coro<T>&&) -> CoroTask<T>;

/// @endsubmodule

}  // namespace pw::async2
