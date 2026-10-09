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

#include "pw_async2/box.h"

#include "pw_allocator/testing.h"
#include "pw_async2/channel.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/func_task.h"
#include "pw_async2/future.h"
#include "pw_async2/select.h"
#include "pw_async2/transform.h"
#include "pw_async2/try.h"
#include "pw_async2/value_future.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::async2::BoxedFuture;
using pw::async2::ChannelStorage;
using pw::async2::Context;
using pw::async2::CreateSpscChannel;
using pw::async2::DispatcherForTest;
using pw::async2::Pending;
using pw::async2::Poll;
using pw::async2::Ready;
using pw::async2::ValueProvider;
using pw::async2::experimental::Map;
using pw::async2::experimental::Then;

TEST(BoxedFuture, TypeErasesIntFuture) {
  pw::allocator::test::AllocatorForTest<256> alloc;
  ValueProvider<int> provider;

  BoxedFuture<int> boxed = BoxFuture(alloc, provider.Get());
  ASSERT_TRUE(boxed.is_pendable());
  static_assert(pw::async2::Future<decltype(boxed)>);

  EXPECT_TRUE(boxed.is_pendable());
  EXPECT_FALSE(boxed.is_complete());

  pw::async2::DispatcherForTest dispatcher;

  int result = 0;
  pw::async2::FuncTask task([&](Context& cx) -> Poll<> {
    PW_TRY_READY_ASSIGN(result, boxed.Pend(cx));
    return Ready();
  });

  dispatcher.Post(task);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_TRUE(boxed.is_pendable());
  EXPECT_FALSE(boxed.is_complete());
  EXPECT_EQ(result, 0);

  provider.Resolve(42);
  EXPECT_FALSE(dispatcher.RunUntilStalled());

  EXPECT_FALSE(boxed.is_pendable());
  EXPECT_TRUE(boxed.is_complete());
  EXPECT_EQ(result, 42);
  EXPECT_GT(alloc.deallocate_size(), 0u);
}

TEST(BoxedFuture, TypeErasesVoidFuture) {
  pw::allocator::test::AllocatorForTest<256> alloc;
  ValueProvider<void> provider;

  BoxedFuture<void> boxed = BoxFuture(alloc, provider.Get());
  ASSERT_TRUE(boxed.is_pendable());
  static_assert(pw::async2::Future<decltype(boxed)>);

  EXPECT_TRUE(boxed.is_pendable());
  EXPECT_FALSE(boxed.is_complete());

  pw::async2::DispatcherForTest dispatcher;

  pw::async2::FuncTask task([&](Context& cx) -> Poll<> {
    Poll<> p = boxed.Pend(cx);
    if (p.IsPending()) {
      return Pending();
    }
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_TRUE(boxed.is_pendable());
  EXPECT_FALSE(boxed.is_complete());

  provider.Resolve();
  dispatcher.AllowBlocking();
  dispatcher.RunToCompletion();
  EXPECT_FALSE(boxed.is_pendable());
  EXPECT_TRUE(boxed.is_complete());
  EXPECT_GT(alloc.deallocate_size(), 0u);
}

TEST(BoxedFuture, TypeErasesCombinator) {
  pw::allocator::test::AllocatorForTest<256> alloc;
  ValueProvider<int> provider_int;
  ValueProvider<char> provider_char;

  BoxedFuture<pw::OptionalTuple<int, char>> boxed =
      BoxFuture(alloc, Select(provider_int.Get(), provider_char.Get()));
  ASSERT_TRUE(boxed.is_pendable());
  static_assert(pw::async2::Future<decltype(boxed)>);

  pw::async2::DispatcherForTest dispatcher;

  pw::OptionalTuple<int, char> result;
  pw::async2::FuncTask task([&](Context& cx) -> Poll<> {
    PW_TRY_READY_ASSIGN(result, boxed.Pend(cx));
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_TRUE(boxed.is_pendable());
  EXPECT_FALSE(boxed.is_complete());

  provider_int.Resolve(42);
  EXPECT_FALSE(dispatcher.RunUntilStalled());

  EXPECT_FALSE(boxed.is_pendable());
  EXPECT_TRUE(boxed.is_complete());
  EXPECT_TRUE(result.has_value<0>());
  EXPECT_EQ(result.value<0>(), 42);
  EXPECT_GT(alloc.deallocate_size(), 0u);
}

TEST(BoxedFuture, TypeErasesFutureChain) {
  pw::allocator::test::AllocatorForTest<256> alloc;
  DispatcherForTest dispatcher;
  ValueProvider<int> provider;
  ChannelStorage<int, 1> storage;
  auto [handle, sender, receiver] = CreateSpscChannel(storage);

  // clang-format off
  BoxedFuture<bool> future = BoxFuture(
      alloc,
      provider.Get()
      | Map([](int x) { return x * 4; })
      | Then([s = std::move(sender)](int x) { return s.Send(x); }));
  // clang-format on

  EXPECT_TRUE(future.is_pendable());
  EXPECT_FALSE(future.is_complete());

  provider.Resolve(5);
  auto result = dispatcher.RunInTaskUntilStalled(future);
  EXPECT_EQ(result, Ready(true));

  pw::Result<int> received = receiver.TryReceive();
  EXPECT_TRUE(received.ok());
  EXPECT_EQ(*received, 20);

  EXPECT_FALSE(future.is_pendable());
  EXPECT_TRUE(future.is_complete());
}

TEST(BoxedFuture, AllocationFailure) {
  pw::allocator::test::AllocatorForTest<256> alloc;
  alloc.Exhaust();
  ValueProvider<int> provider;

  BoxedFuture<int> boxed = BoxFuture(alloc, provider.Get());
  EXPECT_FALSE(boxed.is_pendable());
  EXPECT_FALSE(boxed.is_complete());
}

TEST(BoxedFuture, DestructorDeallocates) {
  pw::allocator::test::AllocatorForTest<256> alloc;
  ValueProvider<int> provider;

  EXPECT_EQ(alloc.deallocate_size(), 0u);
  {
    BoxedFuture<int> boxed = BoxFuture(alloc, provider.Get());
    EXPECT_TRUE(boxed.is_pendable());
  }
  EXPECT_GT(alloc.deallocate_size(), 0u);
}

TEST(BoxedFuture, MoveAssignmentDeallocatesOriginal) {
  pw::allocator::test::AllocatorForTest<256> alloc;
  ValueProvider<int> provider_1;
  ValueProvider<int> provider_2;

  BoxedFuture<int> boxed_1 = BoxFuture(alloc, provider_1.Get());
  BoxedFuture<int> boxed_2 = BoxFuture(alloc, provider_2.Get());

  EXPECT_TRUE(boxed_1.is_pendable());
  EXPECT_TRUE(boxed_2.is_pendable());
  EXPECT_EQ(alloc.deallocate_size(), 0u);

  boxed_1 = std::move(boxed_2);
  EXPECT_GT(alloc.deallocate_size(), 0u);

  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_FALSE(boxed_2.is_pendable());
  EXPECT_TRUE(boxed_1.is_pendable());
  EXPECT_FALSE(boxed_1.is_complete());

  pw::async2::DispatcherForTest dispatcher;
  int result = 0;
  pw::async2::FuncTask task([&](Context& cx) -> Poll<> {
    PW_TRY_READY_ASSIGN(result, boxed_1.Pend(cx));
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_TRUE(boxed_1.is_pendable());

  provider_2.Resolve(1234);
  EXPECT_FALSE(dispatcher.RunUntilStalled());

  EXPECT_FALSE(boxed_1.is_pendable());
  EXPECT_TRUE(boxed_1.is_complete());
  EXPECT_EQ(result, 1234);
}

}  // namespace
