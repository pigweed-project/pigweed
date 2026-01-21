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

#include "pw_assert/check.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/internal/config.h"
#include "pw_async2/task.h"
#include "pw_log/log.h"
#include "pw_thread/sleep.h"

#define PW_TASK_NAME_FMT() PW_LOG_TOKEN_FMT("pw_async2")

namespace pw::async2 {
namespace {

void YieldToAnyThread() {
  // Sleep to yield the CPU in case work must be completed in a lower priority
  // priority thread to make progress. Depending on the RTOS, yield may not
  // allow lower priority threads to be scheduled.
  // TODO: b/456506369 - Switch to pw::this_thread::yield when it is updated.
  this_thread::sleep_for(chrono::SystemClock::duration(1));
}

}  // namespace

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
    waker.task_ = nullptr;
  }
}

void Task::AddWakerLocked(Waker& waker) {
  waker.task_ = this;
  wakers_.push_front(waker);
}

void Task::RemoveWakerLocked(Waker& waker) {
  wakers_.remove(waker);
  waker.task_ = nullptr;
#if PW_ASYNC2_DEBUG_WAIT_REASON
  waker.wait_reason_ = log::kDefaultToken;
#endif  // PW_ASYNC2_DEBUG_WAIT_REASON
}

bool Task::IsRegistered() const {
  std::lock_guard lock(internal::lock());
  return state_ != State::kUnposted;
}

void Task::Deregister() {
  while (!TryDeregister()) {
    YieldToAnyThread();
  }
}

bool Task::TryDeregister() {
  std::lock_guard lock(internal::lock());
  // TODO: b/456555552 - Ideally, it wouldn't be possible to call Deregister
  // on an OwnedTask. Currently it's private, but accessible via Task.
  // Consider having a common BaseTask without Deregister.
  PW_DCHECK(!owned_by_dispatcher_);

  switch (state_) {
    case State::kUnposted:
      return true;
    case State::kSleeping:
      dispatcher_->RemoveSleepingTaskLocked(*this);
      break;
    case State::kRunning:
      // Mark the task as deregistered. The dispatcher thread running the task
      // completes deregistration and moves the task to the unposted state.
      state_ = State::kDeregisteredButRunning;
      [[fallthrough]];
    case State::kDeregisteredButRunning:
      return false;
    case State::kWoken:
      dispatcher_->RemoveWokenTaskLocked(*this);
      break;
  }
  state_ = State::kUnposted;
  RemoveAllWakersLocked();

  // Wake the dispatcher up if this was the last task so that it can see that
  // all tasks have completed.
  if (dispatcher_->woken_.empty() && dispatcher_->sleeping_.empty()) {
    dispatcher_->Wake();
  }
  dispatcher_ = nullptr;
  return true;
}

void Task::Join() {
  while (true) {
    {
      std::lock_guard lock(internal::lock());
      if (state_ == State::kUnposted) {
        return;
      }
    }
    YieldToAnyThread();
  }
}

// Called by the dispatcher to run this task.
Task::RunResult Task::RunInDispatcher() {
  PW_LOG_DEBUG("Dispatcher running task " PW_TASK_NAME_FMT() ":%p",
               name_,
               static_cast<const void*>(this));

  // The task is pended without the lock held.
  bool complete;
  bool requires_waker;
  {
    Waker waker(*this);
    Context context(GetDispatcherWhileRunning(), waker);
    complete = Pend(context).IsReady();
    requires_waker = context.requires_waker_;
  }

  std::lock_guard lock(internal::lock());

  if (complete || state_ == State::kDeregisteredButRunning) {
    switch (state_) {
      case State::kUnposted: {
        // If the Task was already deregistered by another thread, it cannot be
        // an OwnedThread, so there is no need to destroy it.
        return kDeregistered;
      }
      case State::kSleeping:
        // If the task is sleeping, then another thread must have run the
        // dispatcher, which is invalid.
        PW_DASSERT(false);
        PW_UNREACHABLE;
      case State::kRunning:
      case State::kDeregisteredButRunning:
        break;
      case State::kWoken:
        dispatcher_->RemoveWokenTaskLocked(*this);
        break;
    }
    state_ = State::kUnposted;
    dispatcher_ = nullptr;
    RemoveAllWakersLocked();

    PW_LOG_DEBUG("Task " PW_TASK_NAME_FMT() ":%p completed",
                 name_,
                 static_cast<const void*>(this));
    return owned_by_dispatcher_ ? kCompletedNeedsDestroy : kCompleted;
  }

  if (state_ == State::kRunning) {
    PW_LOG_DEBUG(
        "Dispatcher adding task " PW_TASK_NAME_FMT() ":%p to sleep queue",
        name_,
        static_cast<const void*>(this));

    if (requires_waker) {
      PW_CHECK(!wakers_.empty(),
               "Task " PW_TASK_NAME_FMT()
               ":%p returned Pending() without registering a waker",
               name_,
               static_cast<const void*>(this));
      state_ = State::kSleeping;
      dispatcher_->AddSleepingTaskLocked(*this);
    } else {
      // Require the task to be manually re-posted.
      state_ = State::kUnposted;
      dispatcher_ = nullptr;
    }
  }
  PW_LOG_DEBUG(
      "Task " PW_TASK_NAME_FMT() ":%p finished its run and is still pending",
      name_,
      static_cast<const void*>(this));
  return kActive;
}

bool Task::Wake() {
  PW_LOG_DEBUG("Dispatcher waking task " PW_TASK_NAME_FMT() ":%p",
               name_,
               static_cast<const void*>(this));

  switch (state_) {
    case State::kWoken:
      // Do nothing: this has already been woken.
      return false;
    case State::kUnposted:
      // This should be unreachable.
      PW_CHECK(false);
    case State::kRunning:
      // Wake again to indicate that this task should be run once more,
      // as the state of the world may have changed since the task
      // started running.
      break;
    case State::kDeregisteredButRunning:
      return false;  // Do nothing: will be deregistered when the run finishes
    case State::kSleeping:
      dispatcher_->RemoveSleepingTaskLocked(*this);
      // Wake away!
      break;
  }
  state_ = State::kWoken;
  return true;
}

}  // namespace pw::async2
