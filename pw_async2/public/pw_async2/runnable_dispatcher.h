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

#include "pw_async2/dispatcher.h"
namespace pw::async2 {

/// @submodule{pw_async2,dispatchers}

/// `Dispatcher` that supports being run in a user-determined thread. Not all
/// `Dispatcher` implementations can be used in this way.
class RunnableDispatcher : public Dispatcher {
 public:
  /// Runs tasks until no further progress can be made.
  ///
  /// @retval true There are still sleeping tasks in the dispatcher.
  /// @retval false All tasks ran to completion.
  bool RunUntilStalled() { return DoRunUntilStalled(); }

  /// Runs tasks on the dispatcher until all tasks are completed, blocking the
  /// thread as needed.
  ///
  /// @note `RunToCompletion` returns when there are no tasks on the dispatcher,
  /// but new tasks could be posted to `RunToCompletion` from another thread
  /// before the function even returns.
  void RunToCompletion();

  /// Runs the dispatcher on this thread indefinitely, sleeping when there is no
  /// work to perform.
  [[noreturn]] void RunForever();

 private:
  // Allow DispatcherForTestFacade to wrap another dispatcher (call Do*).
  template <typename>
  friend class DispatcherForTestFacade;

  virtual bool DoRunUntilStalled() { return PopAndRunAllReadyTasks(); }

  void WaitForWake() { DoWaitForWake(); }

  /// Blocks until `DoWake()` is called. Must return immediately if `DoWake()`
  /// was already called since the last `DoWaitForWake` call.
  ///
  /// If the implementation is unable to block the thread, it must crash.
  virtual void DoWaitForWake() = 0;
};

/// @endsubmodule

}  // namespace pw::async2
