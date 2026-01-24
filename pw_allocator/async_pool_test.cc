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

#include "pw_allocator/async_pool.h"

#include <cstdint>

#include "pw_allocator/chunk_pool.h"
#include "pw_allocator/testing.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/func_task.h"
#include "pw_unit_test/framework.h"

namespace {

struct U64 {
  std::byte bytes[8];
};

TEST(AsyncPoolTest, LayoutMatches) {
  std::array<std::byte, 256> buffer;
  pw::allocator::ChunkPool base(buffer, pw::allocator::Layout::Of<U64>());
  pw::allocator::AsyncPool pool(base);

  EXPECT_EQ(pool.layout(), base.layout());
}

TEST(AsyncPoolTest, AllocateDeallocate) {
  std::array<std::byte, 256> buffer;
  pw::allocator::ChunkPool base(buffer, pw::allocator::Layout::Of<U64>());
  pw::allocator::AsyncPool pool(base);

  void* ptr = pool.Allocate();
  ASSERT_NE(ptr, nullptr);
  pool.Deallocate(ptr);
}

TEST(AsyncPoolTest, PendAllocateIsNotReadyUntilDeallocate) {
  constexpr size_t kNumU64s = 32;
  constexpr size_t kBufferSize = sizeof(U64) * kNumU64s;
  std::array<std::byte, kBufferSize> buffer;
  pw::allocator::ChunkPool base(buffer, pw::allocator::Layout::Of<U64>());
  pw::allocator::AsyncPool pool(base);

  // Allocate everything.
  std::array<void*, kNumU64s> ptrs;
  for (auto& ptr : ptrs) {
    ptr = pool.Allocate();
    ASSERT_NE(ptr, nullptr);
  }

  // At this point, the pool is empty.
  EXPECT_EQ(pool.Allocate(), nullptr);

  pw::async2::DispatcherForTest dispatcher;
  void* async_ptr = nullptr;
  pw::async2::PendFuncTask task(
      [&](pw::async2::Context& context) -> pw::async2::Poll<> {
        auto poll = pool.PendAllocate(context);
        if (poll.IsPending()) {
          return pw::async2::Pending();
        }
        async_ptr = *poll;
        return pw::async2::Ready();
      });
  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  pool.Deallocate(ptrs[0]);
  dispatcher.RunToCompletion();
  EXPECT_NE(async_ptr, nullptr);
  ptrs[0] = async_ptr;

  // Release everything.
  for (auto& ptr : ptrs) {
    pool.Deallocate(ptr);
    ptr = nullptr;
  }
}

}  // namespace
