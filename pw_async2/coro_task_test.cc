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

#include "pw_async2/coro_task.h"

#include <concepts>

#include "pw_allocator/testing.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

namespace {

using namespace pw::async2;

class CoroTaskTest : public ::testing::Test {
 protected:
  CoroTaskTest() : coro_cx_(alloc_) {}

  pw::allocator::test::AllocatorForTest<2048> alloc_;
  CoroContext coro_cx_;
};

template <typename T>
  requires std::integral<T> || std::floating_point<T>
Coro<T> DoubleIt(CoroContext&, T value) {
  co_return value * 2;
}

TEST_F(CoroTaskTest, RunOnce) {
  DispatcherForTest dispatcher;

  CoroTask task(DoubleIt(coro_cx_, 2.5f));
  dispatcher.Post(task);

  dispatcher.RunToCompletion();

  EXPECT_TRUE(task.has_value());
  EXPECT_EQ(task.value(), 5.f);
  EXPECT_EQ(task.Wait(), 5.f);
}

TEST_F(CoroTaskTest, RunOnceDiscard) {
  DispatcherForTest dispatcher;

  CoroTask<int, ReturnValuePolicy::kDiscard> task(DoubleIt(coro_cx_, 1));
  dispatcher.Post(task);

  dispatcher.RunToCompletion();
}

Coro<pw::Result<int>> ReturnInt(CoroContext&, int val) { co_return val; }

TEST_F(CoroTaskTest, RunOnceInt) {
  DispatcherForTest dispatcher;

  CoroTask task(ReturnInt(coro_cx_, 42));
  dispatcher.Post(task);

  dispatcher.RunToCompletion();

  EXPECT_TRUE(task.has_value());
  ASSERT_TRUE(task.value().ok());
  EXPECT_EQ(task.value().value(), 42);
}

TEST_F(CoroTaskTest, AllocationFailure) {
  alloc_.Exhaust();
  Coro c = DoubleIt(coro_cx_, 100);
  EXPECT_FALSE(c.IsValid());
}

Coro<void> ReturnVoid(CoroContext&) { co_return; }

TEST_F(CoroTaskTest, RunOnceVoid) {
  DispatcherForTest dispatcher;

  CoroTask task(ReturnVoid(coro_cx_));
  dispatcher.Post(task);

  dispatcher.RunToCompletion();
}

}  // namespace
