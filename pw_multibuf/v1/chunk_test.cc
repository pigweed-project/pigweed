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

#include <memory>

#include "pw_multibuf_private/chunk_testing.h"

#if __cplusplus >= 202002L
#include <ranges>
#endif  // __cplusplus >= 202002L

namespace {

// Test fixtures.

using pw::multibuf::OwnedChunk;
using pw::multibuf::test::ChunkTest;

// Some operations that fail in v1 succeed with the v1_adapter.
// See also v1_adapter::Chunk::ClaimPrefix and v1_adapter::Chunk::ClaimSuffix.
#if PW_MULTIBUF_VERSION == 1
#define EXPECT_FALSE_V1(expr) EXPECT_FALSE(expr)
#else
#define EXPECT_FALSE_V1(expr) std::ignore = (expr)
#endif

// Unit tests.

#if __cplusplus >= 202002L
static_assert(std::ranges::contiguous_range<pw::multibuf::Chunk>);
#endif  // __cplusplus >= 202002L

void TakesSpan([[maybe_unused]] pw::ByteSpan span) {}

TEST_F(ChunkTest, IsImplicitlyConvertibleToSpan) {
  OwnedChunk chunk = MakeChunk(kArbitraryChunkSize);
  TakesSpan(*chunk);
}

TEST_F(ChunkTest, ReleaseDestroysOwnedChunkRegion) {
  EXPECT_FALSE(HasAllocations());
  OwnedChunk chunk = MakeChunk(kArbitraryChunkSize);
  EXPECT_TRUE(HasAllocations());
  EXPECT_EQ(chunk.size(), kArbitraryChunkSize);

  chunk.Release();
  EXPECT_EQ(chunk.size(), 0u);
  EXPECT_FALSE(HasAllocations());
}

TEST_F(ChunkTest, DestructorDestroysOwnedChunkRegion) {
  EXPECT_FALSE(HasAllocations());
  {
    OwnedChunk chunk = MakeChunk(kArbitraryChunkSize);
    EXPECT_TRUE(HasAllocations());
    EXPECT_EQ(chunk->size(), kArbitraryChunkSize);
  }
  EXPECT_FALSE(HasAllocations());
}

TEST_F(ChunkTest, DiscardPrefixDiscardsPrefixOfSpan) {
  OwnedChunk chunk = MakeChunk(kArbitraryChunkSize);
  pw::ConstByteSpan old_span = chunk;
  const size_t kDiscarded = 4;
  chunk->DiscardPrefix(kDiscarded);
  EXPECT_EQ(chunk.size(), old_span.size() - kDiscarded);
  EXPECT_EQ(chunk.data(), old_span.data() + kDiscarded);
}

TEST_F(ChunkTest, TakePrefixTakesPrefixOfSpan) {
  OwnedChunk chunk = MakeChunk(kArbitraryChunkSize);
  pw::ConstByteSpan old_span = chunk;
  const size_t kTaken = 4;
  std::optional<OwnedChunk> front_opt = chunk->TakePrefix(kTaken);
  ASSERT_TRUE(front_opt.has_value());
  auto& front = *front_opt;
  EXPECT_EQ(front->size(), kTaken);
  EXPECT_EQ(front->data(), old_span.data());
  EXPECT_EQ(chunk.size(), old_span.size() - kTaken);
  EXPECT_EQ(chunk.data(), old_span.data() + kTaken);
}

TEST_F(ChunkTest, TruncateDiscardsEndOfSpan) {
  OwnedChunk chunk = MakeChunk(kArbitraryChunkSize);
  pw::ConstByteSpan old_span = chunk;
  const size_t kShorter = 5;
  chunk->Truncate(old_span.size() - kShorter);
  EXPECT_EQ(chunk.size(), old_span.size() - kShorter);
  EXPECT_EQ(chunk.data(), old_span.data());
}

TEST_F(ChunkTest, TakeSuffixTakesEndOfSpan) {
  OwnedChunk chunk = MakeChunk(kArbitraryChunkSize);
  pw::ConstByteSpan old_span = chunk;
  const size_t kTaken = 5;
  std::optional<OwnedChunk> tail_opt = chunk->TakeSuffix(kTaken);
  ASSERT_TRUE(tail_opt.has_value());
  auto& tail = *tail_opt;
  EXPECT_EQ(tail.size(), kTaken);
  EXPECT_EQ(tail.data(), old_span.data() + old_span.size() - kTaken);
  EXPECT_EQ(chunk.size(), old_span.size() - kTaken);
  EXPECT_EQ(chunk.data(), old_span.data());
}

TEST_F(ChunkTest, SliceRemovesSidesOfSpan) {
  OwnedChunk chunk = MakeChunk(kArbitraryChunkSize);
  pw::ConstByteSpan old_span = chunk;
  const size_t kBegin = 4;
  const size_t kEnd = 9;
  chunk->Slice(kBegin, kEnd);
  EXPECT_EQ(chunk.data(), old_span.data() + kBegin);
  EXPECT_EQ(chunk.size(), kEnd - kBegin);
}

TEST_F(ChunkTest, RegionPersistsUntilAllChunksReleased) {
  EXPECT_FALSE(HasAllocations());
  OwnedChunk chunk = MakeChunk(kArbitraryChunkSize);
  EXPECT_TRUE(HasAllocations());
  const size_t kSplitPoint = 13;
  auto split_opt = chunk->TakePrefix(kSplitPoint);
  ASSERT_TRUE(split_opt.has_value());
  auto& split = *split_opt;
  // One allocation for the region tracker, one for each of two chunks.
  EXPECT_TRUE(HasAllocations());
  chunk.Release();
  EXPECT_TRUE(HasAllocations());
  split.Release();
  EXPECT_FALSE(HasAllocations());
}

TEST_F(ChunkTest, ClaimPrefixReclaimsDiscardedPrefix) {
  OwnedChunk chunk = MakeChunk(kArbitraryChunkSize);
  pw::ConstByteSpan old_span = chunk;
  const size_t kDiscarded = 4;
  chunk->DiscardPrefix(kDiscarded);
  EXPECT_TRUE(chunk->ClaimPrefix(kDiscarded));
  EXPECT_EQ(chunk.size(), old_span.size());
  EXPECT_EQ(chunk.data(), old_span.data());
}

TEST_F(ChunkTest, ClaimPrefixFailsOnFullRegionChunk) {
  OwnedChunk chunk = MakeChunk(kArbitraryChunkSize);
  EXPECT_FALSE(chunk->ClaimPrefix(1));
}

TEST_F(ChunkTest, ClaimPrefixFailsOnNeighboringChunk) {
  OwnedChunk chunk = MakeChunk(kArbitraryChunkSize);
  const size_t kSplitPoint = 22;
  auto front = chunk->TakePrefix(kSplitPoint);
  ASSERT_TRUE(front.has_value());
  EXPECT_FALSE_V1(chunk->ClaimPrefix(1));
}

TEST_F(ChunkTest,
       ClaimPrefixFailsAtStartOfRegionEvenAfterReleasingChunkAtEndOfRegion) {
  OwnedChunk chunk = MakeChunk(kArbitraryChunkSize);
  const size_t kTaken = 13;
  auto split = chunk->TakeSuffix(kTaken);
  ASSERT_TRUE(split.has_value());
  split->Release();
  EXPECT_FALSE(chunk->ClaimPrefix(1));
}

TEST_F(ChunkTest, ClaimPrefixReclaimsPrecedingChunksDiscardedSuffix) {
  OwnedChunk chunk = MakeChunk(kArbitraryChunkSize);
  const size_t kSplitPoint = 13;
  auto split_opt = chunk->TakePrefix(kSplitPoint);
  ASSERT_TRUE(split_opt.has_value());
  auto& split = *split_opt;
  const size_t kDiscard = 3;
  split->Truncate(split.size() - kDiscard);
  EXPECT_TRUE(chunk->ClaimPrefix(kDiscard));
#if PW_MULTIBUF_VERSION == 1
  EXPECT_FALSE(chunk->ClaimPrefix(1));
#endif
}

TEST_F(ChunkTest, ClaimSuffixReclaimsTruncatedEnd) {
  OwnedChunk chunk = MakeChunk(kArbitraryChunkSize);
  pw::ConstByteSpan old_span = *chunk;
  const size_t kDiscarded = 4;
  chunk->Truncate(old_span.size() - kDiscarded);
  EXPECT_TRUE(chunk->ClaimSuffix(kDiscarded));
  EXPECT_EQ(chunk->size(), old_span.size());
  EXPECT_EQ(chunk->data(), old_span.data());
}

TEST_F(ChunkTest, ClaimSuffixFailsOnFullRegionChunk) {
  OwnedChunk chunk = MakeChunk(kArbitraryChunkSize);
  EXPECT_FALSE(chunk->ClaimSuffix(1));
}

TEST_F(ChunkTest, ClaimSuffixFailsWithNeighboringChunk) {
  OwnedChunk chunk = MakeChunk(kArbitraryChunkSize);
  const size_t kSplitPoint = 22;
  auto split_opt = chunk->TakePrefix(kSplitPoint);
  ASSERT_TRUE(split_opt.has_value());
  auto& split = *split_opt;
  EXPECT_FALSE_V1(split->ClaimSuffix(1));
}

TEST_F(ChunkTest,
       ClaimSuffixFailsAtEndOfRegionEvenAfterReleasingFirstChunkInRegion) {
  OwnedChunk chunk = MakeChunk(kArbitraryChunkSize);
  const size_t kTaken = 22;
  auto split_opt = chunk->TakeSuffix(kTaken);
  ASSERT_TRUE(split_opt.has_value());
  auto& split = *split_opt;
  EXPECT_FALSE(split->ClaimSuffix(1));
}

TEST_F(ChunkTest, ClaimSuffixReclaimsFollowingChunksDiscardedPrefix) {
  OwnedChunk chunk = MakeChunk(kArbitraryChunkSize);
  const size_t kSplitPoint = 22;
  auto split_opt = chunk->TakePrefix(kSplitPoint);
  ASSERT_TRUE(split_opt.has_value());
  auto& split = *split_opt;
  const size_t kDiscarded = 3;
  chunk->DiscardPrefix(kDiscarded);
  EXPECT_TRUE(split->ClaimSuffix(kDiscarded));
  EXPECT_FALSE_V1(split->ClaimSuffix(1));
}

TEST_F(ChunkTest, MergeReturnsFalseForChunksFromDifferentRegions) {
  OwnedChunk chunk_1 = MakeChunk(kArbitraryChunkSize);
  OwnedChunk chunk_2 = MakeChunk(kArbitraryChunkSize);
  EXPECT_FALSE(chunk_1->CanMerge(*chunk_2));
  EXPECT_FALSE(chunk_1->Merge(chunk_2));
  // Ensure that neither chunk was modified
  EXPECT_EQ(chunk_1.size(), kArbitraryChunkSize);
  EXPECT_EQ(chunk_2.size(), kArbitraryChunkSize);
}

TEST_F(ChunkTest, MergeReturnsFalseForNonAdjacentChunksFromSameRegion) {
  const size_t kTakenFromOne = 8;
  const size_t kTakenFromTwo = 4;

  OwnedChunk chunk_1 = MakeChunk(kArbitraryChunkSize);

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

TEST_F(ChunkTest, MergeJoinsMultipleAdjacentChunksFromSameRegion) {
  const size_t kTakenFromOne = 8;
  const size_t kTakenFromTwo = 4;

  OwnedChunk chunk_1 = MakeChunk(kArbitraryChunkSize);

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
  EXPECT_EQ(chunk_2.size(), 0u);
  EXPECT_EQ(chunk_3.size(), 0u);
}

TEST_F(ChunkTest, MergeJoinsAdjacentChunksFromSameRegion) {
  const size_t kTaken = 4;

  OwnedChunk chunk_1 = MakeChunk(kArbitraryChunkSize);

  std::optional<OwnedChunk> chunk_2_opt = chunk_1->TakeSuffix(kTaken);
  ASSERT_TRUE(chunk_2_opt.has_value());
  OwnedChunk& chunk_2 = *chunk_2_opt;

  EXPECT_EQ(chunk_1.size(), kArbitraryChunkSize - kTaken);
  EXPECT_EQ(chunk_2.size(), kTaken);

  EXPECT_TRUE(chunk_1->CanMerge(*chunk_2));
  EXPECT_TRUE(chunk_1->Merge(chunk_2));
  EXPECT_EQ(chunk_1.size(), kArbitraryChunkSize);
  EXPECT_EQ(chunk_2.size(), 0u);
}

}  // namespace
