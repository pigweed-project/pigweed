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

#include "pw_async2/future_task.h"

#include <optional>
#include <type_traits>

#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/poll.h"
#include "pw_async2/value_future.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::async2::DispatcherForTest;
using pw::async2::FutureTask;
using pw::async2::Pending;
using pw::async2::Poll;
using pw::async2::Ready;
using pw::async2::ValueFuture;
using pw::async2::ValueProvider;
using pw::async2::VoidFuture;

TEST(FutureTask, ReadyFuture) {
  DispatcherForTest dispatcher;

  FutureTask<ValueFuture<bool>> task(ValueFuture<bool>::Resolved(true));
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();

  EXPECT_FALSE(task.IsRegistered());
  EXPECT_TRUE(task.Wait());
}

TEST(FutureTask, PendingFuture) {
  DispatcherForTest dispatcher;

  ValueProvider<const char*> provider;
  FutureTask<ValueFuture<const char*>> task(provider.Get());

  dispatcher.Post(task);
  dispatcher.RunUntilStalled();

  EXPECT_TRUE(task.IsRegistered());

  provider.Resolve("O_o");
  dispatcher.RunUntilStalled();

  EXPECT_FALSE(task.IsRegistered());
  EXPECT_STREQ(task.Wait(), "O_o");
}

class MoveOnlyInt {
 public:
  MoveOnlyInt(int value) : value_(value) {}

  MoveOnlyInt(const MoveOnlyInt&) = delete;
  MoveOnlyInt& operator=(const MoveOnlyInt&) = delete;

  MoveOnlyInt(MoveOnlyInt&&) = default;
  MoveOnlyInt& operator=(MoveOnlyInt&&) = default;

  operator int() const { return value_; }

 private:
  int value_;
};

TEST(FutureTask, Wait) {
  DispatcherForTest dispatcher;

  ValueProvider<MoveOnlyInt> provider;

  FutureTask task(provider.Get());
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();

  provider.Resolve(456);
  dispatcher.RunUntilStalled();

  EXPECT_EQ(task.TakePoll().value(), 456);

  EXPECT_EQ(task.Wait(), 456);
}

TEST(FutureTask, MoveFuture) {
  DispatcherForTest dispatcher;

  ValueProvider<int> provider;

  ValueFuture<int> future = provider.Get();

  FutureTask task(std::move(future));
  dispatcher.Post(task);
  provider.Resolve(-100);
  dispatcher.RunUntilStalled();
  EXPECT_EQ(task.TakePoll().value(), -100);
}

TEST(FutureTask, VoidFuture) {
  DispatcherForTest dispatcher;

  ValueProvider<void> provider;

  FutureTask<VoidFuture> task(provider.Get());
  dispatcher.Post(task);

  provider.Resolve();
  dispatcher.RunUntilStalled();

  task.Join();
}

TEST(FutureTask, Reference) {
  DispatcherForTest dispatcher;

  ValueProvider<int> provider;
  ValueFuture<int> future(provider.Get());

  FutureTask task(future);
  static_assert(std::is_same_v<FutureTask<ValueFuture<int>&>, decltype(task)>,
                "Deduces a reference to a future");

  dispatcher.Post(task);

  provider.Resolve(404);
  dispatcher.RunUntilStalled();

  EXPECT_EQ(task.Wait(), 404);
}

TEST(FutureTask, ReferenceTakePoll) {
  DispatcherForTest dispatcher;

  ValueProvider<int> provider;
  ValueFuture<int> future(provider.Get());

  FutureTask<ValueFuture<int>&> task(future);

  EXPECT_EQ(task.TakePoll(), Pending());

  dispatcher.Post(task);

  provider.Resolve(500);
  dispatcher.RunUntilStalled();

  EXPECT_EQ(task.TakePoll(), Ready(500));
}

}  // namespace
