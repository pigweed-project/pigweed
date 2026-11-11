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

#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/func_task.h"
#include "pw_async2/future.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
#include "pw_async2/value_future.h"
#include "pw_compilation_testing/negative_compilation.h"
#include "pw_containers/optional_tuple.h"
#include "pw_polyfill/language_feature_macros.h"
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
  constexpr bool is_pendable() const { return true; }
  constexpr bool is_complete() const { return false; }
  constexpr pw::async2::Poll<int> Pend(pw::async2::Context&) {
    return pw::async2::Pending();
  }
};

struct ConstexprDtorFuture {
  using value_type = int;
  constexpr ConstexprDtorFuture() = default;
  ConstexprDtorFuture(ConstexprDtorFuture&&) = default;
  ConstexprDtorFuture& operator=(ConstexprDtorFuture&&) = default;
  PW_CONSTEXPR_CPP20 ~ConstexprDtorFuture() {}
  constexpr bool is_pendable() const { return true; }
  constexpr bool is_complete() const { return false; }
  constexpr pw::async2::Poll<int> Pend(pw::async2::Context&) {
    return pw::async2::Pending();
  }
};

static_assert(pw::async2::Future<
              SelectFuture<LiteralFuture, LiteralFuture, LiteralFuture>>);
static_assert(pw::async2::Future<SelectFuture<ConstexprDtorFuture>>);
static_assert(
    pw::async2::Future<
        SelectFuture<ValueFuture<int>, ValueFuture<int>, ValueFuture<char>>>);
static_assert(std::is_trivially_destructible_v<
              SelectFuture<LiteralFuture, LiteralFuture, LiteralFuture>>);
static_assert(
    !std::is_trivially_destructible_v<SelectFuture<ConstexprDtorFuture>>);
static_assert(
    !std::is_trivially_destructible_v<SelectFuture<ValueFuture<int>>>);
static_assert(std::is_nothrow_move_constructible_v<
              SelectFuture<ValueFuture<int>, ValueFuture<char>>>);
static_assert(std::is_nothrow_move_assignable_v<
              SelectFuture<ValueFuture<int>, ValueFuture<char>>>);
static_assert(!std::is_copy_constructible_v<SelectFuture<LiteralFuture>>);
static_assert(!std::is_copy_assignable_v<SelectFuture<LiteralFuture>>);

template <typename... Futures>
struct ExpectedSelectFutureLayout {
  std::tuple<Futures...> futures;
  FutureState state;
};
static_assert(sizeof(SelectFuture<LiteralFuture, LiteralFuture>) ==
              sizeof(ExpectedSelectFutureLayout<LiteralFuture, LiteralFuture>));
static_assert(
    sizeof(SelectFuture<ValueFuture<int>, ValueFuture<char>>) ==
    sizeof(ExpectedSelectFutureLayout<ValueFuture<int>, ValueFuture<char>>));
static_assert(
    alignof(SelectFuture<ValueFuture<int>, ValueFuture<char>>) ==
    alignof(ExpectedSelectFutureLayout<ValueFuture<int>, ValueFuture<char>>));

PW_CONSTEXPR_TEST(SelectFuture, DefaultConstruct, {
  SelectFuture<LiteralFuture, LiteralFuture, LiteralFuture> future;
  PW_TEST_EXPECT_FALSE(future.is_pendable());
  PW_TEST_EXPECT_FALSE(future.is_complete());
});

PW_CONSTEXPR_TEST(SelectFuture, ConstructFromFutures, {
  auto future = Select(LiteralFuture{}, LiteralFuture{});
  PW_TEST_EXPECT_TRUE(future.is_pendable());
  PW_TEST_EXPECT_FALSE(future.is_complete());
});

PW_CONSTEXPR_TEST_IF_CPP20(SelectFuture, MoveConstructAndAssign, {
  auto first = Select(LiteralFuture{}, LiteralFuture{});
  auto moved = std::move(first);
  PW_TEST_EXPECT_FALSE(first.is_pendable());  // NOLINT(bugprone-use-after-move)
  PW_TEST_EXPECT_TRUE(moved.is_pendable());

  SelectFuture<LiteralFuture, LiteralFuture> assigned;
  assigned = std::move(moved);
  PW_TEST_EXPECT_FALSE(moved.is_pendable());  // NOLINT(bugprone-use-after-move)
  PW_TEST_EXPECT_TRUE(assigned.is_pendable());

  auto replacement = Select(LiteralFuture{}, LiteralFuture{});
  assigned = std::move(replacement);
  PW_TEST_EXPECT_FALSE(
      replacement.is_pendable());  // NOLINT(bugprone-use-after-move)
  PW_TEST_EXPECT_TRUE(assigned.is_pendable());

  auto nested = Select(Select(LiteralFuture{}), LiteralFuture{});
  PW_TEST_EXPECT_TRUE(nested.is_pendable());
});

PW_CONSTEXPR_TEST_IF_CPP20(SelectFuture, NonTrivialConstexprDestructor, {
  SelectFuture<ConstexprDtorFuture> empty;
  PW_TEST_EXPECT_FALSE(empty.is_pendable());

  auto active = Select(ConstexprDtorFuture{});
  PW_TEST_EXPECT_TRUE(active.is_pendable());

  auto moved = std::move(active);
  PW_TEST_EXPECT_FALSE(
      active.is_pendable());  // NOLINT(bugprone-use-after-move)
  PW_TEST_EXPECT_TRUE(moved.is_pendable());

  empty = std::move(moved);
  PW_TEST_EXPECT_TRUE(empty.is_pendable());

  empty = Select(ConstexprDtorFuture{});
  PW_TEST_EXPECT_TRUE(empty.is_pendable());
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

// Counts constructions, move assignments, and destructions of a type.
struct LifetimeCounts {
  int constructed = 0;
  int default_constructed = 0;
  int move_assigned = 0;
  int destroyed = 0;

  int alive() const { return constructed - destroyed; }
};

LifetimeCounts counted_value_lifetimes;
LifetimeCounts counting_future_lifetimes;

class CountedValue {
 public:
  explicit CountedValue(int value) : value_(value) {
    counted_value_lifetimes.constructed += 1;
  }

  CountedValue(const CountedValue& other) : value_(other.value_) {
    counted_value_lifetimes.constructed += 1;
  }
  CountedValue& operator=(const CountedValue&) = default;

  CountedValue(CountedValue&& other) : value_(other.value_) {
    counted_value_lifetimes.constructed += 1;
  }
  CountedValue& operator=(CountedValue&&) = default;

  ~CountedValue() { counted_value_lifetimes.destroyed += 1; }

  int value() const { return value_; }

 private:
  int value_;
};

// Wraps a ValueFuture<int>, resolving to a CountedValue. Counts its own
// constructions, move assignments, and destructions.
class CountingFuture {
 public:
  using value_type = CountedValue;

  CountingFuture() {
    counting_future_lifetimes.constructed += 1;
    counting_future_lifetimes.default_constructed += 1;
  }

  explicit CountingFuture(ValueFuture<int>&& future)
      : future_(std::move(future)) {
    counting_future_lifetimes.constructed += 1;
  }

  CountingFuture(CountingFuture&& other) : future_(std::move(other.future_)) {
    counting_future_lifetimes.constructed += 1;
  }

  CountingFuture& operator=(CountingFuture&& other) {
    counting_future_lifetimes.move_assigned += 1;
    future_ = std::move(other.future_);
    return *this;
  }

  ~CountingFuture() { counting_future_lifetimes.destroyed += 1; }

  Poll<CountedValue> Pend(Context& cx) {
    Poll<int> poll = future_.Pend(cx);
    if (poll.IsPending()) {
      return Pending();
    }
    return CountedValue(*poll);
  }

  bool is_pendable() const { return future_.is_pendable(); }
  bool is_complete() const { return future_.is_complete(); }

 private:
  ValueFuture<int> future_;
};

static_assert(pw::async2::Future<CountingFuture>);

class SelectFutureLifetime : public ::testing::Test {
 protected:
  SelectFutureLifetime() {
    counted_value_lifetimes = {};
    counting_future_lifetimes = {};
  }

  ~SelectFutureLifetime() override {
    EXPECT_EQ(counted_value_lifetimes.alive(), 0);
    EXPECT_EQ(counting_future_lifetimes.alive(), 0);
    EXPECT_EQ(counting_future_lifetimes.move_assigned, 0);
  }

  DispatcherForTest dispatcher_;
  BroadcastValueProvider<int> provider_1_;
  BroadcastValueProvider<int> provider_2_;
};

TEST_F(SelectFutureLifetime, CompleteThenDestroy) {
  int destroyed_on_completion = 0;
  {
    auto future = Select(CountingFuture(provider_1_.Get()),
                         CountingFuture(provider_2_.Get()));
    EXPECT_EQ(counting_future_lifetimes.alive(), 2);
    EXPECT_EQ(dispatcher_.RunInTaskUntilStalled(future), Pending());

    provider_2_.Resolve(2);
    {
      auto result = dispatcher_.RunInTaskUntilStalled(future);
      ASSERT_TRUE(result.IsReady());
      EXPECT_TRUE(future.is_complete());
      EXPECT_FALSE(future.is_pendable());
      EXPECT_FALSE(result->has_value<0>());
      ASSERT_TRUE(result->has_value<1>());
      EXPECT_EQ(result->value<1>().value(), 2);

      // Both sub-futures are destroyed in place on completion.
      EXPECT_EQ(counting_future_lifetimes.alive(), 0);
      EXPECT_EQ(counted_value_lifetimes.alive(), 1);
      destroyed_on_completion = counting_future_lifetimes.destroyed;
    }
    EXPECT_EQ(counted_value_lifetimes.alive(), 0);
  }
  // Destroying the completed SelectFuture destroys no additional sub-futures.
  EXPECT_EQ(counting_future_lifetimes.destroyed, destroyed_on_completion);
  EXPECT_EQ(counting_future_lifetimes.alive(), 0);
  EXPECT_EQ(counting_future_lifetimes.default_constructed, 0);
  EXPECT_EQ(counting_future_lifetimes.move_assigned, 0);
  EXPECT_GT(counting_future_lifetimes.constructed, 0);
  EXPECT_GT(counted_value_lifetimes.constructed, 0);
}

TEST_F(SelectFutureLifetime, DestroyBeforeCompletion) {
  {
    auto future = Select(CountingFuture(provider_1_.Get()),
                         CountingFuture(provider_2_.Get()));
    EXPECT_EQ(dispatcher_.RunInTaskUntilStalled(future), Pending());
    EXPECT_EQ(counting_future_lifetimes.alive(), 2);
  }
  EXPECT_EQ(counting_future_lifetimes.alive(), 0);
  EXPECT_EQ(counting_future_lifetimes.destroyed,
            counting_future_lifetimes.constructed);
  EXPECT_EQ(counting_future_lifetimes.default_constructed, 0);
  EXPECT_EQ(counting_future_lifetimes.move_assigned, 0);
}

TEST_F(SelectFutureLifetime, MoveBeforeCompletion) {
  {
    auto future = Select(CountingFuture(provider_1_.Get()),
                         CountingFuture(provider_2_.Get()));
    EXPECT_EQ(dispatcher_.RunInTaskUntilStalled(future), Pending());

    SelectFuture<CountingFuture, CountingFuture> moved(std::move(future));
    EXPECT_FALSE(future.is_pendable());  // NOLINT(bugprone-use-after-move)
    EXPECT_FALSE(future.is_complete());  // NOLINT(bugprone-use-after-move)
    EXPECT_TRUE(moved.is_pendable());
    EXPECT_FALSE(moved.is_complete());
    EXPECT_EQ(counting_future_lifetimes.alive(), 2);

    SelectFuture<CountingFuture, CountingFuture> move_assigned;
    move_assigned = std::move(moved);
    EXPECT_FALSE(moved.is_pendable());  // NOLINT(bugprone-use-after-move)
    EXPECT_FALSE(moved.is_complete());  // NOLINT(bugprone-use-after-move)
    ASSERT_TRUE(move_assigned.is_pendable());
    EXPECT_FALSE(move_assigned.is_complete());
    EXPECT_EQ(counting_future_lifetimes.alive(), 2);

    provider_1_.Resolve(1);
    auto result = dispatcher_.RunInTaskUntilStalled(move_assigned);
    ASSERT_TRUE(result.IsReady());
    EXPECT_TRUE(move_assigned.is_complete());
    ASSERT_TRUE(result->has_value<0>());
    EXPECT_EQ(result->value<0>().value(), 1);
    EXPECT_FALSE(result->has_value<1>());
  }
  EXPECT_EQ(counting_future_lifetimes.default_constructed, 0);
  EXPECT_EQ(counting_future_lifetimes.move_assigned, 0);
  EXPECT_GT(counting_future_lifetimes.constructed, 0);
}

TEST_F(SelectFutureLifetime, MoveAssignOverActive) {
  BroadcastValueProvider<int> provider_3;
  BroadcastValueProvider<int> provider_4;
  {
    auto target = Select(CountingFuture(provider_1_.Get()),
                         CountingFuture(provider_2_.Get()));
    auto source = Select(CountingFuture(provider_3.Get()),
                         CountingFuture(provider_4.Get()));
    EXPECT_EQ(counting_future_lifetimes.alive(), 4);

    target = std::move(source);
    EXPECT_FALSE(source.is_pendable());  // NOLINT(bugprone-use-after-move)
    EXPECT_TRUE(target.is_pendable());
    EXPECT_EQ(counting_future_lifetimes.alive(), 2);

    provider_4.Resolve(44);
    auto result = dispatcher_.RunInTaskUntilStalled(target);
    ASSERT_TRUE(result.IsReady());
    ASSERT_TRUE(result->has_value<1>());
    EXPECT_EQ(result->value<1>().value(), 44);
    EXPECT_EQ(counting_future_lifetimes.alive(), 0);
  }
  EXPECT_EQ(counting_future_lifetimes.default_constructed, 0);
  EXPECT_EQ(counting_future_lifetimes.move_assigned, 0);
}

TEST_F(SelectFutureLifetime, ResetBeforeCompletion) {
  auto active = Select(CountingFuture(provider_1_.Get()),
                       CountingFuture(provider_2_.Get()));
  EXPECT_EQ(counting_future_lifetimes.alive(), 2);

  active = {};
  EXPECT_FALSE(active.is_pendable());
  EXPECT_FALSE(active.is_complete());
  EXPECT_EQ(counting_future_lifetimes.alive(), 0);
  EXPECT_EQ(counting_future_lifetimes.default_constructed, 0);
}

template <typename T>
void MoveAssignViaAliases(T& dst, T& src) {
  dst = std::move(src);
}

TEST_F(SelectFutureLifetime, SelfMoveAssignEmpty) {
  SelectFuture<CountingFuture, CountingFuture> empty;
  MoveAssignViaAliases(empty, empty);
  EXPECT_FALSE(empty.is_pendable());
  EXPECT_FALSE(empty.is_complete());
  EXPECT_EQ(counting_future_lifetimes.constructed, 0);
}

TEST_F(SelectFutureLifetime, SelfMoveAssignActive) {
  auto active = Select(CountingFuture(provider_1_.Get()),
                       CountingFuture(provider_2_.Get()));
  EXPECT_EQ(dispatcher_.RunInTaskUntilStalled(active), Pending());
  MoveAssignViaAliases(active, active);
  ASSERT_TRUE(active.is_pendable());
  EXPECT_EQ(counting_future_lifetimes.alive(), 2);

  provider_1_.Resolve(99);
  auto result = dispatcher_.RunInTaskUntilStalled(active);
  ASSERT_TRUE(result.IsReady());
  ASSERT_TRUE(result->has_value<0>());
  EXPECT_EQ(result->value<0>().value(), 99);
  EXPECT_EQ(counting_future_lifetimes.default_constructed, 0);
}

TEST_F(SelectFutureLifetime, SelfMoveAssignCompleted) {
  auto completed = Select(CountingFuture(provider_1_.Get()),
                          CountingFuture(provider_2_.Get()));
  provider_1_.Resolve(99);
  ASSERT_TRUE(dispatcher_.RunInTaskUntilStalled(completed).IsReady());
  ASSERT_TRUE(completed.is_complete());

  MoveAssignViaAliases(completed, completed);
  EXPECT_TRUE(completed.is_complete());
  EXPECT_FALSE(completed.is_pendable());
  EXPECT_EQ(counting_future_lifetimes.alive(), 0);
  EXPECT_EQ(counting_future_lifetimes.default_constructed, 0);
}

// Sub-future whose destructor records the state of its enclosing SelectFuture
// while sub-future destruction is in progress.
class StateCheckingFuture {
 public:
  using value_type = int;

  struct ObservedState {
    int destructor_calls = 0;
    bool was_pendable = true;
    bool was_complete = false;
  };

  StateCheckingFuture() = default;
  StateCheckingFuture(Poll<int> poll_result,
                      const SelectFuture<StateCheckingFuture>** owner,
                      ObservedState* observed)
      : poll_result_(poll_result), owner_(owner), observed_(observed) {}

  StateCheckingFuture(StateCheckingFuture&& other) noexcept
      : poll_result_(other.poll_result_),
        owner_(std::exchange(other.owner_, nullptr)),
        observed_(std::exchange(other.observed_, nullptr)) {}

  StateCheckingFuture& operator=(StateCheckingFuture&& other) noexcept {
    if (this != &other) {
      RecordDestruction();
      poll_result_ = other.poll_result_;
      owner_ = std::exchange(other.owner_, nullptr);
      observed_ = std::exchange(other.observed_, nullptr);
    }
    return *this;
  }

  ~StateCheckingFuture() { RecordDestruction(); }

  Poll<int> Pend(Context&) { return poll_result_; }
  bool is_pendable() const { return observed_ != nullptr; }
  bool is_complete() const { return false; }

 private:
  void RecordDestruction() {
    if (observed_ != nullptr && owner_ != nullptr && *owner_ != nullptr) {
      observed_->destructor_calls += 1;
      observed_->was_pendable = (*owner_)->is_pendable();
      observed_->was_complete = (*owner_)->is_complete();
    }
  }

  Poll<int> poll_result_ = Pending();
  const SelectFuture<StateCheckingFuture>** owner_ = nullptr;
  ObservedState* observed_ = nullptr;
};

static_assert(pw::async2::Future<StateCheckingFuture>);

TEST(SelectFuture, CompleteUpdatesStateBeforeSubfutureDestruction) {
  DispatcherForTest dispatcher;
  const SelectFuture<StateCheckingFuture>* owner = nullptr;
  StateCheckingFuture::ObservedState observed;
  auto future = Select(StateCheckingFuture(Ready(7), &owner, &observed));
  owner = &future;

  auto result = dispatcher.RunInTaskUntilStalled(future);
  ASSERT_TRUE(result.IsReady());
  EXPECT_EQ(observed.destructor_calls, 1);
  EXPECT_FALSE(observed.was_pendable);
  EXPECT_TRUE(observed.was_complete);
}

TEST(SelectFuture, MoveAssignUpdatesStateBeforeSubfutureDestruction) {
  const SelectFuture<StateCheckingFuture>* owner = nullptr;
  StateCheckingFuture::ObservedState observed;
  auto future = Select(StateCheckingFuture(Pending(), &owner, &observed));
  owner = &future;

  future = {};
  EXPECT_EQ(observed.destructor_calls, 1);
  EXPECT_FALSE(observed.was_pendable);
  EXPECT_FALSE(observed.was_complete);
}

TEST(SelectFuture, DestroyUpdatesStateBeforeSubfutureDestruction) {
  const SelectFuture<StateCheckingFuture>* owner = nullptr;
  StateCheckingFuture::ObservedState observed;
  {
    auto future = Select(StateCheckingFuture(Pending(), &owner, &observed));
    owner = &future;
  }
  EXPECT_EQ(observed.destructor_calls, 1);
  EXPECT_FALSE(observed.was_pendable);
  EXPECT_FALSE(observed.was_complete);
}

struct OwnedSelectFuture {
  using value_type = int;

  OwnedSelectFuture() = default;
  explicit OwnedSelectFuture(
      int value,
      std::optional<SelectFuture<OwnedSelectFuture>>* owned = nullptr)
      : value_(value), owned_(owned), pendable_(true) {}

  OwnedSelectFuture(OwnedSelectFuture&& other) noexcept
      : value_(other.value_),
        owned_(std::exchange(other.owned_, nullptr)),
        pendable_(std::exchange(other.pendable_, false)) {}

  OwnedSelectFuture& operator=(OwnedSelectFuture&& other) noexcept {
    if (this != &other) {
      DestroyOwned();
      value_ = other.value_;
      owned_ = std::exchange(other.owned_, nullptr);
      pendable_ = std::exchange(other.pendable_, false);
    }
    return *this;
  }

  ~OwnedSelectFuture() {
    EXPECT_FALSE(destroyed_);
    destroyed_ = true;
    DestroyOwned();
  }

  Poll<int> Pend(Context&) {
    pendable_ = false;
    complete_ = true;
    return Ready(value_);
  }

  bool is_pendable() const { return pendable_; }
  bool is_complete() const { return complete_; }

  void DestroyOwned() {
    if (owned_ != nullptr) {
      owned_->reset();
      owned_ = nullptr;
    }
  }

  int value_ = 0;
  std::optional<SelectFuture<OwnedSelectFuture>>* owned_ = nullptr;
  bool pendable_ = false;
  bool complete_ = false;
  bool destroyed_ = false;
};

static_assert(pw::async2::Future<OwnedSelectFuture>);

TEST(SelectFuture, MoveAssignFromTransitivelyOwnedPendableFuture) {
  DispatcherForTest dispatcher;
  std::optional<SelectFuture<OwnedSelectFuture>> child;
  child.emplace(Select(OwnedSelectFuture(42)));
  auto parent = Select(OwnedSelectFuture(1, &child));

  parent = std::move(*child);
  ASSERT_TRUE(parent.is_pendable());
  auto result = dispatcher.RunInTaskUntilStalled(parent);
  ASSERT_TRUE(result.IsReady());
  ASSERT_TRUE(result->has_value<0>());
  EXPECT_EQ(result->value<0>(), 42);
}

TEST(SelectFuture, MoveAssignFromTransitivelyOwnedCompletedFuture) {
  DispatcherForTest dispatcher;
  std::optional<SelectFuture<OwnedSelectFuture>> child;
  child.emplace(Select(OwnedSelectFuture(7)));
  ASSERT_TRUE(dispatcher.RunInTaskUntilStalled(*child).IsReady());
  ASSERT_TRUE(child->is_complete());
  auto parent = Select(OwnedSelectFuture(1, &child));

  parent = std::move(*child);
  EXPECT_TRUE(parent.is_complete());
  EXPECT_FALSE(parent.is_pendable());
}

TEST_F(SelectFutureLifetime, MoveAfterCompletion) {
  {
    auto future = Select(CountingFuture(provider_1_.Get()),
                         CountingFuture(provider_2_.Get()));
    provider_1_.Resolve(1);
    provider_2_.Resolve(2);
    {
      auto result = dispatcher_.RunInTaskUntilStalled(future);
      ASSERT_TRUE(result.IsReady());
      ASSERT_TRUE(result->has_value<0>());
      ASSERT_TRUE(result->has_value<1>());
      EXPECT_EQ(result->value<0>().value(), 1);
      EXPECT_EQ(result->value<1>().value(), 2);
    }
    EXPECT_EQ(counting_future_lifetimes.alive(), 0);

    SelectFuture<CountingFuture, CountingFuture> moved(std::move(future));
    EXPECT_FALSE(future.is_pendable());  // NOLINT(bugprone-use-after-move)
    EXPECT_FALSE(future.is_complete());  // NOLINT(bugprone-use-after-move)
    EXPECT_TRUE(moved.is_complete());
    EXPECT_EQ(counting_future_lifetimes.alive(), 0);

    auto move_assigned =
        Select(CountingFuture(provider_1_.Get()), CountingFuture());
    EXPECT_EQ(counting_future_lifetimes.default_constructed, 1);
    EXPECT_EQ(counting_future_lifetimes.alive(), 2);
    move_assigned = std::move(moved);
    EXPECT_FALSE(moved.is_pendable());  // NOLINT(bugprone-use-after-move)
    EXPECT_FALSE(moved.is_complete());  // NOLINT(bugprone-use-after-move)
    EXPECT_TRUE(move_assigned.is_complete());
    EXPECT_EQ(counting_future_lifetimes.alive(), 0);
  }
  EXPECT_EQ(counting_future_lifetimes.default_constructed, 1);
  EXPECT_EQ(counting_future_lifetimes.move_assigned, 0);
  EXPECT_GT(counting_future_lifetimes.constructed, 0);
}

TEST_F(SelectFutureLifetime, ReArmAfterCompletion) {
  auto future = Select(CountingFuture(provider_1_.Get()),
                       CountingFuture(provider_2_.Get()));
  provider_1_.Resolve(1);
  ASSERT_TRUE(dispatcher_.RunInTaskUntilStalled(future).IsReady());
  ASSERT_TRUE(future.is_complete());
  EXPECT_EQ(counting_future_lifetimes.alive(), 0);

  BroadcastValueProvider<int> provider_3;
  BroadcastValueProvider<int> provider_4;
  future = Select(CountingFuture(provider_3.Get()),
                  CountingFuture(provider_4.Get()));
  EXPECT_TRUE(future.is_pendable());
  EXPECT_FALSE(future.is_complete());
  EXPECT_EQ(counting_future_lifetimes.alive(), 2);

  provider_3.Resolve(3);
  auto result = dispatcher_.RunInTaskUntilStalled(future);
  ASSERT_TRUE(result.IsReady());
  ASSERT_TRUE(result->has_value<0>());
  EXPECT_EQ(result->value<0>().value(), 3);
  EXPECT_TRUE(future.is_complete());
  EXPECT_EQ(counting_future_lifetimes.alive(), 0);
  EXPECT_EQ(counting_future_lifetimes.default_constructed, 0);
}

TEST_F(SelectFutureLifetime, ResetAfterCompletion) {
  auto future = Select(CountingFuture(provider_1_.Get()),
                       CountingFuture(provider_2_.Get()));
  provider_1_.Resolve(1);
  ASSERT_TRUE(dispatcher_.RunInTaskUntilStalled(future).IsReady());
  ASSERT_TRUE(future.is_complete());

  future = {};
  EXPECT_FALSE(future.is_pendable());
  EXPECT_FALSE(future.is_complete());
  EXPECT_EQ(counting_future_lifetimes.alive(), 0);
  EXPECT_EQ(counting_future_lifetimes.default_constructed, 0);
}

TEST_F(SelectFutureLifetime, DefaultConstructThenDestroy) {
  {
    SelectFuture<CountingFuture, CountingFuture> future;
    EXPECT_FALSE(future.is_pendable());
    EXPECT_FALSE(future.is_complete());

    SelectFuture<CountingFuture, CountingFuture> moved(std::move(future));
    EXPECT_FALSE(moved.is_pendable());
    EXPECT_FALSE(moved.is_complete());
  }
  EXPECT_EQ(counting_future_lifetimes.constructed, 0);
  EXPECT_EQ(counting_future_lifetimes.default_constructed, 0);
  EXPECT_EQ(counting_future_lifetimes.move_assigned, 0);
  EXPECT_EQ(counted_value_lifetimes.constructed, 0);
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

#elif PW_NC_TEST(PwSelectRejectsNonFuture)
PW_NC_EXPECT("All arguments to PW_SELECT or PW_CO_SELECT must be futures");

[[maybe_unused]] Poll<> PwSelectNonFuture(Context& cx, int& not_a_future) {
  PW_SELECT(auto result, cx, not_a_future);
  return Ready();
}

#elif PW_NC_TEST(SelectRejectsLvalues)
PW_NC_EXPECT("Select takes ownership of its futures");

void SelectLvalue(ValueFuture<int>& future) {
  [[maybe_unused]] auto selected = Select(future);
}

#elif PW_NC_TEST(SelectRejectsConstFutures)
PW_NC_EXPECT("Futures passed to Select must not be const");

void SelectConstRvalue(const ValueFuture<int>&& future) {
  [[maybe_unused]] auto selected = Select(std::move(future));
}

#elif PW_NC_TEST(SelectRejectsNonFuture)
PW_NC_EXPECT("All arguments to Select must be Future types");

void SelectNonFuture() { [[maybe_unused]] auto selected = Select(42); }

#elif PW_NC_TEST(SelectFutureRejectsNonFuture)
PW_NC_EXPECT("All types in SelectFuture must be Future types");

void InstantiateNonFutureSelectFuture() {
  [[maybe_unused]] SelectFuture<int> selected;
}

#endif  // PW_NC_TEST

}  // namespace
