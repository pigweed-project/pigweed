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

#include "pw_async/dispatcher.h"  // IWYU pragma: export
// IWYU pragma: begin_exports
// IWYU pragma: begin_exports
#include "pw_async_backend/fake_dispatcher.h"  // nogncheck
// IWYU pragma: end_exports
// IWYU pragma: end_exports

namespace pw::async::test {

/// `FakeDispatcher` is a `Dispatcher` implementation for use in unit tests.
///
/// Threading: `FakeDispatcher` is *NOT* thread-safe and, unlike other
/// `Dispatcher` implementations. This means that tasks must not be posted from
/// multiple threads at once, and tasks cannot be posted from other threads
/// while the dispatcher is executing.
///
/// Time: `FakeDispatcher` uses simulated time. `RunUntil()` and `RunFor()`
/// advance time immediately, and `now()` returns the current simulated time.
///
/// To support various `Task` backends, `FakeDispatcher` wraps a
/// `backend::NativeFakeDispatcher` that implements standard `FakeDispatcher`
/// behavior using `backend::NativeTask` objects.
class FakeDispatcher final : public Dispatcher {
 public:
  FakeDispatcher() : native_dispatcher_(*this) {}

  /// Execute all runnable tasks and return without advancing simulated time.
  /// Returns true iff any tasks were invoked during the run.
  bool RunUntilIdle() { return native_dispatcher_.RunUntilIdle(); }

  /// Run the dispatcher until Now() has reached `end_time`, executing all tasks
  /// that come due before then.
  /// Returns true iff any tasks were invoked during the run.
  bool RunUntil(chrono::SystemClock::time_point end_time) {
    return native_dispatcher_.RunUntil(end_time);
  }

  /// Run the Dispatcher until `duration` has elapsed, executing all tasks that
  /// come due in that period.
  /// Returns true iff any tasks were invoked during the run.
  bool RunFor(chrono::SystemClock::duration duration) {
    return native_dispatcher_.RunFor(duration);
  }

  /// Stop processing tasks. After calling RequestStop(), the next time the
  /// Dispatcher is run, all waiting Tasks will be dequeued and their
  /// TaskFunctions called with a PW_STATUS_CANCELLED status.
  void RequestStop() { native_dispatcher_.RequestStop(); }

  // Dispatcher overrides:
  void Post(Task& task) override { native_dispatcher_.Post(task); }
  void PostAfter(Task& task, chrono::SystemClock::duration delay) override {
    native_dispatcher_.PostAfter(task, delay);
  }
  void PostAt(Task& task, chrono::SystemClock::time_point time) override {
    native_dispatcher_.PostAt(task, time);
  }
  bool Cancel(Task& task) override { return native_dispatcher_.Cancel(task); }

  // VirtualSystemClock overrides:
  chrono::SystemClock::time_point now() override {
    return native_dispatcher_.now();
  }

  // Returns the inner NativeFakeDispatcher containing backend-specific
  // state/logic. Only non-portable code should call these methods!
  backend::NativeFakeDispatcher& native_type() { return native_dispatcher_; }
  const backend::NativeFakeDispatcher& native_type() const {
    return native_dispatcher_;
  }

 private:
  backend::NativeFakeDispatcher native_dispatcher_;
};

}  // namespace pw::async::test
