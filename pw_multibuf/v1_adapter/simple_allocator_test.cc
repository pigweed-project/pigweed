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

#include "pw_multibuf/simple_allocator.h"

#include "pw_allocator/null_allocator.h"
#include "pw_allocator/testing.h"
#include "pw_containers/storage.h"
#include "pw_containers/vector.h"
#include "pw_multibuf/chunk.h"
#include "pw_unit_test/framework.h"

namespace pw::multibuf {
namespace {

using ::pw::allocator::test::AllocatorForTest;

// Test fixture.

class SimpleAllocatorTest : public ::testing::Test {
 protected:
  static constexpr size_t kArbitraryBufferSize = 1024;
  static constexpr size_t kArbitraryMetaSize = 1024;
  static constexpr size_t kAlignment = 8;
  static_assert(kArbitraryBufferSize % kAlignment == 0);

  std::array<std::byte, kArbitraryBufferSize> data_area_;
  AllocatorForTest<kArbitraryMetaSize> meta_alloc_;
};

// Unit tests.

TEST_F(SimpleAllocatorTest, AllocateWholeDataAreaSizeSucceeds) {
  SimpleAllocator simple_allocator(data_area_, meta_alloc_);
  std::optional<MultiBuf> buf = simple_allocator.Allocate(kArbitraryBufferSize);
  ASSERT_TRUE(buf.has_value());
  EXPECT_EQ(buf->size(), kArbitraryBufferSize);
}

TEST_F(SimpleAllocatorTest, AllocateContiguousWholeDataAreaSizeSucceeds) {
  SimpleAllocator simple_allocator(data_area_, meta_alloc_);
  std::optional<MultiBuf> buf =
      simple_allocator.AllocateContiguous(kArbitraryBufferSize);
  ASSERT_TRUE(buf.has_value());
  EXPECT_EQ(buf->Chunks().size(), 1U);
  EXPECT_EQ(buf->size(), kArbitraryBufferSize);
}

TEST_F(SimpleAllocatorTest, AllocateContiguousHalfDataAreaSizeTwiceSucceeds) {
  SimpleAllocator simple_allocator(data_area_, meta_alloc_);
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

TEST_F(SimpleAllocatorTest, AllocateTooLargeReturnsNullopt) {
  SimpleAllocator simple_allocator(data_area_, meta_alloc_);
  std::optional<MultiBuf> buf =
      simple_allocator.Allocate(kArbitraryBufferSize + 1);
  EXPECT_FALSE(buf.has_value());
  std::optional<MultiBuf> contiguous_buf =
      simple_allocator.Allocate(kArbitraryBufferSize + 1);
  EXPECT_FALSE(contiguous_buf.has_value());
}

TEST_F(SimpleAllocatorTest,
       AllocateZeroWithNoMetadataOrDataReturnsEmptyMultiBuf) {
  pw::allocator::NullAllocator meta_alloc;
  SimpleAllocator simple_allocator(ByteSpan(), meta_alloc);
  std::optional<MultiBuf> buf = simple_allocator.Allocate(0);
  ASSERT_TRUE(buf.has_value());
  EXPECT_EQ(buf->size(), 0U);
}

TEST_F(SimpleAllocatorTest, AllocateWithNoMetadataRoomReturnsNullopt) {
  pw::allocator::NullAllocator meta_alloc;
  SimpleAllocator simple_allocator(data_area_, meta_alloc);
  std::optional<MultiBuf> buf = simple_allocator.Allocate(1);
  EXPECT_FALSE(buf.has_value());
}

TEST_F(SimpleAllocatorTest,
       SecondLargeAllocationFailsUntilFirstAllocationReleased) {
  SimpleAllocator simple_allocator(data_area_, meta_alloc_);
  const size_t alloc_size = kArbitraryBufferSize / 2 + 1;
  std::optional<MultiBuf> buf = simple_allocator.Allocate(alloc_size);
  ASSERT_TRUE(buf.has_value());
  EXPECT_EQ(buf->size(), alloc_size);
  EXPECT_FALSE(simple_allocator.Allocate(alloc_size).has_value());
  // Release the first buffer
  buf = std::nullopt;
  EXPECT_TRUE(simple_allocator.Allocate(alloc_size).has_value());
}

TEST_F(SimpleAllocatorTest, AllocateSkipsMiddleAllocations) {
  SimpleAllocator simple_allocator(data_area_, meta_alloc_);
  const size_t alloc_size = kArbitraryBufferSize / 3;
  std::optional<MultiBuf> buf1 = simple_allocator.Allocate(alloc_size);
  std::optional<MultiBuf> buf2 = simple_allocator.Allocate(alloc_size);
  std::optional<MultiBuf> buf3 = simple_allocator.Allocate(alloc_size);
  ASSERT_TRUE(buf1.has_value());
  ASSERT_TRUE(buf2.has_value());
  ASSERT_TRUE(buf3.has_value());
  buf1 = std::nullopt;
  buf3 = std::nullopt;
  // Now `buf2` holds the middle third of data_area
  std::optional<MultiBuf> split = simple_allocator.Allocate(alloc_size * 2);
  ASSERT_TRUE(split.has_value());
  EXPECT_EQ(split->size(), alloc_size * 2);
  EXPECT_EQ(split->Chunks().size(), 2U);
}

TEST_F(SimpleAllocatorTest, FailedAllocationDoesNotHoldOntoChunks) {
  SimpleAllocator simple_allocator(data_area_, meta_alloc_);
  const size_t alloc_size = kArbitraryBufferSize / 2;
  std::optional<MultiBuf> buf1 = simple_allocator.Allocate(alloc_size);
  std::optional<MultiBuf> buf2 = simple_allocator.Allocate(alloc_size);
  ASSERT_TRUE(buf1.has_value());
  ASSERT_TRUE(buf2.has_value());
  buf1 = std::nullopt;
  // When this allocation is attempted, it will initially create a chunk for
  // the first empty region prior to failing.
  EXPECT_FALSE(simple_allocator.Allocate(kArbitraryBufferSize).has_value());
  buf2 = std::nullopt;
  // Ensure that all chunk holds are released by attempting an allocation.
  EXPECT_TRUE(simple_allocator.Allocate(kArbitraryBufferSize).has_value());
}

TEST_F(SimpleAllocatorTest, AllocatorReturnsAlignedChunks) {
  containers::Storage<kAlignment, kArbitraryBufferSize> storage;
  ByteSpan data_area(storage.data(), storage.size());
  SimpleAllocator simple_allocator(data_area, meta_alloc_, kAlignment);
  std::optional<MultiBuf> buf1 = simple_allocator.Allocate(5);

  {
    ASSERT_TRUE(buf1);
    EXPECT_EQ(buf1->Chunks().size(), 1u);
    const auto first_chunk = buf1->Chunks().begin();
    EXPECT_EQ(first_chunk->size(), 5u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(first_chunk->data()) % kAlignment,
              0u);
  }

  std::optional<MultiBuf> buf2 = simple_allocator.Allocate(3);

  {
    ASSERT_TRUE(buf2);
    EXPECT_EQ(buf2->Chunks().size(), 1u);
    const auto first_chunk = buf2->Chunks().begin();
    EXPECT_EQ(first_chunk->size(), 3u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(first_chunk->data()) % kAlignment,
              0u);
  }
}

TEST_F(SimpleAllocatorTest, MultipleChunksAreAllAligned) {
  containers::Storage<kAlignment, kArbitraryBufferSize> storage;
  ByteSpan data_area(storage.data(), storage.size());
  SimpleAllocator simple_allocator(data_area, meta_alloc_, kAlignment);

  // Keep allocating buffers until we fail, alternating betweens ones we want to
  // keep and ones we will free.
  constexpr size_t kBufSize = 250;
  constexpr size_t kRoundedBufSize =
      (kBufSize + kAlignment - 1) / kAlignment * kAlignment;
  Vector<MultiBuf, kArbitraryBufferSize / kRoundedBufSize> bufs_to_keep;
  Vector<MultiBuf, kArbitraryBufferSize / kRoundedBufSize> bufs_to_free;
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
  ASSERT_TRUE(buf);

  // Check that all chunks of the returned buffer are aligned.
  size_t total_size = 0;
  for (const Chunk& chunk : buf->Chunks()) {
    EXPECT_EQ(reinterpret_cast<uintptr_t>(chunk.data()) % kAlignment, 0u);
    total_size += chunk.size();
  }

  EXPECT_EQ(total_size, free_bufs * kRoundedBufSize);
}

TEST_F(SimpleAllocatorTest, ContiguousChunksAreAligned) {
  containers::Storage<kAlignment, kArbitraryBufferSize> storage;
  ByteSpan data_area(storage.data(), storage.size());
  SimpleAllocator simple_allocator(data_area, meta_alloc_, kAlignment);

  // First create some fragmentation.
  std::optional<MultiBuf> buf1 = simple_allocator.Allocate(5);
  ASSERT_TRUE(buf1);
  std::optional<MultiBuf> buf2 = simple_allocator.Allocate(5);
  ASSERT_TRUE(buf2);
  std::optional<MultiBuf> buf3 = simple_allocator.Allocate(5);
  ASSERT_TRUE(buf3);
  std::optional<MultiBuf> buf4 = simple_allocator.Allocate(5);
  ASSERT_TRUE(buf4);
  std::optional<MultiBuf> buf5 = simple_allocator.Allocate(5);
  ASSERT_TRUE(buf5);
  std::optional<MultiBuf> buf6 = simple_allocator.Allocate(5);
  ASSERT_TRUE(buf6);

  buf2.reset();
  buf4.reset();
  buf5.reset();

  // Now allocate some contiguous buffers.
  std::optional<MultiBuf> buf7 = simple_allocator.AllocateContiguous(11);

  {
    ASSERT_TRUE(buf7);
    EXPECT_EQ(buf7->Chunks().size(), 1u);
    const auto first_chunk = buf7->Chunks().begin();
    EXPECT_EQ(first_chunk->size(), 11u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(first_chunk->data()) % kAlignment,
              0u);
  }

  std::optional<MultiBuf> buf8 = simple_allocator.AllocateContiguous(3);

  {
    ASSERT_TRUE(buf8);
    EXPECT_EQ(buf8->Chunks().size(), 1u);
    const auto first_chunk = buf8->Chunks().begin();
    EXPECT_EQ(first_chunk->size(), 3u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(first_chunk->data()) % kAlignment,
              0u);
  }
}

}  // namespace
}  // namespace pw::multibuf
