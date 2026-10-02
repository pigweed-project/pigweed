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

#include "pw_rpc2/internal/method_future.h"

#include <cstddef>
#include <utility>

#include "pw_allocator/testing.h"
#include "pw_async2/await.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/future.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
#include "pw_unit_test/framework.h"

namespace pw::rpc2::internal {
namespace {

static_assert(async2::Future<BoxedMethodFuture>);

class MockFuture {
 public:
  using value_type = void;

  explicit MockFuture(int pends_before_ready = 0,
                      int* destruction_counter = nullptr)
      : pends_remaining_(pends_before_ready),
        destruction_counter_(destruction_counter) {}

  ~MockFuture() {
    if (destruction_counter_ != nullptr) {
      ++(*destruction_counter_);
    }
  }

  MockFuture(const MockFuture&) = delete;
  MockFuture& operator=(const MockFuture&) = delete;

  MockFuture(MockFuture&& other) noexcept
      : pends_remaining_(other.pends_remaining_),
        destruction_counter_(
            std::exchange(other.destruction_counter_, nullptr)) {}

  MockFuture& operator=(MockFuture&& other) noexcept {
    if (this != &other) {
      if (destruction_counter_ != nullptr) {
        ++(*destruction_counter_);
      }
      pends_remaining_ = other.pends_remaining_;
      destruction_counter_ = std::exchange(other.destruction_counter_, nullptr);
    }
    return *this;
  }

  bool is_pendable() const { return pends_remaining_ >= 0; }
  bool is_complete() const { return pends_remaining_ == 0; }

  async2::Poll<void> Pend(async2::Context& cx) {
    if (pends_remaining_ <= 0) {
      return async2::Ready();
    }
    --pends_remaining_;
    if (pends_remaining_ == 0) {
      return async2::Ready();
    }
    cx.ReEnqueue();
    return async2::Pending();
  }

 private:
  int pends_remaining_;
  int* destruction_counter_ = nullptr;
};

class DriverTask : public async2::Task {
 public:
  explicit DriverTask(BoxedMethodFuture future)
      : Task(PW_ASYNC_TASK_NAME("DriverTask")), future_(std::move(future)) {}

  bool completed() const { return completed_; }

 private:
  async2::Poll<> DoPend(async2::Context& cx) override {
    PW_AWAIT(future_, cx);
    completed_ = true;
    return async2::Ready();
  }

  BoxedMethodFuture future_;
  bool completed_ = false;
};

TEST(MethodFutureTest, DefaultConstructedIsEmpty) {
  BoxedMethodFuture future;
  EXPECT_FALSE(future.is_pendable());
  EXPECT_FALSE(future.is_complete());
}

TEST(MethodFutureTest, ImmediateCompletion) {
  alignas(std::max_align_t) std::byte storage[256];
  int dtor_count = 0;

  auto future = BoxedMethodFuture::Emplace<MockFuture>(
      storage, [&] { return MockFuture(1, &dtor_count); });

  EXPECT_TRUE(future.is_pendable());
  EXPECT_FALSE(future.is_complete());
  EXPECT_EQ(dtor_count, 0);

  async2::DispatcherForTest dispatcher;
  DriverTask task(std::move(future));
  dispatcher.Post(task);
  dispatcher.RunToCompletion();

  EXPECT_TRUE(task.completed());
  // The future was destroyed in-place upon completion.
  EXPECT_EQ(dtor_count, 1);
}

TEST(MethodFutureTest, MultiStepCompletion) {
  alignas(std::max_align_t) std::byte storage[256];
  int dtor_count = 0;

  auto future = BoxedMethodFuture::Emplace<MockFuture>(
      storage, [&] { return MockFuture(3, &dtor_count); });

  async2::DispatcherForTest dispatcher;
  DriverTask task(std::move(future));
  dispatcher.Post(task);
  dispatcher.RunToCompletion();

  EXPECT_TRUE(task.completed());
  EXPECT_EQ(dtor_count, 1);
}

TEST(MethodFutureTest, EarlyDestructionInvokesDestructor) {
  alignas(std::max_align_t) std::byte storage[256];
  int dtor_count = 0;

  {
    auto future = BoxedMethodFuture::Emplace<MockFuture>(
        storage, [&] { return MockFuture(5, &dtor_count); });
    EXPECT_TRUE(future.is_pendable());
    EXPECT_EQ(dtor_count, 0);
  }
  // Dropping BoxedMethodFuture before completion calls the inner destructor.
  EXPECT_EQ(dtor_count, 1);
}

TEST(MethodFutureTest, ResetDestroysFuture) {
  alignas(std::max_align_t) std::byte storage[256];
  int dtor_count = 0;

  auto future = BoxedMethodFuture::Emplace<MockFuture>(
      storage, [&] { return MockFuture(5, &dtor_count); });
  EXPECT_TRUE(future.is_pendable());
  EXPECT_EQ(dtor_count, 0);

  future.Reset();
  EXPECT_FALSE(future.is_pendable());
  EXPECT_FALSE(future.is_complete());
  EXPECT_EQ(dtor_count, 1);
}

TEST(MethodFutureTest, ResetIsIdempotent) {
  alignas(std::max_align_t) std::byte storage[256];
  int dtor_count = 0;

  auto future = BoxedMethodFuture::Emplace<MockFuture>(
      storage, [&] { return MockFuture(5, &dtor_count); });
  future.Reset();
  future.Reset();
  EXPECT_EQ(dtor_count, 1);
}

TEST(MethodFutureTest, CompletedFutureRemainsCompleteAfterReset) {
  alignas(std::max_align_t) std::byte storage[256];
  int dtor_count = 0;

  auto future = BoxedMethodFuture::Emplace<MockFuture>(
      storage, [&] { return MockFuture(1, &dtor_count); });
  EXPECT_FALSE(future.is_complete());

  async2::DispatcherForTest dispatcher;
  EXPECT_TRUE(dispatcher.RunInTaskUntilStalled(future).IsReady());

  // Completing the future destroys the implementation, but the future still
  // reports completion.
  EXPECT_TRUE(future.is_complete());
  EXPECT_FALSE(future.is_pendable());
  EXPECT_EQ(dtor_count, 1);

  // Resetting a completed future does not make it look incomplete.
  future.Reset();
  EXPECT_TRUE(future.is_complete());
  EXPECT_EQ(dtor_count, 1);
}

TEST(MethodFutureTest, MoveTransfersCompletion) {
  alignas(std::max_align_t) std::byte storage[256];
  int dtor_count = 0;

  auto future = BoxedMethodFuture::Emplace<MockFuture>(
      storage, [&] { return MockFuture(1, &dtor_count); });

  async2::DispatcherForTest dispatcher;
  EXPECT_TRUE(dispatcher.RunInTaskUntilStalled(future).IsReady());
  EXPECT_TRUE(future.is_complete());

  BoxedMethodFuture moved(std::move(future));
  EXPECT_TRUE(moved.is_complete());
  EXPECT_FALSE(future.is_complete());  // NOLINT(bugprone-use-after-move)
}

TEST(MethodFutureTest, MoveTransfersOwnership) {
  alignas(std::max_align_t) std::byte storage[256];
  int dtor_count = 0;

  auto future1 = BoxedMethodFuture::Emplace<MockFuture>(
      storage, [&] { return MockFuture(1, &dtor_count); });
  EXPECT_TRUE(future1.is_pendable());

  BoxedMethodFuture future2(std::move(future1));
  EXPECT_FALSE(future1.is_pendable());  // NOLINT(bugprone-use-after-move)
  EXPECT_TRUE(future2.is_pendable());
  EXPECT_EQ(dtor_count, 0);

  BoxedMethodFuture future3;
  future3 = std::move(future2);
  EXPECT_FALSE(future2.is_pendable());  // NOLINT(bugprone-use-after-move)
  EXPECT_TRUE(future3.is_pendable());
  EXPECT_EQ(dtor_count, 0);

  async2::DispatcherForTest dispatcher;
  DriverTask task(std::move(future3));
  dispatcher.Post(task);
  dispatcher.RunToCompletion();

  EXPECT_TRUE(task.completed());
  EXPECT_EQ(dtor_count, 1);
}

#if defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")

// Coroutine tests
async2::Coro<void> ImmediateCoro(async2::CoroContext) { co_return; }

async2::Coro<void> AwaitingCoro(async2::CoroContext, MockFuture fut) {
  co_await fut;
  co_return;
}

async2::Coro<void> ParentCoroWithFailingChild(async2::CoroContext,
                                              Allocator& failing_alloc,
                                              MockFuture fut) {
  co_await fut;
  co_await ImmediateCoro(async2::CoroContext(failing_alloc));
  co_return;
}

TEST(CoroFutureTest, ImmediateCoroCompletesAndDeallocates) {
  pw::allocator::test::AllocatorForTest<2048> alloc;
  alignas(std::max_align_t)
      std::byte storage[sizeof(MethodFutureImpl<async2::Coro<void>>)];

  auto future = BoxedMethodFuture::Emplace<async2::Coro<void>>(
      storage, [&] { return ImmediateCoro(async2::CoroContext(alloc)); });

  EXPECT_TRUE(future.is_pendable());
  EXPECT_FALSE(future.is_complete());

  async2::DispatcherForTest dispatcher;
  DriverTask task(std::move(future));
  dispatcher.Post(task);
  dispatcher.RunToCompletion();

  EXPECT_TRUE(task.completed());
  // Verify coroutine frame was deallocated
  EXPECT_EQ(alloc.GetAllocated(), 0u);
}

TEST(CoroFutureTest, AwaitingCoroCompletesAndDestructsLocals) {
  pw::allocator::test::AllocatorForTest<2048> alloc;
  alignas(std::max_align_t)
      std::byte storage[sizeof(MethodFutureImpl<async2::Coro<void>>)];
  int dtor_count = 0;

  auto future = BoxedMethodFuture::Emplace<async2::Coro<void>>(storage, [&] {
    return AwaitingCoro(async2::CoroContext(alloc), MockFuture(2, &dtor_count));
  });

  async2::DispatcherForTest dispatcher;
  DriverTask task(std::move(future));
  dispatcher.Post(task);
  dispatcher.RunToCompletion();

  EXPECT_TRUE(task.completed());
  EXPECT_EQ(dtor_count, 1);
  EXPECT_EQ(alloc.GetAllocated(), 0u);
}

TEST(CoroFutureTest, EarlyDestructionDeallocatesCoroFrameAndLocals) {
  pw::allocator::test::AllocatorForTest<2048> alloc;
  alignas(std::max_align_t)
      std::byte storage[sizeof(MethodFutureImpl<async2::Coro<void>>)];
  int dtor_count = 0;

  {
    auto future = BoxedMethodFuture::Emplace<async2::Coro<void>>(storage, [&] {
      return AwaitingCoro(async2::CoroContext(alloc),
                          MockFuture(10, &dtor_count));
    });
    EXPECT_TRUE(future.is_pendable());
    EXPECT_GT(alloc.GetAllocated(), 0u);
  }
  // Dropping future cancels in-flight coroutine: frame and local variables
  // are destroyed immediately.
  EXPECT_EQ(dtor_count, 1);
  EXPECT_EQ(alloc.GetAllocated(), 0u);
}

TEST(CoroFutureTest,
     NestedCoroAllocationFailureLeavesBoxedMethodFutureIncomplete) {
  pw::allocator::test::AllocatorForTest<2048> alloc;
  pw::allocator::test::AllocatorForTest<64> failing_alloc;
  failing_alloc.Exhaust();
  alignas(std::max_align_t)
      std::byte storage[sizeof(MethodFutureImpl<async2::Coro<void>>)];
  int dtor_count = 0;

  auto future = BoxedMethodFuture::Emplace<async2::Coro<void>>(storage, [&] {
    return ParentCoroWithFailingChild(
        async2::CoroContext(alloc), failing_alloc, MockFuture(2, &dtor_count));
  });

  async2::DispatcherForTest dispatcher;
  EXPECT_TRUE(dispatcher.RunInTaskUntilStalled(future).IsReady());
  EXPECT_FALSE(future.is_pendable());
  EXPECT_FALSE(future.is_complete());
  EXPECT_EQ(dtor_count, 1);
  EXPECT_EQ(alloc.GetAllocated(), 0u);
}

TEST(CoroFutureTest, SizeAndAlignmentConstants) {
  using Impl = MethodFutureImpl<async2::Coro<void>>;
  EXPECT_EQ(sizeof(Impl), sizeof(void*) + sizeof(async2::Coro<void>));
  EXPECT_EQ(alignof(Impl), alignof(void*));
  EXPECT_EQ(sizeof(BoxedMethodFuture), sizeof(void*));
}

#endif  // defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")

}  // namespace
}  // namespace pw::rpc2::internal
