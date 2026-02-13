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

#include "pw_async2/future.h"

#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/func_task.h"
#include "pw_async2/try.h"
#include "pw_compilation_testing/negative_compilation.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::async2::Context;
using pw::async2::DispatcherForTest;
using pw::async2::FuncTask;
using pw::async2::Future;
using pw::async2::Pending;
using pw::async2::Poll;
using pw::async2::Ready;

static_assert(!Future<int>);
static_assert(!Future<pw::async2::FutureCore>);

class FakeFuture {
 public:
  using value_type = int;
  Poll<int> Pend(Context& cx);
  bool is_pendable() const;
  bool is_complete() const;
};
static_assert(Future<FakeFuture>);

class MissingValueType {
 public:
  Poll<int> Pend(Context& cx);
  bool is_pendable() const;
  bool is_complete() const;
};
static_assert(!Future<MissingValueType>);

class MissingPend {
 public:
  using value_type = int;
  bool is_pendable() const;
  bool is_complete() const;
};
static_assert(!Future<MissingPend>);

class ExtraArgIsComplete {
 public:
  using value_type = int;
  Poll<int> Pend(Context& cx);
  bool is_pendable() const;
  bool is_complete(bool maybe_not = false) const;
};
static_assert(!Future<ExtraArgIsComplete>);

class MissingPendable {
 public:
  using value_type = int;
  Poll<int> Pend(Context& cx);
  bool is_complete() const;
};
static_assert(!Future<MissingPendable>);

class WrongReturnPendable {
 public:
  using value_type = int;
  Poll<int> Pend(Context& cx);
  int is_pendable() const;
  bool is_complete() const;
};
static_assert(!Future<WrongReturnPendable>);

class NonConstIsComplete {
 public:
  using value_type = int;
  Poll<int> Pend(Context& cx);
  bool is_pendable() const;
  bool is_complete();
};
static_assert(!Future<NonConstIsComplete>);

class MissingIsComplete {
 public:
  using value_type = int;
  Poll<int> Pend(Context& cx);
  bool is_pendable() const;
};
static_assert(!Future<MissingIsComplete>);

class WrongPendSignature {
 public:
  using value_type = int;
  Poll<int> Pend();  // Missing Context&
  bool is_pendable() const;
  bool is_complete() const;
};
static_assert(!Future<WrongPendSignature>);

class WrongPendReturnType {
 public:
  using value_type = int;
  void Pend(Context& cx);
  bool is_pendable() const;
  bool is_complete() const;
};
static_assert(!Future<WrongPendReturnType>);

class ExtraArgPend {
 public:
  using value_type = int;
  Poll<int> Pend(Context& cx, int extra = 0);
  bool is_pendable() const;
  bool is_complete() const;
};
static_assert(!Future<ExtraArgPend>);

class NonDestructible {
 public:
  ~NonDestructible() = delete;

  using value_type = int;
  Poll<int> Pend(Context& cx);
  bool is_pendable() const;
  bool is_complete() const;
};
static_assert(!Future<NonDestructible>);

class NotDefaultConstructible {
 public:
  NotDefaultConstructible() = delete;

  using value_type = int;
  Poll<int> Pend(Context& cx);
  bool is_pendable() const;
  bool is_complete() const;
};
static_assert(!Future<NotDefaultConstructible>);

#if PW_NC_TEST(FutureWaitReasonMustBeProvided)
PW_NC_EXPECT("kWaitReason");

class BadFuture {
 public:
  Poll<int> Pend(Context& cx) { return core_.DoPend(*this, cx); }

 private:
  friend class pw::async2::FutureCore;

  Poll<int> DoPend(Context&) { return 5; }

  pw::async2::FutureCore core_;
};

[[maybe_unused]] void ShouldAssert() {
  BadFuture future;
  FuncTask task([&](Context& cx) { return future.Pend(cx).Readiness(); });
}
#endif  // PW_NC_TEST

class TestAsyncInt;

class TestIntFuture {
 public:
  using value_type = int;

  constexpr TestIntFuture() : async_int_(nullptr) {}

  TestIntFuture(TestIntFuture&& other) noexcept
      : core_(std::move(other.core_)),
        async_int_(std::exchange(other.async_int_, nullptr)) {}

  TestIntFuture& operator=(TestIntFuture&& other) noexcept {
    if (this != &other) {
      core_ = std::move(other.core_);
      async_int_ = std::exchange(other.async_int_, nullptr);
    }
    return *this;
  }

  Poll<int> Pend(Context& cx) { return core_.DoPend(*this, cx); }

  // Exposed for testing.
  const pw::async2::FutureCore& core() const { return core_; }

  [[nodiscard]] bool is_pendable() const { return core_.is_pendable(); }

  [[nodiscard]] bool is_complete() const { return core_.is_complete(); }

 private:
  friend class TestAsyncInt;
  friend class pw::async2::FutureCore;

  static constexpr const char kWaitReason[] = "TestIntFuture";

  TestIntFuture(TestAsyncInt& async_int);

  TestIntFuture(TestAsyncInt& async_int,
                pw::async2::FutureState::ReadyForCompletion)
      : core_(pw::async2::FutureState::kReadyForCompletion),
        async_int_(&async_int) {}

  Poll<int> DoPend(Context&);

  pw::async2::FutureCore core_;
  TestAsyncInt* async_int_;
};

class TestAsyncInt {
 public:
  TestIntFuture Get() { return TestIntFuture(*this); }

  TestIntFuture SetAndGet(int value) {
    PW_ASSERT(!value_.has_value());
    value_ = value;
    return TestIntFuture(*this, pw::async2::FutureState::kReadyForCompletion);
  }

  void Set(int value) {
    PW_ASSERT(!value_.has_value());
    value_ = value;
    list_.ResolveAll();
  }

  void WakeAllFuturesWithoutSettingValue() {
    TestIntFuture* future;
    while ((future = list_.PopIfAvailable()) != nullptr) {
      future->core_.Wake();
    }
  }

  pw::async2::FutureList<&TestIntFuture::core_>& list() { return list_; }

 private:
  friend class TestIntFuture;

  pw::async2::FutureList<&TestIntFuture::core_> list_;
  std::optional<int> value_;
};

TestIntFuture::TestIntFuture(TestAsyncInt& async_int)
    : core_(pw::async2::FutureState::kPending), async_int_(&async_int) {
  async_int_->list_.Push(core_);
}

Poll<int> TestIntFuture::DoPend(Context&) {
  PW_ASSERT(async_int_ != nullptr);
  if (async_int_->value_.has_value()) {
    return Ready(*async_int_->value_);
  }
  if (!core_.in_list()) {
    async_int_->list_.Push(core_);
  }
  return Pending();
}

static_assert(Future<TestIntFuture>);

TEST(FutureCore, Pend) {
  DispatcherForTest dispatcher;
  TestAsyncInt provider;

  TestIntFuture future = provider.Get();
  EXPECT_TRUE(future.core().is_pendable());
  EXPECT_FALSE(future.core().is_ready());
  EXPECT_FALSE(future.core().is_complete());

  FuncTask task([&](Context& cx) -> Poll<> {
    PW_TRY_READY_ASSIGN(int value, future.Pend(cx));
    EXPECT_EQ(value, 42);
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  provider.Set(42);
  dispatcher.RunToCompletion();

  EXPECT_TRUE(future.core().is_complete());
  EXPECT_FALSE(future.core().is_pendable());
}

TEST(FutureCore, PendReady) {
  DispatcherForTest dispatcher;
  TestAsyncInt provider;

  TestIntFuture future = provider.SetAndGet(65535);
  EXPECT_TRUE(future.core().is_pendable());
  EXPECT_TRUE(future.core().is_ready());
  EXPECT_FALSE(future.core().is_complete());

  FuncTask task([&](Context& cx) -> Poll<> {
    PW_TRY_READY_ASSIGN(int value, future.Pend(cx));
    EXPECT_EQ(value, 65535);
    return Ready();
  });

  dispatcher.Post(task);
  dispatcher.RunToCompletion();

  EXPECT_TRUE(future.core().is_complete());
  EXPECT_FALSE(future.core().is_pendable());
}

TEST(FutureCore, MoveAssign) {
  DispatcherForTest dispatcher;
  TestAsyncInt provider;

  TestIntFuture future1 = provider.Get();
  TestIntFuture future2 = provider.Get();

  future1 = std::move(future2);

  int result = -1;
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_TRY_READY_ASSIGN(int value, future1.Pend(cx));
    result = value;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  provider.Set(100);
  dispatcher.RunToCompletion();
  EXPECT_EQ(result, 100);
}

TEST(FutureCore, MoveConstruct) {
  DispatcherForTest dispatcher;
  TestAsyncInt provider;

  TestIntFuture future1 = provider.Get();
  TestIntFuture future2(std::move(future1));

  int result = -1;
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_TRY_READY_ASSIGN(int value, future2.Pend(cx));
    result = value;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  provider.Set(101);
  dispatcher.RunToCompletion();
  EXPECT_EQ(result, 101);
}

TEST(FutureCore, MultipleFutures) {
  DispatcherForTest dispatcher;
  TestAsyncInt provider;

  TestIntFuture future1 = provider.Get();
  TestIntFuture future2 = provider.Get();
  int result1 = -1;
  int result2 = -1;

  FuncTask task1([&](Context& cx) -> Poll<> {
    PW_TRY_READY_ASSIGN(int value, future1.Pend(cx));
    result1 = value;
    return Ready();
  });

  FuncTask task2([&](Context& cx) -> Poll<> {
    PW_TRY_READY_ASSIGN(int value, future2.Pend(cx));
    result2 = value;
    return Ready();
  });

  dispatcher.Post(task1);
  dispatcher.Post(task2);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  provider.Set(77);
  dispatcher.RunToCompletion();
  EXPECT_EQ(result1, 77);
  EXPECT_EQ(result2, 77);
}

TEST(FutureCore, RelistsItselfOnPending) {
  DispatcherForTest dispatcher;
  TestAsyncInt provider;

  TestIntFuture future = provider.Get();

  int result = -1;
  FuncTask task([&](Context& cx) -> Poll<> {
    PW_TRY_READY_ASSIGN(int value, future.Pend(cx));
    result = value;
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(dispatcher.tasks_polled(), 1u);

  // Spuriously wake the futures.
  provider.WakeAllFuturesWithoutSettingValue();

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(dispatcher.tasks_polled(), 2u);

  provider.Set(88);
  dispatcher.RunToCompletion();
  EXPECT_EQ(dispatcher.tasks_polled(), 3u);
  EXPECT_EQ(result, 88);
}

TEST(FutureCore, Unlist) {
  pw::async2::BaseFutureList list;
  pw::async2::FutureCore core(pw::async2::FutureState::kPending);
  list.Push(core);
  EXPECT_TRUE(core.in_list());

  core.Unlist();
  EXPECT_FALSE(core.in_list());
  EXPECT_TRUE(core.is_initialized());
  EXPECT_TRUE(list.empty());
}

TEST(FutureCore, Reset) {
  pw::async2::BaseFutureList list;
  pw::async2::FutureCore core(pw::async2::FutureState::kPending);

  list.Push(core);
  EXPECT_TRUE(core.in_list());

  core.Reset();
  EXPECT_FALSE(core.in_list());
  EXPECT_FALSE(core.is_initialized());
  EXPECT_TRUE(list.empty());
}

TEST(FutureCore, WakeAndMarkReady) {
  pw::async2::FutureCore core(pw::async2::FutureState::kPending);
  EXPECT_FALSE(core.is_ready());

  core.WakeAndMarkReady();
  EXPECT_TRUE(core.is_ready());
  EXPECT_TRUE(core.is_pendable());
}

TEST(FutureState, DefaultConstruction) {
  pw::async2::FutureState state;
  EXPECT_FALSE(state.is_initialized());
  EXPECT_FALSE(state.is_pendable());
  EXPECT_FALSE(state.is_ready());
  EXPECT_FALSE(state.is_complete());
}

TEST(FutureState, PendingConstruction) {
  pw::async2::FutureState state(pw::async2::FutureState::kPending);
  EXPECT_TRUE(state.is_initialized());
  EXPECT_TRUE(state.is_pendable());
  EXPECT_FALSE(state.is_ready());
  EXPECT_FALSE(state.is_complete());
}

TEST(FutureState, ReadyForCompletionConstruction) {
  pw::async2::FutureState state(pw::async2::FutureState::kReadyForCompletion);
  EXPECT_TRUE(state.is_initialized());
  EXPECT_TRUE(state.is_pendable());
  EXPECT_TRUE(state.is_ready());
  EXPECT_FALSE(state.is_complete());
}

TEST(FutureState, MarkReady) {
  pw::async2::FutureState state(pw::async2::FutureState::kPending);
  state.MarkReady();
  EXPECT_TRUE(state.is_ready());
  EXPECT_TRUE(state.is_pendable());
  EXPECT_FALSE(state.is_complete());
}

TEST(FutureState, MarkComplete) {
  pw::async2::FutureState state(pw::async2::FutureState::kPending);
  state.MarkComplete();
  EXPECT_TRUE(state.is_complete());
  EXPECT_FALSE(state.is_pendable());
  EXPECT_FALSE(state.is_ready());
}

TEST(FutureState, MarkCompleteFromReady) {
  pw::async2::FutureState state(pw::async2::FutureState::kReadyForCompletion);
  state.MarkComplete();
  EXPECT_TRUE(state.is_complete());
  EXPECT_FALSE(state.is_pendable());
  EXPECT_TRUE(state.is_ready());
}

TEST(FutureState, Move) {
  pw::async2::FutureState s1(pw::async2::FutureState::kPending);
  pw::async2::FutureState s2 = std::move(s1);

  EXPECT_FALSE(s1.is_initialized());  // NOLINT(bugprone-use-after-move)
  EXPECT_TRUE(s2.is_initialized());
  EXPECT_TRUE(s2.is_pendable());
}

TEST(FutureState, MoveAssignment) {
  pw::async2::FutureState s1(pw::async2::FutureState::kPending);
  pw::async2::FutureState s2;
  s2 = std::move(s1);

  EXPECT_FALSE(s1.is_initialized());  // NOLINT(bugprone-use-after-move)
  EXPECT_TRUE(s2.is_initialized());
  EXPECT_TRUE(s2.is_pendable());
}

TEST(CustomFutureList, Remove) {
  TestAsyncInt provider;
  TestIntFuture f1 = provider.Get();
  TestIntFuture f2 = provider.Get();
  TestIntFuture f3 = provider.Get();

  TestIntFuture* removed =
      provider.list().Remove([&](TestIntFuture& f) { return &f == &f2; });
  EXPECT_EQ(removed, &f2);

  EXPECT_FALSE(f2.core().in_list());
  EXPECT_TRUE(f1.core().in_list());
  EXPECT_TRUE(f3.core().in_list());

  removed = provider.list().Remove([&](TestIntFuture& f) { return &f == &f1; });
  EXPECT_EQ(removed, &f1);
  EXPECT_FALSE(f1.core().in_list());

  removed = provider.list().Remove([&](TestIntFuture& f) { return &f == &f1; });
  EXPECT_EQ(removed, nullptr);

  removed = provider.list().Remove([&](TestIntFuture& f) { return &f == &f3; });
  EXPECT_EQ(removed, &f3);
  EXPECT_TRUE(provider.list().empty());
}

}  // namespace
