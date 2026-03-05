// Copyright 2023 The Pigweed Authors
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

#include "pw_async2/await.h"

#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/func_task.h"
#include "pw_async2/value_future.h"
#include "pw_unit_test/framework.h"

namespace pw::async2 {
namespace {

TEST(Await, VoidFuturePending) {
  DispatcherForTest dispatcher;
  ValueProvider<void> provider;
  VoidFuture f = provider.Get();

  FuncTask task([&](Context& cx) -> Poll<> {
    PW_AWAIT(f, cx);
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_TRUE(task.IsRegistered());

  provider.Resolve();
  EXPECT_FALSE(dispatcher.RunUntilStalled());
  EXPECT_FALSE(task.IsRegistered());
}

TEST(Await, VoidFutureReady) {
  DispatcherForTest dispatcher;
  VoidFuture f = VoidFuture::Resolved();

  FuncTask task([&](Context& cx) -> Poll<> {
    PW_AWAIT(f, cx);
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_FALSE(dispatcher.RunUntilStalled());
  EXPECT_FALSE(task.IsRegistered());
}

TEST(Await, ValueFutureWaitsForValue) {
  DispatcherForTest dispatcher;
  ValueProvider<int> provider;
  ValueFuture<int> f = provider.Get();
  int result = -1;

  FuncTask task([&](Context& cx) -> Poll<> {
    PW_AWAIT(auto v, f, cx);
    result = v;
    return Ready();
  });

  dispatcher.Post(task);

  // Should stall initially.
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(result, -1);

  // Set value and wake.
  provider.Resolve(42);

  // Should complete now.
  EXPECT_FALSE(dispatcher.RunUntilStalled());
  EXPECT_EQ(result, 42);
}

TEST(Await, ValueFutureReadyImmediately) {
  DispatcherForTest dispatcher;
  ValueFuture<int> f = ValueFuture<int>::Resolved(99);
  int result = -1;

  FuncTask task([&](Context& cx) -> Poll<> {
    PW_AWAIT(auto v, f, cx);
    result = v;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_FALSE(dispatcher.RunUntilStalled());
  EXPECT_EQ(result, 99);
}

}  // namespace
}  // namespace pw::async2
