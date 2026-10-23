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

#include "pw_async2/system_time_provider.h"

#include <chrono>
#include <tuple>
#include <utility>

#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
#include "pw_async2/time_provider.h"
#include "pw_chrono/system_clock.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::async2::Context;
using ::pw::async2::DispatcherForTest;
using ::pw::async2::GetSystemTimeProvider;
using ::pw::async2::Pending;
using ::pw::async2::Poll;
using ::pw::async2::Ready;
using ::pw::async2::Task;
using ::pw::async2::TimeFuture;
using ::pw::chrono::SystemClock;
using ::std::chrono_literals::operator""ms;
using ::std::chrono_literals::operator""s;

struct WaitTask : public Task {
  WaitTask(TimeFuture<SystemClock>&& future)
      : time_completed_(Pending()), future_(std::move(future)) {}

  Poll<> DoPend(Context& cx) final {
    if (future_.Pend(cx).IsPending()) {
      return Pending();
    }
    time_completed_ = SystemClock::now();
    return Ready();
  }

  Poll<SystemClock::time_point> time_completed_ = Pending();
  TimeFuture<SystemClock> future_;
};

TEST(SystemTimeProvider, InvokesTimerAfterDelay) {
  SystemClock::time_point start_time = SystemClock().now();
  SystemClock::time_point expected_completion = start_time + 50ms;
  WaitTask task(GetSystemTimeProvider().WaitUntil(expected_completion));
  DispatcherForTest dispatcher;
  dispatcher.AllowBlocking();
  dispatcher.Post(task);
  dispatcher.RunToCompletion();
  ASSERT_TRUE(task.time_completed_.IsReady());
  EXPECT_GE(*task.time_completed_, expected_completion);
}

TEST(SystemTimeProvider, InvokesTwoTimersInOrder) {
  SystemClock::time_point start_time = SystemClock().now();
  SystemClock::time_point expected_c1 = start_time + 200ms;
  SystemClock::time_point expected_c2 = start_time + 10ms;
  WaitTask t1(GetSystemTimeProvider().WaitUntil(expected_c1));
  WaitTask t2(GetSystemTimeProvider().WaitUntil(expected_c2));
  DispatcherForTest dispatcher;
  dispatcher.Post(t1);
  dispatcher.Post(t2);
  dispatcher.AllowBlocking();
  dispatcher.RunToCompletion();
  ASSERT_TRUE(t1.time_completed_.IsReady());
  EXPECT_GE(t1.time_completed_->time_since_epoch().count(),
            expected_c1.time_since_epoch().count());
  ASSERT_TRUE(t2.time_completed_.IsReady());
  EXPECT_GE(*t2.time_completed_, expected_c2);
  EXPECT_GT(*t1.time_completed_, *t2.time_completed_);
}

TEST(SystemTimeProvider, CancelThroughDestruction) {
  // We can't control the SystemClock's period configuration, so just in case
  // duration cannot be accurately expressed in integer ticks, round the
  // duration up.
  constexpr SystemClock::duration kRoundedArbitraryLongDuration =
      SystemClock::for_at_least(1s);

  std::ignore = GetSystemTimeProvider().WaitFor(kRoundedArbitraryLongDuration);
  // TimeFuture destroyed here and not used.
}

}  // namespace
