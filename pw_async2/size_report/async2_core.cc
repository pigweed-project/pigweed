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

#include <cstdint>

#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/size_report/async2_baseline.h"
#include "pw_bloat/bloat_this_binary.h"

#ifdef _PW_ASYNC2_SIZE_REPORT_COROUTINE
#include "pw_async2/size_report/async2_coro_baseline.h"
#endif  // _PW_ASYNC2_SIZE_REPORT_COROUTINE

#ifdef _PW_ASYNC2_SIZE_REPORT_TIME_FUTURE
#include "pw_async2/context.h"
#include "pw_async2/func_task.h"
#include "pw_async2/poll.h"
#include "pw_async2/system_time_provider.h"
#include "pw_async2/time_provider.h"
#include "pw_chrono/system_clock.h"
#endif  // _PW_ASYNC2_SIZE_REPORT_TIME_FUTURE

namespace pw::async2::size_report {

int Measure() {
  volatile uint32_t mask = bloat::kDefaultMask;

#ifdef _PW_ASYNC2_SIZE_REPORT_COROUTINE
  int result = SetAsync2CoroBaseline(mask);
#else
  int result = SetAsync2Baseline(mask);
#endif  // _PW_ASYNC2_SIZE_REPORT_COROUTINE

#ifdef _PW_ASYNC2_SIZE_REPORT_INCREMENTAL_TASK
  BasicDispatcher& dispatcher = GetDispatcher();
  MockTask task2;
  task2.should_complete = true;
  dispatcher.Post(task2);
  dispatcher.RunToCompletion();
  result += task2.polled;
#endif  // _PW_ASYNC2_SIZE_REPORT_INCREMENTAL_TASK

#ifdef _PW_ASYNC2_SIZE_REPORT_TIME_FUTURE
  BasicDispatcher& dispatcher = GetDispatcher();
  TimeProvider<chrono::SystemClock>& time = GetSystemTimeProvider();
  TimeFuture<chrono::SystemClock> timeout =
      time.WaitFor(chrono::SystemClock::duration(mask));
  FuncTask timer_task([&](Context& cx) -> Poll<> {
    Poll<> poll = timeout.Pend(cx).Readiness();
    PW_BLOAT_COND(poll.IsReady(), mask);
    return poll;
  });
  dispatcher.Post(timer_task);
  PW_BLOAT_COND(dispatcher.RunUntilStalled(), mask);
  timeout = time.WaitFor(chrono::SystemClock::duration(mask));
  timer_task.Deregister();
#endif  // _PW_ASYNC2_SIZE_REPORT_TIME_FUTURE

  return result;
}

}  // namespace pw::async2::size_report

int main() { return pw::async2::size_report::Measure(); }
