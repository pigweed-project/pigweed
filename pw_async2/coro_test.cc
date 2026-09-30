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
#include "pw_async2/await.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/func_task.h"
#include "pw_async2/future.h"
#include "pw_async2/future_task.h"
#include "pw_async2/value_future.h"
#include "pw_compilation_testing/negative_compilation.h"
#include "pw_containers/internal/test_helpers.h"
#include "pw_preprocessor/compiler.h"
#include "pw_status/status.h"
#include "pw_status/try.h"

namespace {

using ::pw::OkStatus;
using ::pw::Result;
using ::pw::Status;
using ::pw::allocator::GetNullAllocator;
using ::pw::allocator::test::AllocatorForTest;
using ::pw::async2::Context;
using ::pw::async2::Coro;
using ::pw::async2::CoroContext;
using ::pw::async2::DispatcherForTest;
using ::pw::async2::FallibleCoro;
using ::pw::async2::FuncTask;
using ::pw::async2::Future;
using ::pw::async2::FutureTask;
using ::pw::async2::Generator;
using ::pw::async2::OptionalValueProvider;
using ::pw::async2::Pending;
using ::pw::async2::Poll;
using ::pw::async2::Ready;
using ::pw::async2::ValueProvider;
using ::pw::async2::Waker;
using ::pw::containers::test::Counter;

// Use PW_NO_INLINE to prevent the compiler from optimizing the coroutine onto
// the stack, ensuring dynamic allocation so allocation failure can be tested.
PW_NO_INLINE Coro<Result<int>> ImmediatelyReturnsFive(CoroContext) {
  co_return 5;
}

PW_NO_INLINE Coro<Status> StoresFiveThenReturns(CoroContext coro_cx, int& out) {
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
  {
    FutureTask task(StoresFiveThenReturns(alloc_, output));
    DispatcherForTest dispatcher;
    dispatcher.Post(task);
    dispatcher.RunToCompletion();
    EXPECT_EQ(task.Wait(), OkStatus());
    EXPECT_EQ(output, 5);
  }
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

bool CreateWithoutRunningImmediatelyReturnsFive(pw::Allocator& alloc) {
  return ImmediatelyReturnsFive(CoroContext(alloc)).is_pendable();
}

bool CreateWithoutRunningStoresFiveThenReturns(pw::Allocator& alloc) {
  int output = 0;
  return StoresFiveThenReturns(CoroContext(alloc), output).is_pendable();
}

TEST(Coro, AllocationFailureProducesInvalidCoro) {
  EXPECT_FALSE(CreateWithoutRunningImmediatelyReturnsFive(GetNullAllocator()));
  EXPECT_FALSE(CreateWithoutRunningImmediatelyReturnsFive(GetNullAllocator()));

  EXPECT_FALSE(CreateWithoutRunningStoresFiveThenReturns(GetNullAllocator()));
  EXPECT_FALSE(CreateWithoutRunningStoresFiveThenReturns(GetNullAllocator()));
}

TEST_F(CoroTest, NoAllocationFailureProducesValidCoro) {
  EXPECT_TRUE(CreateWithoutRunningImmediatelyReturnsFive(alloc_));
  EXPECT_TRUE(CreateWithoutRunningImmediatelyReturnsFive(alloc_));

  EXPECT_TRUE(CreateWithoutRunningStoresFiveThenReturns(alloc_));
  EXPECT_TRUE(CreateWithoutRunningStoresFiveThenReturns(alloc_));

  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(CoroTest, InvalidTaskIfAllocationFails) {
  alloc_.Exhaust();
  auto coro = ImmediatelyReturnsFive(CoroContext(alloc_));
  EXPECT_FALSE(coro.is_pendable());

  DispatcherForTest dispatcher;
  FutureTask task(std::move(coro));
  dispatcher.Post(task);
  EXPECT_DEATH_IF_SUPPORTED(dispatcher.RunToCompletion(), "");
}

TEST_F(CoroTest, ObjectWithCoroMethodIsCallable) {
  ObjectWithCoroMethod obj(4);
  int out = 22;
  {
    FutureTask task(obj.CoroMethodStoresField(alloc_, out));
    DispatcherForTest dispatcher;
    dispatcher.Post(task);
    dispatcher.RunToCompletion();

    EXPECT_EQ(task.Wait(), OkStatus());
    EXPECT_EQ(out, 4);
  }
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
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
  {
    FutureTask task(AddTwoThenStore(alloc_, a, b, output));
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
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
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
  {
    FutureTask task(NumberNine(alloc_));

    DispatcherForTest dispatcher;
    dispatcher.Post(task);
    dispatcher.RunToCompletion();

    EXPECT_EQ(task.Wait(), 9);
  }
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(CoroTest, Memory) {
  {
    FutureTask task(ReturnsAValue(alloc_, 5));

    DispatcherForTest dispatcher;
    dispatcher.Post(task);
    dispatcher.RunToCompletion();

    EXPECT_EQ(task.Wait().value, 9 + 5);
  }
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

Coro<void> WaitUntilFive(CoroContext, FakeFuture& fut) {
  EXPECT_EQ(co_await fut, 5);
}

Coro<Status> AwaitVoidCoroWrapper(CoroContext cx, FakeFuture& fut) {
  co_await WaitUntilFive(cx, fut);
  co_return OkStatus();
}

TEST_F(CoroTest, AwaitVoidCoro) {
  FakeFuture fut;
  {
    CoroContext cx(alloc_);
    FutureTask task(WaitUntilFive(cx, fut));

    DispatcherForTest dispatcher;
    dispatcher.Post(task);

    EXPECT_TRUE(dispatcher.RunUntilStalled());
    EXPECT_EQ(fut.poll_count, 1);

    fut.return_value = 5;
    fut.last_waker.Wake();
    dispatcher.RunToCompletion();

    EXPECT_EQ(fut.poll_count, 2);
  }
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(CoroTest, AwaitVoidCoroInsideAnotherCoroutine) {
  FakeFuture fut;
  {
    FutureTask task(AwaitVoidCoroWrapper(alloc_, fut));

    DispatcherForTest dispatcher;
    dispatcher.Post(task);

    EXPECT_TRUE(dispatcher.RunUntilStalled());
    EXPECT_EQ(fut.poll_count, 1);

    fut.return_value = 5;
    fut.last_waker.Wake();
    dispatcher.RunToCompletion();

    EXPECT_EQ(task.Wait(), OkStatus());
    EXPECT_EQ(fut.poll_count, 2);
  }
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
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
#elif PW_NC_TEST(MakeFallibleVoidCoroNonVoidHandler)
PW_NC_EXPECT("no matching member function for call to 'MakeFallible'");
[[maybe_unused]] Coro<void> VoidCoro(CoroContext) { co_return; }
[[maybe_unused]] void BadHandler() {
  (void)VoidCoro(pw::allocator::GetNullAllocator()).MakeFallible([] {
    return 42;
  });
}
#elif PW_NC_TEST(MakeFallibleIncompatibleHandlerReturnType)
PW_NC_EXPECT("no matching member function for call to 'MakeFallible'");
[[maybe_unused]] Coro<int> IntCoro(CoroContext) { co_return 1; }
[[maybe_unused]] void BadHandler() {
  (void)IntCoro(pw::allocator::GetNullAllocator()).MakeFallible([] {
    return pw::OkStatus();
  });
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
  {
    FutureTask task(
        ThisShouldBeACompilationErrorDoNotDoThis(some_class, alloc_));

    DispatcherForTest dispatcher;
    dispatcher.Post(task);

    dispatcher.RunToCompletion();

    EXPECT_EQ(task.value(), 123);
  }
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

Generator<int> CountToFive(CoroContext cx) {
  for (int i = 1; i <= 5; ++i) {
    co_yield i;
  }
}

Coro<int> SumGenerator(CoroContext cx, Generator<int>& gen) {
  int sum = 0;
  while (auto val = co_await gen.Next()) {
    sum += *val;
  }
  co_return sum;
}

TEST_F(CoroTest, GeneratorYieldsValues) {
  {
    Generator<int> gen = CountToFive(alloc_);
    FutureTask task(SumGenerator(alloc_, gen));

    DispatcherForTest dispatcher;
    dispatcher.Post(task);
    dispatcher.RunToCompletion();

    EXPECT_EQ(task.Wait(), 15);
  }
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

Generator<int> AwaitAndYieldGenerator(CoroContext cx,
                                      OptionalValueProvider<int>& provider) {
  while (true) {
    std::optional<int> val = co_await provider.Get();
    if (!val.has_value()) {
      co_return;
    }
    co_yield *val * 2;
  }
}

TEST_F(CoroTest, GeneratorAwaitsAndYields) {
  OptionalValueProvider<int> provider;
  {
    Generator<int> gen = AwaitAndYieldGenerator(alloc_, provider);
    FutureTask task(SumGenerator(alloc_, gen));

    DispatcherForTest dispatcher;
    dispatcher.Post(task);

    EXPECT_TRUE(dispatcher.RunUntilStalled());

    provider.Resolve(5);
    EXPECT_TRUE(dispatcher.RunUntilStalled());

    provider.Resolve(10);
    EXPECT_TRUE(dispatcher.RunUntilStalled());

    provider.Cancel();
    dispatcher.RunToCompletion();

    EXPECT_EQ(task.Wait(), 30);
  }
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

PW_NO_INLINE Coro<void> ReturnsVoid(CoroContext) { co_return; }

PW_NO_INLINE Coro<int> ReturnsInt(CoroContext, int val) { co_return val; }

Coro<int> AwaitsProvider(CoroContext, OptionalValueProvider<int>& provider) {
  std::optional<int> val = co_await provider.Get();
  co_return val.value_or(-1);
}

TEST(Coro, SatisfiesFutureConcept) {
  static_assert(Future<Coro<void>>);
  static_assert(Future<Coro<int>>);
  static_assert(Future<Coro<Status>>);
}

TEST(Coro, DefaultConstructedIsInvalidFuture) {
  Coro<int> empty;
  EXPECT_FALSE(empty.is_pendable());
  EXPECT_FALSE(empty.is_complete());

  Coro<void> empty_void;
  EXPECT_FALSE(empty_void.is_pendable());
  EXPECT_FALSE(empty_void.is_complete());
}

TEST(Coro, AllocationFailureFutureState) {
  Coro<Result<int>> coro = ImmediatelyReturnsFive(GetNullAllocator());
  EXPECT_FALSE(coro.is_pendable());
  EXPECT_FALSE(coro.is_complete());
}

TEST_F(CoroTest, MoveTransfersFutureState) {
  Coro<int> coro1 = ReturnsInt(alloc_, 42);
  EXPECT_TRUE(coro1.is_pendable());
  EXPECT_FALSE(coro1.is_complete());

  Coro<int> coro2 = std::move(coro1);
  EXPECT_FALSE(coro1.is_pendable());  // NOLINT(bugprone-use-after-move)
  EXPECT_FALSE(coro1.is_complete());  // NOLINT(bugprone-use-after-move)

  EXPECT_TRUE(coro2.is_pendable());
  EXPECT_FALSE(coro2.is_complete());
}

TEST_F(CoroTest, PendCompletesAndUpdatesFutureState) {
  {
    Coro<int> coro = ReturnsInt(alloc_, 5);
    EXPECT_TRUE(coro.is_pendable());
    EXPECT_FALSE(coro.is_complete());

    DispatcherForTest dispatcher;
    Poll<int> result = dispatcher.RunInTaskUntilStalled(coro);
    EXPECT_TRUE(result.IsReady());
    EXPECT_EQ(result.value(), 5);
    EXPECT_FALSE(coro.is_pendable());
    EXPECT_TRUE(coro.is_complete());
  }
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(CoroTest, PendVoidCompletesAndUpdatesFutureState) {
  {
    Coro<void> coro = ReturnsVoid(alloc_);
    EXPECT_TRUE(coro.is_pendable());
    EXPECT_FALSE(coro.is_complete());

    DispatcherForTest dispatcher;
    Poll<void> result = dispatcher.RunInTaskUntilStalled(coro);
    EXPECT_TRUE(result.IsReady());
    EXPECT_FALSE(coro.is_pendable());
    EXPECT_TRUE(coro.is_complete());
  }
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(CoroTest, PendSuspendedCoroRemainsPendableUntilCompletion) {
  OptionalValueProvider<int> provider;
  {
    Coro<int> coro = AwaitsProvider(alloc_, provider);
    EXPECT_TRUE(coro.is_pendable());
    EXPECT_FALSE(coro.is_complete());

    DispatcherForTest dispatcher;
    Poll<int> result = dispatcher.RunInTaskUntilStalled(coro);
    EXPECT_TRUE(result.IsPending());
    EXPECT_TRUE(coro.is_pendable());
    EXPECT_FALSE(coro.is_complete());

    provider.Resolve(42);
    result = dispatcher.RunInTaskUntilStalled(coro);
    EXPECT_TRUE(result.IsReady());
    EXPECT_EQ(result.value(), 42);
    EXPECT_FALSE(coro.is_pendable());
    EXPECT_TRUE(coro.is_complete());
  }
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(CoroTest, AwaitCoroInsideFuncTask) {
  {
    Coro<int> coro = ReturnsInt(alloc_, 5);
    int result = 0;
    FuncTask task([&](Context& cx) -> Poll<> {
      PW_AWAIT(result, coro, cx);
      return Ready();
    });

    DispatcherForTest dispatcher;
    dispatcher.Post(task);
    dispatcher.RunToCompletion();
    EXPECT_EQ(result, 5);
    EXPECT_FALSE(coro.is_pendable());
    EXPECT_TRUE(coro.is_complete());
  }
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(CoroTest, AwaitVoidCoroInsideFuncTask) {
  {
    Coro<void> coro = ReturnsVoid(alloc_);
    bool finished = false;
    FuncTask task([&](Context& cx) -> Poll<> {
      PW_AWAIT(coro, cx);
      finished = true;
      return Ready();
    });

    DispatcherForTest dispatcher;
    dispatcher.Post(task);
    dispatcher.RunToCompletion();
    EXPECT_TRUE(finished);
    EXPECT_FALSE(coro.is_pendable());
    EXPECT_TRUE(coro.is_complete());
  }
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(CoroTest, FutureTaskRunsCoro) {
  {
    FutureTask task(ReturnsInt(alloc_, 42));
    DispatcherForTest dispatcher;
    dispatcher.Post(task);
    dispatcher.RunToCompletion();
    EXPECT_EQ(task.Wait(), 42);
  }
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST(FallibleCoro, DefaultConstructedIsInvalidFuture) {
  FallibleCoro<int> empty;
  EXPECT_FALSE(empty.is_pendable());
  EXPECT_FALSE(empty.is_complete());

  FallibleCoro<void> empty_void;
  EXPECT_FALSE(empty_void.is_pendable());
  EXPECT_FALSE(empty_void.is_complete());

  DispatcherForTest dispatcher;
  EXPECT_DEATH_IF_SUPPORTED(
      dispatcher.RunInTaskUntilStalled(empty).IgnorePoll(), "");
}

TEST_F(CoroTest, FallibleCoroDefaultTemplateParameter) {
  alloc_.Exhaust();

  bool handler_called = false;
  FallibleCoro<int> fut =
      ReturnsInt(alloc_, 42).MakeFallible([&] { handler_called = true; });
  EXPECT_TRUE(fut.is_pendable());

  DispatcherForTest dispatcher;
  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(fut),
            Ready(std::optional<int>(std::nullopt)));
  EXPECT_TRUE(handler_called);

  FallibleCoro<int> fut_no_handler(ReturnsInt(alloc_, 42));
  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(fut_no_handler),
            Ready(std::optional<int>(std::nullopt)));
}

TEST_F(CoroTest, FallibleCoroConstructWithHandlerAndAssignCoro) {
  int handler_calls = 0;
  FallibleCoro<int> fut([&] { ++handler_calls; });
  EXPECT_FALSE(fut.is_pendable());
  EXPECT_FALSE(fut.is_complete());

  // Assign a valid Coro<int>.
  fut = ReturnsInt(alloc_, 42);
  EXPECT_TRUE(fut.is_pendable());
  EXPECT_FALSE(fut.is_complete());

  DispatcherForTest dispatcher;
  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(fut),
            Ready(std::optional<int>(42)));
  EXPECT_EQ(handler_calls, 0);
  EXPECT_FALSE(fut.is_pendable());
  EXPECT_TRUE(fut.is_complete());

  // Assign a Coro<int> that fails to allocate; the original handler is
  // preserved and invoked.
  alloc_.Exhaust();
  fut = ReturnsInt(alloc_, 99);
  EXPECT_TRUE(fut.is_pendable());
  EXPECT_FALSE(fut.is_complete());

  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(fut),
            Ready(std::optional<int>(std::nullopt)));
  EXPECT_EQ(handler_calls, 1);
  EXPECT_FALSE(fut.is_pendable());
  EXPECT_TRUE(fut.is_complete());

  // Assign again; handler is still preserved.
  fut = ReturnsInt(alloc_, 100);
  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(fut),
            Ready(std::optional<int>(std::nullopt)));
  EXPECT_EQ(handler_calls, 2);
}

TEST_F(CoroTest, FallibleCoroMoveConstructAndAssign) {
  {
    auto fut1 = ReturnsInt(alloc_, 42).MakeFallible(-1);
    static_assert(Future<decltype(fut1)>);
    EXPECT_TRUE(fut1.is_pendable());
    EXPECT_FALSE(fut1.is_complete());

    auto fut2 = std::move(fut1);
    EXPECT_FALSE(fut1.is_pendable());  // NOLINT(bugprone-use-after-move)
    EXPECT_FALSE(fut1.is_complete());  // NOLINT(bugprone-use-after-move)
    EXPECT_TRUE(fut2.is_pendable());
    EXPECT_FALSE(fut2.is_complete());

    decltype(fut2) fut3;
    EXPECT_FALSE(fut3.is_pendable());
    EXPECT_FALSE(fut3.is_complete());

    fut3 = std::move(fut2);
    EXPECT_FALSE(fut2.is_pendable());  // NOLINT(bugprone-use-after-move)
    EXPECT_FALSE(fut2.is_complete());  // NOLINT(bugprone-use-after-move)
    EXPECT_TRUE(fut3.is_pendable());
    EXPECT_FALSE(fut3.is_complete());

    DispatcherForTest dispatcher;
    Poll<int> result = dispatcher.RunInTaskUntilStalled(fut3);
    EXPECT_EQ(result, Ready(42));
    EXPECT_FALSE(fut3.is_pendable());
    EXPECT_TRUE(fut3.is_complete());
  }
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(CoroTest, FallibleCoroMoveWhenInitialAllocFailed) {
  alloc_.Exhaust();
  auto fut1 = ReturnsInt(alloc_, 42).MakeFallible(-1);
  EXPECT_TRUE(fut1.is_pendable());
  EXPECT_FALSE(fut1.is_complete());

  auto fut2 = std::move(fut1);
  EXPECT_FALSE(fut1.is_pendable());  // NOLINT(bugprone-use-after-move)
  EXPECT_FALSE(fut1.is_complete());  // NOLINT(bugprone-use-after-move)
  EXPECT_TRUE(fut2.is_pendable());
  EXPECT_FALSE(fut2.is_complete());

  DispatcherForTest dispatcher;
  Poll<int> result = dispatcher.RunInTaskUntilStalled(fut2);
  EXPECT_EQ(result, Ready(-1));
  EXPECT_FALSE(fut2.is_pendable());
  EXPECT_TRUE(fut2.is_complete());
}

TEST_F(CoroTest, FallibleCoroSuspendedRemainsPendableUntilCompletion) {
  OptionalValueProvider<int> provider;
  {
    auto fut = AwaitsProvider(alloc_, provider).MakeFallible(-99);
    EXPECT_TRUE(fut.is_pendable());
    EXPECT_FALSE(fut.is_complete());

    DispatcherForTest dispatcher;
    Poll<int> result = dispatcher.RunInTaskUntilStalled(fut);
    EXPECT_TRUE(result.IsPending());
    EXPECT_TRUE(fut.is_pendable());
    EXPECT_FALSE(fut.is_complete());

    provider.Resolve(42);
    result = dispatcher.RunInTaskUntilStalled(fut);
    EXPECT_EQ(result, Ready(42));
    EXPECT_FALSE(fut.is_pendable());
    EXPECT_TRUE(fut.is_complete());
  }
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(CoroTest, MakeFallibleDefaultNoArgsSuccess) {
  {
    auto fut = ReturnsInt(alloc_, 42).MakeFallible();
    static_assert(
        std::is_same_v<decltype(fut)::value_type, std::optional<int>>);
    EXPECT_TRUE(fut.is_pendable());
    EXPECT_FALSE(fut.is_complete());

    DispatcherForTest dispatcher;
    Poll<std::optional<int>> result = dispatcher.RunInTaskUntilStalled(fut);
    EXPECT_EQ(result, Ready(std::optional<int>(42)));
    EXPECT_FALSE(fut.is_pendable());
    EXPECT_TRUE(fut.is_complete());
  }
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(CoroTest, MakeFallibleDefaultNoArgsFailure) {
  alloc_.Exhaust();
  auto fut = ReturnsInt(alloc_, 42).MakeFallible();
  static_assert(std::is_same_v<decltype(fut)::value_type, std::optional<int>>);
  EXPECT_TRUE(fut.is_pendable());
  EXPECT_FALSE(fut.is_complete());

  DispatcherForTest dispatcher;
  Poll<std::optional<int>> result = dispatcher.RunInTaskUntilStalled(fut);
  EXPECT_EQ(result, Ready(std::optional<int>(std::nullopt)));
  EXPECT_FALSE(fut.is_pendable());
  EXPECT_TRUE(fut.is_complete());
}

TEST_F(CoroTest, MakeFallibleDefaultNoArgsVoidFailure) {
  alloc_.Exhaust();
  auto fut = ReturnsVoid(alloc_).MakeFallible();
  static_assert(std::is_same_v<decltype(fut)::value_type, void>);
  EXPECT_TRUE(fut.is_pendable());
  EXPECT_FALSE(fut.is_complete());

  DispatcherForTest dispatcher;
  Poll<void> result = dispatcher.RunInTaskUntilStalled(fut);
  EXPECT_TRUE(result.IsReady());
  EXPECT_FALSE(fut.is_pendable());
  EXPECT_TRUE(fut.is_complete());
}

TEST_F(CoroTest, MakeFallibleSuccessReturnsOptional) {
  {
    bool handler_called = false;
    auto fut =
        ReturnsInt(alloc_, 42).MakeFallible([&] { handler_called = true; });
    EXPECT_TRUE(fut.is_pendable());
    EXPECT_FALSE(fut.is_complete());

    DispatcherForTest dispatcher;
    Poll<std::optional<int>> result = dispatcher.RunInTaskUntilStalled(fut);
    EXPECT_TRUE(result.IsReady());
    EXPECT_EQ(*result, 42);
    EXPECT_FALSE(handler_called);
    EXPECT_FALSE(fut.is_pendable());
    EXPECT_TRUE(fut.is_complete());
  }
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(CoroTest, MakeFallibleSuccessReturnsFallbackType) {
  {
    bool handler_called = false;
    auto fut = ReturnsInt(alloc_, 42).MakeFallible([&] {
      handler_called = true;
      return -1;
    });
    EXPECT_TRUE(fut.is_pendable());

    DispatcherForTest dispatcher;
    Poll<int> result = dispatcher.RunInTaskUntilStalled(fut);
    EXPECT_TRUE(result.IsReady());
    EXPECT_EQ(*result, 42);
    EXPECT_FALSE(handler_called);
    EXPECT_TRUE(fut.is_complete());
  }
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(CoroTest, MakeFallibleSuccessVoid) {
  {
    bool handler_called = false;
    auto fut = ReturnsVoid(alloc_).MakeFallible([&] { handler_called = true; });
    EXPECT_TRUE(fut.is_pendable());

    DispatcherForTest dispatcher;
    Poll<void> result = dispatcher.RunInTaskUntilStalled(fut);
    EXPECT_TRUE(result.IsReady());
    EXPECT_FALSE(handler_called);
    EXPECT_TRUE(fut.is_complete());
  }
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(CoroTest, MakeFallibleInitialAllocFailureCallsHandler) {
  alloc_.Exhaust();
  bool handler_called = false;
  auto fut =
      ReturnsInt(alloc_, 42).MakeFallible([&] { handler_called = true; });
  EXPECT_TRUE(fut.is_pendable());
  EXPECT_FALSE(fut.is_complete());

  DispatcherForTest dispatcher;
  Poll<std::optional<int>> result = dispatcher.RunInTaskUntilStalled(fut);
  EXPECT_TRUE(result.IsReady());
  EXPECT_EQ(*result, std::nullopt);
  EXPECT_TRUE(handler_called);
  EXPECT_FALSE(fut.is_pendable());
  EXPECT_TRUE(fut.is_complete());
}

TEST_F(CoroTest, MakeFallibleInitialAllocFailureReturnsFallback) {
  alloc_.Exhaust();
  bool handler_called = false;
  auto fut = ReturnsInt(alloc_, 42).MakeFallible([&] {
    handler_called = true;
    return -1;
  });

  DispatcherForTest dispatcher;
  Poll<int> result = dispatcher.RunInTaskUntilStalled(fut);
  EXPECT_TRUE(result.IsReady());
  EXPECT_EQ(*result, -1);
  EXPECT_TRUE(handler_called);
  EXPECT_FALSE(fut.is_pendable());
  EXPECT_TRUE(fut.is_complete());
}

TEST_F(CoroTest, MakeFallibleInitialAllocFailureVoid) {
  alloc_.Exhaust();
  bool handler_called = false;
  auto fut = ReturnsVoid(alloc_).MakeFallible([&] { handler_called = true; });

  DispatcherForTest dispatcher;
  Poll<void> result = dispatcher.RunInTaskUntilStalled(fut);
  EXPECT_TRUE(result.IsReady());
  EXPECT_TRUE(handler_called);
  EXPECT_FALSE(fut.is_pendable());
  EXPECT_TRUE(fut.is_complete());
}

PW_NO_INLINE Coro<Status> InnerCoro(CoroContext) { co_return OkStatus(); }

PW_NO_INLINE Coro<Status> OuterCoroAwaitingInner(CoroContext cx) {
  co_await InnerCoro(cx);
  co_return OkStatus();
}

TEST_F(CoroTest, MakeFallibleNestedAllocFailureRecovers) {
  auto coro = OuterCoroAwaitingInner(alloc_);
  alloc_.Exhaust();  // Prevent InnerCoro from allocating.

  bool handler_called = false;
  auto fut = std::move(coro).MakeFallible([&] {
    handler_called = true;
    return Status::ResourceExhausted();
  });

  DispatcherForTest dispatcher;
  Poll<Status> result = dispatcher.RunInTaskUntilStalled(fut);
  EXPECT_TRUE(result.IsReady());
  EXPECT_EQ(*result, Status::ResourceExhausted());
  EXPECT_TRUE(handler_called);
  EXPECT_FALSE(fut.is_pendable());
  EXPECT_TRUE(fut.is_complete());
}

Coro<int> OuterRecoveringFromInnerAllocFailure(CoroContext cx) {
  std::optional<int> inner = co_await ReturnsInt(cx, 10).MakeFallible();
  co_return inner.value_or(99);
}

TEST_F(CoroTest, CoAwaitFallibleCoroRecoversLocallyInOuterCoro) {
  {
    Coro<int> outer = OuterRecoveringFromInnerAllocFailure(alloc_);
    ASSERT_TRUE(outer.is_pendable());
    alloc_.Exhaust();  // Prevent ReturnsInt from allocating when resumed.

    DispatcherForTest dispatcher;
    Poll<int> result = dispatcher.RunInTaskUntilStalled(outer);
    EXPECT_EQ(result, Ready(99));
    EXPECT_FALSE(outer.is_pendable());
    EXPECT_TRUE(outer.is_complete());
  }
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(CoroTest, MakeFallibleInFutureTask) {
  {
    FutureTask task(ReturnsInt(alloc_, 99).MakeFallible([] { return -1; }));
    DispatcherForTest dispatcher;
    dispatcher.Post(task);
    dispatcher.RunToCompletion();
    EXPECT_EQ(task.Wait(), 99);
  }
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(CoroTest, MakeFallibleStatusFallbackDirectValue) {
  alloc_.Exhaust();
  // Pass Status directly without wrapping in a lambda:
  auto fut =
      OuterCoroAwaitingInner(alloc_).MakeFallible(Status::ResourceExhausted());

  static_assert(std::is_same_v<decltype(fut)::value_type, Status>);

  DispatcherForTest dispatcher;
  Poll<Status> result = dispatcher.RunInTaskUntilStalled(fut);
  EXPECT_TRUE(result.IsReady());
  EXPECT_EQ(*result, Status::ResourceExhausted());
  EXPECT_FALSE(fut.is_pendable());
  EXPECT_TRUE(fut.is_complete());
}

TEST_F(CoroTest, MakeFallibleResultFallbackDirectStatus) {
  alloc_.Exhaust();
  // Pass Status to Coro<Result<int>>:
  auto fut =
      ImmediatelyReturnsFive(alloc_).MakeFallible(Status::ResourceExhausted());

  static_assert(std::is_same_v<decltype(fut)::value_type, Result<int>>);

  DispatcherForTest dispatcher;
  Poll<Result<int>> result = dispatcher.RunInTaskUntilStalled(fut);
  EXPECT_TRUE(result.IsReady());
  EXPECT_EQ(result->status(), Status::ResourceExhausted());
  EXPECT_FALSE(fut.is_pendable());
  EXPECT_TRUE(fut.is_complete());
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

Coro<Status> OuterWithTrackedObjects(CoroContext cx,
                                     std::optional<Status>& returned_status,
                                     TrackedObject argument,
                                     ObjectState& before_inner,
                                     ObjectState& after_inner) {
  TrackedObject before(before_inner);

  returned_status = co_await InnerCoro(cx);

  TrackedObject after(after_inner);
  co_return OkStatus();
}

TEST_F(CoroTest, NestedAllocFailureDestroysObjectsAndInvokesHandler) {
  std::optional<Status> returned_status;

  ObjectState argument = ObjectState::kUninitialized;
  ObjectState before = ObjectState::kUninitialized;
  ObjectState after = ObjectState::kUninitialized;

  Coro<Status> outer_coro = OuterWithTrackedObjects(
      alloc_, returned_status, TrackedObject(argument), before, after);
  ASSERT_TRUE(outer_coro.is_pendable());

  alloc_.Exhaust();  // Prevent allocation of InnerCoro.

  bool error_handler_ran = false;
  auto fut = std::move(outer_coro).MakeFallible([&] {
    error_handler_ran = true;
    return Status::ResourceExhausted();
  });

  DispatcherForTest dispatcher;
  Poll<Status> result = dispatcher.RunInTaskUntilStalled(fut);

  EXPECT_FALSE(returned_status.has_value());
  EXPECT_EQ(argument, ObjectState::kDestroyed);
  EXPECT_EQ(before, ObjectState::kDestroyed);
  EXPECT_EQ(after, ObjectState::kUninitialized);
  EXPECT_TRUE(error_handler_ran);
  EXPECT_TRUE(result.IsReady());
  EXPECT_EQ(*result, Status::ResourceExhausted());
  EXPECT_FALSE(fut.is_pendable());
  EXPECT_TRUE(fut.is_complete());
}

PW_NO_INLINE Coro<int> DoubleValue(CoroContext, int val) { co_return val * 2; }

Coro<int> NestedWithSuspend(CoroContext cx, ValueProvider<int>& provider) {
  int a = co_await provider.Get();
  co_return a + co_await DoubleValue(cx, a);
}

TEST_F(CoroTest, AllocFailureAfterSuspend) {
  ValueProvider<int> provider;
  bool handler_ran = false;
  auto fut = NestedWithSuspend(alloc_, provider).MakeFallible([&] {
    handler_ran = true;
    return -1;
  });

  DispatcherForTest dispatcher;
  FutureTask task(std::move(fut));
  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  alloc_.Exhaust();
  provider.Resolve(5);
  dispatcher.RunToCompletion();

  EXPECT_TRUE(handler_ran);
  EXPECT_EQ(task.Wait(), -1);
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

}  // namespace
