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
#include "pw_async2/allocate_task.h"
#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/coro.h"
#include "pw_async2/coro_or_else_task.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/system_time_provider.h"
#include "pw_chrono/system_clock.h"
#include "pw_log/log.h"
#include "pw_result/result.h"
#include "pw_status/status.h"

using ::pw::Allocator;
using ::pw::OkStatus;
using ::pw::Status;
using ::pw::allocator::GetLibCAllocator;
using ::pw::async2::AllocateTask;
using ::pw::async2::BasicDispatcher;
using ::pw::async2::Coro;
using ::pw::async2::CoroContext;
using ::pw::async2::CoroOrElseTask;
using ::pw::async2::Dispatcher;
using ::pw::async2::GetSystemTimeProvider;
using ::pw::async2::Task;
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
    CoroContext coro_cx(*allocator_);
    Coro<Status> coro = CountCoro(coro_cx, period, times);
    Task* new_task =
        AllocateTask<CoroOrElseTask>(*allocator_, std::move(coro), [](Status) {
          PW_LOG_ERROR("Counter coroutine failed to allocate.");
        });

    // The newly allocated task will be free'd by the dispatcher
    // upon completion.
    dispatcher_->Post(*new_task);
  }
  // examples-task-end

 private:
  // Asynchronous counter implementation.
  Coro<Status> CountCoro(CoroContext&,
                         SystemClock::duration period,
                         int times) {
    PW_LOG_INFO("Counting to %i", times);
    for (int i = 1; i <= times; ++i) {
      co_await time_->WaitFor(period);
      PW_LOG_INFO("%i of %i", i, times);
    }
    co_return OkStatus();
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
