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
#include <mutex>

#include "pw_async2/context.h"
#include "pw_async2/internal/lock.h"
#include "pw_async2/task.h"
#include "pw_async2/waker.h"
#include "pw_containers/intrusive_list.h"
#include "pw_sync/lock_annotations.h"

namespace pw::async2 {

/// @submodule{pw_async2,dispatchers}

/// A single-threaded cooperatively scheduled runtime for async tasks.
///
/// Dispatcher implementations must pop and run tasks with one of the following:
///
/// - `PopAndRunAllReadyTasks()` – Runs tasks until no progress can be made.
///   The dispatcher will be woken when a task is ready to run.
/// - `PopTaskToRun()` and `RunTask()` – Run tasks individually. Dispatcher
///   implementations MUST pop and run tasks until `PopTaskToRun()` returns
///   `nullptr`. The dispatcher will not be woken when a task becomes ready
///   unless `PopTaskToRun()` has returned `nullptr`.
/// - `PopSingleTaskForThisWake()` and `RunTask()` – Run tasks individually. It
///   `PopSingleTaskForThisWake` is intended for use then only a single task (or
///   one final task) should be executed. Is not necessary to call
///   `PopSingleTaskForThisWake()` until it returns `nullptr`. Each call can
///   result in one potentially redundant `DoWake()` call, so `PopTaskToRun`
///   should be used one multiple tasks are executed.
class Dispatcher {
 public:
  Dispatcher(Dispatcher&) = delete;
  Dispatcher& operator=(Dispatcher&) = delete;

  Dispatcher(Dispatcher&&) = delete;
  Dispatcher& operator=(Dispatcher&&) = delete;

  virtual ~Dispatcher() { Deregister(); }

  /// Tells the ``Dispatcher`` to run ``Task`` to completion.
  /// This method does not block.
  ///
  /// After ``Post`` is called, ``Task::Pend`` will be invoked once.
  /// If ``Task::Pend`` does not complete, the ``Dispatcher`` will wait
  /// until the ``Task`` is "awoken", at which point it will call ``Pend``
  /// again until the ``Task`` completes.
  ///
  /// This method is thread-safe and interrupt-safe.
  void Post(Task& task) PW_LOCKS_EXCLUDED(internal::lock());

  /// Outputs log statements about the tasks currently registered with this
  /// dispatcher.
  void LogRegisteredTasks() PW_LOCKS_EXCLUDED(internal::lock());

 protected:
  constexpr Dispatcher() = default;

  /// Pops and runs tasks until there are no tasks ready to run.
  ///
  /// This function may be called by dispatcher implementations to run tasks.
  /// This is a high-level function that runs all ready tasks without logging or
  /// metrics. For more control, use `PopTaskToRun` and `RunTask`.
  ///
  /// @retval true The dispatcher has posted tasks, but they are sleeping.
  /// @retval false The dispatcher has no posted tasks.
  bool PopAndRunAllReadyTasks() PW_LOCKS_EXCLUDED(internal::lock());

  /// Pops a task and marks it as running. The task must be passed to `RunTask`.
  ///
  /// `PopTaskToRun` MUST be called repeatedly until it returns `nullptr`, at
  /// which point the dispatcher will request a wake.
  Task* PopTaskToRun() PW_LOCKS_EXCLUDED(internal::lock()) {
    std::lock_guard lock(internal::lock());
    return PopTaskToRunLocked();
  }

  /// `PopTaskToRun` overload that optionally reports the whether the
  /// `Dispatcher` has registered tasks. This allows callers to distinguish
  /// between there being no woken tasks and no posted tasks at all.
  ///
  /// Like the no-argument overload, `PopTaskToRun` MUST be called repeatedly
  /// until it returns `nullptr`.
  ///
  /// @param[out] has_posted_tasks Set to `true` if the dispatcher has at least
  ///     one task posted, potentially including the task that was popped. Set
  ///     to `false` if the dispatcher has no posted tasks.
  /// @returns A pointer to a task that is ready to run, or `nullptr` if there
  ///     are no ready tasks.
  Task* PopTaskToRun(bool& has_posted_tasks)
      PW_LOCKS_EXCLUDED(internal::lock()) {
    std::lock_guard lock(internal::lock());
    Task* task = PopTaskToRunLocked();
    has_posted_tasks = task != nullptr || !sleeping_.empty();
    return task;
  }

  /// Pop a single task to run. Each call to `PopSingleTaskForThisWake` can
  /// result in up to one `DoWake()` call, so use `PopTaskToRun` or
  /// `PopAndRunAllReadyTasks` to run multiple tasks.
  Task* PopSingleTaskForThisWake() PW_LOCKS_EXCLUDED(internal::lock()) {
    std::lock_guard lock(internal::lock());
    SetWantsWake();
    return PopTaskToRunLocked();
  }

  /// Result from `Dispatcher::RunTask`. Reports the state of the task when it
  /// finished running.
  enum RunTaskResult {
    /// The task is still posted to the dispatcher.
    kActive = Task::kActive,

    /// The task was removed from the dispatcher by another thread.
    kDeregistered = Task::kDeregistered,

    /// The task finished running.
    kCompleted = Task::kCompleted,
  };

  /// Runs the task that was returned from `PopTaskToRun`.
  ///
  /// @warning Do NOT access the `Task` object after `RunTask` returns! The task
  /// could have destroyed, either by the dispatcher or another thread, even if
  /// `RunTask` returns `kActive`. It is only safe to access a popped task
  /// before calling `RunTask`, since it is marked as running and will not be
  /// destroyed until after it runs.
  RunTaskResult RunTask(Task& task) PW_LOCKS_EXCLUDED(internal::lock());

 private:
  friend class Task;
  friend class Waker;

  // Allow DispatcherForTestFacade to wrap another dispatcher (call Do*).
  template <typename>
  friend class DispatcherForTestFacade;

  /// Sends a wakeup signal to this `Dispatcher`.
  ///
  /// This method's implementation must ensure that the `Dispatcher` runs at
  /// some point in the future.
  ///
  /// `DoWake()` will only be called once until one of the following occurs:
  ///
  /// - `PopAndRunAllReadyTasks()` is called,
  /// - `PopTaskToRun()` returns `nullptr`, or
  /// - `PopSingleTaskForThisWake()` is called.
  ///
  /// @note The `internal::lock()` may or may not be held here, so it
  /// must not be acquired by `DoWake`, nor may `DoWake` assume that it has been
  /// acquired.
  virtual void DoWake() PW_LOCKS_EXCLUDED(internal::lock()) = 0;

  void Wake() {
    if (wants_wake_.exchange(false, std::memory_order_relaxed)) {
      DoWake();
    }
  }

  Task* PopTaskToRunLocked() PW_EXCLUSIVE_LOCKS_REQUIRED(internal::lock());

  // Removes references to this `Dispatcher` from all linked `Task`s and
  // `Waker`s.
  void Deregister() PW_LOCKS_EXCLUDED(internal::lock());

  static void UnpostTaskList(IntrusiveList<Task>& list)
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::lock());

  void RemoveWokenTaskLocked(Task& task)
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::lock()) {
    woken_.remove(task);
  }
  void RemoveSleepingTaskLocked(Task& task)
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::lock()) {
    sleeping_.remove(task);
  }
  void AddSleepingTaskLocked(Task& task)
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::lock()) {
    sleeping_.push_front(task);
  }

  // For use by ``Waker``.
  void WakeTask(Task& task) PW_EXCLUSIVE_LOCKS_REQUIRED(internal::lock());

  void LogTaskWakers(const Task& task)
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::lock());

  // Indicates that this Dispatcher should be woken when Wake() is called. This
  // prevents unnecessary wakes when, for example, multiple wakers wake the same
  // task or multiple tasks are posted before the dipsatcher runs.
  //
  // Must be called while the lock is held to prevent missed wakes.
  void SetWantsWake() PW_EXCLUSIVE_LOCKS_REQUIRED(internal::lock()) {
    wants_wake_.store(true, std::memory_order_relaxed);
  }

  IntrusiveList<Task> woken_ PW_GUARDED_BY(internal::lock());
  IntrusiveList<Task> sleeping_ PW_GUARDED_BY(internal::lock());

  // Latches wake requests to avoid duplicate DoWake calls.
  std::atomic<bool> wants_wake_ = false;
};

/// @endsubmodule

}  // namespace pw::async2
