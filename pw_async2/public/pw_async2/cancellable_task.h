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

#include <tuple>

#include "pw_assert/assert.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
#include "pw_async2/waker.h"

namespace pw::async2 {

/// A wrapper around a ``Task`` that allows it to be gracefully cancelled.
/// The base task (``TaskType``) must have a non-private, non-final ``DoPend``.
template <typename TaskType>
class CancellableTask final : public TaskType {
 public:
  using TaskType::TaskType;

  /// Cancels the task.
  ///
  /// This operation does not immediately remove the task from the dispatcher.
  /// Instead, it wakes the task and arranges for it to complete gracefully.
  /// To confirm when the task has terminated, check ``IsRegistered()``.
  ///
  /// If the task has already completed, this is a no-op.
  void Cancel() {
    cancelled_ = true;
    cancel_waker_.Wake();
  }

 protected:
  Poll<> DoPend(Context& cx) final {
    if (cancelled_) {
      return Ready();
    }

    std::ignore = PW_ASYNC_TRY_STORE_WAKER(
        cx, cancel_waker_, "CancellableTask waiting for cancellation");
    return TaskType::DoPend(cx);
  }

 private:
  Waker cancel_waker_;
  bool cancelled_ = false;
};

}  // namespace pw::async2
