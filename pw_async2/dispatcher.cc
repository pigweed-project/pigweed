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

#include <atomic>
#include <iterator>
#include <mutex>
#include <utility>

#include "pw_allocator/shared_ptr.h"
#include "pw_assert/check.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/internal/config.h"
#include "pw_async2/internal/lock.h"
#include "pw_async2/waker.h"
#include "pw_async2_private/yield.h"
#include "pw_containers/intrusive_queue.h"
#include "pw_log/log.h"
#include "pw_log/tokenized_args.h"

namespace pw::async2 {

Dispatcher::~Dispatcher() {
  std::lock_guard lock(internal::lock());
  PW_CHECK(!has_tasks(),
           "Tasks are still registered when the Dispatcher is being "
           "destroyed. Call Terminate() before destruction to deregister all "
           "tasks.");
}

void Dispatcher::Terminate() {
  while (true) {
    {
      std::lock_guard lock(internal::lock());
      terminated_ = true;
      UnpostTaskList(woken_);
      UnpostTaskList(sleeping_);

      if (!has_tasks() && wakes_pending_.load(std::memory_order_acquire) == 0) {
        break;
      }
    }
    internal::YieldToAnyThread();
  }
}

void Dispatcher::Post(Task& task) {
  internal::lock().lock();
  PW_DCHECK(!terminated_,
            "Tasks cannot be posted to a Dispatcher that has been Terminated.");
  task.PostTo(*this);
  // To prevent duplicate wakes, request only if this is the first woken task.
  if (woken_.empty()) {
    wants_wake_ = true;
  }
  woken_.push_back(task);
  Wake();
}

bool Dispatcher::PostAllocatedTask(
    Task* task, allocator::internal::ControlBlock* control_block) {
  if (control_block == nullptr || !control_block->IncrementShared()) {
    return false;
  }

  // If control_block is non-null, task is non-null.
  task->SetControlBlockBeforePosted(*control_block);
  Post(*task);
  return true;
}

Task* Dispatcher::PopTaskToRunLocked() {
  if (woken_.empty()) {
    // There are no tasks ready to run, but the dispatcher should be woken when
    // tasks become ready or new tasks are posted.
    wants_wake_ = true;
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

void Dispatcher::UnpostTaskList(IntrusiveQueue<Task>& list) {
  while (!list.empty()) {
    Task& task = list.front();
    list.pop_front();
    task.UnpostAndReleaseRefFromDispatcherDestructor();
  }
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

void Dispatcher::Wake(Task* task_to_release) {
  const bool wanted_wake = std::exchange(wants_wake_, false);
  if (wanted_wake) {
    wakes_pending_.fetch_add(1, std::memory_order_acquire);
  }

  if (task_to_release == nullptr) {
    internal::lock().unlock();
  } else {
    task_to_release->UnpostAndReleaseRef();
  }

  if (wanted_wake) {
    DoWake();
    wakes_pending_.fetch_sub(1, std::memory_order_release);
  }
}

}  // namespace pw::async2
