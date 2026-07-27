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

#include "pw_async2/await.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/func_task.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::async2::BroadcastValueProvider;
using pw::async2::Context;
using pw::async2::DispatcherForTest;
using pw::async2::FuncTask;
using pw::async2::OptionalBroadcastValueProvider;
using pw::async2::OptionalValueFuture;
using pw::async2::OptionalValueProvider;
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

  FuncTask task([&](Context& cx) -> Poll<> {
    PW_AWAIT(int value, future, cx);
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

  FuncTask task([&](Context& cx) -> Poll<> {
    PW_AWAIT(int value, future, cx);
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
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_AWAIT(auto value, future, cx);
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
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_AWAIT(int value, future, cx);
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
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_AWAIT(int value, future, cx);
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
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_AWAIT(auto value, future, cx);
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

TEST(ValueProvider, Move) {
  DispatcherForTest dispatcher;
  ValueProvider<int> provider;

  ValueFuture<int> future = provider.Get();
  ASSERT_FALSE(future.is_complete());

  ValueProvider<int> provider2 = std::move(provider);
  EXPECT_FALSE(provider.has_future());  // NOLINT(bugprone-use-after-move)
  EXPECT_TRUE(provider2.has_future());

  int result = -1;
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_AWAIT(int value, future, cx);
    result = value;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  provider2.Resolve(82);
  dispatcher.RunToCompletion();
  EXPECT_EQ(result, 82);
}

TEST(ValueProvider, MoveAssignment) {
  DispatcherForTest dispatcher;
  ValueProvider<int> provider;

  ValueFuture<int> future = provider.Get();
  ASSERT_FALSE(future.is_complete());

  ValueProvider<int> provider2;
  provider2 = std::move(provider);
  EXPECT_FALSE(provider.has_future());  // NOLINT(bugprone-use-after-move)
  EXPECT_TRUE(provider2.has_future());

  int result = -1;
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_AWAIT(int value, future, cx);
    result = value;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  provider2.Resolve(82);
  dispatcher.RunToCompletion();
  EXPECT_EQ(result, 82);
}

TEST(OptionalValueProvider, Move) {
  DispatcherForTest dispatcher;
  OptionalValueProvider<int> provider;

  OptionalValueFuture<int> future = provider.Get();
  ASSERT_FALSE(future.is_complete());

  OptionalValueProvider<int> provider2 = std::move(provider);

  std::optional<int> result;
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_AWAIT(auto value, future, cx);
    result = value;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  provider2.Resolve(99);
  dispatcher.RunToCompletion();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 99);
}

TEST(OptionalValueProvider, MoveAssignment) {
  DispatcherForTest dispatcher;
  OptionalValueProvider<int> provider;

  OptionalValueFuture<int> future = provider.Get();
  ASSERT_FALSE(future.is_complete());

  OptionalValueProvider<int> provider2;
  provider2 = std::move(provider);

  std::optional<int> result;
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_AWAIT(auto value, future, cx);
    result = value;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  provider2.Resolve(99);
  dispatcher.RunToCompletion();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 99);
}

TEST(VoidFuture, Pend) {
  DispatcherForTest dispatcher;
  BroadcastValueProvider<void> provider;

  VoidFuture future = provider.Get();
  bool completed = false;

  FuncTask task([&](Context& cx) -> Poll<> {
    PW_AWAIT(future, cx);
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

  FuncTask task([&](Context& cx) -> Poll<> {
    PW_AWAIT(future, cx);
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
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_AWAIT(future, cx);
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

TEST(OptionalValueProvider, Resolve) {
  DispatcherForTest dispatcher;
  OptionalValueProvider<int> provider;

  OptionalValueFuture<int> future = provider.Get();
  ASSERT_FALSE(future.is_complete());

  int result = -1;
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_AWAIT(auto value, future, cx);
    EXPECT_TRUE(value.has_value());
    if (value.has_value()) {
      result = *value;
    }
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  provider.Resolve(123);
  dispatcher.RunToCompletion();
  EXPECT_EQ(result, 123);
}

TEST(OptionalValueProvider, Cancel) {
  DispatcherForTest dispatcher;
  OptionalValueProvider<int> provider;

  OptionalValueFuture<int> future = provider.Get();
  ASSERT_FALSE(future.is_complete());

  int result = -1;
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_AWAIT(auto value, future, cx);
    if (value.has_value()) {
      ADD_FAILURE();
      result = *value;
    }
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  provider.Cancel();
  dispatcher.RunToCompletion();
  EXPECT_EQ(result, -1);
}

TEST(OptionalValueProvider, DestructorCancels) {
  DispatcherForTest dispatcher;
  OptionalValueFuture<int> future;

  {
    OptionalValueProvider<int> provider;
    future = provider.Get();
    EXPECT_FALSE(future.is_complete());
  }

  int result = -1;
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_AWAIT(auto value, future, cx);
    if (value.has_value()) {
      ADD_FAILURE();
      result = *value;
    }
    return Ready();
  });

  dispatcher.Post(task);
  dispatcher.RunToCompletion();
  EXPECT_EQ(result, -1);
}

TEST(OptionalBroadcastValueProvider, Move) {
  DispatcherForTest dispatcher;
  OptionalBroadcastValueProvider<int> provider;

  ValueFuture<std::optional<int>> future1 = provider.Get();
  ValueFuture<std::optional<int>> future2 = provider.Get();
  ASSERT_FALSE(future1.is_complete());
  ASSERT_FALSE(future2.is_complete());

  OptionalBroadcastValueProvider<int> provider2 = std::move(provider);

  std::optional<int> result1;
  FuncTask task1([&](Context& cx) -> Poll<> {
    PW_AWAIT(auto value, future1, cx);
    result1 = value;
    return Ready();
  });

  std::optional<int> result2;
  FuncTask task2([&](Context& cx) -> Poll<> {
    PW_AWAIT(auto value, future2, cx);
    result2 = value;
    return Ready();
  });

  dispatcher.Post(task1);
  dispatcher.Post(task2);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  provider2.Resolve(99);
  dispatcher.RunToCompletion();
  ASSERT_TRUE(result1.has_value());
  EXPECT_EQ(*result1, 99);
  ASSERT_TRUE(result2.has_value());
  EXPECT_EQ(*result2, 99);
}

TEST(OptionalBroadcastValueProvider, MoveAssignment) {
  DispatcherForTest dispatcher;
  OptionalBroadcastValueProvider<int> provider;

  ValueFuture<std::optional<int>> future1 = provider.Get();
  ValueFuture<std::optional<int>> future2 = provider.Get();
  ASSERT_FALSE(future1.is_complete());
  ASSERT_FALSE(future2.is_complete());

  OptionalBroadcastValueProvider<int> provider2;
  provider2 = std::move(provider);

  std::optional<int> result1;
  FuncTask task1([&](Context& cx) -> Poll<> {
    PW_AWAIT(auto value, future1, cx);
    result1 = value;
    return Ready();
  });

  std::optional<int> result2;
  FuncTask task2([&](Context& cx) -> Poll<> {
    PW_AWAIT(auto value, future2, cx);
    result2 = value;
    return Ready();
  });

  dispatcher.Post(task1);
  dispatcher.Post(task2);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  provider2.Resolve(99);
  dispatcher.RunToCompletion();
  ASSERT_TRUE(result1.has_value());
  EXPECT_EQ(*result1, 99);
  ASSERT_TRUE(result2.has_value());
  EXPECT_EQ(*result2, 99);
}

TEST(OptionalBroadcastValueProvider, Resolve) {
  DispatcherForTest dispatcher;
  OptionalBroadcastValueProvider<int> provider;

  ValueFuture<std::optional<int>> future1 = provider.Get();
  ValueFuture<std::optional<int>> future2 = provider.Get();
  ASSERT_FALSE(future1.is_complete());
  ASSERT_FALSE(future2.is_complete());

  std::optional<int> result1 = std::nullopt;
  FuncTask task1([&](Context& cx) -> Poll<> {
    PW_AWAIT(auto value, future1, cx);
    result1 = value;
    return Ready();
  });

  std::optional<int> result2 = std::nullopt;
  FuncTask task2([&](Context& cx) -> Poll<> {
    PW_AWAIT(auto value, future2, cx);
    result2 = value;
    return Ready();
  });

  dispatcher.Post(task1);
  dispatcher.Post(task2);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  provider.Resolve(123);
  dispatcher.RunToCompletion();
  ASSERT_TRUE(result1.has_value());
  EXPECT_EQ(*result1, 123);
  ASSERT_TRUE(result2.has_value());
  EXPECT_EQ(*result2, 123);
}

TEST(OptionalBroadcastValueProvider, Cancel) {
  DispatcherForTest dispatcher;
  OptionalBroadcastValueProvider<int> provider;

  ValueFuture<std::optional<int>> future1 = provider.Get();
  ValueFuture<std::optional<int>> future2 = provider.Get();
  ASSERT_FALSE(future1.is_complete());
  ASSERT_FALSE(future2.is_complete());

  std::optional<int> result1 = 0;
  FuncTask task1([&](Context& cx) -> Poll<> {
    PW_AWAIT(auto value, future1, cx);
    result1 = value;
    return Ready();
  });

  std::optional<int> result2 = 0;
  FuncTask task2([&](Context& cx) -> Poll<> {
    PW_AWAIT(auto value, future2, cx);
    result2 = value;
    return Ready();
  });

  dispatcher.Post(task1);
  dispatcher.Post(task2);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  provider.Cancel();
  dispatcher.RunToCompletion();
  EXPECT_FALSE(result1.has_value());
  EXPECT_FALSE(result2.has_value());
}

TEST(OptionalBroadcastValueProvider, DestructorCancels) {
  DispatcherForTest dispatcher;
  ValueFuture<std::optional<int>> future1;
  ValueFuture<std::optional<int>> future2;

  {
    OptionalBroadcastValueProvider<int> provider;
    future1 = provider.Get();
    future2 = provider.Get();
    EXPECT_FALSE(future1.is_complete());
    EXPECT_FALSE(future2.is_complete());
    // provider goes out of scope here
  }

  std::optional<int> result1 = 0;
  FuncTask task1([&](Context& cx) -> Poll<> {
    PW_AWAIT(auto value, future1, cx);
    result1 = value;
    return Ready();
  });

  std::optional<int> result2 = 0;
  FuncTask task2([&](Context& cx) -> Poll<> {
    PW_AWAIT(auto value, future2, cx);
    result2 = value;
    return Ready();
  });

  dispatcher.Post(task1);
  dispatcher.Post(task2);
  dispatcher.RunToCompletion();
  EXPECT_FALSE(result1.has_value());
  EXPECT_FALSE(result2.has_value());
}

TEST(OptionalValueProvider, MoveAssignmentCancelsExisting) {
  DispatcherForTest dispatcher;
  OptionalValueProvider<int> provider_dest;
  OptionalValueFuture<int> future_dest = provider_dest.Get();

  OptionalValueProvider<int> provider_src;
  OptionalValueFuture<int> future_src = provider_src.Get();

  std::optional<int> result_dest = 0;
  FuncTask task_dest([&](Context& cx) -> Poll<> {
    PW_AWAIT(auto value, future_dest, cx);
    result_dest = value;
    return Ready();
  });
  dispatcher.Post(task_dest);

  std::optional<int> result_src = 0;
  FuncTask task_src([&](Context& cx) -> Poll<> {
    PW_AWAIT(auto value, future_src, cx);
    result_src = value;
    return Ready();
  });
  dispatcher.Post(task_src);

  provider_dest = std::move(provider_src);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_FALSE(result_dest.has_value());

  provider_dest.Resolve(123);
  dispatcher.RunToCompletion();
  ASSERT_TRUE(result_src.has_value());
  EXPECT_EQ(*result_src, 123);
}

TEST(OptionalBroadcastValueProvider, MoveAssignmentCancelsExisting) {
  DispatcherForTest dispatcher;
  OptionalBroadcastValueProvider<int> provider_dest;
  OptionalValueFuture<int> future_dest1 = provider_dest.Get();
  OptionalValueFuture<int> future_dest2 = provider_dest.Get();

  OptionalBroadcastValueProvider<int> provider_src;
  OptionalValueFuture<int> future_src1 = provider_src.Get();
  OptionalValueFuture<int> future_src2 = provider_src.Get();

  std::optional<int> result_dest1 = 0;
  FuncTask task_dest1([&](Context& cx) -> Poll<> {
    PW_AWAIT(auto value, future_dest1, cx);
    result_dest1 = value;
    return Ready();
  });
  dispatcher.Post(task_dest1);

  std::optional<int> result_dest2 = 0;
  FuncTask task_dest2([&](Context& cx) -> Poll<> {
    PW_AWAIT(auto value, future_dest2, cx);
    result_dest2 = value;
    return Ready();
  });
  dispatcher.Post(task_dest2);

  std::optional<int> result_src1 = 0;
  FuncTask task_src1([&](Context& cx) -> Poll<> {
    PW_AWAIT(auto value, future_src1, cx);
    result_src1 = value;
    return Ready();
  });
  dispatcher.Post(task_src1);

  std::optional<int> result_src2 = 0;
  FuncTask task_src2([&](Context& cx) -> Poll<> {
    PW_AWAIT(auto value, future_src2, cx);
    result_src2 = value;
    return Ready();
  });
  dispatcher.Post(task_src2);

  provider_dest = std::move(provider_src);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_FALSE(result_dest1.has_value());
  EXPECT_FALSE(result_dest2.has_value());

  provider_dest.Resolve(123);
  dispatcher.RunToCompletion();
  ASSERT_TRUE(result_src1.has_value());
  EXPECT_EQ(*result_src1, 123);
  ASSERT_TRUE(result_src2.has_value());
  EXPECT_EQ(*result_src2, 123);
}

class DerivedTestFuture : public ValueFuture<pw::Status> {
 public:
  DerivedTestFuture(ValueFuture<pw::Status>&& base, int requested_value)
      : ValueFuture<pw::Status>(std::move(base)),
        requested_value_(requested_value) {}
  int requested_value() const { return requested_value_; }

 private:
  int requested_value_;
};

TEST(DerivedValueProvider, ResolveIf) {
  DispatcherForTest dispatcher;
  pw::async2::DerivedValueProvider<DerivedTestFuture> provider;

  constexpr int kRequestedValue = 5;

  DerivedTestFuture future = provider.Get(kRequestedValue);
  pw::Status result = pw::Status::Unknown();

  FuncTask task([&](Context& cx) -> Poll<> {
    PW_AWAIT(result, future, cx);
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(result, pw::Status::Unknown());

  int current_value = 1;
  auto resolve_if_value_matches =
      [&current_value](DerivedTestFuture& f) -> std::optional<pw::Status> {
    if (f.requested_value() == current_value) {
      return pw::OkStatus();
    }
    return std::nullopt;
  };

  for (; current_value < kRequestedValue; ++current_value) {
    bool resolved = provider.ResolveIf(resolve_if_value_matches);
    EXPECT_FALSE(resolved);
    EXPECT_TRUE(dispatcher.RunUntilStalled());
    EXPECT_EQ(result, pw::Status::Unknown());
  }

  bool resolved = provider.ResolveIf(resolve_if_value_matches);
  EXPECT_TRUE(resolved);
  dispatcher.RunToCompletion();
  EXPECT_EQ(result, pw::OkStatus());
}

class DerivedVoidFuture : public ValueFuture<void> {
 public:
  DerivedVoidFuture(ValueFuture<void>&& base, bool condition)
      : ValueFuture<void>(std::move(base)), condition_(condition) {}
  bool condition() const { return condition_; }

 private:
  bool condition_;
};

TEST(DerivedValueProvider, ResolveIfVoid) {
  DispatcherForTest dispatcher;
  pw::async2::DerivedValueProvider<DerivedVoidFuture> provider;

  DerivedVoidFuture future = provider.Get(true);
  bool completed = false;

  FuncTask task([&](Context& cx) -> Poll<> {
    PW_AWAIT(future, cx);
    completed = true;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_FALSE(completed);

  bool resolved =
      provider.ResolveIf([](DerivedVoidFuture& f) { return !f.condition(); });
  EXPECT_FALSE(resolved);
  EXPECT_FALSE(completed);

  resolved =
      provider.ResolveIf([](DerivedVoidFuture& f) { return f.condition(); });
  EXPECT_TRUE(resolved);
  dispatcher.RunToCompletion();
  EXPECT_TRUE(completed);
}

TEST(DerivedValueProvider, TryGet) {
  pw::async2::DerivedValueProvider<DerivedTestFuture> provider;

  std::optional<DerivedTestFuture> future1 = provider.TryGet(42);
  EXPECT_TRUE(future1.has_value());
  EXPECT_EQ(future1->requested_value(), 42);

  // Second TryGet while a future is pending should return std::nullopt.
  std::optional<DerivedTestFuture> future2 = provider.TryGet(100);
  EXPECT_FALSE(future2.has_value());

  // Resolving the pending future allows TryGet to succeed again.
  bool resolved =
      provider.ResolveIf([](DerivedTestFuture& f) -> std::optional<pw::Status> {
        if (f.requested_value() == 42) {
          return pw::OkStatus();
        }
        return std::nullopt;
      });
  EXPECT_TRUE(resolved);

  std::optional<DerivedTestFuture> future3 = provider.TryGet(100);
  EXPECT_TRUE(future3.has_value());
  EXPECT_EQ(future3->requested_value(), 100);

  provider.Resolve(pw::OkStatus());
}

TEST(ValueListProvider, BasicFifo) {
  DispatcherForTest dispatcher;
  pw::async2::ValueListProvider<int> provider;

  EXPECT_TRUE(provider.empty());
  EXPECT_EQ(provider.size(), 0u);

  ValueFuture<int> future1 = provider.Get();
  ValueFuture<int> future2 = provider.Get();

  EXPECT_FALSE(provider.empty());
  EXPECT_EQ(provider.size(), 2u);

  int result1 = 0;
  FuncTask task1([&](Context& cx) -> Poll<> {
    PW_AWAIT(int value, future1, cx);
    result1 = value;
    return Ready();
  });
  dispatcher.Post(task1);

  int result2 = 0;
  FuncTask task2([&](Context& cx) -> Poll<> {
    PW_AWAIT(int value, future2, cx);
    result2 = value;
    return Ready();
  });
  dispatcher.Post(task2);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(result1, 0);
  EXPECT_EQ(result2, 0);

  // Resolve first.
  provider.ResolveFirst(11);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(result1, 11);
  EXPECT_EQ(result2, 0);
  EXPECT_EQ(provider.size(), 1u);

  // Resolve second.
  provider.ResolveFirst(22);
  dispatcher.RunToCompletion();
  EXPECT_EQ(result1, 11);
  EXPECT_EQ(result2, 22);
  EXPECT_TRUE(provider.empty());
  EXPECT_EQ(provider.size(), 0u);
}

TEST(ValueListProvider, ResolveFirstMatching) {
  DispatcherForTest dispatcher;
  pw::async2::DerivedValueListProvider<DerivedTestFuture> provider;

  DerivedTestFuture future1 = provider.Get(100);
  DerivedTestFuture future2 = provider.Get(10);

  pw::Status result1 = pw::Status::Unknown();
  FuncTask task1([&](Context& cx) -> Poll<> {
    PW_AWAIT(result1, future1, cx);
    return Ready();
  });
  dispatcher.Post(task1);

  pw::Status result2 = pw::Status::Unknown();
  FuncTask task2([&](Context& cx) -> Poll<> {
    PW_AWAIT(result2, future2, cx);
    return Ready();
  });
  dispatcher.Post(task2);

  EXPECT_TRUE(dispatcher.RunUntilStalled());

  // Resolve future2 (matching 10) out of order.
  bool resolved = provider.ResolveFirstMatching(
      [](DerivedTestFuture& f) -> std::optional<pw::Status> {
        if (f.requested_value() == 10) {
          return pw::OkStatus();
        }
        return std::nullopt;
      });

  EXPECT_TRUE(resolved);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(result1, pw::Status::Unknown());
  EXPECT_EQ(result2, pw::OkStatus());
  EXPECT_EQ(provider.size(), 1u);

  // Now resolve the remaining future1 (matching 100).
  resolved = provider.ResolveFirstMatching(
      [](DerivedTestFuture& f) -> std::optional<pw::Status> {
        if (f.requested_value() == 100) {
          return pw::Status::ResourceExhausted();
        }
        return std::nullopt;
      });

  EXPECT_TRUE(resolved);
  dispatcher.RunToCompletion();
  EXPECT_EQ(result1, pw::Status::ResourceExhausted());
  EXPECT_TRUE(provider.empty());
}

TEST(ValueListProvider, ResolveAllMatching) {
  DispatcherForTest dispatcher;
  pw::async2::DerivedValueListProvider<DerivedTestFuture> provider;

  DerivedTestFuture future1 = provider.Get(30);
  DerivedTestFuture future2 = provider.Get(40);
  DerivedTestFuture future3 = provider.Get(15);

  pw::Status result1 = pw::Status::Unknown();
  FuncTask task1([&](Context& cx) -> Poll<> {
    PW_AWAIT(result1, future1, cx);
    return Ready();
  });
  dispatcher.Post(task1);

  pw::Status result2 = pw::Status::Unknown();
  FuncTask task2([&](Context& cx) -> Poll<> {
    PW_AWAIT(result2, future2, cx);
    return Ready();
  });
  dispatcher.Post(task2);

  pw::Status result3 = pw::Status::Unknown();
  FuncTask task3([&](Context& cx) -> Poll<> {
    PW_AWAIT(result3, future3, cx);
    return Ready();
  });
  dispatcher.Post(task3);

  EXPECT_TRUE(dispatcher.RunUntilStalled());

  // Stateful resource simulation: 50 available.
  int available_bytes = 50;
  size_t resolved_count = provider.ResolveAllMatching(
      [&](DerivedTestFuture& f) -> std::optional<pw::Status> {
        if (f.requested_value() <= available_bytes) {
          available_bytes -= f.requested_value();
          return pw::OkStatus();
        }
        return std::nullopt;
      });

  EXPECT_EQ(resolved_count, 2u);
  EXPECT_EQ(available_bytes, 5);  // 50 - 30 (future1) - 15 (future3)

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(result1, pw::OkStatus());
  EXPECT_EQ(result2, pw::Status::Unknown());
  EXPECT_EQ(result3, pw::OkStatus());
  EXPECT_EQ(provider.size(), 1u);

  // Clean up the remaining future.
  provider.ResolveFirst(pw::Status::Aborted());
  dispatcher.RunToCompletion();
}

TEST(ValueListProvider, ResolveAll) {
  DispatcherForTest dispatcher;
  pw::async2::ValueListProvider<int> provider;

  ValueFuture<int> future1 = provider.Get();
  ValueFuture<int> future2 = provider.Get();
  ValueFuture<int> future3 = provider.Get();

  int result1 = 0;
  FuncTask task1([&](Context& cx) -> Poll<> {
    PW_AWAIT(int value, future1, cx);
    result1 = value;
    return Ready();
  });
  dispatcher.Post(task1);

  int result2 = 0;
  FuncTask task2([&](Context& cx) -> Poll<> {
    PW_AWAIT(int value, future2, cx);
    result2 = value;
    return Ready();
  });
  dispatcher.Post(task2);

  int result3 = 0;
  FuncTask task3([&](Context& cx) -> Poll<> {
    PW_AWAIT(int value, future3, cx);
    result3 = value;
    return Ready();
  });
  dispatcher.Post(task3);

  EXPECT_TRUE(dispatcher.RunUntilStalled());

  provider.ResolveAll([](ValueFuture<int>&) { return 99; });

  dispatcher.RunToCompletion();
  EXPECT_EQ(result1, 99);
  EXPECT_EQ(result2, 99);
  EXPECT_EQ(result3, 99);
  EXPECT_TRUE(provider.empty());
}

TEST(ValueListProvider, ResolveFirstOnEmptyList) {
  pw::async2::ValueListProvider<int> provider;
  EXPECT_TRUE(provider.empty());
  provider.ResolveFirst(42);
  provider.ResolveFirst();
}

TEST(ValueListProvider, ResolveFirstMatchingNoMatch) {
  pw::async2::DerivedValueListProvider<DerivedTestFuture> provider;
  DerivedTestFuture future = provider.Get(50);

  bool resolved = provider.ResolveFirstMatching(
      [](DerivedTestFuture& f) -> std::optional<pw::Status> {
        if (f.requested_value() == 100) {
          return pw::OkStatus();
        }
        return std::nullopt;
      });

  EXPECT_FALSE(resolved);
  EXPECT_EQ(provider.size(), 1u);
  provider.ResolveFirst(pw::OkStatus());
}

TEST(ValueListProvider, ResolveFirstMatchingMultipleMatches) {
  DispatcherForTest dispatcher;
  pw::async2::DerivedValueListProvider<DerivedTestFuture> provider;

  DerivedTestFuture future1 = provider.Get(10);
  DerivedTestFuture future2 = provider.Get(10);

  pw::Status result1 = pw::Status::Unknown();
  FuncTask task1([&](Context& cx) -> Poll<> {
    PW_AWAIT(result1, future1, cx);
    return Ready();
  });
  dispatcher.Post(task1);

  pw::Status result2 = pw::Status::Unknown();
  FuncTask task2([&](Context& cx) -> Poll<> {
    PW_AWAIT(result2, future2, cx);
    return Ready();
  });
  dispatcher.Post(task2);

  EXPECT_TRUE(dispatcher.RunUntilStalled());

  // Two futures match 10.
  bool resolved = provider.ResolveFirstMatching(
      [](DerivedTestFuture& f) -> std::optional<pw::Status> {
        if (f.requested_value() == 10) {
          return pw::OkStatus();
        }
        return std::nullopt;
      });

  EXPECT_TRUE(resolved);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(result1, pw::OkStatus());
  EXPECT_EQ(result2, pw::Status::Unknown());
  EXPECT_EQ(provider.size(), 1u);

  provider.ResolveFirst(pw::OkStatus());
  dispatcher.RunToCompletion();
}

TEST(ValueListProvider, ResolveAllMatchingNoMatch) {
  pw::async2::DerivedValueListProvider<DerivedTestFuture> provider;
  DerivedTestFuture future1 = provider.Get(50);
  DerivedTestFuture future2 = provider.Get(60);

  size_t count = provider.ResolveAllMatching(
      [](DerivedTestFuture& f) -> std::optional<pw::Status> {
        if (f.requested_value() == 100) {
          return pw::OkStatus();
        }
        return std::nullopt;
      });

  EXPECT_EQ(count, 0u);
  EXPECT_EQ(provider.size(), 2u);

  provider.ResolveFirst(pw::OkStatus());
  provider.ResolveFirst(pw::OkStatus());
}

TEST(ValueListProvider, ResolveAllOnEmptyList) {
  pw::async2::ValueListProvider<int> provider;
  bool invoked = false;
  provider.ResolveAll([&](ValueFuture<int>&) {
    invoked = true;
    return 42;
  });
  EXPECT_FALSE(invoked);
}

}  // namespace
