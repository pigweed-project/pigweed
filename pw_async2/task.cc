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

#include "pw_async2/internal/logging.h"
// logging.h must be included first

#include <mutex>

#include "pw_allocator/allocator.h"
#include "pw_allocator/internal/control_block.h"
#include "pw_assert/check.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/internal/config.h"
#include "pw_async2/task.h"
#include "pw_async2_private/yield.h"
#include "pw_log/log.h"
#include "pw_preprocessor/compiler.h"
#include "pw_thread/sleep.h"

#define PW_TASK_NAME_FMT() PW_LOG_TOKEN_FMT("pw_async2")

namespace pw::async2 {

Task::~Task() {
  PW_DCHECK_INT_EQ(
      state_,
      State::kUnposted,
      "Tasks must be deregistered before they are destroyed; "
      "the " PW_TASK_NAME_FMT() " task is still posted to a dispatcher",
      name_);
}

void Task::RemoveAllWakersLocked() {
  while (!wakers_.empty()) {
    Waker& waker = wakers_.front();
    wakers_.pop_front();
    waker.ClearTask();
  }
}

bool Task::IsRegistered() const {
  std::lock_guard lock(internal::lock());
  return state_ != State::kUnposted;
}

void Task::Deregister() {
  while (!TryDeregister()) {
    internal::YieldToAnyThread();
  }
}

bool Task::TryDeregister() {
  // This function does not use std::lock_guard since the UnpostAndReleaseRef
  // function releases the lock. Lock correctness is ensured by Clang's
  // thread safety annotations.
  internal::lock().lock();

  switch (state_) {
    case State::kUnposted:
      internal::lock().unlock();
      return true;
    case State::kSleeping:
      dispatcher_->RemoveSleepingTaskLocked(*this);
      break;
    case State::kWokenWhileRunning:
      dispatcher_->RemoveWokenTaskLocked(*this);
      [[fallthrough]];
    case State::kRunning:
      // Mark the task as deregistered. The dispatcher thread running the task
      // completes deregistration and moves the task to the unposted state.
      state_ = State::kDeregisteredButRunning;
      [[fallthrough]];
    case State::kDeregisteredButRunning:
      internal::lock().unlock();
      return false;
    case State::kWoken:
      dispatcher_->RemoveWokenTaskLocked(*this);
      break;
  }

  dispatcher_->DeregisterTask(*this);
  return true;
}

void Task::BlockingJoin() {
  while (true) {
    {
      std::lock_guard lock(internal::lock());
      if (state_ == State::kUnposted) {
        return;
      }
    }
    internal::YieldToAnyThread();
  }
}

allocator::internal::ControlBlock* Task::Unpost() {
  state_ = State::kUnposted;
  dispatcher_ = nullptr;
  RemoveAllWakersLocked();
  return std::exchange(control_block_, nullptr);
}

void Task::UnpostAndReleaseRef() {
  allocator::internal::ControlBlock* const control_block = Unpost();
  internal::lock().unlock();

  if (control_block != nullptr) {
    ReleaseSharedRef(control_block);
  }
}

void Task::UnpostAndReleaseRefFromDispatcherDestructor() {
  allocator::internal::ControlBlock* const control_block = Unpost();
  if (control_block != nullptr) {
    internal::lock().unlock();
    ReleaseSharedRef(control_block);
    internal::lock().lock();
  }
}

// Called by the dispatcher to run this task.
RunTaskResult Task::RunInDispatcher() {
  PW_LOG_DEBUG("Dispatcher running task " PW_TASK_NAME_FMT() ":%p",
               name_,
               static_cast<const void*>(this));

  // Reset the waker requirement flag before every run.
  waker_requirement_ = kWakerNeeded;

  // The task is pended without the lock held.
  const bool complete = Pend(*this).IsReady();

  // This function does not use std::lock_guard since the UnpostAndReleaseRef
  // function releases the lock. Lock correctness is ensured by Clang's
  // thread safety annotations.
  internal::lock().lock();

  if (complete || state_ == State::kDeregisteredButRunning) {
    switch (state_) {
      case State::kUnposted:
        // Invalid state -- if unregistered from another thread, the state
        // becomes kDeregisteredButRunning.
      case State::kSleeping:
        // If the task is sleeping, then another thread must have run the
        // dispatcher, which is invalid.
        PW_DASSERT(false);
        PW_UNREACHABLE;
      case State::kRunning:
      case State::kDeregisteredButRunning:
        break;
      case State::kWokenWhileRunning:
      case State::kWoken:
        dispatcher_->RemoveWokenTaskLocked(*this);
        break;
    }
    PW_LOG_DEBUG("Task " PW_TASK_NAME_FMT() ":%p completed",
                 name_,
                 static_cast<const void*>(this));

    UnpostAndReleaseRef();
    return RunTaskResult::kCompleted;
  }

  if (state_ == State::kRunning) {
    PW_LOG_DEBUG(
        "Dispatcher adding task " PW_TASK_NAME_FMT() ":%p to sleep queue",
        name_,
        static_cast<const void*>(this));

    if (waker_requirement_ == kWakerNeeded) {
      PW_CHECK(!wakers_.empty(),
               "Task " PW_TASK_NAME_FMT()
               ":%p returned Pending() without registering a waker",
               name_,
               static_cast<const void*>(this));
      state_ = State::kSleeping;
      dispatcher_->AddSleepingTaskLocked(*this);
    } else {
      state_ = State::kUnposted;
      dispatcher_ = nullptr;
    }
  } else if (state_ == State::kWokenWhileRunning) {
    state_ = State::kWoken;
  }
  internal::lock().unlock();

  PW_LOG_DEBUG(
      "Task " PW_TASK_NAME_FMT() ":%p finished its run and is still pending",
      name_,
      static_cast<const void*>(this));
  return RunTaskResult::kActive;
}

void Task::Wake() {
  PW_LOG_DEBUG("Dispatcher waking task " PW_TASK_NAME_FMT() ":%p",
               name_,
               static_cast<const void*>(this));

  switch (state_) {
    case State::kUnposted:
      PW_UNREACHABLE;
    case State::kSleeping:
      dispatcher_->RemoveSleepingTaskLocked(*this);
      // Wake away!
      state_ = State::kWoken;
      break;
    case State::kRunning:
      // Wake again to indicate that this task should be run once more,
      // as the state of the world may have changed since the task
      // started running.
      state_ = State::kWokenWhileRunning;
      break;
    case State::kDeregisteredButRunning:
      internal::lock().unlock();
      return;  // Do nothing: will be deregistered when the run finishes
    case State::kWokenWhileRunning:
    case State::kWoken:
      // Do nothing: this has already been woken.
      internal::lock().unlock();
      return;
  }
  dispatcher_->AddWokenTaskLocked(*this);
  dispatcher_->Wake();
}

}  // namespace pw::async2
