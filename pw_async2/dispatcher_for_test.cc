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

#include "pw_async2/dispatcher_for_test.h"

#include <atomic>
#include <utility>

#include "pw_assert/check.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
#include "pw_async2/waker.h"

namespace pw::async2 {
namespace {

using DispatcherForTestImpl =
    DispatcherForTestFacade<backend::NativeDispatcherForTest>;

}  // namespace

template <>
DispatcherForTestImpl::IdleTask::~IdleTask() {
  PW_CHECK(!IsRegistered());
}

template <>
DispatcherForTestImpl::~DispatcherForTestFacade() {
  PW_CHECK_INT_EQ(blocking_until_released_.load(std::memory_order_relaxed), 0);
  Terminate();
}

template <>
Poll<> DispatcherForTestImpl::IdleTask::DoPend(Context& cx) {
  // Store the waker before checking should_complete_ to avoid a race with the
  // Complete function.
  PW_ASYNC_STORE_WAKER(cx, waker_, "DispatcherForTest waiting for Release()");
  if (should_complete_.load(std::memory_order_relaxed)) {
    return Ready();
  }
  return Pending();
}

template <>
bool DispatcherForTestImpl::DoRunUntilStalled() {
  bool has_posted_tasks;
  Task* task;
  while ((task = PopTaskToRun(has_posted_tasks)) != nullptr) {
    tasks_polled_.fetch_add(1, std::memory_order_relaxed);
    if (RunTask(*task) == RunTaskResult::kCompleted) {
      tasks_completed_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  return has_posted_tasks;
}

template <>
void DispatcherForTestImpl::RunToCompletionUntilReleased() {
  int prior = blocking_until_released_.fetch_add(1, std::memory_order_acquire);
  PW_CHECK_INT_GE(prior, -1);
  PW_CHECK_INT_LE(prior, 0);

  bool orig_blocking_is_allowed_ = std::exchange(blocking_is_allowed_, true);

  Post(idle_task_);
  RunToCompletion();

  idle_task_.Reset();
  blocking_is_allowed_ = orig_blocking_is_allowed_;
}

template <>
void DispatcherForTestImpl::Release() {
  idle_task_.Complete();
  int prior = blocking_until_released_.fetch_sub(1, std::memory_order_release);
  PW_CHECK_INT_GE(prior, 0);
  PW_CHECK_INT_LE(prior, 1);
}

template <>
void DispatcherForTestImpl::DoWake() {
  wake_count_.fetch_add(1, std::memory_order_relaxed);
  native().DoWake();
}

template <>
void DispatcherForTestImpl::DoWaitForWake() {
  PW_CHECK(
      blocking_is_allowed_,
      "The DispatcherForTest attempted to block a thread to wait for a wake. "
      "If this is intentional, call dispatcher.AllowBlocking() to allow it.");
  native().DoWaitForWake();
}

}  // namespace pw::async2
