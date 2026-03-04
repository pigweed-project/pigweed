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

#include "pw_async2/fallible_coro_task.h"

#include "pw_allocator/null_allocator.h"
#include "pw_allocator/testing.h"
#include "pw_async2/coro.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/value_future.h"
#include "pw_containers/internal/test_helpers.h"
#include "pw_status/status.h"
#include "pw_status/try.h"

namespace {

using ::pw::OkStatus;
using ::pw::Result;
using ::pw::Status;
using ::pw::allocator::GetNullAllocator;
using ::pw::allocator::test::AllocatorForTest;
using ::pw::async2::Coro;
using ::pw::async2::CoroContext;
using ::pw::async2::DispatcherForTest;
using ::pw::async2::FallibleCoroTask;
using ::pw::async2::ReturnValuePolicy;
using ::pw::async2::ValueProvider;
using ::pw::containers::test::Counter;

Coro<Result<int>> ImmediatelyReturnsFive(CoroContext&) { co_return 5; }

Coro<Status> StoresFiveThenReturns(CoroContext& coro_cx, int& out) {
  PW_CO_TRY_ASSIGN(out, co_await ImmediatelyReturnsFive(coro_cx));
  co_return OkStatus();
}

class FallibleCoroTaskTest : public ::testing::Test {
 protected:
  FallibleCoroTaskTest() : coro_cx_(alloc_) {}

  AllocatorForTest<256> alloc_;
  CoroContext coro_cx_;
};

TEST_F(FallibleCoroTaskTest, BasicFunctionsWithoutYieldingRun) {
  int output = 0;
  bool error_handler_ran = false;
  FallibleCoroTask task(StoresFiveThenReturns(coro_cx_, output),
                        [&error_handler_ran] { error_handler_ran = true; });
  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  dispatcher.RunToCompletion();
  EXPECT_EQ(output, 5);
  EXPECT_FALSE(error_handler_ran);
  ASSERT_TRUE(task.has_value());
  EXPECT_EQ(task.value(), OkStatus());
}

TEST_F(FallibleCoroTaskTest, AllocationFailureProducesInvalidCoro) {
  CoroContext null_cx(GetNullAllocator());
  EXPECT_FALSE(ImmediatelyReturnsFive(null_cx).IsValid());
  bool error_handler_ran = false;
  int output = 0;
  FallibleCoroTask task(StoresFiveThenReturns(null_cx, output),
                        [&error_handler_ran] { error_handler_ran = true; });
  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  dispatcher.RunToCompletion();
  EXPECT_TRUE(error_handler_ran);
  EXPECT_FALSE(task.has_value());
}

enum class ObjectState {
  kUninitialized,
  kConstructed,
  kDestroyed,
};

class TrackedObject {
 public:
  explicit TrackedObject(ObjectState& state) : state_(state) {
    state_ = ObjectState::kConstructed;
  }

  ~TrackedObject() { state_ = ObjectState::kDestroyed; }

 private:
  ObjectState& state_;
};

Coro<Status> Inner(CoroContext&) { co_return OkStatus(); }

Coro<Status> Outer(CoroContext& cx,
                   std::optional<Status>& returned_status,
                   TrackedObject argument,
                   ObjectState& before_inner,
                   ObjectState& after_inner) {
  TrackedObject before(before_inner);

  returned_status = co_await Inner(cx);

  TrackedObject after(after_inner);
  co_return OkStatus();
}

TEST_F(FallibleCoroTaskTest, AllocationFailureInNestedCoroAborts) {
  std::optional<Status> returned_status;

  ObjectState argument = ObjectState::kUninitialized;
  ObjectState before = ObjectState::kUninitialized;
  ObjectState after = ObjectState::kUninitialized;

  Coro<Status> outer_coro =
      Outer(coro_cx_, returned_status, TrackedObject(argument), before, after);
  ASSERT_TRUE(outer_coro.IsValid());

  alloc_.Exhaust();  // Prevent allocation of Inner coroutine.

  bool error_handler_ran = false;
  FallibleCoroTask task(std::move(outer_coro),
                        [&error_handler_ran] { error_handler_ran = true; });

  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  dispatcher.RunToCompletion();

  EXPECT_FALSE(returned_status.has_value());

  EXPECT_EQ(argument, ObjectState::kDestroyed);
  EXPECT_EQ(before, ObjectState::kDestroyed);
  EXPECT_EQ(after, ObjectState::kUninitialized);
  EXPECT_TRUE(error_handler_ran);
  EXPECT_FALSE(task.has_value());
}

Coro<Counter> GetAndDouble(CoroContext& cx, ValueProvider<Counter>& provider) {
  Counter value = co_await provider.Get();
  co_return Counter(value.value * 2);
}

Coro<Counter> GetTwoAndDouble(CoroContext& cx,
                              ValueProvider<Counter>& provider) {
  Counter one = co_await GetAndDouble(cx, provider);
  Counter two = co_await provider.Get();
  co_return Counter(one.value + two.value * 2);
}

Coro<Counter> Accumulate(CoroContext& cx, ValueProvider<Counter>& provider) {
  co_return Counter(co_await GetAndDouble(cx, provider) +
                    co_await GetAndDouble(cx, provider) +
                    co_await GetTwoAndDouble(cx, provider));
}

TEST(FallibleCoroTaskTestWithBigAllocator, NestedBlockingCoroutines) {
  AllocatorForTest<2048> alloc;
  CoroContext coro_cx(alloc);
  ValueProvider<Counter> provider;

  bool error_handler_ran = false;
  FallibleCoroTask task(Accumulate(coro_cx, provider),
                        [&error_handler_ran] { error_handler_ran = true; });

  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  provider.Resolve(Counter(1));
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  provider.Resolve(Counter(2));
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  provider.Resolve(Counter(3));
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  provider.Resolve(Counter(4));
  dispatcher.RunToCompletion();

  EXPECT_EQ(task.Wait()->value, 2 + 4 + 6 + 8);
  EXPECT_FALSE(error_handler_ran);
}

TEST(FallibleCoroTaskTestWithBigAllocator, AllocFailureAfterSuspend) {
  AllocatorForTest<2048> alloc;
  CoroContext coro_cx(alloc);
  ValueProvider<Counter> provider;

  bool error_handler_ran = false;
  FallibleCoroTask task(Accumulate(coro_cx, provider),
                        [&error_handler_ran] { error_handler_ran = true; });

  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  provider.Resolve(Counter(1));
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  alloc.Exhaust();
  provider.Resolve(Counter(2));
  dispatcher.RunToCompletion();
  EXPECT_TRUE(error_handler_ran);

  EXPECT_FALSE(task.Wait().has_value());
}

Coro<int> ReturnsInteger(CoroContext&, int value) { co_return value; }

TEST_F(FallibleCoroTaskTest, StoreReturnValue) {
  FallibleCoroTask task(ReturnsInteger(coro_cx_, 42), [] { FAIL(); });
  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  dispatcher.RunToCompletion();
  EXPECT_TRUE(task.has_value());
  EXPECT_EQ(task.value(), 42);
  EXPECT_EQ(task.Wait(), 42);
}

TEST_F(FallibleCoroTaskTest, DiscardReturnValue) {
  auto handler = [] { FAIL(); };
  // Explicitly specify kDiscard policy
  FallibleCoroTask<int, decltype(handler), ReturnValuePolicy::kDiscard> task(
      ReturnsInteger(coro_cx_, 42), std::move(handler));
  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  dispatcher.RunToCompletion();
  // task.value() is not available
}

TEST_F(FallibleCoroTaskTest, WaitBlocks) {
  FallibleCoroTask task(ReturnsInteger(coro_cx_, 99), [] { FAIL(); });

  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  dispatcher.RunToCompletion();
  EXPECT_EQ(task.Wait(), 99);
}

Coro<Status> FailingInnerCoro(CoroContext& cx, ValueProvider<int>& provider) {
  co_await provider.Get();
  co_return co_await Inner(cx);
}

Coro<Status> OuterCoro(CoroContext& cx, ValueProvider<int>& provider) {
  co_return co_await FailingInnerCoro(cx, provider);
}

TEST(FallibleCoroTaskTestWithBigAllocator, AwaitNestedCoroutineAbortedCrashes) {
  AllocatorForTest<2048> alloc;
  CoroContext coro_cx(alloc);
  ValueProvider<int> provider;
  bool error_handler_ran = false;
  FallibleCoroTask task(OuterCoro(coro_cx, provider),
                        [&error_handler_ran] { error_handler_ran = true; });

  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  alloc.Exhaust();
  provider.Resolve(1);
  dispatcher.RunToCompletion();

  EXPECT_TRUE(error_handler_ran);
}

}  // namespace
