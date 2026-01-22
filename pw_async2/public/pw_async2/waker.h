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
#include "pw_async2/internal/config.h"
#include "pw_async2/internal/lock.h"
#include "pw_containers/intrusive_forward_list.h"
#include "pw_log/tokenized_args.h"
#include "pw_sync/lock_annotations.h"

namespace pw::async2 {

class Task;
class Waker;

namespace internal {

template <typename Callable>
[[nodiscard]] constexpr auto InvokeWithNodiscard(Callable&& callable) {
  return callable();
}

[[nodiscard]] bool CloneWaker(Waker& waker_in,
                              Waker& waker_out,
                              log::Token wait_reason = log::kDefaultToken)
    PW_LOCKS_EXCLUDED(internal::lock());

}  // namespace internal

/// @submodule{pw_async2,wakers}

/// Attempts to store a waker associated with the current context into
/// ``waker_or_queue_out``. If the waker cannot be stored into
/// ``waker_or_queue_out`` due to the target waker already referencing a task or
/// the queue being full, crashes.
///
/// When the stored waker is later awoken with
/// :cpp:func:`pw::async2::Waker::Wake`, the :cpp:class:`pw::async2::Task`
/// associated with ``cx`` will be awoken and its ``DoPend`` method will be
/// invoked again.
///
/// ``wait_reason_string`` is a human-readable description of why the task is
/// blocked. If the module configuration option ``PW_ASYNC2_DEBUG_WAIT_REASON``
/// is set, this string will be stored with the waker and reported by
/// ``Dispatcher::LogRegisteredTasks`` when its associated task is blocked.
#define PW_ASYNC_STORE_WAKER(context, waker_or_queue_out, wait_reason_string) \
  do {                                                                        \
    bool waker_or_queue_had_space = PW_ASYNC_TRY_STORE_WAKER(                 \
        context, waker_or_queue_out, wait_reason_string);                     \
    PW_ASSERT(waker_or_queue_had_space);                                      \
  } while (0)

/// Attempts to store a waker associated with the current context into
/// ``waker_or_queue_out``. If the waker cannot be stored into
/// ``waker_or_queue_out`` due to the target waker already referencing a task or
/// the queue being full, returns ``false`` and does not modify it.
///
/// When the stored waker is later awoken with
/// :cpp:func:`pw::async2::Waker::Wake`, the :cpp:class:`pw::async2::Task`
/// associated with ``cx`` will be awoken and its ``DoPend`` method will be
/// invoked again.
///
/// ``wait_reason_string`` is a human-readable description of why the task is
/// blocked. If the module configuration option ``PW_ASYNC2_DEBUG_WAIT_REASON``
/// is set, this string will be stored with the waker and reported by
/// ``Dispatcher::LogRegisteredTasks`` when its associated task is blocked.
#define PW_ASYNC_TRY_STORE_WAKER(                             \
    context, waker_or_queue_out, wait_reason_string)          \
  _PW_ASYNC_TRY_GET_WAKER(::pw::async2::internal::StoreWaker, \
                          context,                            \
                          waker_or_queue_out,                 \
                          wait_reason_string)

/// Attempts to store a waker associated with ``waker_in`` into
/// ``waker_or_queue_out``. If the waker cannot be stored into
/// ``waker_or_queue_out`` due to the target waker already referencing a task or
/// the queue being full, crashes.
///
/// When the stored waker is later awoken with
/// :cpp:func:`pw::async2::Waker::Wake`, the :cpp:class:`pw::async2::Task`
/// associated with ``cx`` will be awoken and its ``DoPend`` method will be
/// invoked again.
///
/// ``wait_reason_string`` is a human-readable description of why the task is
/// blocked. If the module configuration option ``PW_ASYNC2_DEBUG_WAIT_REASON``
/// is set, this string will be stored with the waker and reported by
/// ``Dispatcher::LogRegisteredTasks`` when its associated task is blocked.
#define PW_ASYNC_CLONE_WAKER(waker_in, waker_or_queue_out, wait_reason_string) \
  do {                                                                         \
    bool waker_or_queue_had_space = PW_ASYNC_TRY_CLONE_WAKER(                  \
        waker_in, waker_or_queue_out, wait_reason_string);                     \
    PW_ASSERT(waker_or_queue_had_space);                                       \
  } while (0)

/// Attempts to store a waker associated with ``waker_in`` into
/// ``waker_or_queue_out``. If the waker cannot be stored into
/// ``waker_or_queue_out`` due to the target waker already referencing a task or
/// the queue being full, returns ``false`` and does not modify it.
///
/// When the stored waker is later awoken with
/// :cpp:func:`pw::async2::Waker::Wake`, the :cpp:class:`pw::async2::Task`
/// associated with ``cx`` will be awoken and its ``DoPend`` method will be
/// invoked again.
///
/// ``wait_reason_string`` is a human-readable description of why the task is
/// blocked. If the module configuration option ``PW_ASYNC2_DEBUG_WAIT_REASON``
/// is set, this string will be stored with the waker and reported by
/// ``Dispatcher::LogRegisteredTasks`` when its associated task is blocked.
#define PW_ASYNC_TRY_CLONE_WAKER(                             \
    waker_in, waker_or_queue_out, wait_reason_string)         \
  _PW_ASYNC_TRY_GET_WAKER(::pw::async2::internal::CloneWaker, \
                          waker_in,                           \
                          waker_or_queue_out,                 \
                          wait_reason_string)

/// @endsubmodule

// Base implementation of the TRY_{STORE,CLONE}_WAKER macros.
#define _PW_ASYNC_TRY_GET_WAKER(                                        \
    func, source, waker_or_queue_out, wait_reason_string)               \
  ::pw::async2::internal::InvokeWithNodiscard(                          \
      [&]() PW_NO_LOCK_SAFETY_ANALYSIS {                                \
        [[maybe_unused]] constexpr const char*                          \
            pw_async2_wait_reason_must_be_string = wait_reason_string;  \
        constexpr ::pw::log::Token pw_async2_wait_reason =              \
            PW_LOG_TOKEN("pw_async2", wait_reason_string);              \
        return func(source, waker_or_queue_out, pw_async2_wait_reason); \
      })

/// @submodule{pw_async2,wakers}

/// An object which can respond to asynchronous events by queueing work to
/// be done in response, such as placing a ``Task`` on a ``Dispatcher``
/// loop.
///
/// ``Waker`` s are often held by I/O objects, custom concurrency
/// primitives, or interrupt handlers. Once the thing the ``Task`` was
/// waiting for is available, ``Wake`` should be called so that the ``Task``
/// is alerted and may process the event.
///
/// ``Waker`` s may be held for any lifetime, and will be automatically
/// nullified when the underlying ``Dispatcher`` or ``Task`` is deleted.
///
/// ``Waker`` s are most commonly created by ``Dispatcher`` s, which pass
/// them into ``Task::Pend`` via its ``Context`` argument.
class Waker : public pw::IntrusiveForwardList<Waker>::Item {
  friend class Task;
  friend class Dispatcher;

 public:
  constexpr Waker() = default;
  Waker(Waker&& other) noexcept PW_LOCKS_EXCLUDED(internal::lock());

  /// Replace this ``Waker`` with another.
  ///
  /// This operation is guaranteed to be thread-safe.
  Waker& operator=(Waker&& other) noexcept PW_LOCKS_EXCLUDED(internal::lock());

  ~Waker() noexcept { RemoveFromTaskWakerList(); }

  /// Wakes up the ``Waker``'s creator, alerting it that an asynchronous
  /// event has occurred that may allow it to make progress.
  ///
  /// \note Wake() causes the waker to remove itself from the Task's waker list.
  ///
  /// This operation is guaranteed to be thread-safe.
  void Wake() PW_LOCKS_EXCLUDED(internal::lock());

  /// Returns whether this ``Waker`` is empty.
  ///
  /// Empty wakers are those that perform no action upon wake. These may be
  /// created either via the default no-argument constructor or by
  /// calling ``Clear`` or ``std::move`` on a ``Waker``, after which the
  /// moved-from ``Waker`` will be empty.
  ///
  /// This operation is guaranteed to be thread-safe.
  [[nodiscard]] bool IsEmpty() const PW_LOCKS_EXCLUDED(internal::lock());

  /// Clears this ``Waker``.
  ///
  /// After this call, ``Wake`` will no longer perform any action, and
  /// ``IsEmpty`` will return ``true``.
  ///
  /// This operation is guaranteed to be thread-safe.
  void Clear() PW_LOCKS_EXCLUDED(internal::lock()) {
    RemoveFromTaskWakerList();
  }

 private:
  friend bool internal::CloneWaker(Waker& waker_in,
                                   Waker& waker_out,
                                   log::Token wait_reason);

  Waker(Task& task) PW_LOCKS_EXCLUDED(internal::lock()) : task_(&task) {
    InsertIntoTaskWakerList();
  }

  /// INTERNAL-ONLY: users should use the `PW_ASYNC_CLONE_WAKER` macro.
  ///
  /// Creates a second ``Waker`` from this ``Waker``.
  ///
  /// ``Clone`` is made explicit in order to allow for easier tracking of
  /// the different ``Waker``s that may wake up a ``Task``.
  ///
  /// This operation is guaranteed to be thread-safe.
  void InternalCloneIntoLocked(Waker& waker_out, log::Token wait_reason) &
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::lock());

  void InsertIntoTaskWakerList();
  void InsertIntoTaskWakerListLocked()
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::lock());
  void RemoveFromTaskWakerList();
  void RemoveFromTaskWakerListLocked()
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::lock());

  // The ``Task`` to poll when awoken.
  Task* task_ PW_GUARDED_BY(internal::lock()) = nullptr;

#if PW_ASYNC2_DEBUG_WAIT_REASON
  log::Token wait_reason_ = log::kDefaultToken;
#endif  // PW_ASYNC2_DEBUG_WAIT_REASON
};

/// @endsubmodule

}  // namespace pw::async2
