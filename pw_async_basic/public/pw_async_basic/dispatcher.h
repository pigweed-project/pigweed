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

#include "pw_async/dispatcher.h"
#include "pw_async/task.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/timed_thread_notification.h"
#include "pw_thread/thread_core.h"

namespace pw::async {

/// @module{pw_async_basic}

/// BasicDispatcher is a generic implementation of Dispatcher.
class BasicDispatcher : public Dispatcher, public thread::ThreadCore {
 public:
  explicit BasicDispatcher() = default;
  ~BasicDispatcher() override;

  /// Execute all runnable tasks and return without waiting.
  void RunUntilIdle();

  /// Run the dispatcher until Now() has reached `end_time`, executing all tasks
  /// that come due before then.
  void RunUntil(chrono::SystemClock::time_point end_time);

  /// Run the dispatcher until `duration` has elapsed, executing all tasks that
  /// come due in that period.
  void RunFor(chrono::SystemClock::duration duration);

  /// Stop processing tasks. If the dispatcher is serving a task loop, break out
  /// of the loop, dequeue all waiting tasks, and call their TaskFunctions with
  /// a PW_STATUS_CANCELLED status. If no task loop is being served, execute the
  /// dequeueing procedure the next time the Dispatcher is run.
  void RequestStop() PW_LOCKS_EXCLUDED(lock_);

  // ThreadCore overrides:

  /// Run the dispatcher until RequestStop() is called. Overrides
  /// ThreadCore::Run() so that BasicDispatcher is compatible with
  /// pw::Thread.
  void Run() override PW_LOCKS_EXCLUDED(lock_);

  // Dispatcher overrides:
  void Post(Task& task) override;
  void PostAt(Task& task, chrono::SystemClock::time_point time) override;
  bool Cancel(Task& task) override PW_LOCKS_EXCLUDED(lock_);

  // VirtualSystemClock overrides:
  chrono::SystemClock::time_point now() override {
    return chrono::SystemClock::now();
  }

 protected:
  /// Execute the given task and provide the status to it.
  ///
  /// Note: Once `ExecuteTask` has finished executing, the task object might
  /// have been freed already (e.g. HeapDispatcher).
  virtual void ExecuteTask(backend::NativeTask& task, Status status)
      PW_LOCKS_EXCLUDED(lock_);

 private:
  // Insert |task| into task_queue_ maintaining its min-heap property, keyed by
  // |time_due|.
  void PostTaskLocked(backend::NativeTask& task,
                      chrono::SystemClock::time_point time_due)
      PW_EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // If no tasks are due, sleep until a notification is received or the next
  // task comes due; whichever occurs first.
  void MaybeSleep() PW_EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // If no tasks are due, sleep until a notification is received, the next task
  // comes due, or a timeout elapses; whichever occurs first.
  void MaybeSleepUntil(std::optional<chrono::SystemClock::time_point> wake_time)
      PW_EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // Dequeue and run each task that is due.
  void ExecuteDueTasks() PW_EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // Dequeue each task and call each TaskFunction with a PW_STATUS_CANCELLED
  // status.
  void DrainTaskQueue() PW_EXCLUSIVE_LOCKS_REQUIRED(lock_);

  sync::InterruptSpinLock lock_;
  sync::TimedThreadNotification timed_notification_;
  bool stop_requested_ PW_GUARDED_BY(lock_) = false;
  // A priority queue of scheduled Tasks sorted by earliest due times first.
  IntrusiveList<backend::NativeTask> task_queue_ PW_GUARDED_BY(lock_);
};

}  // namespace pw::async
