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

#include "pw_async/context.h"
#include "pw_async/task_function.h"
#include "pw_chrono/system_clock.h"
#include "pw_containers/intrusive_list.h"

namespace pw::async {
class Dispatcher;
class BasicDispatcher;
namespace test::backend {
class NativeFakeDispatcher;
}
}  // namespace pw::async

namespace pw::async::backend {

// Task backend for BasicDispatcher.
class NativeTask final : public IntrusiveList<NativeTask>::Item {
 private:
  friend class ::pw::async::Task;
  friend class ::pw::async::BasicDispatcher;
  friend class ::pw::async::test::backend::NativeFakeDispatcher;

  NativeTask(::pw::async::Task& task) : task_(task) {}
  explicit NativeTask(::pw::async::Task& task, TaskFunction&& f)
      : func_(std::move(f)), task_(task) {}
  void operator()(Context& ctx, Status status) { func_(ctx, status); }
  void set_function(TaskFunction&& f) { func_ = std::move(f); }

  pw::chrono::SystemClock::time_point due_time() const { return due_time_; }
  void set_due_time(chrono::SystemClock::time_point due_time) {
    due_time_ = due_time;
  }

  TaskFunction func_ = nullptr;
  // task_ is placed after func_ to take advantage of the padding that would
  // otherwise be added here. On 32-bit systems, func_ and due_time_ have an
  // alignment of 8, but func_ has a size of 12 by default. Thus, 4 bytes of
  // padding would be added here, which is just enough for a pointer.
  Task& task_;
  pw::chrono::SystemClock::time_point due_time_;
  Dispatcher* dispatcher_ = nullptr;
};

using NativeTaskHandle = NativeTask&;

}  // namespace pw::async::backend
