// Copyright 2025 The Pigweed Authors
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

#include "pw_async2/value_future.h"

#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/func_task.h"
#include "pw_async2/try.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::async2::BroadcastValueProvider;
using pw::async2::Context;
using pw::async2::DispatcherForTest;
using pw::async2::PendFuncTask;
using pw::async2::Poll;
using pw::async2::Ready;
using pw::async2::ValueFuture;
using pw::async2::ValueProvider;
using pw::async2::VoidFuture;

TEST(ValueFuture, Pend) {
  DispatcherForTest dispatcher;
  BroadcastValueProvider<int> provider;

  ValueFuture<int> future = provider.Get();
  int result = -1;

  PendFuncTask task([&](Context& cx) -> Poll<> {
    PW_TRY_READY_ASSIGN(int value, future.Pend(cx));
    result = value;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  provider.Resolve(27);
  dispatcher.RunToCompletion();
  EXPECT_EQ(result, 27);
}

TEST(ValueFuture, Resolved) {
  DispatcherForTest dispatcher;
  auto future = ValueFuture<int>::Resolved(42);
  int result = -1;

  PendFuncTask task([&](Context& cx) -> Poll<> {
    PW_TRY_READY_ASSIGN(int value, future.Pend(cx));
    result = value;
    return Ready();
  });

  dispatcher.Post(task);
  dispatcher.RunToCompletion();
  EXPECT_EQ(result, 42);
}

struct CannotConstruct {
  CannotConstruct() = delete;
};

TEST(ValueFuture, Default) {
  PW_CONSTINIT static ValueFuture<CannotConstruct> future;
  EXPECT_FALSE(future.is_complete());
}

TEST(ValueFuture, ResolvedInPlace) {
  DispatcherForTest dispatcher;
  auto future = ValueFuture<std::pair<int, int>>::Resolved(9, 3);

  std::optional<std::pair<int, int>> result;
  PendFuncTask task([&](Context& cx) -> Poll<> {
    PW_TRY_READY_ASSIGN(auto value, future.Pend(cx));
    result = value;
    return Ready();
  });

  dispatcher.Post(task);
  dispatcher.RunToCompletion();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->first, 9);
  EXPECT_EQ(result->second, 3);
}

TEST(ValueProvider, VendsAndResolvesFuture) {
  DispatcherForTest dispatcher;
  ValueProvider<int> provider;

  ValueFuture<int> future = provider.Get();
  ASSERT_FALSE(future.is_complete());

  int result = -1;
  PendFuncTask task([&](Context& cx) -> Poll<> {
    PW_TRY_READY_ASSIGN(int value, future.Pend(cx));
    result = value;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  provider.Resolve(91);
  dispatcher.RunToCompletion();
  EXPECT_EQ(result, 91);
}

TEST(ValueProvider, OnlyAllowsOneFutureToExist) {
  DispatcherForTest dispatcher;
  ValueProvider<int> provider;

  {
    std::optional<ValueFuture<int>> future1 = provider.TryGet();
    std::optional<ValueFuture<int>> future2 = provider.TryGet();
    EXPECT_TRUE(future1.has_value());
    EXPECT_FALSE(future2.has_value());
  }

  // `future1` went out of scope, so we should be allowed to get a new one.
  ValueFuture<int> future = provider.Get();
  ASSERT_FALSE(future.is_complete());

  int result = -1;
  PendFuncTask task([&](Context& cx) -> Poll<> {
    PW_TRY_READY_ASSIGN(int value, future.Pend(cx));
    result = value;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  provider.Resolve(82);
  dispatcher.RunToCompletion();
  EXPECT_EQ(result, 82);

  // The operation has resolved, so a new future should be obtainable.
  ValueFuture<int> new_future = provider.Get();
  EXPECT_FALSE(new_future.is_complete());
}

TEST(ValueProvider, ResolveInPlace) {
  DispatcherForTest dispatcher;
  ValueProvider<std::pair<int, int>> provider;

  ValueFuture<std::pair<int, int>> future = provider.Get();
  ASSERT_FALSE(future.is_complete());

  std::optional<std::pair<int, int>> result;
  PendFuncTask task([&](Context& cx) -> Poll<> {
    PW_TRY_READY_ASSIGN(auto value, future.Pend(cx));
    result = value;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  provider.Resolve(9, 3);
  dispatcher.RunToCompletion();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->first, 9);
  EXPECT_EQ(result->second, 3);
}

}  // namespace

TEST(VoidFuture, Pend) {
  DispatcherForTest dispatcher;
  BroadcastValueProvider<void> provider;

  VoidFuture future = provider.Get();
  bool completed = false;

  PendFuncTask task([&](Context& cx) -> Poll<> {
    PW_TRY_READY(future.Pend(cx));
    completed = true;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_FALSE(completed);

  provider.Resolve();
  dispatcher.RunToCompletion();
  EXPECT_TRUE(completed);
}

TEST(VoidFuture, Resolved) {
  DispatcherForTest dispatcher;
  auto future = VoidFuture::Resolved();
  bool completed = false;

  PendFuncTask task([&](Context& cx) -> Poll<> {
    PW_TRY_READY(future.Pend(cx));
    completed = true;
    return Ready();
  });

  dispatcher.Post(task);
  dispatcher.RunToCompletion();
  EXPECT_TRUE(completed);
}

TEST(VoidFuture, Default) {
  PW_CONSTINIT static VoidFuture future;
  EXPECT_FALSE(future.is_complete());
}

TEST(ValueProviderVoid, VendsAndResolvesFuture) {
  DispatcherForTest dispatcher;
  ValueProvider<void> provider;

  VoidFuture future = provider.Get();
  ASSERT_FALSE(future.is_complete());

  bool completed = false;
  PendFuncTask task([&](Context& cx) -> Poll<> {
    PW_TRY_READY(future.Pend(cx));
    completed = true;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_FALSE(completed);

  provider.Resolve();
  dispatcher.RunToCompletion();
  EXPECT_TRUE(completed);
}
