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

#include "pw_async2/simulated_time_provider.h"

#include "pw_async2/dispatcher_for_test.h"
#include "pw_chrono/system_clock.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::async2::Context;
using ::pw::async2::DispatcherForTest;
using ::pw::async2::Pending;
using ::pw::async2::Poll;
using ::pw::async2::Ready;
using ::pw::async2::SimulatedTimeProvider;
using ::pw::async2::Task;
using ::pw::async2::TimeFuture;
using ::pw::chrono::SystemClock;
using ::std::chrono_literals::operator""min;
using ::std::chrono_literals::operator""h;

static_assert(pw::async2::Future<TimeFuture<SystemClock>>);

// We can't control the SystemClock's period configuration, so just in case
// 42 hours cannot be accurately expressed in integer ticks, round the
// duration up.
constexpr SystemClock::duration kRoundedArbitraryDuration =
    SystemClock::for_at_least(42h);

TEST(SimulatedTimeProvider, InitialTime) {
  SimulatedTimeProvider<SystemClock> tp;

  EXPECT_EQ(SystemClock::time_point(SystemClock::duration(0)), tp.now());
}

TEST(SimulatedTimeProvider, SetTime) {
  SimulatedTimeProvider<SystemClock> tp;

  tp.SetTime(SystemClock::time_point(kRoundedArbitraryDuration));
  EXPECT_EQ(kRoundedArbitraryDuration, tp.now().time_since_epoch());
}

TEST(SimulatedTimeProvider, AdvanceTime) {
  SimulatedTimeProvider<SystemClock> tp;

  const SystemClock::time_point before_timestamp = tp.now();
  tp.AdvanceTime(kRoundedArbitraryDuration);
  const SystemClock::time_point after_timestamp = tp.now();

  EXPECT_EQ(kRoundedArbitraryDuration, tp.now().time_since_epoch());
  EXPECT_EQ(kRoundedArbitraryDuration, after_timestamp - before_timestamp);
}

struct WaitTask : public Task {
  WaitTask(TimeFuture<SystemClock>&& future)
      : future_(std::move(future)), completed_(false) {}

  Poll<> DoPend(Context& cx) final {
    if (future_.Pend(cx).IsPending()) {
      return Pending();
    }
    completed_ = true;
    return Ready();
  }
  TimeFuture<SystemClock> future_;
  bool completed_;
};

TEST(SimulatedTimeProvider, AdvanceTimeRunsPastTimers) {
  SimulatedTimeProvider<SystemClock> tp;
  WaitTask task(tp.WaitFor(1h));
  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  tp.AdvanceTime(30min);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  tp.AdvanceTime(40min);
  dispatcher.RunToCompletion();
}

TEST(SimulatedTimeProvider, AdvanceUntilNextExpirationRunsPastTimers) {
  SimulatedTimeProvider<SystemClock> tp;
  WaitTask task(tp.WaitFor(1h));
  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  EXPECT_TRUE(tp.AdvanceUntilNextExpiration());
  dispatcher.RunToCompletion();
  EXPECT_FALSE(tp.AdvanceUntilNextExpiration());
}

TEST(SimulatedTimeProvider, TimeUntilNextExpirationGetsTimerDelay) {
  SimulatedTimeProvider<SystemClock> tp;
  auto timer = tp.WaitFor(1h);
  ASSERT_TRUE(tp.TimeUntilNextExpiration().has_value());
  EXPECT_GE(*tp.TimeUntilNextExpiration(), 1h);
}

TEST(SimulatedTimeProvider,
     TimeUntilNextExpirationAfterCompletionReturnsNullopt) {
  SimulatedTimeProvider<SystemClock> tp;
  auto timer = tp.WaitFor(1h);
  tp.AdvanceTime(90min);
  EXPECT_FALSE(tp.TimeUntilNextExpiration().has_value());
}

TEST(SimulatedTimeProvider, TimeUntilNextExpirationAfterDestroyReturnsNullopt) {
  SimulatedTimeProvider<SystemClock> tp;
  {
    auto timer = tp.WaitFor(1h);
  }
  EXPECT_FALSE(tp.TimeUntilNextExpiration().has_value());
}

TEST(SimulatedTimeProvider, TimerWithPastExpirationExpiresImmediately) {
  SimulatedTimeProvider<SystemClock> tp;
  auto start = tp.now();
  tp.AdvanceTime(90min);
  auto timer = tp.WaitUntil(start + 30min);
  DispatcherForTest dispatcher;
  EXPECT_TRUE(dispatcher.RunInTaskUntilStalled(timer).IsReady());
}

TEST(SimulatedTimeProvider, MultipleMovedTimersExpireInOrder) {
  SimulatedTimeProvider<SystemClock> tp;
  // Insert out-of-order to check that they become sorted.
  auto t3_init = tp.WaitFor(3h);
  auto t1_init = tp.WaitFor(1h);
  auto t2_init = tp.WaitFor(2h);
  // Move out of order to check that sorting is preserved.
  WaitTask t2(std::move(t2_init));
  WaitTask t1(std::move(t1_init));
  WaitTask t3(std::move(t3_init));

  DispatcherForTest dispatcher;
  dispatcher.Post(t1);
  dispatcher.Post(t2);
  dispatcher.Post(t3);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_FALSE(t1.completed_);
  EXPECT_FALSE(t2.completed_);
  EXPECT_FALSE(t3.completed_);

  // t1 should expire first.
  EXPECT_TRUE(tp.AdvanceUntilNextExpiration());
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_TRUE(t1.completed_);
  EXPECT_FALSE(t2.completed_);
  EXPECT_FALSE(t3.completed_);

  // Then t2.
  EXPECT_TRUE(tp.AdvanceUntilNextExpiration());
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_TRUE(t1.completed_);
  EXPECT_TRUE(t2.completed_);
  EXPECT_FALSE(t3.completed_);

  // Then t3.
  EXPECT_TRUE(tp.AdvanceUntilNextExpiration());
  dispatcher.RunToCompletion();
  EXPECT_TRUE(t1.completed_);
  EXPECT_TRUE(t2.completed_);
  EXPECT_TRUE(t3.completed_);
}

}  // namespace
