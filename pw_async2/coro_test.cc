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

#include "pw_async2/coro.h"

#include <optional>

#include "pw_allocator/null_allocator.h"
#include "pw_allocator/testing.h"
#include "pw_async2/coro_task.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_compilation_testing/negative_compilation.h"
#include "pw_containers/internal/test_helpers.h"
#include "pw_status/status.h"
#include "pw_status/try.h"

namespace {

using ::pw::OkStatus;
using ::pw::Result;
using ::pw::Status;
using ::pw::allocator::Allocator;
using ::pw::allocator::GetNullAllocator;
using ::pw::allocator::test::AllocatorForTest;
using ::pw::async2::Context;
using ::pw::async2::Coro;
using ::pw::async2::CoroContext;
using ::pw::async2::CoroTask;
using ::pw::async2::DispatcherForTest;
using ::pw::async2::Pending;
using ::pw::async2::Poll;
using ::pw::async2::Waker;
using ::pw::containers::test::Counter;

Coro<Result<int>> ImmediatelyReturnsFive(CoroContext) { co_return 5; }

Coro<Status> StoresFiveThenReturns(CoroContext coro_cx, int& out) {
  PW_CO_TRY_ASSIGN(out, co_await ImmediatelyReturnsFive(coro_cx));
  co_return OkStatus();
}

class ObjectWithCoroMethod {
 public:
  ObjectWithCoroMethod(int x) : x_(x) {}
  Coro<Status> CoroMethodStoresField(CoroContext, int& out) {
    out = x_;
    co_return OkStatus();
  }

 private:
  int x_;
};

class CoroTest : public ::testing::Test {
 protected:
  AllocatorForTest<2048> alloc_;
};

TEST_F(CoroTest, BasicFunctionsWithoutYieldingRun) {
  int output = 0;
  CoroTask task = StoresFiveThenReturns(alloc_, output);
  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  dispatcher.RunToCompletion();
  EXPECT_EQ(task.Wait(), OkStatus());
  EXPECT_EQ(output, 5);
}

TEST(Coro, AllocationFailureProducesInvalidCoro) {
  EXPECT_FALSE(
      ImmediatelyReturnsFive(CoroContext(GetNullAllocator())).IsValid());
  int x = 0;
  EXPECT_FALSE(
      StoresFiveThenReturns(CoroContext(GetNullAllocator()), x).IsValid());
}

TEST_F(CoroTest, ObjectWithCoroMethodIsCallable) {
  ObjectWithCoroMethod obj(4);
  int out = 22;
  CoroTask task = obj.CoroMethodStoresField(alloc_, out);
  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  dispatcher.RunToCompletion();

  EXPECT_EQ(task.Wait(), OkStatus());
  EXPECT_EQ(out, 4);
}

struct FakeFuture {
  using value_type = int;

  FakeFuture() : poll_count(0), return_value(Pending()), last_waker() {}

  FakeFuture(const FakeFuture&) = delete;
  FakeFuture& operator=(const FakeFuture&) = delete;

  FakeFuture(FakeFuture&&) = default;
  FakeFuture& operator=(FakeFuture&&) = default;

  bool is_pendable() const { return true; }
  bool is_complete() const { return false; }

  Poll<int> Pend(Context& cx) {
    ++poll_count;
    PW_ASYNC_STORE_WAKER(
        cx, last_waker, "FakeFuture is waiting for last_waker");
    return return_value;
  }

  int poll_count;
  Poll<int> return_value;
  Waker last_waker;
};

Coro<Result<int>> AddTwo(CoroContext, FakeFuture& a, FakeFuture& b) {
  co_return co_await a + co_await b;
}

Coro<Status> AddTwoThenStore(CoroContext alloc_,
                             FakeFuture& a,
                             FakeFuture& b,
                             int& out) {
  PW_CO_TRY_ASSIGN(out, co_await AddTwo(alloc_, a, b));
  co_return OkStatus();
}

TEST_F(CoroTest, AwaitMultipleAndAwakenRuns) {
  FakeFuture a;
  FakeFuture b;
  int output = 0;
  CoroTask task = AddTwoThenStore(alloc_, a, b, output);
  DispatcherForTest dispatcher;
  dispatcher.Post(task);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(a.poll_count, 1);
  EXPECT_EQ(b.poll_count, 0);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(a.poll_count, 1);
  EXPECT_EQ(b.poll_count, 0);

  int a_value = 4;
  a.return_value = a_value;
  a.last_waker.Wake();
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(a.poll_count, 2);
  EXPECT_EQ(b.poll_count, 1);

  int b_value = 5;
  b.return_value = b_value;
  b.last_waker.Wake();
  dispatcher.RunToCompletion();

  EXPECT_EQ(task.Wait(), OkStatus());

  EXPECT_EQ(a.poll_count, 2);
  EXPECT_EQ(b.poll_count, 2);
  EXPECT_EQ(output, a_value + b_value);
}

Coro<Counter> MultiplyByThree(CoroContext, Counter value) {
  co_return Counter(value.value * 3);
}

Coro<int> NumberNine(CoroContext cx) {
  co_return co_await MultiplyByThree(cx, 3);
}

Coro<Counter> ReturnsAValue(CoroContext cx, int add) {
  co_return Counter(add + co_await NumberNine(cx));
}

TEST_F(CoroTest, ReturnsInt) {
  CoroTask task(NumberNine(alloc_));

  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  dispatcher.RunToCompletion();

  EXPECT_EQ(task.Wait(), 9);
}

TEST_F(CoroTest, Memory) {
  CoroTask task(ReturnsAValue(alloc_, 5));

  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  dispatcher.RunToCompletion();

  EXPECT_EQ(task.Wait().value, 9 + 5);
}

Coro<void> WaitUntilFive(CoroContext, FakeFuture& fut) {
  EXPECT_EQ(co_await fut, 5);
}

TEST_F(CoroTest, AwaitVoidCoro) {
  FakeFuture fut;
  CoroContext cx(alloc_);
  CoroTask task(WaitUntilFive(cx, fut));

  DispatcherForTest dispatcher;
  dispatcher.Post(task);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(fut.poll_count, 1);

  fut.return_value = 5;
  fut.last_waker.Wake();
  dispatcher.RunToCompletion();

  EXPECT_EQ(fut.poll_count, 2);
}

#if PW_NC_TEST(CoroContextReference)
PW_NC_EXPECT("CoroContext must be passed by value");
[[maybe_unused]] Coro<void> CoroContextReference(CoroContext&) { co_return; }
#elif PW_NC_TEST(CoroContextConstReference)
PW_NC_EXPECT("CoroContext must be passed by value");
[[maybe_unused]] Coro<void> CoroContextConstReference(const CoroContext&) {
  co_return;
}
#elif PW_NC_TEST(ClassMethodReference)
PW_NC_EXPECT("CoroContext must be passed by value");
struct [[maybe_unused]] Foo {
  Coro<void> Method(CoroContext&) { co_return; }
};
#elif PW_NC_TEST(ClassMethodConstReference)
PW_NC_EXPECT("CoroContext must be passed by value");
struct [[maybe_unused]] Foo {
  Coro<void> Method(const CoroContext&) { co_return; }
};
#elif PW_NC_TEST(NonMemberFunctionWithSecondCoroContextArgument)
PW_NC_EXPECT("CoroContext must be passed by value");

class Foo {};

[[maybe_unused]] Coro<void> CoroContextAsSecondArg(Foo, CoroContext) {
  co_return;
}

[[maybe_unused]] void Invoke() {
  CoroContextAsSecondArg(Foo{}, CoroContext(pw::allocator::GetNullAllocator()));
}
#elif PW_NC_TEST(NoArguments)
PW_NC_EXPECT("CoroContext must be passed by value");
[[maybe_unused]] Coro<void> CoroContextReference() { co_return; }
#elif PW_NC_TEST(TwoContextArguments)
PW_NC_EXPECT("must have exactly one CoroContext argument");
[[maybe_unused]] Coro<void> TwoContexts(CoroContext, int, CoroContext&) {
  co_return;
}
#endif  // PW_NC_TEST
        //

class SomeClass {};

// With C++20 coroutines, it's not possible to distinguish between a member
// function with CoroContext as its first arg and a free function with
// CoroContext as its second arg. We could require a different context type for
// member functions, but that introduces complexity without solving any real
// problems. Coroutines declared in this way look odd, but work just fine.
Coro<int> ThisShouldBeACompilationErrorDoNotDoThis(SomeClass&, CoroContext) {
  co_return 123;
}

TEST_F(CoroTest, FreeFunctionThatLooksLikeAMember) {
  SomeClass some_class;
  CoroTask task(ThisShouldBeACompilationErrorDoNotDoThis(some_class, alloc_));

  DispatcherForTest dispatcher;
  dispatcher.Post(task);

  dispatcher.RunToCompletion();

  EXPECT_EQ(task.value(), 123);
}

}  // namespace
