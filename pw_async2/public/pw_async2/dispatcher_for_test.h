// Copyright 2025 The Pigweed Authors
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

#include <atomic>
#include <utility>

#include "pw_async2/future_task.h"
#include "pw_async2/runnable_dispatcher.h"
#include "pw_async2/waker.h"

namespace pw::async2 {

/// @submodule{pw_async2,dispatchers}

/// `DispatcherForTestFacade` defines the interface for `DispatcherForTest`.
/// Backends must provide:
///
/// - A ``pw_async2_backend/native_dispatcher_for_test.h`` header.
/// - A class or alias named ``pw::async2::backend::NativeDispatcherForTest``
///   that:
///   - Implements `RunnableDispatcher`.
///   - Is default constructible.
template <typename Native>
class DispatcherForTestFacade final : public RunnableDispatcher {
 public:
  /// `DispatcherForTest` is default constructible.
  DispatcherForTestFacade() = default;

  DispatcherForTestFacade(const DispatcherForTestFacade&) = delete;
  DispatcherForTestFacade& operator=(const DispatcherForTestFacade&) = delete;

  DispatcherForTestFacade(DispatcherForTestFacade&&) = delete;
  DispatcherForTestFacade& operator=(DispatcherForTestFacade&&) = delete;

  ~DispatcherForTestFacade() override;

  /// Whether to allow the dispatcher to block by calling `DoWaitForWake`.
  /// `RunToCompletion` may block the thread if there are no tasks ready to run.
  void AllowBlocking() { blocking_is_allowed_ = true; }

  /// Runs a future in a `FutureTask` until no progress can be made.
  template <typename T>
  auto RunInTaskUntilStalled(T& future) PW_LOCKS_EXCLUDED(internal::lock()) {
    FutureTask<T&> task(future);
    native().Post(task);
    native().RunUntilStalled();

    // Ensure that the task is no longer registered, as it will be destroyed
    // once we return.
    //
    // This operation will not block because we are on the dispatcher thread
    // and the dispatcher is not currently running (we just ran it).
    task.Deregister();

    return task.TakePoll();
  }

  /// Runs the disptacher until all tasks have completed and
  /// `DispatcherForTest::Release` has been called. This allows a test to wait
  /// until a series of steps complete, independent of the tasks registered at a
  /// particular time.
  ///
  /// `Release()` must be called exactly once per `RunToCompletionUntilReleased`
  /// call on a different thread.
  void RunToCompletionUntilReleased();

  /// `Release()` allows a `RunToCompletionUntilReleased` call in a different
  /// thread to complete. `Release()` must be called exactly once per
  /// `RunToCompletionUntilReleased` call.
  void Release();

  /// Returns the total number of times the dispatcher has called a task's
  /// ``Pend()`` method.
  uint32_t tasks_polled() const {
    return tasks_polled_.load(std::memory_order_relaxed);
  }

  /// Returns the total number of tasks the dispatcher has run to completion.
  uint32_t tasks_completed() const {
    return tasks_completed_.load(std::memory_order_relaxed);
  }

  /// Returns the total number of times the dispatcher has been woken.
  uint32_t wake_count() const {
    return wake_count_.load(std::memory_order_relaxed);
  }

 private:
  // Task to keep the Dispatcher busy in RunToCompletionUntilReleased().
  class IdleTask : public Task {
   public:
    IdleTask() : Task(PW_ASYNC_TASK_NAME("IdleTask")) {}

    ~IdleTask() override;

    void Complete() {
      should_complete_.store(true, std::memory_order_relaxed);
      waker_.Wake();
    }

    void Reset() {
      PW_DASSERT(!IsRegistered());
      should_complete_.store(false, std::memory_order_relaxed);
    }

   private:
    Poll<> DoPend(Context& cx) override;

    Waker waker_;
    std::atomic<bool> should_complete_ = false;
  };

  // These functions are implemented in dispatcher_for_test.cc for the
  // NativeDispatcherForTest specialization only.
  bool DoRunUntilStalled() override;

  void DoWake() override;

  void DoWaitForWake() override;

  RunnableDispatcher& native() { return native_; }

  Native native_;
  IdleTask idle_task_;
  bool blocking_is_allowed_ = false;
  std::atomic<int> blocking_until_released_ = false;

  // TODO: b/401049619 - Optionally provide metrics for production dispatchers.
  std::atomic<uint32_t> tasks_polled_ = 0u;
  std::atomic<uint32_t> tasks_completed_ = 0u;
  std::atomic<uint32_t> wake_count_ = 0u;
};

/// @endsubmodule

}  // namespace pw::async2

#include "pw_async2_backend/native_dispatcher_for_test.h"

namespace pw::async2 {

/// @submodule{pw_async2,dispatchers}

/// `DispatcherForTest` is a `RunnableDispatcher` implementation to use in unit
/// tests. See `DispatcherForTestFacade` for details.
using DispatcherForTest =
    DispatcherForTestFacade<backend::NativeDispatcherForTest>;

/// @endsubmodule

}  // namespace pw::async2
