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

#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/future.h"
#include "pw_async2/system_time_provider.h"
#include "pw_log/log.h"
#include "pw_unit_test/framework.h"

namespace examples {

// DOCSTAG: [decl]
using ::pw::async2::Context;
using ::pw::async2::GetSystemTimeProvider;
using ::pw::async2::Poll;
using ::pw::async2::Task;
using ::pw::async2::TimeFuture;
using ::pw::chrono::SystemClock;

class TimerTask : public Task {
 public:
  TimerTask(SystemClock::duration duration)
      : Task(PW_ASYNC_TASK_NAME("TimerTask")), duration_(duration) {}

 private:
  // Contains the task's core async logic.
  Poll<> DoPend(Context& cx) override;
  SystemClock::duration duration_;
  TimeFuture<SystemClock> time_future_;
};
// DOCSTAG: [decl]

// DOCSTAG: [impl]
Poll<> TimerTask::DoPend(Context& cx) {
  // Acquire a future when the task first runs.
  if (!time_future_.is_pendable()) {
    time_future_ = GetSystemTimeProvider().WaitFor(duration_);
  }

  // Wait for the timer to complete.
  if (time_future_.Pend(cx).IsPending()) {
    PW_LOG_INFO("Waiting…");
    // Notify the dispatcher that the task is stalled.
    return pw::async2::Pending();
  }

  PW_LOG_INFO("Done!");
  // Notify the dispatcher that the task is complete.
  return pw::async2::Ready();
}
// DOCSTAG: [impl]

// Headers are double-included below so that readers can see what headers
// the example depends on.
// DOCSTAG: [main]
#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/future.h"
#include "pw_async2/system_time_provider.h"
#include "pw_log/log.h"

using ::pw::async2::BasicDispatcher;
using ::std::chrono_literals::operator""ms;

int main() {
  // The cooperative scheduler of pw_async2.
  BasicDispatcher dispatcher;
  // Create a task that only progresses after 100ms have passed.
  TimerTask timer_task(100ms);
  // Queue the task with the dispatcher.
  dispatcher.Post(timer_task);
  // Run until all tasks are complete.
  dispatcher.RunToCompletion();
  return 0;
}
// DOCSTAG: [main]

}  // namespace examples

namespace {

TEST(Quickstart, CompilesAndRuns) { examples::main(); }

}  // namespace
