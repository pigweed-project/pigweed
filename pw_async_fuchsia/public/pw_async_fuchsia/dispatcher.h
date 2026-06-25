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

#include <lib/async/dispatcher.h>
#include <zircon/assert.h>

#include "pw_async/dispatcher.h"
#include "pw_async/task.h"

namespace pw::async_fuchsia {

struct AllocatedTaskAndFunction {
  pw::async::Task task;
  pw::async::TaskFunction func;
};

// TODO: https://fxbug.dev/42075970 - Replace these temporary allocating
// utilities.
inline void PostAt(pw::async::Dispatcher* dispatcher,
                   pw::async::TaskFunction&& task,
                   pw::chrono::SystemClock::time_point time) {
  AllocatedTaskAndFunction* t = new AllocatedTaskAndFunction();
  t->func = std::move(task);
  t->task.set_function([t](pw::async::Context& ctx, pw::Status status) {
    t->func(ctx, status);
    delete t;
  });
  dispatcher->PostAt(t->task, time);
}

inline void PostAfter(pw::async::Dispatcher* dispatcher,
                      pw::async::TaskFunction&& task,
                      pw::chrono::SystemClock::duration delay) {
  PostAt(dispatcher, std::move(task), dispatcher->now() + delay);
}

inline void Post(pw::async::Dispatcher* dispatcher,
                 pw::async::TaskFunction&& task) {
  PostAt(dispatcher, std::move(task), dispatcher->now());
}

class FuchsiaDispatcher final : public async::Dispatcher {
 public:
  explicit FuchsiaDispatcher(async_dispatcher_t* dispatcher)
      : dispatcher_(dispatcher) {}
  ~FuchsiaDispatcher() override = default;

  chrono::SystemClock::time_point now() override;

  void Post(async::Task& task) override;
  void PostAt(async::Task& task, chrono::SystemClock::time_point time) override;

  bool Cancel(async::Task& task) override;

  async_dispatcher_t* native() const { return dispatcher_; }

 private:
  async_dispatcher_t* dispatcher_;
};

}  // namespace pw::async_fuchsia
