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

#include "coin_slot.h"

#include "pw_async2/context.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/func_task.h"
#include "pw_async2/poll.h"
#include "pw_async2/try.h"
#include "pw_unit_test/framework.h"

using ::pw::async2::Context;
using ::pw::async2::Poll;
using ::pw::async2::Ready;

namespace codelab {

TEST(CoinSlotTest, PendAndDeposit) {
  CoinSlot coin_slot;
  unsigned int coins = 0;

  pw::async2::PendFuncTask task([&](Context& context) -> Poll<> {
    PW_TRY_READY_ASSIGN(coins, coin_slot.Pend(context));
    return Ready();
  });

  pw::async2::DispatcherForTest dispatcher;
  dispatcher.Post(task);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(coins, 0u) << "No deposits yet";

  coin_slot.Deposit();

  dispatcher.RunToCompletion();
  EXPECT_EQ(coins, 1u);
}

TEST(CoinSlotTest, MultipleDeposits) {
  struct : pw::async2::Task {
    pw::async2::Poll<> DoPend(Context& context) override {
      while (true) {
        unsigned int coins;
        PW_TRY_READY_ASSIGN(coins, coin_slot.Pend(context));
        total_coins += coins;
      }
    }

    CoinSlot coin_slot;
    unsigned int total_coins = 0;
  } task;

  pw::async2::DispatcherForTest dispatcher;
  dispatcher.Post(task);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.total_coins, 0u) << "No deposits yet";

  task.coin_slot.Deposit();
  task.coin_slot.Deposit();
  task.coin_slot.Deposit();

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.total_coins, 3u);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.total_coins, 3u) << "No more deposits yet";

  task.coin_slot.Deposit();
  task.coin_slot.Deposit();

  EXPECT_EQ(task.total_coins, 3u) << "More deposits, but haven't run the task";

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.total_coins, 5u);
}

}  // namespace codelab
