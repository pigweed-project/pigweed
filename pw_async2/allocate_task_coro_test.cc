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
#include "pw_async2/fallible_coro_task.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::allocator::test::AllocatorForTest;
using ::pw::async2::Coro;
using ::pw::async2::CoroContext;
using ::pw::async2::CoroTask;
using ::pw::async2::DispatcherForTest;
using ::pw::async2::FallibleCoroTask;

Coro<int> SimpleCoro(CoroContext, int value) { co_return value; }

class AllocateTaskCoroTest : public ::testing::Test {
 protected:
  AllocatorForTest<1024> alloc_;
  DispatcherForTest dispatcher_;
};

TEST_F(AllocateTaskCoroTest, AllocatesWithRvalueCoro) {
  auto task = dispatcher_.Post(alloc_, SimpleCoro(alloc_, 42));
  ASSERT_NE(task, nullptr);

  dispatcher_.RunToCompletion();
  EXPECT_EQ(task->value(), 42);
}

TEST_F(AllocateTaskCoroTest, AllocatesWithCoroFunc) {
  auto task = dispatcher_.Post<SimpleCoro>(alloc_, 123);
  ASSERT_NE(task, nullptr);

  dispatcher_.RunToCompletion();
  EXPECT_EQ(task->value(), 123);
}

// Test with move-only argument to ensure forwarding works
struct MoveOnly {
  MoveOnly(int v) : val(v) {}
  MoveOnly(MoveOnly&&) = default;
  MoveOnly(const MoveOnly&) = delete;
  int val;
};

Coro<int> MoveOnlyCoro(CoroContext, MoveOnly m) { co_return m.val; }

TEST_F(AllocateTaskCoroTest, ForwardsArguments) {
  auto task = dispatcher_.Post<MoveOnlyCoro>(alloc_, MoveOnly(456));
  ASSERT_NE(task, nullptr);

  dispatcher_.RunToCompletion();
  EXPECT_EQ(task->value(), 456);
}

TEST_F(AllocateTaskCoroTest, AllocationFailure) {
  alloc_.Exhaust();
  EXPECT_EQ(dispatcher_.Post<SimpleCoro>(alloc_, 0), nullptr);
}

TEST_F(AllocateTaskCoroTest, AllocateAsSharedPtr) {
  auto task = alloc_.MakeShared<CoroTask<int>>(SimpleCoro(alloc_, 42));
  ASSERT_NE(task, nullptr);

  dispatcher_.PostShared(task);
  dispatcher_.RunToCompletion();

  EXPECT_EQ(task->value(), 42);
}

TEST_F(AllocateTaskCoroTest, AllocateFallibleAsSharedPtr) {
  auto task = alloc_.MakeShared<FallibleCoroTask<int>>(SimpleCoro(alloc_, 42),
                                                       [] { FAIL(); });
  ASSERT_NE(task, nullptr);

  dispatcher_.PostShared(task);
  dispatcher_.RunToCompletion();

  EXPECT_EQ(task->value(), 42);
}

TEST_F(AllocateTaskCoroTest, AllocateFallibleCoroTaskAsTask) {
  auto task = dispatcher_.Post<FallibleCoroTask<int>>(
      alloc_, SimpleCoro(alloc_, 42), [] { FAIL(); });
  ASSERT_NE(task, nullptr);

  dispatcher_.RunToCompletion();

  EXPECT_EQ(task->value(), 42);
}

Coro<int> NestedCoroutineInvocations(CoroContext cx, int value) {
  if (value == 0) {
    co_return 0;
  }

  co_return value + co_await NestedCoroutineInvocations(cx, value - 1);
}

TEST_F(AllocateTaskCoroTest, AllocatesFallibleWithCoroRvalueAndErrorHandler) {
  struct FailIfCalled {
    void operator()() { FAIL(); }
  } fail_if_called;

  static_assert(
      std::is_same_v<
          decltype(dispatcher_.Post(
              alloc_, NestedCoroutineInvocations(alloc_, 2), fail_if_called)),
          pw::SharedPtr<FallibleCoroTask<int, FailIfCalled>>>,
      "Error handler should be decayed unless explicitly specified");

  static_assert(
      std::is_same_v<
          decltype(dispatcher_.Post<int, FailIfCalled&>(
              alloc_, NestedCoroutineInvocations(alloc_, 3), fail_if_called)),
          pw::SharedPtr<FallibleCoroTask<int, FailIfCalled&>>>,
      "Error handler should not be decayed when explicitly specified");

  auto task = dispatcher_.Post(
      alloc_, NestedCoroutineInvocations(alloc_, 3), fail_if_called);
  ASSERT_NE(task, nullptr);

  dispatcher_.RunToCompletion();

  EXPECT_EQ(task->value(), 6);
}

TEST_F(AllocateTaskCoroTest, FallibleCoroTaskNestedCoroutineAllocationFailure) {
  bool handler_ran = false;
  auto task = dispatcher_.Post(alloc_,
                               NestedCoroutineInvocations(alloc_, 42),
                               [&handler_ran] { handler_ran = true; });
  ASSERT_NE(task, nullptr);

  alloc_.Exhaust();
  dispatcher_.RunToCompletion();

  EXPECT_TRUE(handler_ran);
  EXPECT_FALSE(task->has_value());
}

struct MemberCoroClass {
  int value;
  Coro<int> MemberCoro(CoroContext, int v) { co_return value + v; }
};

TEST_F(AllocateTaskCoroTest, AllocatesWithMemberCoro) {
  MemberCoroClass obj{100};

  auto task = dispatcher_.Post<&MemberCoroClass::MemberCoro>(alloc_, obj, 42);
  ASSERT_NE(task, nullptr);

  dispatcher_.RunToCompletion();
  EXPECT_EQ(task->value(), 142);
}

TEST_F(AllocateTaskCoroTest, AllocatesWithMemberCoroPointer) {
  MemberCoroClass obj{200};

  auto task = dispatcher_.Post<&MemberCoroClass::MemberCoro>(alloc_, &obj, 42);
  ASSERT_NE(task, nullptr);

  dispatcher_.RunToCompletion();
  EXPECT_EQ(task->value(), 242);
}

TEST_F(AllocateTaskCoroTest, AllocatesByCallingMemberCoro) {
  MemberCoroClass obj{300};

  auto task = dispatcher_.Post(alloc_, obj.MemberCoro(alloc_, 42));
  ASSERT_NE(task, nullptr);

  dispatcher_.RunToCompletion();
  EXPECT_EQ(task->value(), 342);
}

}  // namespace
