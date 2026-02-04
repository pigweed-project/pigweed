// Copyright 2024 The Pigweed Authors
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

#include "pw_multibuf/v1/allocator.h"

#include "gtest/gtest.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/poll.h"
#include "pw_multibuf/v1/allocator_async.h"

namespace pw::multibuf::v1 {
namespace {

using ::pw::async2::Context;
using ::pw::async2::DispatcherForTest;
using ::pw::async2::Pending;
using ::pw::async2::Poll;
using ::pw::async2::PollOptional;
using ::pw::async2::Ready;
using ::pw::async2::Task;
using ::pw::multibuf::v1::ContiguityRequirement;
using ::pw::multibuf::v1::kAllowDiscontiguous;

struct AllocateExpectation {
  size_t min_size;
  size_t desired_size;
  ContiguityRequirement contiguous;
  pw::Result<MultiBuf> result;
};

class MockMultiBufAllocator : public MultiBufAllocator {
 public:
  ~MockMultiBufAllocator() override {
    // All expectations should have been met and removed.
    EXPECT_EQ(expected_allocate_, std::nullopt);
  }

  void ExpectAllocateAndReturn(size_t min_size,
                               size_t desired_size,
                               ContiguityRequirement contiguous,
                               pw::Result<MultiBuf> result) {
    // Multiple simultaneous expectations are not supported.
    ASSERT_FALSE(expected_allocate_.has_value());
    expected_allocate_ = {
        min_size, desired_size, contiguous, std::move(result)};
  }

  using MultiBufAllocator::MoreMemoryAvailable;

 private:
  pw::Result<MultiBuf> DoAllocate(size_t min_size,
                                  size_t desired_size,
                                  ContiguityRequirement contiguous) final {
    EXPECT_NE(expected_allocate_, std::nullopt);
    if (!expected_allocate_.has_value()) {
      return Status::FailedPrecondition();
    }
    AllocateExpectation expected = std::move(*expected_allocate_);
    expected_allocate_ = std::nullopt;
    EXPECT_EQ(min_size, expected.min_size);
    EXPECT_EQ(desired_size, expected.desired_size);
    EXPECT_EQ(contiguous, expected.contiguous);
    return std::move(expected.result);
  }
  std::optional<size_t> DoGetBackingCapacity() final { return {}; }

  std::optional<AllocateExpectation> expected_allocate_;
};

// ########## MultiBufAllocatorAsync

class AllocateTask : public Task {
 public:
  AllocateTask(MultiBufAllocationFuture&& future)
      : future_(std::move(future)), last_result_(Pending()) {}

  MultiBufAllocationFuture future_;
  PollOptional<MultiBuf> last_result_;

 private:
  Poll<> DoPend(Context& cx) override {
    last_result_ = future_.Pend(cx);
    if (last_result_.IsReady()) {
      return Ready();
    }
    return Pending();
  }
};

TEST(MultiBufAllocatorAsync, MultiBufAllocationFutureCtor) {
  MockMultiBufAllocator mbuf_alloc;
  MultiBufAllocatorAsync async_alloc{mbuf_alloc};
  {
    MultiBufAllocationFuture fut = async_alloc.AllocateAsync(44u, 33u);
    EXPECT_EQ(44u, fut.min_size());
    EXPECT_EQ(33u, fut.desired_size());
    EXPECT_FALSE(fut.needs_contiguous());
    EXPECT_EQ(&mbuf_alloc, &fut.allocator());
  }
  {
    MultiBufAllocationFuture fut =
        async_alloc.AllocateContiguousAsync(66u, 55u);
    EXPECT_EQ(66u, fut.min_size());
    EXPECT_EQ(55u, fut.desired_size());
    EXPECT_TRUE(fut.needs_contiguous());
    EXPECT_EQ(&mbuf_alloc, &fut.allocator());
  }
}

TEST(MultiBufAllocatorAsync,
     AllocateAsyncReturnsImmediatelyAvailableAllocation) {
  MockMultiBufAllocator mbuf_alloc;
  MultiBufAllocatorAsync async_alloc{mbuf_alloc};

  AllocateTask task(async_alloc.AllocateAsync(44, 33));
  mbuf_alloc.ExpectAllocateAndReturn(44, 33, kAllowDiscontiguous, MultiBuf());

  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  dispatcher.RunToCompletion();

  ASSERT_TRUE(task.last_result_.IsReady());
  ASSERT_TRUE(task.last_result_->has_value());
}

TEST(MultiBufAllocatorAsync, AllocateAsyncWillNotPollUntilMoreMemoryAvailable) {
  MockMultiBufAllocator mbuf_alloc;
  MultiBufAllocatorAsync async_alloc{mbuf_alloc};

  AllocateTask task(async_alloc.AllocateAsync(44, 33));
  DispatcherForTest dispatcher;
  dispatcher.Post(task);

  // First attempt will return `ResourceExhausted` to signal temporary OOM.
  mbuf_alloc.ExpectAllocateAndReturn(
      44, 33, kAllowDiscontiguous, Status::ResourceExhausted());
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_TRUE(task.last_result_.IsPending());

  // Re-running the dispatcher should not poll the pending task since it has
  // not been awoken. `AllocateAndReturn` should *not* be called.
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  // Insufficient memory should not awaken the task.
  mbuf_alloc.MoreMemoryAvailable(30, 30);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  // Sufficient memory will awaken and return the memory
  mbuf_alloc.MoreMemoryAvailable(50, 50);
  mbuf_alloc.ExpectAllocateAndReturn(44, 33, kAllowDiscontiguous, MultiBuf());
  dispatcher.RunToCompletion();
}

TEST(MultiBufAllocatorAsync, MoveMultiBufAllocationFuture) {
  MockMultiBufAllocator mbuf_alloc;
  MultiBufAllocatorAsync async_alloc{mbuf_alloc};
  MultiBufAllocationFuture fut1 = async_alloc.AllocateAsync(44u, 33u);
  EXPECT_EQ(44u, fut1.min_size());
  EXPECT_EQ(33u, fut1.desired_size());

  // Test move ctor
  MultiBufAllocationFuture fut2{std::move(fut1)};
  EXPECT_EQ(44u, fut2.min_size());
  EXPECT_EQ(33u, fut2.desired_size());

  // Test move assign
  MultiBufAllocationFuture fut3{std::move(fut2)};
  EXPECT_EQ(44u, fut3.min_size());
  EXPECT_EQ(33u, fut3.desired_size());

  // Test task behavior works after two moves
  AllocateTask task(std::move(fut3));
  mbuf_alloc.ExpectAllocateAndReturn(44, 33, kAllowDiscontiguous, MultiBuf());

  DispatcherForTest dispatcher;
  dispatcher.Post(task);
  dispatcher.RunToCompletion();

  ASSERT_TRUE(task.last_result_.IsReady());
  ASSERT_TRUE(task.last_result_->has_value());
}

}  // namespace
}  // namespace pw::multibuf::v1
