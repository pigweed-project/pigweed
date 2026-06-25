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
#include "pw_async_basic/dispatcher.h"

#include <mutex>
#include <utility>

#include "pw_assert/check.h"
#include "pw_chrono/system_clock.h"

using namespace std::chrono_literals;

namespace pw::async {

BasicDispatcher::~BasicDispatcher() {
  RequestStop();
  lock_.lock();
  DrainTaskQueue();
  lock_.unlock();
}

void BasicDispatcher::Run() {
  lock_.lock();
  while (!stop_requested_) {
    MaybeSleep();
    ExecuteDueTasks();
  }
  DrainTaskQueue();
  lock_.unlock();
}

void BasicDispatcher::RunUntilIdle() {
  lock_.lock();
  ExecuteDueTasks();
  if (stop_requested_) {
    DrainTaskQueue();
  }
  lock_.unlock();
}

void BasicDispatcher::RunUntil(chrono::SystemClock::time_point end_time) {
  lock_.lock();
  while (now() < end_time && !stop_requested_) {
    MaybeSleepUntil(end_time);
    ExecuteDueTasks();
  }
  if (stop_requested_) {
    DrainTaskQueue();
  }
  lock_.unlock();
}

void BasicDispatcher::RunFor(chrono::SystemClock::duration duration) {
  RunUntil(now() + duration);
}

void BasicDispatcher::MaybeSleep() { return MaybeSleepUntil(std::nullopt); }

void BasicDispatcher::MaybeSleepUntil(
    std::optional<chrono::SystemClock::time_point> wake_time = std::nullopt) {
  if (task_queue_.empty() || task_queue_.front().due_time_ > now()) {
    // Sleep until either the due time of the next task or the specified
    // wake_time if available. Otherwise sleep until a notification is received.
    // Notifications are sent when tasks are posted or 'stop' is requested.
    if (!task_queue_.empty()) {
      auto task_due_time = task_queue_.front().due_time_;
      wake_time = std::min(wake_time.value_or(task_due_time), task_due_time);
    }
    lock_.unlock();
    if (wake_time.has_value()) {
      std::ignore = timed_notification_.try_acquire_until(*wake_time);
    } else {
      timed_notification_.acquire();
    }
    lock_.lock();
  }
}

void BasicDispatcher::ExecuteTask(backend::NativeTask& task, Status status) {
  Context ctx{this, &task.task_};
  task(ctx, status);
  // task object might be freed already (e.g. HeapDispatcher).
}

void BasicDispatcher::ExecuteDueTasks() {
  while (!task_queue_.empty() && task_queue_.front().due_time_ <= now() &&
         !stop_requested_) {
    backend::NativeTask& task = task_queue_.front();
    task_queue_.pop_front();
    task.dispatcher_ = nullptr;

    lock_.unlock();
    ExecuteTask(task, OkStatus());
    lock_.lock();
  }
}

void BasicDispatcher::RequestStop() {
  {
    std::lock_guard lock(lock_);
    stop_requested_ = true;
  }
  timed_notification_.release();
}

void BasicDispatcher::DrainTaskQueue() {
  while (!task_queue_.empty()) {
    backend::NativeTask& task = task_queue_.front();
    task_queue_.pop_front();
    task.dispatcher_ = nullptr;

    lock_.unlock();
    ExecuteTask(task, Status::Cancelled());
    lock_.lock();
  }
}

void BasicDispatcher::Post(Task& task) {
  std::lock_guard lock(lock_);
  if (!task.native_type().unlisted()) {
    PW_CHECK(task.native_type().dispatcher_ == this,
             "Attempted to post a task that is already posted to a different "
             "Dispatcher.");
    if (task.native_type().due_time() <= now()) {
      // Do nothing for already-due tasks, to retain their queue position.
      return;
    }
  }
  PostTaskLocked(task.native_type(), now());
}

void BasicDispatcher::PostAt(Task& task, chrono::SystemClock::time_point time) {
  std::lock_guard lock(lock_);
  if (!task.native_type().unlisted()) {
    PW_CHECK(task.native_type().dispatcher_ == this,
             "Attempted to post a task that is already posted to a different "
             "Dispatcher.");
  }
  PostTaskLocked(task.native_type(), time);
}

bool BasicDispatcher::Cancel(Task& task) {
  std::lock_guard lock(lock_);
  bool removed = task_queue_.remove(task.native_type());
  if (removed) {
    task.native_type().dispatcher_ = nullptr;
  }
  return removed;
}

void BasicDispatcher::PostTaskLocked(backend::NativeTask& task,
                                     chrono::SystemClock::time_point time_due) {
  task.due_time_ = time_due;
  if (!task.unlisted()) {
    PW_CHECK(task.dispatcher_ == this,
             "Attempted to post a task that is already posted to a different "
             "Dispatcher.");
    task_queue_.remove(task);
  }
  task.dispatcher_ = this;
  // Insert the new task in the queue after all tasks with the same or earlier
  // deadline to ensure FIFO execution order.
  auto it_front = task_queue_.begin();
  auto it_behind = task_queue_.before_begin();
  while (it_front != task_queue_.end() && time_due >= it_front->due_time_) {
    ++it_front;
    ++it_behind;
  }
  task_queue_.insert_after(it_behind, task);
  timed_notification_.release();
}

}  // namespace pw::async
