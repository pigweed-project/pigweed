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

#include "pw_async2/select.h"

#include <variant>

#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/poll.h"
#include "pw_async2/value_future.h"
#include "pw_unit_test/constexpr.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::async2::BroadcastValueProvider;
using ::pw::async2::Context;
using ::pw::async2::DispatcherForTest;
using ::pw::async2::Pending;
using ::pw::async2::Poll;
using ::pw::async2::Select;
using ::pw::async2::SelectFuture;
using ::pw::async2::ValueFuture;

struct LiteralFuture {
  using value_type = int;
  constexpr LiteralFuture() = default;
  constexpr bool is_complete() const { return false; }
  constexpr pw::async2::Poll<int> Pend(pw::async2::Context&) {
    return pw::async2::Pending();
  }
};

static_assert(pw::async2::Future<
              SelectFuture<LiteralFuture, LiteralFuture, LiteralFuture>>);
static_assert(
    pw::async2::Future<
        SelectFuture<ValueFuture<int>, ValueFuture<int>, ValueFuture<char>>>);

PW_CONSTEXPR_TEST(SelectFuture, DefaultConstruct, {
  SelectFuture<LiteralFuture, LiteralFuture, LiteralFuture> future;
  PW_TEST_EXPECT_FALSE(future.is_pendable());
  PW_TEST_EXPECT_FALSE(future.is_complete());
});

TEST(SelectFuture, Pend_OneReady) {
  DispatcherForTest dispatcher;

  BroadcastValueProvider<int> int_provider;
  BroadcastValueProvider<char> char_provider;

  auto future = Select(int_provider.Get(), char_provider.Get());

  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(future), Pending());

  char_provider.Resolve('y');
  auto result = dispatcher.RunInTaskUntilStalled(future);
  ASSERT_TRUE(result.IsReady());

  EXPECT_FALSE(result->has_value<0>());
  ASSERT_TRUE(result->has_value<1>());
  EXPECT_EQ(result->value<1>(), 'y');

  EXPECT_TRUE(future.is_complete());
}

TEST(SelectFuture, Pend_MultipleReady) {
  DispatcherForTest dispatcher;

  BroadcastValueProvider<int> int_provider;
  BroadcastValueProvider<char> char_provider;
  BroadcastValueProvider<bool> bool_provider;

  auto future =
      Select(int_provider.Get(), char_provider.Get(), bool_provider.Get());

  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(future), Pending());

  char_provider.Resolve('y');
  bool_provider.Resolve(false);

  auto result = dispatcher.RunInTaskUntilStalled(future);
  ASSERT_TRUE(result.IsReady());

  EXPECT_FALSE(result->has_value<0>());
  ASSERT_TRUE(result->has_value<1>());
  ASSERT_TRUE(result->has_value<2>());

  EXPECT_EQ(result->value<1>(), 'y');
  EXPECT_EQ(result->value<2>(), false);

  EXPECT_TRUE(future.is_complete());
}

class SimpleFuture {
 public:
  using value_type = int;

  SimpleFuture() = default;

  SimpleFuture(SimpleFuture&& other) = default;
  SimpleFuture& operator=(SimpleFuture&& other) = default;

  ~SimpleFuture() { core_.Unlist(); }

  pw::async2::Poll<int> Pend(pw::async2::Context& cx) {
    return core_.DoPend(*this, cx);
  }

  bool is_pendable() const { return core_.is_pendable(); }

  bool is_complete() const { return core_.is_complete(); }

 private:
  friend class SimpleFutureProvider;
  friend class pw::async2::FutureCore;

  static constexpr char kWaitReason[] = "SimpleFuture";

  explicit SimpleFuture(pw::async2::FutureState::Pending)
      : core_(pw::async2::FutureState::kPending) {}

  pw::async2::Poll<int> DoPend(pw::async2::Context&) {
    if (ready_) {
      return 1;
    }
    return pw::async2::Pending();
  }

  void Complete() {
    ready_ = true;
    core_.WakeAndMarkReady();
  }

  bool ready_ = false;
  pw::async2::FutureCore core_;
};

class SimpleFutureProvider {
 public:
  SimpleFuture Get() {
    SimpleFuture future(pw::async2::FutureState::kPending);
    futures_.Push(future);
    return future;
  }

  bool has_active_futures() const { return !futures_.empty(); }

  void ResolveOne() {
    if (SimpleFuture* future = futures_.PopIfAvailable()) {
      future->Complete();
    }
  }

 private:
  pw::async2::FutureList<&SimpleFuture::core_> futures_;
};

TEST(SelectFuture, DestroysFuturesOnCompletion) {
  DispatcherForTest dispatcher;
  SimpleFutureProvider provider1;
  SimpleFutureProvider provider2;
  EXPECT_FALSE(provider1.has_active_futures());
  EXPECT_FALSE(provider2.has_active_futures());

  auto future = Select(provider1.Get(), provider2.Get());
  EXPECT_TRUE(provider1.has_active_futures());
  EXPECT_TRUE(provider2.has_active_futures());

  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(future), Pending());

  provider2.ResolveOne();

  auto result = dispatcher.RunInTaskUntilStalled(future);
  ASSERT_TRUE(result.IsReady());
  EXPECT_FALSE(result->has_value<0>());
  EXPECT_TRUE(result->has_value<1>());

  // Even though only one future was resolved, both should be destroyed.
  EXPECT_FALSE(provider1.has_active_futures());
  EXPECT_FALSE(provider2.has_active_futures());
}

}  // namespace
