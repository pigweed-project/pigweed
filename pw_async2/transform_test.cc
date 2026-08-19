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

#include "pw_async2/transform.h"

#include "pw_async2/channel.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/future.h"
#include "pw_async2/poll.h"
#include "pw_async2/value_future.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::async2::ChannelStorage;
using ::pw::async2::CreateSpscChannel;
using ::pw::async2::DispatcherForTest;
using ::pw::async2::Ready;
using ::pw::async2::ValueFuture;
using ::pw::async2::ValueProvider;
using ::pw::async2::experimental::Map;
using ::pw::async2::experimental::Then;

struct MoveOnly {
  MoveOnly(int val) : value(val) {}
  MoveOnly(const MoveOnly&) = delete;
  MoveOnly& operator=(const MoveOnly&) = delete;
  MoveOnly(MoveOnly&&) = default;
  MoveOnly& operator=(MoveOnly&&) = default;
  int value;
};

TEST(FuturePipe, Map) {
  DispatcherForTest dispatcher;
  ValueProvider<int> provider;

  auto future = provider.Get() | Map([](int x) { return x * 2; });
  static_assert(std::is_same_v<decltype(future)::value_type, int>);
  EXPECT_TRUE(future.is_pendable());
  EXPECT_FALSE(future.is_complete());

  provider.Resolve(10);
  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(future), Ready(20));

  EXPECT_FALSE(future.is_pendable());
  EXPECT_TRUE(future.is_complete());
}

TEST(FuturePipe, MapMoveOnly) {
  DispatcherForTest dispatcher;
  ValueProvider<MoveOnly> provider;

  auto future = provider.Get() | Map([](MoveOnly x) { return x.value * 2; });
  static_assert(std::is_same_v<decltype(future)::value_type, int>);
  EXPECT_TRUE(future.is_pendable());

  provider.Resolve(MoveOnly(10));
  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(future), Ready(20));
}

TEST(FuturePipe, MapChain) {
  DispatcherForTest dispatcher;
  ValueProvider<int> provider;

  // clang-format off
  auto future = provider.Get()
              | Map([](int x) { return x + 5; })
              | Map([](int x) { return static_cast<char>(x * 5); });
  // clang-format on
  static_assert(std::is_same_v<decltype(future)::value_type, char>);

  EXPECT_TRUE(future.is_pendable());
  EXPECT_FALSE(future.is_complete());

  provider.Resolve(10);
  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(future), Ready('K'));
}

TEST(FuturePipe, Then) {
  DispatcherForTest dispatcher;
  ValueProvider<int> provider;
  ChannelStorage<int, 1> storage;
  auto [handle, sender, receiver] = CreateSpscChannel(storage);

  auto future = provider.Get() | Then([s = std::move(sender)](int x) mutable {
                  return s.Send(x * 10);
                });
  static_assert(std::is_same_v<decltype(future)::value_type, bool>);

  EXPECT_TRUE(future.is_pendable());
  EXPECT_FALSE(future.is_complete());

  provider.Resolve(5);
  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(future), Ready(true));

  pw::Result<int> received = receiver.TryReceive();
  EXPECT_TRUE(received.ok());
  EXPECT_EQ(*received, 50);

  EXPECT_FALSE(future.is_pendable());
  EXPECT_TRUE(future.is_complete());
}

TEST(FuturePipe, ThenMoveOnly) {
  DispatcherForTest dispatcher;
  ValueProvider<MoveOnly> provider;
  ChannelStorage<int, 1> storage;
  auto [handle, sender, receiver] = CreateSpscChannel(storage);

  auto future =
      provider.Get() | Then([s = std::move(sender)](MoveOnly x) mutable {
        return s.Send(x.value * 10);
      });
  static_assert(std::is_same_v<decltype(future)::value_type, bool>);

  provider.Resolve(MoveOnly(5));
  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(future), Ready(true));

  pw::Result<int> received = receiver.TryReceive();
  EXPECT_TRUE(received.ok());
  EXPECT_EQ(*received, 50);
}

TEST(FuturePipe, MapVoidFuture) {
  DispatcherForTest dispatcher;
  ValueProvider<void> provider;

  auto future = provider.Get() | Map([]() { return 42; });
  static_assert(std::is_same_v<typename decltype(future)::value_type, int>);
  EXPECT_TRUE(future.is_pendable());
  EXPECT_FALSE(future.is_complete());

  provider.Resolve();
  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(future), Ready(42));

  EXPECT_FALSE(future.is_pendable());
  EXPECT_TRUE(future.is_complete());
}

TEST(FuturePipe, MapToVoid) {
  DispatcherForTest dispatcher;
  ValueProvider<int> provider;
  int output = 0;

  auto future = provider.Get() | Map([&output](int x) { output = x; });
  static_assert(std::is_same_v<typename decltype(future)::value_type, void>);
  EXPECT_TRUE(future.is_pendable());
  EXPECT_FALSE(future.is_complete());

  provider.Resolve(42);
  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(future), Ready());
  EXPECT_EQ(output, 42);

  EXPECT_FALSE(future.is_pendable());
  EXPECT_TRUE(future.is_complete());
}

TEST(FuturePipe, MapVoidToVoid) {
  DispatcherForTest dispatcher;
  ValueProvider<void> provider;
  bool executed = false;

  auto future = provider.Get() | Map([&executed]() { executed = true; });
  static_assert(std::is_same_v<typename decltype(future)::value_type, void>);
  EXPECT_TRUE(future.is_pendable());
  EXPECT_FALSE(future.is_complete());

  provider.Resolve();
  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(future), Ready());
  EXPECT_TRUE(executed);

  EXPECT_FALSE(future.is_pendable());
  EXPECT_TRUE(future.is_complete());
}

TEST(FuturePipe, ThenVoidFuture) {
  DispatcherForTest dispatcher;
  ValueProvider<void> provider1;
  ValueProvider<int> provider2;

  auto future = provider1.Get() | Then([&]() { return provider2.Get(); });
  static_assert(std::is_same_v<typename decltype(future)::value_type, int>);
  EXPECT_TRUE(future.is_pendable());
  EXPECT_FALSE(future.is_complete());

  provider1.Resolve();
  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(future), ::pw::async2::Pending());

  provider2.Resolve(123);
  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(future), Ready(123));

  EXPECT_FALSE(future.is_pendable());
  EXPECT_TRUE(future.is_complete());
}

TEST(FuturePipe, ThenToVoid) {
  DispatcherForTest dispatcher;
  ValueProvider<int> provider1;
  ValueProvider<void> provider2;
  int output = 0;

  auto future = provider1.Get() | Then([&](int x) {
                  output = x;
                  return provider2.Get();
                });
  static_assert(std::is_same_v<typename decltype(future)::value_type, void>);
  EXPECT_TRUE(future.is_pendable());
  EXPECT_FALSE(future.is_complete());

  provider1.Resolve(42);
  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(future), ::pw::async2::Pending());
  EXPECT_EQ(output, 42);

  provider2.Resolve();
  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(future), Ready());

  EXPECT_FALSE(future.is_pendable());
  EXPECT_TRUE(future.is_complete());
}

TEST(FuturePipe, ThenVoidToVoid) {
  DispatcherForTest dispatcher;
  ValueProvider<void> provider1;
  ValueProvider<void> provider2;
  bool executed = false;

  auto future = provider1.Get() | Then([&]() {
                  executed = true;
                  return provider2.Get();
                });
  static_assert(std::is_same_v<typename decltype(future)::value_type, void>);
  EXPECT_TRUE(future.is_pendable());
  EXPECT_FALSE(future.is_complete());

  provider1.Resolve();
  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(future), ::pw::async2::Pending());
  EXPECT_TRUE(executed);

  provider2.Resolve();
  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(future), Ready());

  EXPECT_FALSE(future.is_pendable());
  EXPECT_TRUE(future.is_complete());
}

TEST(FuturePipe, DefaultConstruct) {
  auto map_func = [](int x) { return x; };
  using MapFutureType =
      decltype(std::declval<ValueFuture<int>>() | Map(map_func));
  MapFutureType map_future;
  EXPECT_FALSE(map_future.is_pendable());
  EXPECT_FALSE(map_future.is_complete());

  auto then_func = [](int) { return ValueFuture<int>(); };
  using ThenFutureType =
      decltype(std::declval<ValueFuture<int>>() | Then(then_func));
  ThenFutureType then_future;
  EXPECT_FALSE(then_future.is_pendable());
  EXPECT_FALSE(then_future.is_complete());
}

}  // namespace
