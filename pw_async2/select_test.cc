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
#include "pw_async2/func_task.h"
#include "pw_async2/future.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
#include "pw_async2/value_future.h"
#include "pw_compilation_testing/negative_compilation.h"
#include "pw_containers/optional_tuple.h"
#include "pw_unit_test/constexpr.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::OptionalTuple;
using ::pw::async2::BroadcastValueProvider;
using ::pw::async2::Context;
using ::pw::async2::DispatcherForTest;
using ::pw::async2::FuncTask;
using ::pw::async2::FutureState;
using ::pw::async2::Pending;
using ::pw::async2::Poll;
using ::pw::async2::Ready;
using ::pw::async2::Select;
using ::pw::async2::SelectFuture;
using ::pw::async2::Task;
using ::pw::async2::ValueFuture;
using ::pw::async2::ValueProvider;
using ::pw::async2::VoidFuture;

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

TEST(SelectFuture, Pend_VoidFuture) {
  DispatcherForTest dispatcher;

  BroadcastValueProvider<int> int_provider;
  BroadcastValueProvider<void> void_provider;

  auto future = Select(int_provider.Get(), void_provider.Get());

  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(future), Pending());

  void_provider.Resolve();
  auto result = dispatcher.RunInTaskUntilStalled(future);
  ASSERT_TRUE(result.IsReady());

  EXPECT_FALSE(result->has_value<0>());
  EXPECT_TRUE(result->has_value<1>());

  EXPECT_TRUE(future.is_complete());
}

TEST(SelectFuture, Pend_OnlyVoidFutures) {
  DispatcherForTest dispatcher;

  BroadcastValueProvider<void> void_provider1;
  BroadcastValueProvider<void> void_provider2;

  auto future = Select(void_provider1.Get(), void_provider2.Get());

  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(future), Pending());

  void_provider2.Resolve();
  auto result = dispatcher.RunInTaskUntilStalled(future);
  ASSERT_TRUE(result.IsReady());

  EXPECT_FALSE(result->has_value<0>());
  EXPECT_TRUE(result->has_value<1>());

  EXPECT_TRUE(future.is_complete());
}

TEST(PwSelect, LeavesIncompleteFuturesPending) {
  DispatcherForTest dispatcher;
  ValueProvider<int> int_provider;
  ValueProvider<char> char_provider;
  ValueFuture<int> int_future = int_provider.Get();
  ValueFuture<char> char_future = char_provider.Get();

  OptionalTuple<int, char> results;
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_SELECT(results, cx, int_future, char_future);
    return Ready();
  });
  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  char_provider.Resolve('y');
  EXPECT_FALSE(dispatcher.RunUntilStalled());

  EXPECT_FALSE(results.has_value<0>());
  ASSERT_TRUE(results.has_value<1>());
  EXPECT_EQ(results.value<1>(), 'y');

  // Unlike Select, PW_SELECT leaves the futures with their owner. The one that
  // did not complete is still waiting on its provider.
  EXPECT_TRUE(char_future.is_complete());
  EXPECT_TRUE(int_future.is_pendable());
  EXPECT_TRUE(int_provider.has_future());
}

TEST(PwSelect, ReturnsAllCompletedFutures) {
  DispatcherForTest dispatcher;
  ValueProvider<int> int_provider;
  ValueProvider<void> void_provider;
  ValueFuture<int> int_future = int_provider.Get();
  VoidFuture void_future = void_provider.Get();

  int_provider.Resolve(42);
  void_provider.Resolve();

  FuncTask task([&](Context& cx) -> Poll<> {
    PW_SELECT(auto result, cx, int_future, void_future);
    EXPECT_TRUE(result.has_value<0>());
    EXPECT_EQ(result.value<0>(), 42);
    EXPECT_TRUE(result.has_value<1>());
    return Ready();
  });
  dispatcher.Post(task);
  EXPECT_FALSE(dispatcher.RunUntilStalled());
}

TEST(PwSelect, SkipsFuturesThatAreNotPendable) {
  DispatcherForTest dispatcher;
  ValueProvider<int> provider;
  ValueFuture<int> unarmed;  // Never armed, so its branch is disabled.
  ValueFuture<int> armed = provider.Get();

  OptionalTuple<int, int> results;
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_SELECT(results, cx, unarmed, armed);
    return Ready();
  });
  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  provider.Resolve(7);
  EXPECT_FALSE(dispatcher.RunUntilStalled());

  EXPECT_FALSE(results.has_value<0>());
  ASSERT_TRUE(results.has_value<1>());
  EXPECT_EQ(results.value<1>(), 7);
}

// Composite future that owns two child futures and selects over them with
// PW_SELECT. PW_SELECT keeps no references to the child futures after it
// returns, so the composite future may move between polls.
class FirstOfTwoFuture {
 public:
  using value_type = OptionalTuple<int, char>;

  FirstOfTwoFuture() = default;

  FirstOfTwoFuture(ValueFuture<int> first, ValueFuture<char> second)
      : first_(std::move(first)),
        second_(std::move(second)),
        state_(FutureState::kPending) {}

  Poll<value_type> Pend(Context& cx) {
    PW_SELECT(auto result, cx, first_, second_);
    state_.MarkComplete();
    return result;
  }

  [[nodiscard]] bool is_pendable() const { return state_.is_pendable(); }
  [[nodiscard]] bool is_complete() const { return state_.is_complete(); }

  const ValueFuture<int>& first() const { return first_; }

 private:
  ValueFuture<int> first_;
  ValueFuture<char> second_;
  FutureState state_;
};

static_assert(pw::async2::Future<FirstOfTwoFuture>);

TEST(PwSelect, OwnerMayMoveBetweenPolls) {
  DispatcherForTest dispatcher;
  ValueProvider<int> int_provider;
  ValueProvider<char> char_provider;

  FirstOfTwoFuture original(int_provider.Get(), char_provider.Get());
  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(original), Pending());

  // Moving the composite future relocates its child futures.
  FirstOfTwoFuture moved(std::move(original));
  char_provider.Resolve('y');

  auto result = dispatcher.RunInTaskUntilStalled(moved);
  ASSERT_TRUE(result.IsReady());
  EXPECT_FALSE(result->has_value<0>());
  ASSERT_TRUE(result->has_value<1>());
  EXPECT_EQ(result->value<1>(), 'y');

  EXPECT_TRUE(moved.first().is_pendable());
  EXPECT_TRUE(int_provider.has_future());
}

// Event loop in the style of the pw_async2 codelab's vending machine. Waits for
// coins and key presses, re-arming only the future that completed.
class VendingLoopTask final : public Task {
 public:
  VendingLoopTask(ValueProvider<unsigned>& coin_slot,
                  ValueProvider<int>& keypad)
      : coin_slot_(coin_slot), keypad_(keypad) {}

  unsigned coins() const { return coins_; }
  int last_key() const { return last_key_; }
  int coin_futures_created() const { return coin_futures_created_; }
  int key_futures_created() const { return key_futures_created_; }

 private:
  Poll<> DoPend(Context& cx) override {
    while (true) {
      if (!coin_future_.is_pendable()) {
        coin_future_ = coin_slot_.Get();
        coin_futures_created_ += 1;
      }
      if (!key_future_.is_pendable()) {
        key_future_ = keypad_.Get();
        key_futures_created_ += 1;
      }

      PW_SELECT(auto result, cx, coin_future_, key_future_);
      if (result.has_value<0>()) {
        coins_ += result.value<0>();
      }
      if (result.has_value<1>()) {
        last_key_ = result.value<1>();
      }
    }
  }

  ValueProvider<unsigned>& coin_slot_;
  ValueProvider<int>& keypad_;
  ValueFuture<unsigned> coin_future_;
  ValueFuture<int> key_future_;
  unsigned coins_ = 0;
  int last_key_ = 0;
  int coin_futures_created_ = 0;
  int key_futures_created_ = 0;
};

TEST(PwSelect, EventLoopReArmsOnlyTheCompletedFuture) {
  DispatcherForTest dispatcher;
  ValueProvider<unsigned> coin_slot;
  ValueProvider<int> keypad;
  VendingLoopTask task(coin_slot, keypad);
  dispatcher.Post(task);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.coin_futures_created(), 1);
  EXPECT_EQ(task.key_futures_created(), 1);

  // A coin completes the select, but the key press future keeps waiting, so a
  // key press arriving now is not dropped.
  coin_slot.Resolve(2u);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.coins(), 2u);
  EXPECT_EQ(task.coin_futures_created(), 2);
  EXPECT_EQ(task.key_futures_created(), 1);
  EXPECT_TRUE(keypad.has_future());

  keypad.Resolve(3);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.last_key(), 3);
  EXPECT_EQ(task.coin_futures_created(), 2);
  EXPECT_EQ(task.key_futures_created(), 2);

  // When both complete before the task runs, both results are delivered.
  coin_slot.Resolve(1u);
  keypad.Resolve(4);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.coins(), 3u);
  EXPECT_EQ(task.last_key(), 4);

  task.Deregister();
}

#if PW_NC_TEST(PwSelectRejectsTemporaries)
PW_NC_EXPECT_CLANG("no matching function for call to 'PendSelect'");
PW_NC_EXPECT_GCC("cannot bind non-const lvalue reference");

[[maybe_unused]] Poll<> SelectTemporary(Context& cx,
                                        ValueProvider<int>& provider) {
  // A temporary would be recreated every time the function is polled.
  PW_SELECT(auto result, cx, provider.Get());
  return Ready();
}

#elif PW_NC_TEST(PwSelectRejectsConstFutures)
PW_NC_EXPECT("must not be const");

[[maybe_unused]] Poll<> SelectConst(Context& cx,
                                    const ValueFuture<int>& future) {
  PW_SELECT(auto result, cx, future);
  return Ready();
}

#endif  // PW_NC_TEST

}  // namespace
