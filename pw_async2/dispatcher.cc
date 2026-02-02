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

#include "pw_async2/internal/logging.h"
// logging.h must be included first

#include <iterator>
#include <mutex>

#include "pw_assert/check.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/internal/config.h"
#include "pw_async2/internal/owned_task.h"
#include "pw_async2/waker.h"
#include "pw_log/log.h"
#include "pw_log/tokenized_args.h"

namespace pw::async2 {

void Dispatcher::Deregister() {
  std::lock_guard lock(internal::lock());
  UnpostTaskList(woken_);
  UnpostTaskList(sleeping_);
}

void Dispatcher::Post(Task& task) {
  {
    std::lock_guard lock(internal::lock());
    task.PostTo(*this);
    // To prevent duplicate wakes, request only if this is the first woken task.
    if (woken_.empty()) {
      SetWantsWake();
    }
    woken_.push_back(task);
  }
  // Unlike in `WakeTask`, here we know that the `Dispatcher` will not be
  // destroyed out from under our feet because we're in a method being called on
  // the `Dispatcher` by a user.
  Wake();
}

Task* Dispatcher::PopTaskToRunLocked() {
  if (woken_.empty()) {
    // There are no tasks ready to run, but the dispatcher should be woken when
    // tasks become ready or new tasks are posted.
    SetWantsWake();
    PW_LOG_DEBUG("Dispatcher has no woken tasks to run");
    return nullptr;
  }
  Task& task = woken_.front();
  woken_.pop_front();
  // The task must be marked running before the lock is released to prevent it
  // from being deregistered by another thread.
  task.MarkRunning();
  return &task;
}

bool Dispatcher::PopAndRunAllReadyTasks() {
  bool has_posted_tasks;
  Task* task;
  while ((task = PopTaskToRun(has_posted_tasks)) != nullptr) {
    RunTask(*task);
  }
  return has_posted_tasks;
}

Dispatcher::RunTaskResult Dispatcher::RunTask(Task& task) {
  const Task::RunResult run_result = task.RunInDispatcher();

  // If this is an OwnedTask, then no other threads should be accessing it, so
  // it is safe to destroy it without holding internal::lock().
  if (run_result == Task::kCompletedNeedsDestroy) {
    static_cast<internal::OwnedTask&>(task).Destroy();
    return RunTaskResult::kCompleted;
  }
  return static_cast<RunTaskResult>(run_result);
}

void Dispatcher::UnpostTaskList(IntrusiveList<Task>& list) {
  while (!list.empty()) {
    list.front().Unpost();
    list.pop_front();
  }
}

void Dispatcher::WakeTask(Task& task) {
  if (!task.Wake()) {
    return;
  }

  woken_.push_back(task);

  // It's quite annoying to make this call under the lock, as it can result in
  // extra thread wakeup/sleep cycles.
  //
  // However, releasing the lock first would allow for the possibility that the
  // `Dispatcher` has been destroyed, making the call invalid.
  Wake();
}

// TODO: b/456478818 - Provide task iteration API and rework LogRegisteredTasks
//     to use it.
void Dispatcher::LogRegisteredTasks() {
  PW_LOG_INFO("pw::async2::Dispatcher");
  std::lock_guard lock(internal::lock());

  PW_LOG_INFO("Woken tasks:");
  for (const Task& task : woken_) {
    PW_LOG_INFO("  - " PW_LOG_TOKEN_FMT("pw_async2") ":%p",
                task.name_,
                static_cast<const void*>(&task));
  }
  PW_LOG_INFO("Sleeping tasks:");
  for (const Task& task : sleeping_) {
    int waker_count = static_cast<int>(
        std::distance(task.wakers_.begin(), task.wakers_.end()));

    PW_LOG_INFO("  - " PW_LOG_TOKEN_FMT("pw_async2") ":%p (%d wakers)",
                task.name_,
                static_cast<const void*>(&task),
                waker_count);

    LogTaskWakers(task);
  }
}

void Dispatcher::LogTaskWakers([[maybe_unused]] const Task& task) {
#if PW_ASYNC2_DEBUG_WAIT_REASON
  int i = 0;
  for (const Waker& waker : task.wakers_) {
    i++;
    if (waker.wait_reason_ != log::kDefaultToken) {
      PW_LOG_INFO("    * Waker %d: " PW_LOG_TOKEN_FMT("pw_async2"),
                  i,
                  waker.wait_reason_);
    }
  }
#endif  // PW_ASYNC2_DEBUG_WAIT_REASON
}

}  // namespace pw::async2
