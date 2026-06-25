// Copyright 2024 The Pigweed Authors
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

#include <lib/async/task.h>

#include <atomic>

#include "pw_async/task_function.h"
#include "pw_chrono/system_clock.h"

namespace pw::async_fuchsia {

// NativeTask friend forward declaration.
class FuchsiaDispatcher;

}  // namespace pw::async_fuchsia

namespace pw::async::test::backend {

// NativeTask friend forward declaration.
class NativeFakeDispatcher;

}  // namespace pw::async::test::backend

namespace pw::async::backend {

class NativeTask final : public async_task_t {
 private:
  friend class ::pw::async_fuchsia::FuchsiaDispatcher;
  friend class ::pw::async::test::backend::NativeFakeDispatcher;
  friend class ::pw::async::Task;

  explicit NativeTask(::pw::async::Task& task);
  explicit NativeTask(::pw::async::Task& task, TaskFunction&& f);

  void operator()(Context& ctx, Status status);

  void set_function(TaskFunction&& f);

  chrono::SystemClock::time_point due_time() const;

  void set_due_time(chrono::SystemClock::time_point due_time);

  bool is_posted() const { return is_posted_.load(); }
  void set_posted(bool posted) { is_posted_.store(posted); }

  static void Handler(async_dispatcher_t* /*dispatcher*/,
                      async_task_t* task,
                      zx_status_t status);

  TaskFunction func_;
  Task& task_;
  // `dispatcher_` is set by a Dispatcher to its own address before forwarding a
  // task to the underlying Zircon async-loop, so that the Dispatcher pointer in
  // the Context may be set in a type-safe manner inside `Handler` when invoked
  // by the async-loop.
  Dispatcher* dispatcher_ = nullptr;
  std::atomic<bool> is_posted_{false};
};

using NativeTaskHandle = NativeTask&;

}  // namespace pw::async::backend
