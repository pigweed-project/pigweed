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

#include "pw_assert/assert.h"
#include "pw_async2/context.h"
#include "pw_async2/internal/lock.h"
#include "pw_async2/poll.h"
#include "pw_containers/intrusive_forward_list.h"
#include "pw_containers/intrusive_list.h"
#include "pw_log/tokenized_args.h"
#include "pw_sync/lock_annotations.h"

namespace pw::async2 {
namespace internal {

class OwnedTask;

}  // namespace internal

/// @submodule{pw_async2,tasks}

/// Generates a token for use as a task name.
#define PW_ASYNC_TASK_NAME(name) PW_LOG_TOKEN_EXPR("pw_async2", name)

class Dispatcher;

/// A task which may complete one or more asynchronous operations.
///
/// The `Task` interface is commonly implemented by users wishing to schedule
/// work on an  asynchronous `Dispatcher`. To do this, users may subclass
/// `Task`, providing an implementation of the `DoPend` method which advances
/// the state of the `Task` as far as possible before yielding back to the
/// `Dispatcher`.
///
/// This process works similarly to cooperatively-scheduled green threads or
/// coroutines, with a `Task` representing a single logical "thread" of
/// execution. Unlike some green thread or coroutine implementations, `Task`
/// does not imply a separately-allocated stack: `Task` state is most commonly
/// stored in fields of the `Task` subclass.
///
/// Once defined by a user, `Task`s may be run by passing them to a `Dispatcher`
/// via `Dispatcher::Post`. The `Dispatcher` will then `Pend` the `Task` every
/// time that the `Task` indicates it is able to make progress.
///
/// Note that `Task` objects *must not* be destroyed while they are actively
/// being `Pend`'d by a `Dispatcher`. To protect against this, be sure to do one
/// of the following:
///
/// - Use dynamic lifetimes by creating `OwnedTask` objects that continue to
///   live until they receive a `DoDestroy` call.
/// - Create `Task` objects whose stack-based lifetimes outlive their associated
///   `Dispatcher`.
/// - Call `Deregister` on the `Task` prior to its destruction. NOTE that
///   `Deregister` may not be called from inside the `Task`'s own `Pend` method.
class Task : public IntrusiveList<Task>::Item {
 public:
  /// Creates a task with the specified name. To generate a name token, use the
  /// `PW_ASYNC_TASK_NAME` macro, e.g.
  ///
  /// @code{.cpp}
  /// class MyTask : public pw::async2::Task {
  ///   MyTask() : pw::async2::Task(PW_ASYNC_TASK_NAME("MyTask")) {}
  /// };
  /// @endcode
  explicit constexpr Task(log::Token name = kDefaultName) : name_(name) {}

  Task(const Task&) = delete;
  Task(Task&&) = delete;
  Task& operator=(const Task&) = delete;
  Task& operator=(Task&&) = delete;

  /// The task must not be registered with a `Dispatcher` upon destruction.
  /// Tasks are deregistered automatically upon completion or `Dispatcher`
  /// destruction. This is necessary to ensure that neither the `Dispatcher` nor
  /// `Waker` reference the `Task` object after destruction.
  ///
  /// The `Task` destructor cannot perform this deregistration. Other threads
  /// may be polling the task, and since the subclass destructor runs first,
  /// subclass state accessed by `Pend` would be invalidated before the base
  /// destructor could deregister the task.
  virtual ~Task();

  /// A public interface for `DoPend`.
  ///
  /// `DoPend` is normally invoked by a `Dispatcher` after a `Task` has been
  /// `Post` ed.
  ///
  /// This wrapper should only be called by `Task` s delegating to other `Task`
  /// s.  For example, a `class MainTask` might have separate fields for
  /// `TaskA` and `TaskB`, and could invoke `Pend` on these types within its own
  /// `DoPend` implementation.
  Poll<> Pend(Context& cx) { return DoPend(cx); }

  /// Whether or not the `Task` is registered with a `Dispatcher`.
  ///
  /// Returns `true` after this `Task` is passed to `Dispatcher::Post` until one
  /// of the following occurs:
  ///
  /// - This `Task` returns `Ready` from its `Pend` method.
  /// - `Task::Deregister` is called.
  /// - The associated `Dispatcher` is destroyed.
  bool IsRegistered() const;

  /// Deregisters this `Task` from the linked `Dispatcher` and any associated
  /// `Waker` values.
  ///
  /// This must not be invoked from inside this task's `Pend` function, as this
  /// will result in a deadlock.
  ///
  /// Deregister will *not* destroy the underlying `Task`.
  ///
  /// @note If this task's `Pend` method is currently being run on the
  /// dispatcher, this method will block until `Pend` completes.
  ///
  /// @warning This method cannot guard against the dispatcher itself being
  /// destroyed, so this method must not be called concurrently with destruction
  /// of the dispatcher associated with this `Task`.
  void Deregister() PW_LOCKS_EXCLUDED(internal::lock());

  /// Blocks this thread until the task finishes running.
  ///
  /// @pre The task must be posted to a `Dispatcher` that runs in a different
  /// thread.
  void Join() PW_LOCKS_EXCLUDED(internal::lock());

 private:
  friend class Dispatcher;
  friend internal::OwnedTask;
  friend class Waker;

  static constexpr log::Token kDefaultName =
      PW_LOG_TOKEN("pw_async2", "(anonymous)");

  struct OwnedTag {};

  // Constructor for OwnedTask objects.
  constexpr Task(log::Token name, OwnedTag)
      : owned_by_dispatcher_(true), name_(name) {}

  /// Attempts to deregister this task.
  ///
  /// If the task is currently running, this will return false and the task
  /// will not be deregistered.
  bool TryDeregister() PW_LOCKS_EXCLUDED(internal::lock());

  /// Attempts to advance this `Task` to completion.
  ///
  /// This method should not perform synchronous waiting, as doing so may block
  /// the main `Dispatcher` loop and prevent other `Task` s from
  /// progressing. Because of this, `Task` s should not invoke blocking
  /// `Dispatcher` methods such as `RunUntilComplete`.
  ///
  /// `Task`s should also avoid invoking `RunUntilStalled` on their own
  /// `Dispatcher`.
  ///
  /// Returns `Ready` if complete, or `Pending` if the `Task` was not yet
  /// able to complete.
  ///
  /// If `Pending` is returned, the `Task` must ensure it is woken up when
  /// it is able to make progress. To do this, `Task::Pend` must arrange for
  /// `Waker::Wake` to be called, either by storing a copy of the `Waker`
  /// away to be awoken by another system (such as an interrupt handler).
  virtual Poll<> DoPend(Context&) = 0;

  // Sets this task to use the provided dispatcher.
  void PostTo(Dispatcher& dispatcher)
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::lock()) {
    PW_DASSERT(state_ == State::kUnposted);
    PW_DASSERT(dispatcher_ == nullptr);
    state_ = State::kWoken;
    dispatcher_ = &dispatcher;
  }

  // Clears the task's dispatcher.
  void Unpost() PW_EXCLUSIVE_LOCKS_REQUIRED(internal::lock()) {
    state_ = State::kUnposted;
    dispatcher_ = nullptr;
    RemoveAllWakersLocked();
  }

  void MarkRunning() PW_EXCLUSIVE_LOCKS_REQUIRED(internal::lock()) {
    state_ = State::kRunning;
  }

  // Result from `RunInDispatcher`. This is a superset of
  // Dispatcher::RunTaskResult, which merges kCompletedNeedsDestroy into
  // kCompleted.
  enum RunResult {
    // The task is still posted to the dispatcher.
    kActive,

    // The task was removed from the dispatcher by another thread.
    kDeregistered,

    // The task finished running.
    kCompleted,

    // The task finished running and must be deleted by the dispatcher.
    kCompletedNeedsDestroy,
  };

  // Called by the dispatcher to run this task. The task has already been marked
  // as running.
  RunResult RunInDispatcher() PW_LOCKS_EXCLUDED(internal::lock());

  // Called by the dispatcher to wake this task. Returns whether the task
  // actually needed to be woken.
  [[nodiscard]] bool Wake() PW_EXCLUSIVE_LOCKS_REQUIRED(internal::lock());

  // Unlinks all `Waker` objects associated with this `Task.`
  void RemoveAllWakersLocked() PW_EXCLUSIVE_LOCKS_REQUIRED(internal::lock());

  // Adds a `Waker` to the linked list of `Waker` s tracked by this
  // `Task`.
  void AddWakerLocked(Waker&) PW_EXCLUSIVE_LOCKS_REQUIRED(internal::lock());

  // Removes a `Waker` from the linked list of `Waker`s tracked by this `Task`.
  //
  // Precondition: the provided waker *must* be in the list of `Waker`s tracked
  // by this `Task`.
  void RemoveWakerLocked(Waker&) PW_EXCLUSIVE_LOCKS_REQUIRED(internal::lock());

  Dispatcher& GetDispatcherWhileRunning() PW_NO_LOCK_SAFETY_ANALYSIS {
    return *dispatcher_;
  }

  enum class State : unsigned char {
    kUnposted,
    kRunning,
    kDeregisteredButRunning,
    kWoken,
    kSleeping,
  };

  // The current state of the task.
  State state_ PW_GUARDED_BY(internal::lock()) = State::kUnposted;

  bool owned_by_dispatcher_ PW_GUARDED_BY(internal::lock()) = false;

  // A pointer to the dispatcher this task is associated with.
  //
  // This will be non-null when `state_` is anything other than `kUnposted`.
  //
  // This value must be cleared by the dispatcher upon destruction in order to
  // prevent null access.
  Dispatcher* dispatcher_ PW_GUARDED_BY(internal::lock()) = nullptr;

  // Linked list of `Waker` s that may awaken this `Task`.
  IntrusiveForwardList<Waker> wakers_ PW_GUARDED_BY(internal::lock());

  // Optional user-facing name for the task. If set, it will be included in
  // debug logs.
  log::Token name_;
};

/// @endsubmodule

}  // namespace pw::async2
