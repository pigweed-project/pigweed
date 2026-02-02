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

#include "pw_async2/task.h"
#include "pw_log/tokenized_args.h"

/// @submodule{pw_async2,tasks}

namespace pw::async2::internal {

/// A `Task` that the `Dispatcher` takes ownership of when it is posted.
///
/// `OwnedTask` adds a `virtual` `DoDestroy()` function. This function is called
/// by the dispatcher after the task completes. `DoDestroy()` must destroy the
/// task and deallocate its memory.
///
/// After it is posted to a dispatcher, an `OwnedTask` may only be accessed from
/// the dispatcher via the `Task::DoPend()` implementation.
class OwnedTask : public Task {
 protected:
  explicit constexpr OwnedTask(log::Token name = kDefaultName)
      : Task(name, Task::OwnedTag{}) {}

 private:
  friend Dispatcher;

  // Deregister is private on OwnedTask because it's unsafe to call after the
  // task is posted. The dispatcher could delete the task at any time, so posted
  // tasks should not be accessed outside of the dispatcher.
  using Task::Deregister;

  /// Destroys this task and frees its memory.
  ///
  /// This function is currently private and is only called the dispatcher. It
  /// could potentially be used by a `Task` delegating to other `OwnedTask`s.
  void Destroy() { DoDestroy(); }

  /// The `DoDestroy` implementation must destroy this task and free its memory.
  ///
  /// `DoDestroy` is normally invoked by a `Dispatcher` after a `Post`ed
  /// `OwnedTask` completes.
  ///
  /// This function is currently private, but potentially could be used by a
  /// `Task`s delegating to other `OwnedTask`s.
  virtual void DoDestroy() = 0;
};

/// @endsubmodule

}  // namespace pw::async2::internal
