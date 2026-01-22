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

#include "pw_async2/poll.h"
#include "pw_async2/waker.h"
#include "pw_log/tokenized_args.h"

namespace pw::async2 {

class Context;
class Dispatcher;

namespace internal {

/// INTERNAL-ONLY: users should use the `PW_ASYNC_STORE_WAKER` macro instead.
///
/// Saves a ``Waker`` into ``waker_out`` which, when awoken, will cause the
/// current task to be ``Pend``'d by its dispatcher.
[[nodiscard]] bool StoreWaker(Context& cx,
                              Waker& waker_out,
                              log::Token wait_reason);

}  // namespace internal

/// @submodule{pw_async2,context}

/// Context for an asynchronous ``Task``.
///
/// This object contains resources needed for scheduling asynchronous work,
/// such as the current ``Dispatcher`` and the ``Waker`` for the current task.
///
/// ``Context`` s are most often created by ``Dispatcher`` s, which pass them
/// into ``Task::Pend``.
class Context {
 public:
  /// Creates a new ``Context`` containing the currently-running ``Dispatcher``
  /// and a ``Waker`` for the current ``Task``.
  Context(Dispatcher& dispatcher, Waker& waker)
      : dispatcher_(&dispatcher), waker_(&waker), requires_waker_(true) {}

  /// The ``Dispatcher`` on which the current ``Task`` is executing.
  ///
  /// This can be used for spawning new tasks using
  /// ``dispatcher().Post(task);``.
  Dispatcher& dispatcher() { return *dispatcher_; }

  /// Queues the current ``Task::Pend`` to run again in the future, possibly
  /// after other work is performed.
  ///
  /// This may be used by ``Task`` implementations that wish to provide
  /// additional fairness by yielding to the dispatch loop rather than perform
  /// too much work in a single iteration.
  ///
  /// This is semantically equivalent to calling:
  ///
  /// ```
  /// Waker waker;
  /// PW_ASYNC_STORE_WAKER(cx, waker, ...);
  /// waker.Wake();
  /// ```
  void ReEnqueue();

  /// Indicates that the task has not completed, but that it also does not need
  /// to register a waker and go to sleep. This results in the task being
  /// removed from the dispatcher, requiring it to be manually re-posted to run
  /// again.
  template <typename T = ReadyType>
  Poll<T> Unschedule() {
    requires_waker_ = false;
    return Pending();
  }

 private:
  friend class Task;
  friend bool internal::StoreWaker(Context& cx,
                                   Waker& waker_out,
                                   log::Token wait_reason);

  Dispatcher* dispatcher_;
  Waker* waker_;
  bool requires_waker_;
};

/// @endsubmodule

}  // namespace pw::async2
