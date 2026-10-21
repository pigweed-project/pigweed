// Copyright 2022 The Pigweed Authors
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

#include <optional>

#include "pw_async/context.h"        // IWYU pragma: export
#include "pw_async/task_function.h"  // IWYU pragma: export
#include "pw_async_backend/task.h"   // IWYU pragma: export

namespace pw::async {

/// @module{pw_async}

namespace test {
class FakeDispatcher;
}

/// A `Task` represents a unit of work (`TaskFunction`) that can be executed on
/// a `Dispatcher`. To support various `Dispatcher` backends, it wraps a
/// `backend::NativeTask`, which contains backend-specific state and methods.
class Task final {
 public:
  /// The default constructor creates a `Task` without a function.
  /// `set_function()` must be called before posting the `Task`.
  Task() : native_type_(*this) {}

  /// Constructs a Task that calls `f` when executed on a `Dispatcher`.
  explicit Task(TaskFunction&& f) : native_type_(*this, std::move(f)) {}

  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;
  Task(Task&&) = delete;
  Task& operator=(Task&&) = delete;

  /// Configure the `TaskFunction` after construction. This MUST NOT be called
  /// while this `Task` is pending in a `Dispatcher`.
  void set_function(TaskFunction&& f) {
    native_type_.set_function(std::move(f));
  }

  /// Executes this task.
  void operator()(Context& ctx, Status status) { native_type_(ctx, status); }

  /// Returns the inner `NativeTask` containing backend-specific state. Only
  /// `Dispatcher` backends or non-portable code should call these methods!
  backend::NativeTask& native_type() { return native_type_; }
  const backend::NativeTask& native_type() const { return native_type_; }

 private:
  backend::NativeTask native_type_;
};

}  // namespace pw::async
