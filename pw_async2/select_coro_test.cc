// Copyright 2026 The Pigweed Authors
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

#include "pw_allocator/testing.h"
#include "pw_async2/coro.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/future_task.h"
#include "pw_async2/select.h"
#include "pw_async2/simulated_time_provider.h"
#include "pw_async2/time_provider.h"
#include "pw_async2/value_future.h"
#include "pw_chrono/system_clock.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::allocator::test::AllocatorForTest;
using ::pw::async2::Coro;
using ::pw::async2::CoroContext;
using ::pw::async2::DispatcherForTest;
using ::pw::async2::FutureTask;
using ::pw::async2::SimulatedTimeProvider;
using ::pw::async2::TimeProvider;
using ::pw::async2::ValueFuture;
using ::pw::async2::ValueProvider;
using ::pw::chrono::SystemClock;

// Durations are in clock ticks so that the test is independent of the
// SystemClock period.
constexpr SystemClock::duration kTimeout(10);
constexpr SystemClock::duration kMessageInterval(3);

// Counts messages until a deadline expires. The deadline is created once, so
// messages do not reset it.
Coro<int> CountMessagesUntilDeadline(CoroContext,
                                     ValueProvider<int>& messages,
                                     TimeProvider<SystemClock>& time) {
  auto deadline = time.WaitFor(kTimeout);
  ValueFuture<int> next_message = messages.Get();
  int count = 0;

  while (true) {
    PW_CO_SELECT(auto result, next_message, deadline);
    if (result.has_value<0>()) {
      count += 1;
      next_message = messages.Get();
    }
    if (result.has_value<1>()) {
      co_return count;
    }
  }
}

TEST(PwCoSelect, DeadlinePersistsAcrossIterations) {
  AllocatorForTest<2048> alloc;
  DispatcherForTest dispatcher;
  SimulatedTimeProvider<SystemClock> time;
  ValueProvider<int> messages;

  FutureTask task(CountMessagesUntilDeadline(alloc, messages, time));
  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  // Messages arrive more often than the timeout. If the deadline were recreated
  // for each message, as with Select, it would never expire.
  for (int i = 0; i < 3; ++i) {
    time.AdvanceTime(kMessageInterval);
    messages.Resolve(i);
    EXPECT_TRUE(dispatcher.RunUntilStalled());
  }

  time.AdvanceTime(kTimeout - 3 * kMessageInterval + SystemClock::duration(1));
  EXPECT_FALSE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.Wait(), 3);
}

struct FuturesCreated {
  int coin = 0;
  int key = 0;
};

// Sums coins until `done_key` is pressed. Only the future that completed is
// re-armed; the other keeps waiting.
Coro<unsigned> SumCoinsUntilKey(CoroContext,
                                ValueProvider<unsigned>& coin_slot,
                                ValueProvider<int>& keypad,
                                int done_key,
                                FuturesCreated& created) {
  ValueFuture<unsigned> coin = coin_slot.Get();
  ValueFuture<int> key = keypad.Get();
  created.coin += 1;
  created.key += 1;
  unsigned total = 0;

  while (true) {
    PW_CO_SELECT(auto result, coin, key);
    if (result.has_value<0>()) {
      total += result.value<0>();
      coin = coin_slot.Get();
      created.coin += 1;
    }
    if (result.has_value<1>()) {
      if (result.value<1>() == done_key) {
        co_return total;
      }
      key = keypad.Get();
      created.key += 1;
    }
  }
}

TEST(PwCoSelect, ReArmsOnlyTheCompletedFuture) {
  AllocatorForTest<2048> alloc;
  DispatcherForTest dispatcher;
  ValueProvider<unsigned> coin_slot;
  ValueProvider<int> keypad;
  FuturesCreated created;

  FutureTask task(
      SumCoinsUntilKey(alloc, coin_slot, keypad, /*done_key=*/0, created));
  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  coin_slot.Resolve(1u);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(created.coin, 2);
  EXPECT_EQ(created.key, 1);  // The key future kept waiting.
  EXPECT_TRUE(keypad.has_future());

  keypad.Resolve(5);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(created.coin, 2);
  EXPECT_EQ(created.key, 2);

  // Both futures complete before the coroutine resumes, so both are handled.
  coin_slot.Resolve(2u);
  keypad.Resolve(0);
  EXPECT_FALSE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.Wait(), 3u);
}

}  // namespace
