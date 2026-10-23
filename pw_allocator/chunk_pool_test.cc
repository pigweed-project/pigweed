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

#include "pw_allocator/chunk_pool.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "pw_allocator/internal/counter.h"
#include "pw_allocator/layout.h"
#include "pw_allocator/testing.h"
#include "pw_unit_test/framework.h"

namespace {

// Test fixtures.

using ::pw::allocator::Layout;
using ::pw::allocator::test::Counter;
using ChunkPoolTest = pw::allocator::test::TestWithCounters;

struct U64 {
  std::byte bytes[8];
};

// Unit tests.

TEST_F(ChunkPoolTest, Capabilities) {
  std::array<std::byte, 256> buffer;
  pw::allocator::ChunkPool pool(buffer, Layout::Of<U64>());
  EXPECT_EQ(pool.capabilities(), pw::allocator::ChunkPool::kCapabilities);
}

TEST_F(ChunkPoolTest, AllocateDeallocate) {
  std::array<std::byte, 256> buffer;
  pw::allocator::ChunkPool pool(buffer, Layout::Of<U64>());

  void* ptr = pool.Allocate();
  ASSERT_NE(ptr, nullptr);
  pool.Deallocate(ptr);
}

TEST_F(ChunkPoolTest, ExhaustTwice) {
  constexpr size_t kNumU64s = 32;
  constexpr size_t kBufferSize = sizeof(U64) * kNumU64s;
  std::array<std::byte, kBufferSize> buffer;
  pw::allocator::ChunkPool pool(buffer, Layout::Of<U64>());

  // Allocate everything.
  std::array<void*, kNumU64s> ptrs;
  for (auto& ptr : ptrs) {
    ptr = pool.Allocate();
    ASSERT_NE(ptr, nullptr);
  }

  // At this point, the pool is empty.
  EXPECT_EQ(pool.Allocate(), nullptr);

  // Now refill the pool, and show it can be emptied again.
  for (auto& ptr : ptrs) {
    pool.Deallocate(ptr);
    ptr = nullptr;
  }
  for (auto& ptr : ptrs) {
    ptr = pool.Allocate();
    ASSERT_NE(ptr, nullptr);
  }

  // Release everything.
  for (auto& ptr : ptrs) {
    pool.Deallocate(ptr);
    ptr = nullptr;
  }
}

TEST_F(ChunkPoolTest, NewDelete) {
  std::array<std::byte, 256> buffer;
  pw::allocator::ChunkPool pool(buffer, Layout::Of<Counter>());

  auto* counter = pool.New<Counter>(867u);
  ASSERT_NE(counter, nullptr);
  EXPECT_EQ(counter->value(), 867u);
  pool.Delete(counter);
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 1u);
}

TEST_F(ChunkPoolTest, NewDeleteBoundedArray) {
  std::array<std::byte, 256> buffer;
  Layout layout(sizeof(Counter) * 3, alignof(Counter));
  pw::allocator::ChunkPool pool(buffer, layout);

  auto* counters = pool.New<Counter[3]>();
  ASSERT_NE(counters, nullptr);
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(counters[i].value(), i);
  }
  pool.Delete<Counter[3]>(counters);
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 3u);
}

TEST_F(ChunkPoolTest, NewDeleteUnboundedArray) {
  std::array<std::byte, 256> buffer;
  Layout layout(sizeof(Counter) * 5, alignof(Counter));
  pw::allocator::ChunkPool pool(buffer, layout);

  auto* counters = pool.New<Counter[]>();
  ASSERT_NE(counters, nullptr);
  for (size_t i = 0; i < 5; ++i) {
    EXPECT_EQ(counters[i].value(), i);
  }
  pool.Delete<Counter[]>(counters, 5);
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 5u);
}

TEST_F(ChunkPoolTest, NewDeleteArray) {
  std::array<std::byte, 256> buffer;
  Layout layout(sizeof(Counter) * 3, alignof(Counter));
  pw::allocator::ChunkPool pool(buffer, layout);

  auto* counters = pool.New<Counter[3]>();
  ASSERT_NE(counters, nullptr);
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(counters[i].value(), i);
  }
  pool.DeleteArray(counters, 3);
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 3u);
}

TEST_F(ChunkPoolTest, MakeUnique) {
  std::array<std::byte, 256> buffer;
  pw::allocator::ChunkPool pool(buffer, Layout::Of<Counter>());
  {
    auto counter = pool.MakeUnique<Counter>(5309u);
    ASSERT_NE(counter, nullptr);
    EXPECT_EQ(counter->value(), 5309u);
  }
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 1u);
}

TEST_F(ChunkPoolTest, MakeUniqueBoundedArray) {
  std::array<std::byte, 256> buffer;
  Layout layout(sizeof(Counter) * 7, alignof(Counter));
  pw::allocator::ChunkPool pool(buffer, layout);
  {
    auto counters = pool.MakeUnique<Counter[7]>();
    ASSERT_NE(counters, nullptr);
    for (size_t i = 0; i < 7; ++i) {
      EXPECT_EQ(counters[i].value(), i);
    }
  }
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 7u);
}

TEST_F(ChunkPoolTest, MakeUniqueBoundedArrayDifferentType) {
  std::array<std::byte, 256> buffer;
  Layout layout(sizeof(Counter) * 7, alignof(Counter));
  pw::allocator::ChunkPool pool(buffer, layout);
  auto bytes = pool.MakeUnique<std::byte[sizeof(Counter) * 7]>();
  ASSERT_NE(bytes, nullptr);
  EXPECT_EQ(bytes.size(), layout.size());
}

TEST_F(ChunkPoolTest, MakeUniqueUnboundedArray) {
  std::array<std::byte, 256> buffer;
  Layout layout(sizeof(Counter) * 9, alignof(Counter));
  pw::allocator::ChunkPool pool(buffer, layout);
  {
    auto counters = pool.MakeUnique<Counter[]>();
    ASSERT_NE(counters, nullptr);
    for (size_t i = 0; i < 9; ++i) {
      EXPECT_EQ(counters[i].value(), i);
    }
  }
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 9u);
}

TEST_F(ChunkPoolTest, MakeUniqueUnboundedArrayDifferentType) {
  std::array<std::byte, 256> buffer;
  Layout layout(sizeof(Counter) * 9, alignof(Counter));
  pw::allocator::ChunkPool pool(buffer, layout);
  auto bytes = pool.MakeUnique<std::byte[]>();
  ASSERT_NE(bytes, nullptr);
  EXPECT_EQ(bytes.size(), layout.size());
}

}  // namespace
