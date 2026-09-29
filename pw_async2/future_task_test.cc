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
using pw::async2::ReturnValuePolicy;
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

  task.BlockingJoin();
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

TEST(FutureTask, DiscardReturnValue) {
  DispatcherForTest dispatcher;

  ValueProvider<int> provider;
  FutureTask<ValueFuture<int>, ReturnValuePolicy::kDiscard> task(
      provider.Get());

  dispatcher.Post(task);
  dispatcher.RunUntilStalled();

  EXPECT_TRUE(task.IsRegistered());

  provider.Resolve(123);
  dispatcher.RunUntilStalled();

  EXPECT_FALSE(task.IsRegistered());
}

TEST(FutureTask, HasValue) {
  DispatcherForTest dispatcher;

  ValueProvider<int> provider;
  FutureTask task(provider.Get());

  EXPECT_FALSE(task.has_value());

  dispatcher.Post(task);
  dispatcher.RunUntilStalled();

  EXPECT_FALSE(task.has_value());

  provider.Resolve(42);
  dispatcher.RunUntilStalled();

  EXPECT_TRUE(task.has_value());
  EXPECT_EQ(task.value(), 42);
}

TEST(FutureTask, VoidFutureKeepPolicy) {
  DispatcherForTest dispatcher;

  ValueProvider<void> provider;
  FutureTask<VoidFuture, ReturnValuePolicy::kKeep> task(provider.Get());

  EXPECT_EQ(task.TakePoll(), Pending());

  dispatcher.Post(task);
  provider.Resolve();
  dispatcher.RunUntilStalled();

  EXPECT_EQ(task.TakePoll(), Ready());
}

TEST(FutureTask, TypeTraits) {
  static_assert(std::is_default_constructible_v<FutureTask<ValueFuture<int>>>);
  static_assert(std::is_default_constructible_v<FutureTask<VoidFuture>>);
  static_assert(
      !std::is_default_constructible_v<FutureTask<ValueFuture<int>&>>);
  static_assert(!std::is_default_constructible_v<FutureTask<VoidFuture&>>);

  static_assert(!std::is_copy_constructible_v<FutureTask<ValueFuture<int>>>);
  static_assert(!std::is_copy_assignable_v<FutureTask<ValueFuture<int>>>);
  static_assert(!std::is_move_constructible_v<FutureTask<ValueFuture<int>>>);
  static_assert(!std::is_move_assignable_v<FutureTask<ValueFuture<int>>>);

  static_assert(
      std::is_assignable_v<FutureTask<ValueFuture<int>>&, ValueFuture<int>>);
  static_assert(std::is_assignable_v<FutureTask<VoidFuture>&, VoidFuture>);
  static_assert(
      !std::is_assignable_v<FutureTask<ValueFuture<int>&>&, ValueFuture<int>>);
  static_assert(!std::is_assignable_v<FutureTask<VoidFuture&>&, VoidFuture>);
}

TEST(FutureTask, DefaultConstructedStateAndCrash) {
  FutureTask<ValueFuture<int>> task;
  EXPECT_FALSE(task.is_pendable());
  EXPECT_FALSE(task.is_complete());
  EXPECT_FALSE(task.has_value());

  FutureTask<VoidFuture> void_task;
  EXPECT_FALSE(void_task.is_pendable());
  EXPECT_FALSE(void_task.is_complete());

  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  EXPECT_DEATH_IF_SUPPORTED(dispatcher.RunUntilStalled(), "");
  task.Deregister();
}

TEST(FutureTask, IsPendableAndIsComplete) {
  DispatcherForTest dispatcher;
  ValueProvider<int> provider;

  FutureTask<ValueFuture<int>> task(provider.Get());
  EXPECT_TRUE(task.is_pendable());
  EXPECT_FALSE(task.is_complete());

  dispatcher.Post(task);
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(task.is_pendable());
  EXPECT_FALSE(task.is_complete());

  provider.Resolve(77);
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(task.is_pendable());
  EXPECT_TRUE(task.is_complete());
  EXPECT_EQ(task.value(), 77);
}

TEST(FutureTask, IsPendableAndIsCompleteDiscard) {
  DispatcherForTest dispatcher;
  ValueProvider<void> provider;

  FutureTask<VoidFuture> task(provider.Get());
  EXPECT_TRUE(task.is_pendable());
  EXPECT_FALSE(task.is_complete());

  dispatcher.Post(task);
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(task.is_pendable());
  EXPECT_FALSE(task.is_complete());

  provider.Resolve();
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(task.is_pendable());
  EXPECT_TRUE(task.is_complete());
}

TEST(FutureTask, AssignFutureAndReuse) {
  DispatcherForTest dispatcher;
  ValueProvider<int> provider;

  FutureTask<ValueFuture<int>> task;
  EXPECT_FALSE(task.is_pendable());
  EXPECT_FALSE(task.is_complete());
  EXPECT_FALSE(task.has_value());

  task = provider.Get();
  EXPECT_TRUE(task.is_pendable());
  EXPECT_FALSE(task.is_complete());
  EXPECT_FALSE(task.has_value());

  dispatcher.Post(task);
  provider.Resolve(10);
  dispatcher.RunUntilStalled();

  EXPECT_FALSE(task.is_pendable());
  EXPECT_TRUE(task.is_complete());
  EXPECT_TRUE(task.has_value());
  EXPECT_EQ(task.value(), 10);

  // Assign a new future after completion and run again.
  task = provider.Get();
  EXPECT_TRUE(task.is_pendable());
  EXPECT_FALSE(task.is_complete());
  EXPECT_FALSE(task.has_value());

  dispatcher.Post(task);
  provider.Resolve(20);
  dispatcher.RunUntilStalled();

  EXPECT_FALSE(task.is_pendable());
  EXPECT_TRUE(task.is_complete());
  EXPECT_TRUE(task.has_value());
  EXPECT_EQ(task.value(), 20);

  // Reset to a default-constructed future.
  task.reset();
  EXPECT_FALSE(task.is_pendable());
  EXPECT_FALSE(task.is_complete());
  EXPECT_FALSE(task.has_value());
}

TEST(FutureTask, AssignFutureAfterDeregister) {
  DispatcherForTest dispatcher;
  ValueProvider<int> provider1;
  ValueProvider<int> provider2;

  FutureTask<ValueFuture<int>> task(provider1.Get());
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(task.IsRegistered());
  EXPECT_TRUE(provider1.has_future());

  task.Deregister();
  EXPECT_FALSE(task.IsRegistered());

  task = provider2.Get();
  EXPECT_FALSE(provider1.has_future());
  EXPECT_TRUE(provider2.has_future());

  dispatcher.Post(task);
  provider2.Resolve(99);
  dispatcher.RunUntilStalled();

  EXPECT_EQ(task.Wait(), 99);
}

TEST(FutureTask, ResetCancelsPendingFuture) {
  DispatcherForTest dispatcher;
  ValueProvider<int> provider;

  FutureTask<ValueFuture<int>> task(provider.Get());
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(provider.has_future());

  task.Deregister();
  task.reset();
  EXPECT_FALSE(provider.has_future());
  EXPECT_FALSE(task.is_pendable());

  task = provider.Get();
  dispatcher.Post(task);
  provider.Resolve(55);
  dispatcher.RunUntilStalled();
  EXPECT_EQ(task.Wait(), 55);
}

TEST(FutureTask, AssignFutureDiscardPolicy) {
  DispatcherForTest dispatcher;
  ValueProvider<void> provider;

  FutureTask<VoidFuture> task;
  EXPECT_FALSE(task.is_pendable());

  task = provider.Get();
  EXPECT_TRUE(task.is_pendable());

  dispatcher.Post(task);
  provider.Resolve();
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(task.is_complete());

  task.emplace_future(provider.Get());
  EXPECT_TRUE(task.is_pendable());
  EXPECT_FALSE(task.is_complete());

  dispatcher.Post(task);
  provider.Resolve();
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(task.is_complete());

  task.reset();
  EXPECT_FALSE(task.is_pendable());
  EXPECT_FALSE(task.is_complete());
}

class CustomSumFuture {
 public:
  using value_type = int;

  constexpr CustomSumFuture() = default;
  constexpr CustomSumFuture(int a, int b)
      : sum_(a + b), state_(pw::async2::FutureState::kReadyForCompletion) {}

  CustomSumFuture(CustomSumFuture&&) = default;
  CustomSumFuture& operator=(CustomSumFuture&&) = default;

  bool is_pendable() const { return state_.is_pendable(); }
  bool is_complete() const { return state_.is_complete(); }

  Poll<int> Pend(pw::async2::Context&) {
    PW_ASSERT(is_pendable());
    state_.MarkComplete();
    return Ready(sum_);
  }

 private:
  int sum_ = 0;
  pw::async2::FutureState state_;
};

static_assert(pw::async2::Future<CustomSumFuture>);

TEST(FutureTask, EmplaceFutureInPlace) {
  DispatcherForTest dispatcher;

  FutureTask<CustomSumFuture> task;
  EXPECT_FALSE(task.is_pendable());

  task.emplace_future(15, 27);
  EXPECT_TRUE(task.is_pendable());

  dispatcher.Post(task);
  dispatcher.RunUntilStalled();
  EXPECT_EQ(task.Wait(), 42);

  task.reset();
  EXPECT_FALSE(task.is_pendable());
  EXPECT_FALSE(task.has_value());

  FutureTask<CustomSumFuture, ReturnValuePolicy::kDiscard> discard_task;
  discard_task.emplace_future(3, 4);
  EXPECT_TRUE(discard_task.is_pendable());
  dispatcher.Post(discard_task);
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(discard_task.is_complete());
}

}  // namespace
