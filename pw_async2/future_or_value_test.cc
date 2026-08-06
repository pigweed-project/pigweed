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

#include "pw_async2/future_or_value.h"

#include <memory>
#include <utility>

#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/func_task.h"
#include "pw_async2/value_future.h"
#include "pw_unit_test/framework.h"

namespace pw::async2 {
namespace {

TEST(FutureOrValueTest, DefaultConstructedIsEmpty) {
  static_assert(
      std::is_same_v<FutureOrValue<ValueFuture<int>>::value_type, int>);
  static_assert(
      std::is_same_v<FutureOrValue<ValueFuture<void>>::value_type, void>);

  static_assert(!std::is_copy_constructible_v<FutureOrValue<ValueFuture<int>>>);
  static_assert(!std::is_copy_assignable_v<FutureOrValue<ValueFuture<int>>>);
  static_assert(!std::is_move_constructible_v<FutureOrValue<ValueFuture<int>>>);
  static_assert(!std::is_move_assignable_v<FutureOrValue<ValueFuture<int>>>);

  static_assert(
      !std::is_copy_constructible_v<FutureOrValue<ValueFuture<void>>>);
  static_assert(!std::is_copy_assignable_v<FutureOrValue<ValueFuture<void>>>);
  static_assert(
      !std::is_move_constructible_v<FutureOrValue<ValueFuture<void>>>);
  static_assert(!std::is_move_assignable_v<FutureOrValue<ValueFuture<void>>>);

  FutureOrValue<ValueFuture<int>> fov;
  EXPECT_TRUE(fov.empty());
  EXPECT_FALSE(fov.has_value());
  EXPECT_FALSE(fov.has_future());
}

TEST(FutureOrValueTest, HasFuture) {
  DispatcherForTest dispatcher;
  ValueProvider<int> provider;

  FutureOrValue<ValueFuture<int>> fov;
  EXPECT_TRUE(fov.empty());
  EXPECT_FALSE(fov.has_future());

  fov = provider.Get();
  EXPECT_FALSE(fov.empty());
  EXPECT_TRUE(fov.has_future());

  FuncTask task([&](Context& cx) -> Poll<> {
    if (!fov.Advance(cx)) {
      return Pending();
    }
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  EXPECT_TRUE(fov.has_future());  // Still pending

  provider.Resolve(42);
  dispatcher.RunToCompletion();

  EXPECT_FALSE(fov.has_future());  // Now ready
  EXPECT_TRUE(fov.has_value());
}

TEST(FutureOrValueTest, AdvanceWithResolvedFuture) {
  DispatcherForTest dispatcher;

  FutureOrValue<ValueFuture<int>> fov;
  fov = ValueFuture<int>::Resolved(42);

  FuncTask task([&](Context& cx) -> Poll<> {
    fov.Advance(cx);
    return Ready();
  });

  dispatcher.Post(task);
  dispatcher.RunToCompletion();

  EXPECT_TRUE(fov.has_value());
  EXPECT_EQ(*fov, 42);
  EXPECT_EQ(fov.value(), 42);
}

struct Point {
  int x;
  int y;
};

TEST(FutureOrValueTest, MemberAccessAndConstDereference) {
  DispatcherForTest dispatcher;
  FutureOrValue<ValueFuture<Point>> fov(
      ValueFuture<Point>::Resolved(Point{10, 20}));

  FuncTask task([&](Context& cx) -> Poll<> {
    EXPECT_TRUE(fov.Advance(cx));
    return Ready();
  });
  dispatcher.Post(task);
  dispatcher.RunToCompletion();

  EXPECT_TRUE(fov.has_value());
  EXPECT_EQ(fov->x, 10);
  EXPECT_EQ(fov->y, 20);
  EXPECT_EQ(fov.value().x, 10);
  EXPECT_EQ(fov.value().y, 20);

  const auto& const_fov = fov;
  EXPECT_EQ((*const_fov).x, 10);
  EXPECT_EQ(const_fov->y, 20);
  EXPECT_EQ(const_fov.value().x, 10);
  EXPECT_EQ(const_fov.value().y, 20);
}

TEST(FutureOrValueTest, MoveOnlyValueType) {
  DispatcherForTest dispatcher;
  ValueProvider<std::unique_ptr<int>> provider;

  FutureOrValue<ValueFuture<std::unique_ptr<int>>> fov(provider.Get());

  std::unique_ptr<int> result;
  FuncTask task([&](Context& cx) -> Poll<> {
    if (!fov.Advance(cx)) {
      return Pending();
    }
    result = fov.Take();
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  provider.Resolve(std::make_unique<int>(99));
  dispatcher.RunToCompletion();

  ASSERT_NE(result, nullptr);
  EXPECT_EQ(*result, 99);
  EXPECT_FALSE(fov.has_value());
}

TEST(FutureOrValueTest, AdvanceMultipleTimes) {
  DispatcherForTest dispatcher;

  FutureOrValue<ValueFuture<int>> fov;
  fov = ValueFuture<int>::Resolved(42);

  int advance_count = 0;
  FuncTask task([&](Context& cx) -> Poll<> {
    if (fov.Advance(cx)) {
      advance_count++;
    }
    if (fov.Advance(cx)) {
      advance_count++;
    }
    return Ready();
  });

  dispatcher.Post(task);
  dispatcher.RunToCompletion();

  EXPECT_TRUE(fov.has_value());
  EXPECT_EQ(advance_count, 2);
}

TEST(FutureOrValueTest, AdvanceWithPendingFuture) {
  DispatcherForTest dispatcher;
  ValueProvider<int> provider;

  FutureOrValue<ValueFuture<int>> fov;
  fov = provider.Get();

  int result = -1;
  FuncTask task([&](Context& cx) -> Poll<> {
    if (!fov.Advance(cx)) {
      return Pending();
    }
    result = *fov;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  EXPECT_FALSE(fov.has_value());
  EXPECT_EQ(result, -1);

  provider.Resolve(100);

  dispatcher.RunToCompletion();

  EXPECT_TRUE(fov.has_value());
  EXPECT_EQ(result, 100);
}

TEST(FutureOrValueTest, TakeValue) {
  DispatcherForTest dispatcher;
  FutureOrValue<ValueFuture<int>> fov;
  fov = ValueFuture<int>::Resolved(42);

  FuncTask task([&](Context& cx) -> Poll<> {
    EXPECT_TRUE(fov.Advance(cx));
    return Ready();
  });
  dispatcher.Post(task);
  dispatcher.RunToCompletion();

  EXPECT_TRUE(fov.has_value());
  EXPECT_FALSE(fov.empty());
  int val = fov.Take();
  EXPECT_EQ(val, 42);
  EXPECT_TRUE(fov.empty());
  EXPECT_FALSE(fov.has_value());  // State goes to Empty
}

TEST(FutureOrValueTest, Reset) {
  DispatcherForTest dispatcher;
  FutureOrValue<ValueFuture<int>> fov;
  fov = ValueFuture<int>::Resolved(42);

  FuncTask task([&](Context& cx) -> Poll<> {
    EXPECT_TRUE(fov.Advance(cx));
    return Ready();
  });
  dispatcher.Post(task);
  dispatcher.RunToCompletion();

  EXPECT_TRUE(fov.has_value());
  EXPECT_FALSE(fov.empty());
  fov.Reset();
  EXPECT_TRUE(fov.empty());
  EXPECT_FALSE(fov.has_value());
}

class DestructionTrackingFuture {
 public:
  using value_type = int;

  DestructionTrackingFuture() = default;
  explicit DestructionTrackingFuture(bool* destroyed) : destroyed_(destroyed) {}

  DestructionTrackingFuture(DestructionTrackingFuture&& other) noexcept
      : destroyed_(other.destroyed_), pendable_(other.pendable_) {
    other.destroyed_ = nullptr;
    other.pendable_ = false;
  }

  DestructionTrackingFuture& operator=(
      DestructionTrackingFuture&& other) noexcept {
    if (destroyed_ != nullptr) {
      *destroyed_ = true;
    }
    destroyed_ = other.destroyed_;
    pendable_ = other.pendable_;
    other.destroyed_ = nullptr;
    other.pendable_ = false;
    return *this;
  }

  ~DestructionTrackingFuture() {
    if (destroyed_ != nullptr) {
      *destroyed_ = true;
    }
  }

  bool is_pendable() const { return pendable_; }
  bool is_complete() const { return !pendable_; }

  Poll<int> Pend(Context&) {
    pendable_ = false;
    return Ready(123);
  }

 private:
  bool* destroyed_ = nullptr;
  bool pendable_ = true;
};

TEST(FutureOrValueTest, DestroysFutureOnResolution) {
  DispatcherForTest dispatcher;
  bool future_destroyed = false;

  FutureOrValue<DestructionTrackingFuture> fov(
      DestructionTrackingFuture{&future_destroyed});

  EXPECT_FALSE(future_destroyed);
  EXPECT_TRUE(fov.has_future());

  FuncTask task([&](Context& cx) -> Poll<> {
    if (!fov.Advance(cx)) {
      return Pending();
    }
    return Ready();
  });

  dispatcher.Post(task);
  dispatcher.RunToCompletion();

  EXPECT_TRUE(future_destroyed);
  EXPECT_TRUE(fov.has_value());
  EXPECT_EQ(*fov, 123);
}

class DestructionTrackingVoidFuture {
 public:
  using value_type = void;

  DestructionTrackingVoidFuture() = default;
  explicit DestructionTrackingVoidFuture(bool* destroyed)
      : destroyed_(destroyed) {}

  DestructionTrackingVoidFuture(DestructionTrackingVoidFuture&& other) noexcept
      : destroyed_(other.destroyed_), pendable_(other.pendable_) {
    other.destroyed_ = nullptr;
    other.pendable_ = false;
  }

  DestructionTrackingVoidFuture& operator=(
      DestructionTrackingVoidFuture&& other) noexcept {
    if (destroyed_ != nullptr) {
      *destroyed_ = true;
    }
    destroyed_ = other.destroyed_;
    pendable_ = other.pendable_;
    other.destroyed_ = nullptr;
    other.pendable_ = false;
    return *this;
  }

  ~DestructionTrackingVoidFuture() {
    if (destroyed_ != nullptr) {
      *destroyed_ = true;
    }
  }

  bool is_pendable() const { return pendable_; }
  bool is_complete() const { return !pendable_; }

  Poll<> Pend(Context&) {
    pendable_ = false;
    return Ready();
  }

 private:
  bool* destroyed_ = nullptr;
  bool pendable_ = true;
};

TEST(FutureOrValueTest, DestroysVoidFutureOnResolution) {
  DispatcherForTest dispatcher;
  bool future_destroyed = false;

  FutureOrValue<DestructionTrackingVoidFuture> fov(
      DestructionTrackingVoidFuture{&future_destroyed});

  EXPECT_FALSE(future_destroyed);
  EXPECT_TRUE(fov.has_future());

  FuncTask task([&](Context& cx) -> Poll<> {
    if (!fov.Advance(cx)) {
      return Pending();
    }
    return Ready();
  });

  dispatcher.Post(task);
  dispatcher.RunToCompletion();

  EXPECT_TRUE(future_destroyed);
  EXPECT_TRUE(fov.has_value());
}

TEST(FutureOrValueTest, VoidFuture) {
  DispatcherForTest dispatcher;
  ValueProvider<void> provider;

  FutureOrValue<ValueFuture<void>> fov;
  fov = provider.Get();

  bool ready = false;
  FuncTask task([&](Context& cx) -> Poll<> {
    if (!fov.Advance(cx)) {
      return Pending();
    }
    ready = true;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  EXPECT_FALSE(fov.has_value());
  EXPECT_FALSE(ready);

  provider.Resolve();

  dispatcher.RunToCompletion();

  EXPECT_TRUE(fov.has_value());
  EXPECT_TRUE(ready);
}

TEST(FutureOrValueTest, VoidTakeAndReset) {
  DispatcherForTest dispatcher;
  ValueProvider<void> p1;
  FutureOrValue<ValueFuture<void>> fov(p1.Get());

  FuncTask task([&](Context& cx) -> Poll<> {
    if (!fov.Advance(cx)) {
      return Pending();
    }
    return Ready();
  });
  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  p1.Resolve();
  dispatcher.RunToCompletion();

  EXPECT_TRUE(fov.has_value());
  EXPECT_FALSE(fov.empty());
  fov.Take();
  EXPECT_TRUE(fov.empty());
  EXPECT_FALSE(fov.has_value());
  EXPECT_FALSE(fov.has_future());

  // Re-assign and Reset
  ValueProvider<void> p2;
  fov = p2.Get();
  EXPECT_FALSE(fov.empty());
  EXPECT_TRUE(fov.has_future());
  fov.Reset();
  EXPECT_TRUE(fov.empty());
  EXPECT_FALSE(fov.has_future());
  EXPECT_FALSE(fov.has_value());
}

TEST(FutureOrValueTest, ReassignmentCancelsPendingFuture) {
  bool future1_destroyed = false;
  bool future2_destroyed = false;

  FutureOrValue<DestructionTrackingFuture> fov(
      DestructionTrackingFuture{&future1_destroyed});

  EXPECT_FALSE(future1_destroyed);
  EXPECT_TRUE(fov.has_future());

  // Reassign new future cancels the previous one
  fov = DestructionTrackingFuture{&future2_destroyed};
  EXPECT_TRUE(future1_destroyed);
  EXPECT_FALSE(future2_destroyed);
  EXPECT_TRUE(fov.has_future());

  // Reset cancels the current future
  fov.Reset();
  EXPECT_TRUE(future2_destroyed);
  EXPECT_FALSE(fov.has_future());
  EXPECT_FALSE(fov.has_value());
}

TEST(FutureOrValueTest, MacroTryAdvance1) {
  DispatcherForTest dispatcher;
  ValueProvider<int> p1;
  FutureOrValue<ValueFuture<int>> fov1(p1.Get());

  int result = -1;
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_FOV_TRY_ADVANCE(cx, fov1);
    result = *fov1;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(result, -1);

  p1.Resolve(42);
  dispatcher.RunToCompletion();
  EXPECT_EQ(result, 42);
}

TEST(FutureOrValueTest, MacroTryAdvance2) {
  DispatcherForTest dispatcher;
  ValueProvider<int> p1;
  ValueProvider<int> p2;

  FutureOrValue<ValueFuture<int>> fov1(p1.Get());
  FutureOrValue<ValueFuture<int>> fov2(p2.Get());

  int result = -1;
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_FOV_TRY_ADVANCE(cx, fov1, fov2);
    result = *fov1 + *fov2;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(result, -1);

  p1.Resolve(10);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(result, -1);

  p2.Resolve(20);
  dispatcher.RunToCompletion();
  EXPECT_EQ(result, 30);
}

TEST(FutureOrValueTest, MacroTryAdvance3) {
  DispatcherForTest dispatcher;
  ValueProvider<int> p1, p2, p3;
  FutureOrValue<ValueFuture<int>> f1(p1.Get()), f2(p2.Get()), f3(p3.Get());

  int result = 0;
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_FOV_TRY_ADVANCE(cx, f1, f2, f3);
    result = *f1 + *f2 + *f3;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  p1.Resolve(1);
  p2.Resolve(2);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(result, 0);

  p3.Resolve(3);
  dispatcher.RunToCompletion();
  EXPECT_EQ(result, 6);
}

TEST(FutureOrValueTest, MacroTryAdvance4) {
  DispatcherForTest dispatcher;
  ValueProvider<int> p1, p2, p3, p4;
  FutureOrValue<ValueFuture<int>> f1(p1.Get()), f2(p2.Get()), f3(p3.Get()),
      f4(p4.Get());

  int result = 0;
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_FOV_TRY_ADVANCE(cx, f1, f2, f3, f4);
    result = *f1 + *f2 + *f3 + *f4;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  p1.Resolve(1);
  p2.Resolve(2);
  p3.Resolve(3);
  p4.Resolve(4);
  dispatcher.RunToCompletion();
  EXPECT_EQ(result, 10);
}

TEST(FutureOrValueTest, MacroTryAdvance5) {
  DispatcherForTest dispatcher;
  ValueProvider<int> p1, p2, p3, p4, p5;
  FutureOrValue<ValueFuture<int>> f1(p1.Get()), f2(p2.Get()), f3(p3.Get()),
      f4(p4.Get()), f5(p5.Get());

  int result = 0;
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_FOV_TRY_ADVANCE(cx, f1, f2, f3, f4, f5);
    result = *f1 + *f2 + *f3 + *f4 + *f5;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  p1.Resolve(1);
  p2.Resolve(2);
  p3.Resolve(3);
  p4.Resolve(4);
  p5.Resolve(5);
  dispatcher.RunToCompletion();
  EXPECT_EQ(result, 15);
}

TEST(FutureOrValueTest, MacroTryAdvance6) {
  DispatcherForTest dispatcher;
  ValueProvider<int> p1, p2, p3, p4, p5, p6;
  FutureOrValue<ValueFuture<int>> f1(p1.Get()), f2(p2.Get()), f3(p3.Get()),
      f4(p4.Get()), f5(p5.Get()), f6(p6.Get());

  int result = 0;
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_FOV_TRY_ADVANCE(cx, f1, f2, f3, f4, f5, f6);
    result = *f1 + *f2 + *f3 + *f4 + *f5 + *f6;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  p1.Resolve(1);
  p2.Resolve(2);
  p3.Resolve(3);
  p4.Resolve(4);
  p5.Resolve(5);
  p6.Resolve(6);
  dispatcher.RunToCompletion();
  EXPECT_EQ(result, 21);
}

TEST(FutureOrValueTest, MacroTryAdvance7) {
  DispatcherForTest dispatcher;
  ValueProvider<int> p1, p2, p3, p4, p5, p6, p7;
  FutureOrValue<ValueFuture<int>> f1(p1.Get()), f2(p2.Get()), f3(p3.Get()),
      f4(p4.Get()), f5(p5.Get()), f6(p6.Get()), f7(p7.Get());

  int result = 0;
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_FOV_TRY_ADVANCE(cx, f1, f2, f3, f4, f5, f6, f7);
    result = *f1 + *f2 + *f3 + *f4 + *f5 + *f6 + *f7;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  p1.Resolve(1);
  p2.Resolve(2);
  p3.Resolve(3);
  p4.Resolve(4);
  p5.Resolve(5);
  p6.Resolve(6);
  p7.Resolve(7);
  dispatcher.RunToCompletion();
  EXPECT_EQ(result, 28);
}

TEST(FutureOrValueTest, MacroTryAdvance8) {
  DispatcherForTest dispatcher;
  ValueProvider<int> p1, p2, p3, p4, p5, p6, p7, p8;
  FutureOrValue<ValueFuture<int>> f1(p1.Get()), f2(p2.Get()), f3(p3.Get()),
      f4(p4.Get()), f5(p5.Get()), f6(p6.Get()), f7(p7.Get()), f8(p8.Get());

  int result = 0;
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_FOV_TRY_ADVANCE(cx, f1, f2, f3, f4, f5, f6, f7, f8);
    result = *f1 + *f2 + *f3 + *f4 + *f5 + *f6 + *f7 + *f8;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  p1.Resolve(1);
  p2.Resolve(2);
  p3.Resolve(3);
  p4.Resolve(4);
  p5.Resolve(5);
  p6.Resolve(6);
  p7.Resolve(7);
  p8.Resolve(8);
  dispatcher.RunToCompletion();
  EXPECT_EQ(result, 36);
}

TEST(FutureOrValueTest, MacroTryAdvance9) {
  DispatcherForTest dispatcher;
  ValueProvider<int> p1, p2, p3, p4, p5, p6, p7, p8, p9;
  FutureOrValue<ValueFuture<int>> f1(p1.Get()), f2(p2.Get()), f3(p3.Get()),
      f4(p4.Get()), f5(p5.Get()), f6(p6.Get()), f7(p7.Get()), f8(p8.Get()),
      f9(p9.Get());

  int result = 0;
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_FOV_TRY_ADVANCE(cx, f1, f2, f3, f4, f5, f6, f7, f8, f9);
    result = *f1 + *f2 + *f3 + *f4 + *f5 + *f6 + *f7 + *f8 + *f9;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  p1.Resolve(1);
  p2.Resolve(2);
  p3.Resolve(3);
  p4.Resolve(4);
  p5.Resolve(5);
  p6.Resolve(6);
  p7.Resolve(7);
  p8.Resolve(8);
  p9.Resolve(9);
  dispatcher.RunToCompletion();
  EXPECT_EQ(result, 45);
}

TEST(FutureOrValueTest, MacroTryAdvanceMixedValueAndVoid) {
  DispatcherForTest dispatcher;
  ValueProvider<int> p_val;
  ValueProvider<void> p_void;

  FutureOrValue<ValueFuture<int>> fov_val(p_val.Get());
  FutureOrValue<ValueFuture<void>> fov_void(p_void.Get());

  int result = -1;
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_FOV_TRY_ADVANCE(cx, fov_val, fov_void);
    result = *fov_val;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(result, -1);

  p_val.Resolve(42);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(result, -1);

  p_void.Resolve();
  dispatcher.RunToCompletion();
  EXPECT_EQ(result, 42);
}

}  // namespace
}  // namespace pw::async2
