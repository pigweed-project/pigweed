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

// DOCSTAG: [pw_async2-examples-count]
#define PW_LOG_MODULE_NAME "ASYNC_COUNTER"

#include "pw_allocator/libc_allocator.h"
#include "pw_assert/check.h"
#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/coro.h"
#include "pw_async2/coro_task.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/system_time_provider.h"
#include "pw_chrono/system_clock.h"
#include "pw_log/log.h"

using ::pw::Allocator;
using ::pw::allocator::GetLibCAllocator;
using ::pw::async2::BasicDispatcher;
using ::pw::async2::Coro;
using ::pw::async2::CoroContext;
using ::pw::async2::Dispatcher;
using ::pw::async2::GetSystemTimeProvider;
using ::pw::async2::TimeProvider;
using ::pw::chrono::SystemClock;
using ::std::chrono_literals::operator""ms;

namespace {

class Counter {
 public:
  // examples-constructor-start
  Counter(Dispatcher& dispatcher,
          Allocator& allocator,
          TimeProvider<SystemClock>& time)
      : dispatcher_(&dispatcher), allocator_(&allocator), time_(&time) {}
  // examples-constructor-end

  // examples-task-start
  // Posts a new asynchronous task which will count up to `times`, one count
  // per `period`.
  void StartCounting(SystemClock::duration period, int times) {
    // Allocate a new task, which is freed by the dispatcher when it completes.
    PW_CHECK(
        dispatcher_->Post(*allocator_, CountCoro(*allocator_, period, times)) !=
        nullptr);
  }
  // examples-task-end

 private:
  // Asynchronous counter implementation.
  Coro<void> CountCoro(CoroContext, SystemClock::duration period, int times) {
    PW_LOG_INFO("Counting to %i", times);
    for (int i = 1; i <= times; ++i) {
      co_await time_->WaitFor(period);
      PW_LOG_INFO("%i of %i", i, times);
    }
  }

  Dispatcher* dispatcher_;
  Allocator* allocator_;
  TimeProvider<SystemClock>* time_;
};

}  // namespace

int main() {
  Allocator& alloc = GetLibCAllocator();
  TimeProvider<SystemClock>& time = GetSystemTimeProvider();
  BasicDispatcher dispatcher;

  Counter counter(dispatcher, alloc, time);
  counter.StartCounting(/*period=*/500ms, /*times=*/5);

  dispatcher.RunToCompletion();
}
// DOCSTAG: [pw_async2-examples-count]
