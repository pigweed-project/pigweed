// Copyright 2023 The Pigweed Authors
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

#include "pw_multibuf/v1/chunk.h"

#include <memory>

#if __cplusplus >= 202002L
#include <ranges>
#endif  // __cplusplus >= 202002L

#include "pw_allocator/testing.h"
#include "pw_multibuf/v1/header_chunk_region_tracker.h"
#include "pw_unit_test/framework.h"

namespace pw::multibuf::v1 {
namespace {

using ::pw::allocator::test::AllocatorForTest;

/// Returns literal with ``_size`` suffix as a ``size_t``.
///
/// This is useful for writing size-related test assertions without
/// explicit (verbose) casts.
constexpr size_t operator""_size(unsigned long long n) { return n; }

const size_t kArbitraryAllocatorSize = 1024;
const size_t kArbitraryChunkSize = 32;

#if __cplusplus >= 202002L
static_assert(std::ranges::contiguous_range<Chunk>);
#endif  // __cplusplus >= 202002L

void TakesSpan([[maybe_unused]] ByteSpan span) {}

TEST(Chunk, IsImplicitlyConvertibleToSpan) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  std::optional<OwnedChunk> chunk =
      HeaderChunkRegionTracker::AllocateRegionAsChunk(allocator,
                                                      kArbitraryChunkSize);
  ASSERT_TRUE(chunk.has_value());
  // ``Chunk`` should convert to ``ByteSpan``.
  TakesSpan(**chunk);
}

TEST(OwnedChunk, ReleaseDestroysChunkRegion) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  const auto& metrics = allocator.metrics();
  auto tracker =
      HeaderChunkRegionTracker::AllocateRegion(allocator, kArbitraryChunkSize);
  ASSERT_NE(tracker, nullptr);
  EXPECT_EQ(metrics.num_allocations.value(), 1_size);

  std::optional<OwnedChunk> chunk_opt = tracker->CreateFirstChunk();
  ASSERT_TRUE(chunk_opt.has_value());
  auto& chunk = *chunk_opt;
  EXPECT_EQ(metrics.num_allocations.value(), 2_size);
  EXPECT_EQ(chunk.size(), kArbitraryChunkSize);

  chunk.Release();
  EXPECT_EQ(chunk.size(), 0_size);
  EXPECT_EQ(metrics.num_deallocations.value(), 2_size);
  EXPECT_EQ(metrics.allocated_bytes.value(), 0_size);
}

TEST(OwnedChunk, DestructorDestroysChunkRegion) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  const auto& metrics = allocator.metrics();
  auto tracker =
      HeaderChunkRegionTracker::AllocateRegion(allocator, kArbitraryChunkSize);
  ASSERT_NE(tracker, nullptr);
  EXPECT_EQ(metrics.num_allocations.value(), 1_size);

  {
    std::optional<OwnedChunk> chunk = tracker->CreateFirstChunk();
    ASSERT_TRUE(chunk.has_value());
    EXPECT_EQ(metrics.num_allocations.value(), 2_size);
    EXPECT_EQ(chunk->size(), kArbitraryChunkSize);
  }

  EXPECT_EQ(metrics.num_deallocations.value(), 2_size);
  EXPECT_EQ(metrics.allocated_bytes.value(), 0_size);
}

TEST(Chunk, DiscardPrefixDiscardsPrefixOfSpan) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  std::optional<OwnedChunk> chunk_opt =
      HeaderChunkRegionTracker::AllocateRegionAsChunk(allocator,
                                                      kArbitraryChunkSize);
  ASSERT_TRUE(chunk_opt.has_value());
  auto& chunk = *chunk_opt;
  ConstByteSpan old_span = chunk;
  const size_t kDiscarded = 4;
  chunk->DiscardPrefix(kDiscarded);
  EXPECT_EQ(chunk.size(), old_span.size() - kDiscarded);
  EXPECT_EQ(chunk.data(), old_span.data() + kDiscarded);
}

TEST(Chunk, TakePrefixTakesPrefixOfSpan) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  std::optional<OwnedChunk> chunk_opt =
      HeaderChunkRegionTracker::AllocateRegionAsChunk(allocator,
                                                      kArbitraryChunkSize);
  ASSERT_TRUE(chunk_opt.has_value());
  auto& chunk = *chunk_opt;
  ConstByteSpan old_span = chunk;
  const size_t kTaken = 4;
  std::optional<OwnedChunk> front_opt = chunk->TakePrefix(kTaken);
  ASSERT_TRUE(front_opt.has_value());
  auto& front = *front_opt;
  EXPECT_EQ(front->size(), kTaken);
  EXPECT_EQ(front->data(), old_span.data());
  EXPECT_EQ(chunk.size(), old_span.size() - kTaken);
  EXPECT_EQ(chunk.data(), old_span.data() + kTaken);
}

TEST(Chunk, TruncateDiscardsEndOfSpan) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  std::optional<OwnedChunk> chunk_opt =
      HeaderChunkRegionTracker::AllocateRegionAsChunk(allocator,
                                                      kArbitraryChunkSize);
  ASSERT_TRUE(chunk_opt.has_value());
  auto& chunk = *chunk_opt;
  ConstByteSpan old_span = chunk;
  const size_t kShorter = 5;
  chunk->Truncate(old_span.size() - kShorter);
  EXPECT_EQ(chunk.size(), old_span.size() - kShorter);
  EXPECT_EQ(chunk.data(), old_span.data());
}

TEST(Chunk, TakeSuffixTakesEndOfSpan) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  std::optional<OwnedChunk> chunk_opt =
      HeaderChunkRegionTracker::AllocateRegionAsChunk(allocator,
                                                      kArbitraryChunkSize);
  ASSERT_TRUE(chunk_opt.has_value());
  auto& chunk = *chunk_opt;
  ConstByteSpan old_span = chunk;
  const size_t kTaken = 5;
  std::optional<OwnedChunk> tail_opt = chunk->TakeSuffix(kTaken);
  ASSERT_TRUE(tail_opt.has_value());
  auto& tail = *tail_opt;
  EXPECT_EQ(tail.size(), kTaken);
  EXPECT_EQ(tail.data(), old_span.data() + old_span.size() - kTaken);
  EXPECT_EQ(chunk.size(), old_span.size() - kTaken);
  EXPECT_EQ(chunk.data(), old_span.data());
}

TEST(Chunk, SliceRemovesSidesOfSpan) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  std::optional<OwnedChunk> chunk_opt =
      HeaderChunkRegionTracker::AllocateRegionAsChunk(allocator,
                                                      kArbitraryChunkSize);
  ASSERT_TRUE(chunk_opt.has_value());
  auto& chunk = *chunk_opt;
  ConstByteSpan old_span = chunk;
  const size_t kBegin = 4;
  const size_t kEnd = 9;
  chunk->Slice(kBegin, kEnd);
  EXPECT_EQ(chunk.data(), old_span.data() + kBegin);
  EXPECT_EQ(chunk.size(), kEnd - kBegin);
}

TEST(Chunk, RegionPersistsUntilAllChunksReleased) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  const auto& metrics = allocator.metrics();
  std::optional<OwnedChunk> chunk_opt =
      HeaderChunkRegionTracker::AllocateRegionAsChunk(allocator,
                                                      kArbitraryChunkSize);
  ASSERT_TRUE(chunk_opt.has_value());
  auto& chunk = *chunk_opt;
  // One allocation for the region tracker, one for the chunk.
  EXPECT_EQ(metrics.num_allocations.value(), 2_size);
  const size_t kSplitPoint = 13;
  auto split_opt = chunk->TakePrefix(kSplitPoint);
  ASSERT_TRUE(split_opt.has_value());
  auto& split = *split_opt;
  // One allocation for the region tracker, one for each of two chunks.
  EXPECT_EQ(metrics.num_allocations.value(), 3_size);
  chunk.Release();
  EXPECT_EQ(metrics.num_deallocations.value(), 1_size);
  split.Release();
  EXPECT_EQ(metrics.num_deallocations.value(), 3_size);
}

TEST(Chunk, ClaimPrefixReclaimsDiscardedPrefix) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  std::optional<OwnedChunk> chunk_opt =
      HeaderChunkRegionTracker::AllocateRegionAsChunk(allocator,
                                                      kArbitraryChunkSize);
  ASSERT_TRUE(chunk_opt.has_value());
  auto& chunk = *chunk_opt;
  ConstByteSpan old_span = chunk;
  const size_t kDiscarded = 4;
  chunk->DiscardPrefix(kDiscarded);
  EXPECT_TRUE(chunk->ClaimPrefix(kDiscarded));
  EXPECT_EQ(chunk.size(), old_span.size());
  EXPECT_EQ(chunk.data(), old_span.data());
}

TEST(Chunk, ClaimPrefixFailsOnFullRegionChunk) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  std::optional<OwnedChunk> chunk_opt =
      HeaderChunkRegionTracker::AllocateRegionAsChunk(allocator,
                                                      kArbitraryChunkSize);
  ASSERT_TRUE(chunk_opt.has_value());
  auto& chunk = *chunk_opt;
  EXPECT_FALSE(chunk->ClaimPrefix(1));
}

TEST(Chunk, ClaimPrefixFailsOnNeighboringChunk) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  std::optional<OwnedChunk> chunk_opt =
      HeaderChunkRegionTracker::AllocateRegionAsChunk(allocator,
                                                      kArbitraryChunkSize);
  ASSERT_TRUE(chunk_opt.has_value());
  auto& chunk = *chunk_opt;
  const size_t kSplitPoint = 22;
  auto front = chunk->TakePrefix(kSplitPoint);
  ASSERT_TRUE(front.has_value());
  EXPECT_FALSE(chunk->ClaimPrefix(1));
}

TEST(Chunk,
     ClaimPrefixFailsAtStartOfRegionEvenAfterReleasingChunkAtEndOfRegion) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  std::optional<OwnedChunk> chunk_opt =
      HeaderChunkRegionTracker::AllocateRegionAsChunk(allocator,
                                                      kArbitraryChunkSize);
  ASSERT_TRUE(chunk_opt.has_value());
  auto& chunk = *chunk_opt;
  const size_t kTaken = 13;
  auto split = chunk->TakeSuffix(kTaken);
  ASSERT_TRUE(split.has_value());
  split->Release();
  EXPECT_FALSE(chunk->ClaimPrefix(1));
}

TEST(Chunk, ClaimPrefixReclaimsPrecedingChunksDiscardedSuffix) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  std::optional<OwnedChunk> chunk_opt =
      HeaderChunkRegionTracker::AllocateRegionAsChunk(allocator,
                                                      kArbitraryChunkSize);
  ASSERT_TRUE(chunk_opt.has_value());
  auto& chunk = *chunk_opt;
  const size_t kSplitPoint = 13;
  auto split_opt = chunk->TakePrefix(kSplitPoint);
  ASSERT_TRUE(split_opt.has_value());
  auto& split = *split_opt;
  const size_t kDiscard = 3;
  split->Truncate(split.size() - kDiscard);
  EXPECT_TRUE(chunk->ClaimPrefix(kDiscard));
  EXPECT_FALSE(chunk->ClaimPrefix(1));
}

TEST(Chunk, ClaimSuffixReclaimsTruncatedEnd) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  std::optional<OwnedChunk> chunk_opt =
      HeaderChunkRegionTracker::AllocateRegionAsChunk(allocator,
                                                      kArbitraryChunkSize);
  ASSERT_TRUE(chunk_opt.has_value());
  auto& chunk = *chunk_opt;
  ConstByteSpan old_span = *chunk;
  const size_t kDiscarded = 4;
  chunk->Truncate(old_span.size() - kDiscarded);
  EXPECT_TRUE(chunk->ClaimSuffix(kDiscarded));
  EXPECT_EQ(chunk->size(), old_span.size());
  EXPECT_EQ(chunk->data(), old_span.data());
}

TEST(Chunk, ClaimSuffixFailsOnFullRegionChunk) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  std::optional<OwnedChunk> chunk_opt =
      HeaderChunkRegionTracker::AllocateRegionAsChunk(allocator,
                                                      kArbitraryChunkSize);
  ASSERT_TRUE(chunk_opt.has_value());
  auto& chunk = *chunk_opt;
  EXPECT_FALSE(chunk->ClaimSuffix(1));
}

TEST(Chunk, ClaimSuffixFailsWithNeighboringChunk) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  std::optional<OwnedChunk> chunk_opt =
      HeaderChunkRegionTracker::AllocateRegionAsChunk(allocator,
                                                      kArbitraryChunkSize);
  ASSERT_TRUE(chunk_opt.has_value());
  auto& chunk = *chunk_opt;
  const size_t kSplitPoint = 22;
  auto split_opt = chunk->TakePrefix(kSplitPoint);
  ASSERT_TRUE(split_opt.has_value());
  auto& split = *split_opt;
  EXPECT_FALSE(split->ClaimSuffix(1));
}

TEST(Chunk, ClaimSuffixFailsAtEndOfRegionEvenAfterReleasingFirstChunkInRegion) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  std::optional<OwnedChunk> chunk_opt =
      HeaderChunkRegionTracker::AllocateRegionAsChunk(allocator,
                                                      kArbitraryChunkSize);
  ASSERT_TRUE(chunk_opt.has_value());
  auto& chunk = *chunk_opt;
  const size_t kTaken = 22;
  auto split_opt = chunk->TakeSuffix(kTaken);
  ASSERT_TRUE(split_opt.has_value());
  auto& split = *split_opt;
  EXPECT_FALSE(split->ClaimSuffix(1));
}

TEST(Chunk, ClaimSuffixReclaimsFollowingChunksDiscardedPrefix) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  std::optional<OwnedChunk> chunk_opt =
      HeaderChunkRegionTracker::AllocateRegionAsChunk(allocator,
                                                      kArbitraryChunkSize);
  ASSERT_TRUE(chunk_opt.has_value());
  auto& chunk = *chunk_opt;
  const size_t kSplitPoint = 22;
  auto split_opt = chunk->TakePrefix(kSplitPoint);
  ASSERT_TRUE(split_opt.has_value());
  auto& split = *split_opt;
  const size_t kDiscarded = 3;
  chunk->DiscardPrefix(kDiscarded);
  EXPECT_TRUE(split->ClaimSuffix(kDiscarded));
  EXPECT_FALSE(split->ClaimSuffix(1));
}

TEST(Chunk, MergeReturnsFalseForChunksFromDifferentRegions) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  std::optional<OwnedChunk> chunk_1_opt =
      HeaderChunkRegionTracker::AllocateRegionAsChunk(allocator,
                                                      kArbitraryChunkSize);
  ASSERT_TRUE(chunk_1_opt.has_value());
  OwnedChunk& chunk_1 = *chunk_1_opt;
  std::optional<OwnedChunk> chunk_2_opt =
      HeaderChunkRegionTracker::AllocateRegionAsChunk(allocator,
                                                      kArbitraryChunkSize);
  ASSERT_TRUE(chunk_2_opt.has_value());
  OwnedChunk& chunk_2 = *chunk_2_opt;
  EXPECT_FALSE(chunk_1->CanMerge(*chunk_2));
  EXPECT_FALSE(chunk_1->Merge(chunk_2));
  // Ensure that neither chunk was modified
  EXPECT_EQ(chunk_1.size(), kArbitraryChunkSize);
  EXPECT_EQ(chunk_2.size(), kArbitraryChunkSize);
}

TEST(Chunk, MergeReturnsFalseForNonAdjacentChunksFromSameRegion) {
  const size_t kTakenFromOne = 8;
  const size_t kTakenFromTwo = 4;

  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  std::optional<OwnedChunk> chunk_1_opt =
      HeaderChunkRegionTracker::AllocateRegionAsChunk(allocator,
                                                      kArbitraryChunkSize);
  ASSERT_TRUE(chunk_1_opt.has_value());
  OwnedChunk& chunk_1 = *chunk_1_opt;

  std::optional<OwnedChunk> chunk_2_opt = chunk_1->TakeSuffix(kTakenFromOne);
  ASSERT_TRUE(chunk_2_opt.has_value());
  OwnedChunk& chunk_2 = *chunk_2_opt;

  std::optional<OwnedChunk> chunk_3_opt = chunk_2->TakeSuffix(kTakenFromTwo);
  ASSERT_TRUE(chunk_3_opt.has_value());
  OwnedChunk& chunk_3 = *chunk_3_opt;

  EXPECT_FALSE(chunk_1->CanMerge(*chunk_3));
  EXPECT_FALSE(chunk_1->Merge(chunk_3));
  EXPECT_EQ(chunk_1.size(), kArbitraryChunkSize - kTakenFromOne);
  EXPECT_EQ(chunk_2.size(), kTakenFromOne - kTakenFromTwo);
  EXPECT_EQ(chunk_3.size(), kTakenFromTwo);
}

TEST(Chunk, MergeJoinsMultipleAdjacentChunksFromSameRegion) {
  const size_t kTakenFromOne = 8;
  const size_t kTakenFromTwo = 4;

  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  std::optional<OwnedChunk> chunk_1_opt =
      HeaderChunkRegionTracker::AllocateRegionAsChunk(allocator,
                                                      kArbitraryChunkSize);
  ASSERT_TRUE(chunk_1_opt.has_value());
  OwnedChunk& chunk_1 = *chunk_1_opt;

  std::optional<OwnedChunk> chunk_2_opt = chunk_1->TakeSuffix(kTakenFromOne);
  ASSERT_TRUE(chunk_2_opt.has_value());
  OwnedChunk& chunk_2 = *chunk_2_opt;

  std::optional<OwnedChunk> chunk_3_opt = chunk_2->TakeSuffix(kTakenFromTwo);
  ASSERT_TRUE(chunk_3_opt.has_value());
  OwnedChunk& chunk_3 = *chunk_3_opt;

  EXPECT_TRUE(chunk_1->CanMerge(*chunk_2));
  EXPECT_TRUE(chunk_1->Merge(chunk_2));
  EXPECT_TRUE(chunk_1->CanMerge(*chunk_3));
  EXPECT_TRUE(chunk_1->Merge(chunk_3));

  EXPECT_EQ(chunk_1.size(), kArbitraryChunkSize);
  EXPECT_EQ(chunk_2.size(), 0_size);
  EXPECT_EQ(chunk_3.size(), 0_size);
}

TEST(Chunk, MergeJoinsAdjacentChunksFromSameRegion) {
  const size_t kTaken = 4;

  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  std::optional<OwnedChunk> chunk_1_opt =
      HeaderChunkRegionTracker::AllocateRegionAsChunk(allocator,
                                                      kArbitraryChunkSize);
  ASSERT_TRUE(chunk_1_opt.has_value());
  OwnedChunk& chunk_1 = *chunk_1_opt;
  std::optional<OwnedChunk> chunk_2_opt = chunk_1->TakeSuffix(kTaken);
  ASSERT_TRUE(chunk_2_opt.has_value());
  OwnedChunk& chunk_2 = *chunk_2_opt;
  EXPECT_EQ(chunk_1.size(), kArbitraryChunkSize - kTaken);
  EXPECT_EQ(chunk_2.size(), kTaken);

  EXPECT_TRUE(chunk_1->CanMerge(*chunk_2));
  EXPECT_TRUE(chunk_1->Merge(chunk_2));
  EXPECT_EQ(chunk_1.size(), kArbitraryChunkSize);
  EXPECT_EQ(chunk_2.size(), 0_size);
}

}  // namespace
}  // namespace pw::multibuf::v1
