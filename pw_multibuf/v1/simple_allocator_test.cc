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

#include "pw_multibuf/v1/simple_allocator.h"

#include <vector>

#include "gtest/gtest.h"
#include "pw_allocator/null_allocator.h"
#include "pw_allocator/testing.h"

namespace pw::multibuf::v1 {
namespace {

using ::pw::allocator::test::AllocatorForTest;

constexpr size_t kArbitraryBufferSize = 1024;
constexpr size_t kArbitraryMetaSize = 1024;

// For the tests that test alignment...
constexpr size_t kAlignment = 8;
static_assert(kArbitraryBufferSize % kAlignment == 0);

TEST(SimpleAllocator, AllocateWholeDataAreaSizeSucceeds) {
  std::array<std::byte, kArbitraryBufferSize> data_area;
  AllocatorForTest<kArbitraryMetaSize> meta_alloc;
  SimpleAllocator simple_allocator(data_area, meta_alloc);
  std::optional<MultiBuf> buf = simple_allocator.Allocate(kArbitraryBufferSize);
  ASSERT_TRUE(buf.has_value());
  EXPECT_EQ(buf->size(), kArbitraryBufferSize);
}

TEST(SimpleAllocator, AllocateContiguousWholeDataAreaSizeSucceeds) {
  std::array<std::byte, kArbitraryBufferSize> data_area;
  AllocatorForTest<kArbitraryMetaSize> meta_alloc;
  SimpleAllocator simple_allocator(data_area, meta_alloc);
  std::optional<MultiBuf> buf =
      simple_allocator.AllocateContiguous(kArbitraryBufferSize);
  ASSERT_TRUE(buf.has_value());
  EXPECT_EQ(buf->Chunks().size(), 1U);
  EXPECT_EQ(buf->size(), kArbitraryBufferSize);
}

TEST(SimpleAllocator, AllocateContiguousHalfDataAreaSizeTwiceSucceeds) {
  std::array<std::byte, kArbitraryBufferSize> data_area;
  AllocatorForTest<kArbitraryMetaSize> meta_alloc;
  SimpleAllocator simple_allocator(data_area, meta_alloc);
  std::optional<MultiBuf> buf =
      simple_allocator.AllocateContiguous(kArbitraryBufferSize / 2);
  ASSERT_TRUE(buf.has_value());
  EXPECT_EQ(buf->Chunks().size(), 1U);
  EXPECT_EQ(buf->size(), kArbitraryBufferSize / 2);

  std::optional<MultiBuf> buf2 =
      simple_allocator.AllocateContiguous(kArbitraryBufferSize / 2);
  ASSERT_TRUE(buf2.has_value());
  EXPECT_EQ(buf2->Chunks().size(), 1U);
  EXPECT_EQ(buf2->size(), kArbitraryBufferSize / 2);
}

TEST(SimpleAllocator, AllocateTooLargeReturnsNullopt) {
  std::array<std::byte, kArbitraryBufferSize> data_area;
  AllocatorForTest<kArbitraryMetaSize> meta_alloc;
  SimpleAllocator simple_allocator(data_area, meta_alloc);
  std::optional<MultiBuf> buf =
      simple_allocator.Allocate(kArbitraryBufferSize + 1);
  EXPECT_FALSE(buf.has_value());
  std::optional<MultiBuf> contiguous_buf =
      simple_allocator.Allocate(kArbitraryBufferSize + 1);
  EXPECT_FALSE(contiguous_buf.has_value());
}

TEST(SimpleAllocator, AllocateZeroWithNoMetadataOrDataReturnsEmptyMultiBuf) {
  std::array<std::byte, 0> data_area;
  pw::allocator::NullAllocator meta_alloc;
  SimpleAllocator simple_allocator(data_area, meta_alloc);
  std::optional<MultiBuf> buf = simple_allocator.Allocate(0);
  ASSERT_TRUE(buf.has_value());
  EXPECT_EQ(buf->size(), 0U);
}

TEST(SimpleAllocator, AllocateWithNoMetadataRoomReturnsNullopt) {
  std::array<std::byte, kArbitraryBufferSize> data_area;
  pw::allocator::NullAllocator meta_alloc;
  SimpleAllocator simple_allocator(data_area, meta_alloc);
  std::optional<MultiBuf> buf = simple_allocator.Allocate(1);
  EXPECT_FALSE(buf.has_value());
}

TEST(SimpleAllocator, SecondLargeAllocationFailsUntilFirstAllocationReleased) {
  std::array<std::byte, kArbitraryBufferSize> data_area;
  AllocatorForTest<kArbitraryMetaSize> meta_alloc;
  SimpleAllocator simple_allocator(data_area, meta_alloc);
  const size_t alloc_size = kArbitraryBufferSize / 2 + 1;
  std::optional<MultiBuf> buf = simple_allocator.Allocate(alloc_size);
  ASSERT_TRUE(buf.has_value());
  EXPECT_EQ(buf->size(), alloc_size);
  EXPECT_FALSE(simple_allocator.Allocate(alloc_size).has_value());
  // Release the first buffer
  buf = std::nullopt;
  EXPECT_TRUE(simple_allocator.Allocate(alloc_size).has_value());
}

TEST(SimpleAllocator, AllocateSkipsMiddleAllocations) {
  std::array<std::byte, kArbitraryBufferSize> data_area;
  AllocatorForTest<kArbitraryMetaSize> meta_alloc;
  SimpleAllocator simple_allocator(data_area, meta_alloc);
  const size_t alloc_size = kArbitraryBufferSize / 3;
  std::optional<MultiBuf> buf1 = simple_allocator.Allocate(alloc_size);
  std::optional<MultiBuf> buf2 = simple_allocator.Allocate(alloc_size);
  std::optional<MultiBuf> buf3 = simple_allocator.Allocate(alloc_size);
  EXPECT_TRUE(buf1.has_value());
  EXPECT_TRUE(buf2.has_value());
  EXPECT_TRUE(buf3.has_value());
  buf1 = std::nullopt;
  buf3 = std::nullopt;
  // Now `buf2` holds the middle third of data_area
  std::optional<MultiBuf> split = simple_allocator.Allocate(alloc_size * 2);
  ASSERT_TRUE(split.has_value());
  EXPECT_EQ(split->size(), alloc_size * 2);
  EXPECT_EQ(split->Chunks().size(), 2U);
}

TEST(SimpleAllocator, FailedAllocationDoesNotHoldOntoChunks) {
  std::array<std::byte, kArbitraryBufferSize> data_area;
  AllocatorForTest<kArbitraryMetaSize> meta_alloc;
  SimpleAllocator simple_allocator(data_area, meta_alloc);
  const size_t alloc_size = kArbitraryBufferSize / 2;
  std::optional<MultiBuf> buf1 = simple_allocator.Allocate(alloc_size);
  std::optional<MultiBuf> buf2 = simple_allocator.Allocate(alloc_size);
  EXPECT_TRUE(buf1.has_value());
  EXPECT_TRUE(buf2.has_value());
  buf1 = std::nullopt;
  // When this allocation is attempted, it will initially create a chunk for
  // the first empty region prior to failing.
  EXPECT_FALSE(simple_allocator.Allocate(kArbitraryBufferSize).has_value());
  buf2 = std::nullopt;
  // Ensure that all chunk holds are released by attempting an allocation.
  EXPECT_TRUE(simple_allocator.Allocate(kArbitraryBufferSize).has_value());
}

TEST(SimpleAllocator, AllocatorReturnsAlignedChunks) {
  alignas(kAlignment) std::array<std::byte, kArbitraryBufferSize> data_area;
  AllocatorForTest<kArbitraryMetaSize> meta_alloc;
  SimpleAllocator simple_allocator(data_area, meta_alloc, kAlignment);
  std::optional<MultiBuf> buf1 = simple_allocator.Allocate(5);

  {
    EXPECT_TRUE(buf1);
    EXPECT_EQ(buf1->Chunks().size(), 1u);
    const auto first_chunk = buf1->Chunks().begin();
    EXPECT_EQ(first_chunk->size(), 5u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(first_chunk->data()) % kAlignment,
              0u);
  }

  std::optional<MultiBuf> buf2 = simple_allocator.Allocate(3);

  {
    EXPECT_TRUE(buf2);
    EXPECT_EQ(buf2->Chunks().size(), 1u);
    const auto first_chunk = buf2->Chunks().begin();
    EXPECT_EQ(first_chunk->size(), 3u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(first_chunk->data()) % kAlignment,
              0u);
  }
}

TEST(SimpleAllocator, MultipleChunksAreAllAligned) {
  alignas(kAlignment) std::array<std::byte, kArbitraryBufferSize> data_area;
  AllocatorForTest<kArbitraryMetaSize> meta_alloc;
  SimpleAllocator simple_allocator(data_area, meta_alloc, kAlignment);
  std::vector<MultiBuf> bufs_to_keep, bufs_to_free;

  // Keep allocating buffers until we fail, alternating betweens ones we want to
  // keep and ones we will free.
  constexpr size_t kBufSize = 250;
  constexpr size_t kRoundedBufSize =
      (kBufSize + kAlignment - 1) / kAlignment * kAlignment;
  for (;;) {
    std::optional<MultiBuf> buf = simple_allocator.Allocate(kBufSize);
    if (!buf) {
      break;
    }
    bufs_to_keep.push_back(*std::move(buf));
    buf = simple_allocator.Allocate(kBufSize);
    if (!buf) {
      break;
    }
    bufs_to_free.push_back(*std::move(buf));
  }

  const size_t free_bufs = bufs_to_free.size();
  EXPECT_GT(free_bufs, 1u);

  // Free bufs_to_free which should leave us with lots of fragmentation.
  bufs_to_free.clear();

  // We should be able to allocate `free_bufs * kRoundedBufSize` because every
  // buffer we freed should have been rounded up to the alignment.
  std::optional<MultiBuf> buf =
      simple_allocator.Allocate(free_bufs * kRoundedBufSize);
  EXPECT_TRUE(buf);

  // Check that all chunks of the returned buffer are aligned.
  size_t total_size = 0;
  for (const Chunk& chunk : buf->Chunks()) {
    EXPECT_EQ(reinterpret_cast<uintptr_t>(chunk.data()) % kAlignment, 0u);
    total_size += chunk.size();
  }

  EXPECT_EQ(total_size, free_bufs * kRoundedBufSize);
}

TEST(SimpleAllocator, ContiguousChunksAreAligned) {
  alignas(kAlignment) std::array<std::byte, kArbitraryBufferSize> data_area;
  AllocatorForTest<kArbitraryMetaSize> meta_alloc;
  SimpleAllocator simple_allocator(data_area, meta_alloc, kAlignment);

  // First create some fragmentation.
  std::optional<MultiBuf> buf1 = simple_allocator.Allocate(5);
  EXPECT_TRUE(buf1);
  std::optional<MultiBuf> buf2 = simple_allocator.Allocate(5);
  EXPECT_TRUE(buf1);
  std::optional<MultiBuf> buf3 = simple_allocator.Allocate(5);
  EXPECT_TRUE(buf1);
  std::optional<MultiBuf> buf4 = simple_allocator.Allocate(5);
  EXPECT_TRUE(buf1);
  std::optional<MultiBuf> buf5 = simple_allocator.Allocate(5);
  EXPECT_TRUE(buf1);
  std::optional<MultiBuf> buf6 = simple_allocator.Allocate(5);
  EXPECT_TRUE(buf1);

  buf2.reset();
  buf4.reset();
  buf5.reset();

  // Now allocate some contiguous buffers.
  std::optional<MultiBuf> buf7 = simple_allocator.AllocateContiguous(11);

  {
    EXPECT_TRUE(buf7);
    EXPECT_EQ(buf7->Chunks().size(), 1u);
    const auto first_chunk = buf7->Chunks().begin();
    EXPECT_EQ(first_chunk->size(), 11u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(first_chunk->data()) % kAlignment,
              0u);
  }

  std::optional<MultiBuf> buf8 = simple_allocator.AllocateContiguous(3);

  {
    EXPECT_TRUE(buf8);
    EXPECT_EQ(buf8->Chunks().size(), 1u);
    const auto first_chunk = buf8->Chunks().begin();
    EXPECT_EQ(first_chunk->size(), 3u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(first_chunk->data()) % kAlignment,
              0u);
  }
}

}  // namespace
}  // namespace pw::multibuf::v1
