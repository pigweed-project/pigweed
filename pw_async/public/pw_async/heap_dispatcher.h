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

#include "pw_async/function_dispatcher.h"

namespace pw::async {

/// @module{pw_async}

/// HeapDispatcher wraps an existing Dispatcher and allocates Task objects on
/// the heap before posting them to the existing Dispatcher. After Tasks run,
/// they are automatically freed.
class HeapDispatcher final : public FunctionDispatcher {
 public:
  HeapDispatcher(Dispatcher& dispatcher) : dispatcher_(dispatcher) {}
  ~HeapDispatcher() override = default;

  Dispatcher& inner() const { return dispatcher_; }

  // FunctionDispatcher overrides:
  using FunctionDispatcher::Post;
  Status PostAt(TaskFunction&& task_func,
                chrono::SystemClock::time_point time) override;

  // Dispatcher overrides:
  inline void Post(Task& task) override { return dispatcher_.Post(task); }
  inline void PostAt(Task& task,
                     chrono::SystemClock::time_point time) override {
    return dispatcher_.PostAt(task, time);
  }
  inline bool Cancel(Task& task) override { return dispatcher_.Cancel(task); }

  // VirtualSystemClock overrides:
  inline chrono::SystemClock::time_point now() override {
    return dispatcher_.now();
  }

 private:
  Dispatcher& dispatcher_;
};

}  // namespace pw::async
