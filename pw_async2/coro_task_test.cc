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

#include <concepts>

#include "pw_allocator/testing.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/future_task.h"
#include "pw_preprocessor/compiler.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

namespace {

using namespace pw::async2;

class FutureTaskCoroTest : public ::testing::Test {
 protected:
  FutureTaskCoroTest() : coro_cx_(alloc_) {}

  pw::allocator::test::AllocatorForTest<2048> alloc_;
  CoroContext coro_cx_;
};

// Use PW_NO_INLINE to prevent the compiler from optimizing the coroutine onto
// the stack, ensuring dynamic allocation so allocation failure can be tested.
template <typename T>
  requires std::integral<T> || std::floating_point<T>
PW_NO_INLINE Coro<T> DoubleIt(CoroContext, T value) {
  co_return value * 2;
}

TEST_F(FutureTaskCoroTest, RunOnce) {
  DispatcherForTest dispatcher;

  FutureTask task(DoubleIt(coro_cx_, 2.5f));
  dispatcher.Post(task);

  dispatcher.RunToCompletion();

  EXPECT_TRUE(task.has_value());
  EXPECT_EQ(task.value(), 5.f);
  EXPECT_EQ(task.Wait(), 5.f);
}

TEST_F(FutureTaskCoroTest, RunOnceDiscard) {
  DispatcherForTest dispatcher;

  FutureTask<Coro<int>, ReturnValuePolicy::kDiscard> task(
      DoubleIt(coro_cx_, 1));
  dispatcher.Post(task);

  dispatcher.RunToCompletion();
}

Coro<pw::Result<int>> ReturnInt(CoroContext, int val) { co_return val; }

TEST_F(FutureTaskCoroTest, RunOnceInt) {
  DispatcherForTest dispatcher;

  FutureTask task(ReturnInt(coro_cx_, 42));
  dispatcher.Post(task);

  dispatcher.RunToCompletion();

  EXPECT_TRUE(task.has_value());
  ASSERT_TRUE(task.value().ok());
  EXPECT_EQ(task.value().value(), 42);
}

TEST_F(FutureTaskCoroTest, InvalidTaskIfAllocationFails) {
  alloc_.Exhaust();
  Coro<int> coro = DoubleIt(coro_cx_, 100);
  EXPECT_FALSE(coro.is_pendable());

  DispatcherForTest dispatcher;
  FutureTask task(std::move(coro));
  dispatcher.Post(task);
  EXPECT_DEATH_IF_SUPPORTED(dispatcher.RunToCompletion(), "");
}

TEST_F(FutureTaskCoroTest, ValidTaskIfAllocationSucceeds) {
  {
    Coro<int> coro = DoubleIt(coro_cx_, 100);
    EXPECT_TRUE(coro.is_pendable());
    FutureTask task(std::move(coro));
  }
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

Coro<void> ReturnVoid(CoroContext) { co_return; }

TEST_F(FutureTaskCoroTest, RunOnceVoid) {
  DispatcherForTest dispatcher;

  FutureTask task(ReturnVoid(coro_cx_));
  dispatcher.Post(task);

  dispatcher.RunToCompletion();
}

TEST_F(FutureTaskCoroTest, DefaultConstructAndAssignCoro) {
  DispatcherForTest dispatcher;

  FutureTask<Coro<int>> task;
  EXPECT_FALSE(task.is_pendable());
  EXPECT_FALSE(task.is_complete());

  task = DoubleIt(coro_cx_, 10);
  EXPECT_TRUE(task.is_pendable());
  EXPECT_FALSE(task.is_complete());

  dispatcher.Post(task);
  dispatcher.RunToCompletion();

  EXPECT_FALSE(task.is_pendable());
  EXPECT_TRUE(task.is_complete());
  EXPECT_EQ(task.Wait(), 20);

  task.emplace_future(DoubleIt(coro_cx_, 21));
  EXPECT_TRUE(task.is_pendable());
  EXPECT_FALSE(task.is_complete());
  EXPECT_FALSE(task.has_value());

  dispatcher.Post(task);
  dispatcher.RunToCompletion();

  EXPECT_TRUE(task.is_complete());
  EXPECT_EQ(task.Wait(), 42);

  task.reset();
  EXPECT_FALSE(task.is_pendable());
  EXPECT_FALSE(task.is_complete());
  EXPECT_FALSE(task.has_value());
}

TEST_F(FutureTaskCoroTest, DefaultConstructAndAssignFallibleCoro) {
  DispatcherForTest dispatcher;

  FutureTask<FallibleCoro<int>> task;
  EXPECT_FALSE(task.is_pendable());
  EXPECT_FALSE(task.is_complete());

  bool error_handler_ran = false;
  task = DoubleIt(coro_cx_, 15).MakeFallible([&] { error_handler_ran = true; });
  EXPECT_TRUE(task.is_pendable());

  dispatcher.Post(task);
  dispatcher.RunToCompletion();

  EXPECT_FALSE(error_handler_ran);
  EXPECT_EQ(task.Wait(), 30);

  // emplace_future with Coro and error handler directly.
  alloc_.Exhaust();
  task.emplace_future(DoubleIt(coro_cx_, 15),
                      [&] { error_handler_ran = true; });
  EXPECT_TRUE(task.is_pendable());

  dispatcher.Post(task);
  dispatcher.RunToCompletion();

  EXPECT_TRUE(error_handler_ran);
  EXPECT_EQ(task.Wait(), std::nullopt);
}

}  // namespace
