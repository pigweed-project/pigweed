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

#include "pw_async2/callback_task.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/value_future.h"
#include "pw_thread/sleep.h"
#include "pw_thread/test_thread_context.h"
#include "pw_thread/thread.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::async2::BroadcastValueProvider;
using pw::async2::CallbackTask;
using pw::async2::DispatcherForTest;
using pw::async2::OptionalBroadcastValueProvider;
using pw::async2::OptionalValueProvider;
using pw::async2::ValueProvider;

using namespace std::chrono_literals;

TEST(ValueFuture, ResolveFromOtherThread) {
  DispatcherForTest dispatcher;
  ValueProvider<int> provider;

  std::optional<int> result;
  CallbackTask task([&](int value) { result = value; }, provider.Get());
  dispatcher.Post(task);

  pw::thread::test::TestThreadContext context;
  pw::Thread resolver_thread(context.options(), [&provider]() {
    pw::this_thread::sleep_for(1ms);
    provider.Resolve(42);
  });

  dispatcher.AllowBlocking();
  dispatcher.RunToCompletion();
  resolver_thread.join();

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 42);
}

TEST(ValueFuture, Broadcast_ResolveFromOtherThread) {
  DispatcherForTest dispatcher;
  BroadcastValueProvider<int> provider;

  std::optional<int> result1;
  CallbackTask task1([&](int value) { result1 = value; }, provider.Get());
  std::optional<int> result2;
  CallbackTask task2([&](int value) { result2 = value; }, provider.Get());

  dispatcher.Post(task1);
  dispatcher.Post(task2);

  pw::thread::test::TestThreadContext context;
  pw::Thread resolver_thread(context.options(), [&provider]() {
    pw::this_thread::sleep_for(1ms);
    provider.Resolve(123);
  });

  dispatcher.AllowBlocking();
  dispatcher.RunToCompletion();
  resolver_thread.join();

  ASSERT_TRUE(result1.has_value());
  EXPECT_EQ(*result1, 123);
  ASSERT_TRUE(result2.has_value());
  EXPECT_EQ(*result2, 123);
}

TEST(ValueFuture, Void_ResolveFromOtherThread) {
  DispatcherForTest dispatcher;
  ValueProvider<void> provider;

  bool completed = false;
  CallbackTask task([&]() { completed = true; }, provider.Get());
  dispatcher.Post(task);

  pw::thread::test::TestThreadContext context;
  pw::Thread resolver_thread(context.options(), [&provider]() {
    pw::this_thread::sleep_for(1ms);
    provider.Resolve();
  });

  dispatcher.AllowBlocking();
  dispatcher.RunToCompletion();
  resolver_thread.join();

  EXPECT_TRUE(completed);
}

TEST(OptionalValueProvider, ResolveFromOtherThread) {
  DispatcherForTest dispatcher;
  OptionalValueProvider<int> provider;

  std::optional<int> result;
  CallbackTask task([&](std::optional<int> value) { result = value; },
                    provider.Get());
  dispatcher.Post(task);

  pw::thread::test::TestThreadContext context;
  pw::Thread resolver_thread(context.options(), [&provider]() {
    pw::this_thread::sleep_for(1ms);
    provider.Resolve(42);
  });

  dispatcher.AllowBlocking();
  dispatcher.RunToCompletion();
  resolver_thread.join();

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 42);
}

TEST(OptionalValueProvider, CancelFromOtherThread) {
  DispatcherForTest dispatcher;
  OptionalValueProvider<int> provider;

  std::optional<int> result = 5;
  CallbackTask task([&](std::optional<int> value) { result = value; },
                    provider.Get());
  dispatcher.Post(task);

  pw::thread::test::TestThreadContext context;
  pw::Thread resolver_thread(context.options(), [&provider]() {
    pw::this_thread::sleep_for(1ms);
    provider.Cancel();
  });

  dispatcher.AllowBlocking();
  dispatcher.RunToCompletion();
  resolver_thread.join();

  EXPECT_FALSE(result.has_value());
}

TEST(ValueProvider, MoveToThread) {
  DispatcherForTest dispatcher;
  ValueProvider<int> provider;

  std::optional<int> result;
  CallbackTask task([&](int value) { result = value; }, provider.Get());
  dispatcher.Post(task);

  // Move the provider into the thread lambda.
  pw::thread::test::TestThreadContext context;
  pw::Thread resolver_thread(context.options(),
                             [moved_provider = std::move(provider)]() mutable {
                               pw::this_thread::sleep_for(1ms);
                               moved_provider.Resolve(42);
                             });

  dispatcher.AllowBlocking();
  dispatcher.RunToCompletion();
  resolver_thread.join();

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 42);
}

TEST(OptionalBroadcastValueProvider, ResolveFromOtherThread) {
  DispatcherForTest dispatcher;
  OptionalBroadcastValueProvider<int> provider;

  std::optional<int> result1;
  CallbackTask task1([&](std::optional<int> value) { result1 = value; },
                     provider.Get());

  std::optional<int> result2;
  CallbackTask task2([&](std::optional<int> value) { result2 = value; },
                     provider.Get());

  dispatcher.Post(task1);
  dispatcher.Post(task2);

  pw::thread::test::TestThreadContext context;
  pw::Thread resolver_thread(context.options(), [&provider]() {
    pw::this_thread::sleep_for(1ms);
    provider.Resolve(42);
  });

  dispatcher.AllowBlocking();
  dispatcher.RunToCompletion();
  resolver_thread.join();

  ASSERT_TRUE(result1.has_value());
  EXPECT_EQ(*result1, 42);

  ASSERT_TRUE(result2.has_value());
  EXPECT_EQ(*result2, 42);
}

TEST(OptionalBroadcastValueProvider, CancelFromOtherThread) {
  DispatcherForTest dispatcher;
  OptionalBroadcastValueProvider<int> provider;

  std::optional<int> result1 = 5;
  CallbackTask task1([&](std::optional<int> value) { result1 = value; },
                     provider.Get());

  std::optional<int> result2 = 5;
  CallbackTask task2([&](std::optional<int> value) { result2 = value; },
                     provider.Get());

  dispatcher.Post(task1);
  dispatcher.Post(task2);

  pw::thread::test::TestThreadContext context;
  pw::Thread resolver_thread(context.options(), [&provider]() {
    pw::this_thread::sleep_for(1ms);
    provider.Cancel();
  });

  dispatcher.AllowBlocking();
  dispatcher.RunToCompletion();
  resolver_thread.join();

  EXPECT_FALSE(result1.has_value());

  EXPECT_FALSE(result2.has_value());
}

TEST(OptionalValueProvider, DestructFromOtherThread) {
  DispatcherForTest dispatcher;
  std::optional<OptionalValueProvider<int>> provider;
  provider.emplace();

  std::optional<int> result = 5;  // nullopt after provider is destroyed
  CallbackTask task([&](std::optional<int> value) { result = value; },
                    provider->Get());
  dispatcher.Post(task);

  pw::thread::test::TestThreadContext context;
  pw::Thread resolver_thread(context.options(), [&provider]() {
    pw::this_thread::sleep_for(1ms);
    provider.reset();
  });

  dispatcher.AllowBlocking();
  dispatcher.RunToCompletion();
  resolver_thread.join();

  EXPECT_FALSE(result.has_value());
}

}  // namespace
