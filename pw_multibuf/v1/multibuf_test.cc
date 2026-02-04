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

#include "pw_multibuf/multibuf.h"

#include "pw_assert/check.h"
#include "pw_bytes/array.h"
#include "pw_bytes/suffix.h"
#include "pw_multibuf_private/test_utils.h"
#include "pw_span/span.h"
#include "pw_unit_test/framework.h"

namespace pw::multibuf::v1 {
namespace {

using namespace pw::multibuf::v1::test_utils;

#if __cplusplus >= 202002L
static_assert(std::forward_iterator<MultiBuf::iterator>);
static_assert(std::forward_iterator<MultiBuf::const_iterator>);
static_assert(std::forward_iterator<MultiBufChunks::iterator>);
static_assert(std::forward_iterator<MultiBufChunks::const_iterator>);
#endif  // __cplusplus >= 202002L

static_assert(
    sizeof(MultiBufChunks) == sizeof(MultiBuf),
    "MultiBuf is a byte view of MultiBufChunks and does not add members");

TEST(MultiBuf, IsDefaultConstructible) { [[maybe_unused]] MultiBuf buf; }

TEST(MultiBuf, WithOneChunkReleases) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  const auto& metrics = allocator.metrics();
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(allocator, kArbitraryChunkSize));
  EXPECT_EQ(metrics.num_allocations.value(), 2U);
  buf.Release();
  EXPECT_EQ(metrics.num_deallocations.value(), 2U);
}

TEST(MultiBuf, WithOneChunkReleasesOnDestruction) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  const auto& metrics = allocator.metrics();
  {
    MultiBuf buf;
    buf.PushFrontChunk(MakeChunk(allocator, kArbitraryChunkSize));
    EXPECT_EQ(metrics.num_allocations.value(), 2U);
  }
  EXPECT_EQ(metrics.num_deallocations.value(), 2U);
}

TEST(MultiBuf, WithMultipleChunksReleasesAllOnDestruction) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  const auto& metrics = allocator.metrics();
  {
    MultiBuf buf;
    buf.PushFrontChunk(MakeChunk(allocator, kArbitraryChunkSize));
    buf.PushFrontChunk(MakeChunk(allocator, kArbitraryChunkSize));
    EXPECT_EQ(metrics.num_allocations.value(), 4U);
  }
  EXPECT_EQ(metrics.num_deallocations.value(), 4U);
}

TEST(MultiBuf, SizeReturnsNumberOfBytes) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  EXPECT_EQ(buf.size(), 0U);
  buf.PushFrontChunk(MakeChunk(allocator, kArbitraryChunkSize));
  EXPECT_EQ(buf.size(), kArbitraryChunkSize);
  buf.PushFrontChunk(MakeChunk(allocator, kArbitraryChunkSize));
  EXPECT_EQ(buf.size(), kArbitraryChunkSize * 2);
}

TEST(MultiBuf, EmptyIfNoChunks) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  EXPECT_EQ(buf.size(), 0U);
  EXPECT_TRUE(buf.empty());
  buf.PushFrontChunk(MakeChunk(allocator, kArbitraryChunkSize));
  EXPECT_NE(buf.size(), 0U);
  EXPECT_FALSE(buf.empty());
}

TEST(MultiBuf, EmptyIfOnlyEmptyChunks) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  EXPECT_TRUE(buf.empty());
  buf.PushFrontChunk(MakeChunk(allocator, 0));
  EXPECT_TRUE(buf.empty());
  buf.PushFrontChunk(MakeChunk(allocator, 0));
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(buf.size(), 0U);
}

TEST(MultiBuf, EmptyIsFalseIfAnyNonEmptyChunks) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(allocator, 0));
  EXPECT_TRUE(buf.empty());
  buf.PushFrontChunk(MakeChunk(allocator, kArbitraryChunkSize));
  EXPECT_FALSE(buf.empty());
  EXPECT_EQ(buf.size(), kArbitraryChunkSize);
}

TEST(MultiBuf, ClaimPrefixReclaimsFirstChunkPrefix) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  OwnedChunk chunk = MakeChunk(allocator, 16);
  chunk->DiscardPrefix(7);
  buf.PushFrontChunk(std::move(chunk));
  EXPECT_EQ(buf.size(), 9U);
  EXPECT_EQ(buf.ClaimPrefix(7), true);
  EXPECT_EQ(buf.size(), 16U);
}

TEST(MultiBuf, ClaimPrefixOnFirstChunkWithoutPrefixReturnsFalse) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(allocator, 16));
  EXPECT_EQ(buf.size(), 16U);
  EXPECT_EQ(buf.ClaimPrefix(7), false);
  EXPECT_EQ(buf.size(), 16U);
}

TEST(MultiBuf, ClaimPrefixWithoutChunksReturnsFalse) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  EXPECT_EQ(buf.size(), 0U);
  EXPECT_EQ(buf.ClaimPrefix(7), false);
  EXPECT_EQ(buf.size(), 0U);
}

TEST(MultiBuf, ClaimSuffixReclaimsLastChunkSuffix) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  OwnedChunk chunk = MakeChunk(allocator, 16U);
  chunk->Truncate(9U);
  buf.PushFrontChunk(std::move(chunk));
  buf.PushFrontChunk(MakeChunk(allocator, 4U));
  EXPECT_EQ(buf.size(), 13U);
  EXPECT_EQ(buf.ClaimSuffix(7U), true);
  EXPECT_EQ(buf.size(), 20U);
}

TEST(MultiBuf, ClaimSuffixOnLastChunkWithoutSuffixReturnsFalse) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(allocator, 16U));
  EXPECT_EQ(buf.size(), 16U);
  EXPECT_EQ(buf.ClaimPrefix(7U), false);
  EXPECT_EQ(buf.size(), 16U);
}

TEST(MultiBuf, ClaimSuffixWithoutChunksReturnsFalse) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  EXPECT_EQ(buf.size(), 0U);
  EXPECT_EQ(buf.ClaimSuffix(7U), false);
  EXPECT_EQ(buf.size(), 0U);
}

TEST(MultiBuf, DiscardPrefixWithZeroDoesNothing) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.DiscardPrefix(0);
  EXPECT_EQ(buf.size(), 0U);
}

TEST(MultiBuf, DiscardPrefixDiscardsPartialChunk) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(allocator, 16U));
  buf.DiscardPrefix(5U);
  EXPECT_EQ(buf.size(), 11U);
}

TEST(MultiBuf, DiscardPrefixDiscardsWholeChunk) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(allocator, 16U));
  buf.PushFrontChunk(MakeChunk(allocator, 3U));
  buf.DiscardPrefix(16U);
  EXPECT_EQ(buf.size(), 3U);
}

TEST(MultiBuf, DiscardPrefixDiscardsMultipleChunks) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(allocator, 16U));
  buf.PushFrontChunk(MakeChunk(allocator, 4U));
  buf.PushFrontChunk(MakeChunk(allocator, 3U));
  buf.DiscardPrefix(21U);
  EXPECT_EQ(buf.size(), 2U);
}

TEST(MultiBuf, SliceDiscardsPrefixAndSuffixWholeAndPartialChunks) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk(allocator, {1_b, 1_b, 1_b}));
  buf.PushBackChunk(MakeChunk(allocator, {2_b, 2_b, 2_b}));
  buf.PushBackChunk(MakeChunk(allocator, {3_b, 3_b, 3_b}));
  buf.PushBackChunk(MakeChunk(allocator, {4_b, 4_b, 4_b}));
  buf.Slice(4, 7);
  ExpectElementsEqual(buf, {2_b, 2_b, 3_b});
}

TEST(MultiBuf, SliceDoesNotModifyChunkMemory) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  std::array<std::byte, 4> kBytes = {1_b, 2_b, 3_b, 4_b};
  OwnedChunk chunk = MakeChunk(allocator, kBytes);
  ConstByteSpan span(chunk);
  buf.PushFrontChunk(std::move(chunk));
  buf.Slice(2, 3);
  ExpectElementsEqual(span, kBytes);
}

TEST(MultiBuf, TruncateRemovesFinalEmptyChunk) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(allocator, 3U));
  buf.PushFrontChunk(MakeChunk(allocator, 3U));
  buf.Truncate(3U);
  EXPECT_EQ(buf.size(), 3U);
  EXPECT_EQ(buf.Chunks().size(), 1U);
}

TEST(MultiBuf, TruncateRemovesWholeAndPartialChunks) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(allocator, 3U));
  buf.PushFrontChunk(MakeChunk(allocator, 3U));
  buf.Truncate(2U);
  EXPECT_EQ(buf.size(), 2U);
}

TEST(MultiBuf, TruncateAfterRemovesWholeAndPartialChunks) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(allocator, 3U));
  buf.PushFrontChunk(MakeChunk(allocator, 0U));
  buf.PushFrontChunk(MakeChunk(allocator, 1U));
  auto it = buf.begin();
  ++it;
  buf.TruncateAfter(it);
  EXPECT_EQ(buf.size(), 2U);
}

TEST(MultiBuf, TruncateEmptyBuffer) {
  MultiBuf buf;
  buf.Truncate(0);
  EXPECT_TRUE(buf.empty());
}

TEST(MultiBuf, TakePrefixWithNoBytesDoesNothing) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  std::optional<MultiBuf> empty_front = buf.TakePrefix(0);
  ASSERT_TRUE(empty_front.has_value());
  EXPECT_EQ(buf.size(), 0U);
  EXPECT_EQ(empty_front->size(), 0U);
}

TEST(MultiBuf, TakePrefixReturnsPartialChunk) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk(allocator, {1_b, 2_b, 3_b}));
  std::optional<MultiBuf> old_front = buf.TakePrefix(2);
  ASSERT_TRUE(old_front.has_value());
  ExpectElementsEqual(*old_front, {1_b, 2_b});
  ExpectElementsEqual(buf, {3_b});
}

TEST(MultiBuf, TakePrefixReturnsWholeAndPartialChunks) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk(allocator, {1_b, 2_b, 3_b}));
  buf.PushBackChunk(MakeChunk(allocator, {4_b, 5_b, 6_b}));
  std::optional<MultiBuf> old_front = buf.TakePrefix(4);
  ASSERT_TRUE(old_front.has_value());
  ExpectElementsEqual(*old_front, {1_b, 2_b, 3_b, 4_b});
  ExpectElementsEqual(buf, {5_b, 6_b});
}

TEST(MultiBuf, TakeSuffixReturnsWholeAndPartialChunks) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk(allocator, {1_b, 2_b, 3_b}));
  buf.PushBackChunk(MakeChunk(allocator, {4_b, 5_b, 6_b}));
  std::optional<MultiBuf> old_tail = buf.TakeSuffix(4);
  ASSERT_TRUE(old_tail.has_value());
  ExpectElementsEqual(buf, {1_b, 2_b});
  ExpectElementsEqual(*old_tail, {3_b, 4_b, 5_b, 6_b});
}

TEST(MultiBuf, PushPrefixPrependsData) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk(allocator, {1_b, 2_b, 3_b}));
  buf.PushBackChunk(MakeChunk(allocator, {4_b, 5_b, 6_b}));
  MultiBuf buf2;
  buf2.PushBackChunk(MakeChunk(allocator, {7_b, 8_b}));
  buf2.PushPrefix(std::move(buf));
  ExpectElementsEqual(buf2, {1_b, 2_b, 3_b, 4_b, 5_b, 6_b, 7_b, 8_b});
}

TEST(MultiBuf, PushSuffixAppendsData) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk(allocator, {1_b, 2_b, 3_b}));
  buf.PushBackChunk(MakeChunk(allocator, {4_b, 5_b, 6_b}));
  MultiBuf buf2;
  buf2.PushBackChunk(MakeChunk(allocator, {7_b, 8_b}));
  buf2.PushSuffix(std::move(buf));
  ExpectElementsEqual(buf2, {7_b, 8_b, 1_b, 2_b, 3_b, 4_b, 5_b, 6_b});
}

TEST(MultiBuf, PushFrontChunkAddsBytesToFront) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;

  const std::array<std::byte, 3> kBytesOne = {0_b, 1_b, 2_b};
  auto chunk_one = MakeChunk(allocator, kBytesOne);
  buf.PushFrontChunk(std::move(chunk_one));
  ExpectElementsEqual(buf, kBytesOne);

  const std::array<std::byte, 4> kBytesTwo = {9_b, 10_b, 11_b, 12_b};
  auto chunk_two = MakeChunk(allocator, kBytesTwo);
  buf.PushFrontChunk(std::move(chunk_two));

  // clang-format off
  ExpectElementsEqual(buf, {
      9_b, 10_b, 11_b, 12_b,
      0_b, 1_b, 2_b,
  });
  // clang-format on
}

TEST(MultiBuf, InsertChunkOnEmptyBufAddsFirstChunk) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;

  const std::array<std::byte, 3> kBytes = {0_b, 1_b, 2_b};
  auto chunk = MakeChunk(allocator, kBytes);
  auto inserted_iter = buf.InsertChunk(buf.Chunks().begin(), std::move(chunk));
  EXPECT_EQ(inserted_iter, buf.Chunks().begin());
  ExpectElementsEqual(buf, kBytes);
  EXPECT_EQ(++inserted_iter, buf.Chunks().end());
}

TEST(MultiBuf, InsertChunkAtEndOfBufAddsLastChunk) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;

  // Add a chunk to the beginning
  buf.PushFrontChunk(MakeChunk(allocator, kArbitraryChunkSize));

  const std::array<std::byte, 3> kBytes = {0_b, 1_b, 2_b};
  auto chunk = MakeChunk(allocator, kBytes);
  auto inserted_iter = buf.InsertChunk(buf.Chunks().end(), std::move(chunk));
  EXPECT_EQ(inserted_iter, ++buf.Chunks().begin());
  EXPECT_EQ(++inserted_iter, buf.Chunks().end());
  const Chunk& second_chunk = *(++buf.Chunks().begin());
  ExpectElementsEqual(second_chunk, kBytes);
}

TEST(MultiBuf, TakeChunkAtBeginRemovesAndReturnsFirstChunk) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  auto insert_iter = buf.Chunks().begin();
  insert_iter = buf.InsertChunk(insert_iter, MakeChunk(allocator, 2));
  insert_iter = buf.InsertChunk(++insert_iter, MakeChunk(allocator, 4));

  auto [chunk_iter, chunk] = buf.TakeChunk(buf.Chunks().begin());
  EXPECT_EQ(chunk.size(), 2U);
  EXPECT_EQ(chunk_iter->size(), 4U);
  ++chunk_iter;
  EXPECT_EQ(chunk_iter, buf.Chunks().end());
}

TEST(MultiBuf, TakeChunkOnLastInsertedIterReturnsLastInserted) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  auto iter = buf.Chunks().begin();
  iter = buf.InsertChunk(iter, MakeChunk(allocator, 42));
  iter = buf.InsertChunk(++iter, MakeChunk(allocator, 11));
  iter = buf.InsertChunk(++iter, MakeChunk(allocator, 65));
  OwnedChunk chunk;
  std::tie(iter, chunk) = buf.TakeChunk(iter);
  EXPECT_EQ(iter, buf.Chunks().end());
  EXPECT_EQ(chunk.size(), 65U);
}

TEST(MultiBuf, RangeBasedForLoopsCompile) {
  MultiBuf buf;
  for ([[maybe_unused]] std::byte& byte : buf) {
  }
  for ([[maybe_unused]] const std::byte& byte : buf) {
  }
  for ([[maybe_unused]] Chunk& chunk : buf.Chunks()) {
  }
  for ([[maybe_unused]] const Chunk& chunk : buf.Chunks()) {
  }

  const MultiBuf const_buf;
  for ([[maybe_unused]] const std::byte& byte : const_buf) {
  }
  for ([[maybe_unused]] const Chunk& chunk : const_buf.Chunks()) {
  }
}

TEST(MultiBuf, IteratorAdvancesNAcrossChunks) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk(allocator, {1_b, 2_b, 3_b}));
  buf.PushBackChunk(MakeChunk(allocator, {4_b, 5_b, 6_b}));

  MultiBuf::iterator iter = buf.begin();
  iter += 4;
  EXPECT_EQ(*iter, 5_b);
}

TEST(MultiBuf, IteratorAdvancesNAcrossZeroLengthChunk) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk(allocator, 0));
  buf.PushBackChunk(MakeChunk(allocator, {1_b, 2_b, 3_b}));
  buf.PushBackChunk(MakeChunk(allocator, 0));
  buf.PushBackChunk(MakeChunk(allocator, {4_b, 5_b, 6_b}));

  MultiBuf::iterator iter = buf.begin();
  iter += 4;
  EXPECT_EQ(*iter, 5_b);
}

TEST(MultiBuf, ConstIteratorAdvancesNAcrossChunks) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk(allocator, {1_b, 2_b, 3_b}));
  buf.PushBackChunk(MakeChunk(allocator, {4_b, 5_b, 6_b}));

  MultiBuf::const_iterator iter = buf.cbegin();
  iter += 4;
  EXPECT_EQ(*iter, 5_b);
}

TEST(MultiBuf, IteratorSkipsEmptyChunks) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk(allocator, 0));
  buf.PushBackChunk(MakeChunk(allocator, 0));
  buf.PushBackChunk(MakeChunk(allocator, {1_b}));
  buf.PushBackChunk(MakeChunk(allocator, 0));
  buf.PushBackChunk(MakeChunk(allocator, {2_b, 3_b}));
  buf.PushBackChunk(MakeChunk(allocator, 0));

  MultiBuf::iterator it = buf.begin();
  ASSERT_EQ(*it++, 1_b);
  ASSERT_EQ(*it++, 2_b);
  ASSERT_EQ(*it++, 3_b);
  ASSERT_EQ(it, buf.end());
}

constexpr auto kSequentialBytes =
    bytes::Initialized<6>([](size_t i) { return i + 1; });

TEST(MultiBuf, CopyToFromEmptyMultiBuf) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  std::array<std::byte, 6> buffer = {};
  StatusWithSize result = buf.CopyTo(buffer);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.size(), 0u);

  result = buf.CopyTo({});
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.size(), 0u);
}

TEST(MultiBuf, CopyToEmptyDestination) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk(allocator, {1_b, 2_b, 3_b, 4_b}));
  StatusWithSize result = buf.CopyTo({});
  ASSERT_EQ(result.status(), Status::ResourceExhausted());
  EXPECT_EQ(result.size(), 0u);
}

TEST(MultiBuf, CopyToOneChunk) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk(allocator, {1_b, 2_b, 3_b, 4_b}));

  std::array<std::byte, 4> buffer = {};
  StatusWithSize result = buf.CopyTo(buffer);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.size(), 4u);
  EXPECT_TRUE(
      std::equal(buffer.begin(), buffer.end(), kSequentialBytes.begin()));
}

TEST(MultiBuf, CopyToVariousChunks) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk(allocator, {1_b}));
  buf.PushBackChunk(MakeChunk(allocator, {2_b, 3_b}));
  buf.PushBackChunk(MakeChunk(allocator, {}));
  buf.PushBackChunk(MakeChunk(allocator, {4_b, 5_b, 6_b}));

  std::array<std::byte, 6> buffer = {};
  StatusWithSize result = buf.CopyTo(buffer);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.size(), 6u);
  EXPECT_TRUE(
      std::equal(buffer.begin(), buffer.end(), kSequentialBytes.begin()));
}

TEST(MultiBuf, CopyToInTwoParts) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;

  constexpr size_t kMultiBufSize = 6;
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk(allocator, {1_b}));
  buf.PushBackChunk(MakeChunk(allocator, {2_b, 3_b}));
  buf.PushBackChunk(MakeChunk(allocator, {}));
  buf.PushBackChunk(MakeChunk(allocator, {4_b, 5_b, 6_b}));
  ASSERT_EQ(buf.size(), kMultiBufSize);

  for (size_t first = 0; first < kMultiBufSize; ++first) {
    std::array<std::byte, kMultiBufSize> buffer = {};
    StatusWithSize result = buf.CopyTo(span(buffer).first(first));
    ASSERT_EQ(result.status(), Status::ResourceExhausted());
    ASSERT_EQ(result.size(), first);

    result = buf.CopyTo(span(buffer).last(kMultiBufSize - first),
                        result.size());  // start from last offset
    ASSERT_EQ(result.status(), OkStatus());
    ASSERT_EQ(result.size(), kMultiBufSize - first);

    ASSERT_TRUE(
        std::equal(buffer.begin(), buffer.end(), kSequentialBytes.begin()))
        << "The whole buffer should have copied";
  }
}

TEST(MultiBuf, CopyToPositionIsEnd) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk(allocator, {1_b}));
  buf.PushBackChunk(MakeChunk(allocator, {2_b, 3_b}));
  buf.PushBackChunk(MakeChunk(allocator, {}));

  StatusWithSize result = buf.CopyTo({}, 3u);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.size(), 0u);
}

TEST(MultiBuf, CopyFromIntoOneChunk) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf mb;
  mb.PushBackChunk(MakeChunk(allocator, 6));

  StatusWithSize result = mb.CopyFrom(kSequentialBytes);
  EXPECT_EQ(result.status(), OkStatus());
  ASSERT_EQ(result.size(), 6u);
  EXPECT_TRUE(std::equal(mb.begin(), mb.end(), kSequentialBytes.begin()));
}

TEST(MultiBuf, CopyFromIntoMultipleChunks) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf mb;
  mb.PushBackChunk(MakeChunk(allocator, 2));
  mb.PushBackChunk(MakeChunk(allocator, 0));
  mb.PushBackChunk(MakeChunk(allocator, 3));
  mb.PushBackChunk(MakeChunk(allocator, 1));
  mb.PushBackChunk(MakeChunk(allocator, 0));

  StatusWithSize result = mb.CopyFrom(kSequentialBytes);
  EXPECT_EQ(result.status(), OkStatus());
  ASSERT_EQ(result.size(), 6u);
  EXPECT_TRUE(std::equal(mb.begin(), mb.end(), kSequentialBytes.begin()));
}

TEST(MultiBuf, CopyFromInTwoParts) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;

  for (size_t first = 0; first < kSequentialBytes.size(); ++first) {
    MultiBuf mb;
    mb.PushBackChunk(MakeChunk(allocator, 1));
    mb.PushBackChunk(MakeChunk(allocator, 0));
    mb.PushBackChunk(MakeChunk(allocator, 0));
    mb.PushBackChunk(MakeChunk(allocator, 2));
    mb.PushBackChunk(MakeChunk(allocator, 3));
    ASSERT_EQ(mb.size(), kSequentialBytes.size());

    StatusWithSize result = mb.CopyFrom(span(kSequentialBytes).first(first));
    ASSERT_EQ(result.status(), OkStatus());
    ASSERT_EQ(result.size(), first);

    result = mb.CopyFrom(
        span(kSequentialBytes).last(kSequentialBytes.size() - first),
        result.size());  // start from last offset
    ASSERT_EQ(result.status(), OkStatus());
    ASSERT_EQ(result.size(), kSequentialBytes.size() - first);

    ASSERT_TRUE(std::equal(mb.begin(), mb.end(), kSequentialBytes.begin()))
        << "The whole buffer should have copied";
  }
}

TEST(MultiBuf, CopyFromAndTruncate) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;

  for (size_t to_copy = 0; to_copy < kSequentialBytes.size(); ++to_copy) {
    MultiBuf mb;
    mb.PushBackChunk(MakeChunk(allocator, 1));
    mb.PushBackChunk(MakeChunk(allocator, 0));
    mb.PushBackChunk(MakeChunk(allocator, 0));
    mb.PushBackChunk(MakeChunk(allocator, 2));
    mb.PushBackChunk(MakeChunk(allocator, 3));
    mb.PushBackChunk(MakeChunk(allocator, 0));
    ASSERT_EQ(mb.size(), kSequentialBytes.size());

    StatusWithSize result =
        mb.CopyFromAndTruncate(span(kSequentialBytes).first(to_copy));
    ASSERT_EQ(result.status(), OkStatus());
    ASSERT_EQ(result.size(), to_copy);
    ASSERT_EQ(mb.size(), result.size());
    ASSERT_TRUE(std::equal(mb.begin(), mb.end(), kSequentialBytes.begin()));
  }
}

TEST(MultiBuf, CopyFromAndTruncateFromOffset) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;

  static constexpr std::array<std::byte, 6> kZeroes = {};

  // Sweep offsets 0–6 (inclusive), and copy 0–all bytes for each offset.
  for (size_t offset = 0; offset <= kSequentialBytes.size(); ++offset) {
    for (size_t to_copy = 0; to_copy <= kSequentialBytes.size() - offset;
         ++to_copy) {
      MultiBuf mb;
      mb.PushBackChunk(MakeChunk(allocator, 2));
      mb.PushBackChunk(MakeChunk(allocator, 0));
      mb.PushBackChunk(MakeChunk(allocator, 3));
      mb.PushBackChunk(MakeChunk(allocator, 0));
      mb.PushBackChunk(MakeChunk(allocator, 0));
      mb.PushBackChunk(MakeChunk(allocator, 1));
      ASSERT_EQ(mb.size(), kSequentialBytes.size());

      StatusWithSize result =
          mb.CopyFromAndTruncate(span(kSequentialBytes).first(to_copy), offset);
      ASSERT_EQ(result.status(), OkStatus());
      ASSERT_EQ(result.size(), to_copy);
      ASSERT_EQ(mb.size(), offset + to_copy);

      // MultiBuf contains to_copy 0s followed by to_copy sequential bytes.
      ASSERT_TRUE(std::equal(mb.begin(), mb.begin() + offset, kZeroes.begin()));
      ASSERT_TRUE(
          std::equal(mb.begin() + offset, mb.end(), kSequentialBytes.begin()));
    }
  }
}

TEST(MultiBuf, CopyFromIntoEmptyMultibuf) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf mb;

  StatusWithSize result = mb.CopyFrom({});
  EXPECT_EQ(result.status(), OkStatus());  // empty source, so copy succeeded
  EXPECT_EQ(result.size(), 0u);

  result = mb.CopyFrom(kSequentialBytes);
  EXPECT_EQ(result.status(), Status::ResourceExhausted());
  EXPECT_EQ(result.size(), 0u);

  mb.PushBackChunk(MakeChunk(allocator, 0));  // add an empty chunk

  result = mb.CopyFrom({});
  EXPECT_EQ(result.status(), OkStatus());  // empty source, so copy succeeded
  EXPECT_EQ(result.size(), 0u);

  result = mb.CopyFrom(kSequentialBytes);
  EXPECT_EQ(result.status(), Status::ResourceExhausted());
  EXPECT_EQ(result.size(), 0u);
}

TEST(MultiBuf, IsContiguousTrueForEmptyBuffer) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;

  MultiBuf buf;
  EXPECT_TRUE(buf.IsContiguous());

  buf.PushBackChunk(MakeChunk(allocator, {}));
  EXPECT_TRUE(buf.IsContiguous());
  buf.PushBackChunk(MakeChunk(allocator, {}));
  EXPECT_TRUE(buf.IsContiguous());
  buf.PushBackChunk(MakeChunk(allocator, {}));
  EXPECT_TRUE(buf.IsContiguous());
}

TEST(MultiBuf, IsContiguousTrueForSingleNonEmptyChunk) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;

  MultiBuf buf;
  buf.PushBackChunk(MakeChunk(allocator, {1_b}));
  EXPECT_TRUE(buf.IsContiguous());
  buf.PushBackChunk(MakeChunk(allocator, {}));
  EXPECT_TRUE(buf.IsContiguous());
  buf.PushFrontChunk(MakeChunk(allocator, {}));
  EXPECT_TRUE(buf.IsContiguous());
}

TEST(MultiBuf, IsContiguousFalseIfMultipleNonEmptyChunks) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;

  MultiBuf buf;
  buf.PushBackChunk(MakeChunk(allocator, {1_b}));
  buf.PushBackChunk(MakeChunk(allocator, {2_b}));
  EXPECT_FALSE(buf.IsContiguous());
}

TEST(MultiBuf, ContiguousSpanAcrossMultipleChunks) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  OwnedChunk chunk_1 = MakeChunk(allocator, 10);
  const ConstByteSpan contiguous_span = chunk_1;
  OwnedChunk chunk_2 = chunk_1->TakeSuffix(5).value();
  OwnedChunk chunk_3 = chunk_2->TakeSuffix(5).value();
  OwnedChunk chunk_4 = chunk_3->TakeSuffix(1).value();

  MultiBuf buf;
  buf.PushBackChunk(std::move(chunk_1));       // 5 bytes
  buf.PushBackChunk(std::move(chunk_2));       // 0 bytes
  buf.PushBackChunk(std::move(chunk_3));       // 4 bytes
  buf.PushBackChunk(std::move(chunk_4));       // 1 byte
  buf.PushBackChunk(MakeChunk(allocator, 0));  // empty

  auto it = buf.Chunks().begin();
  ASSERT_EQ((it++)->size(), 5u);
  ASSERT_EQ((it++)->size(), 0u);
  ASSERT_EQ((it++)->size(), 4u);
  ASSERT_EQ((it++)->size(), 1u);
  ASSERT_EQ((it++)->size(), 0u);
  ASSERT_EQ(it, buf.Chunks().end());

  EXPECT_TRUE(buf.IsContiguous());
  ByteSpan span = buf.ContiguousSpan().value();
  EXPECT_EQ(span.data(), contiguous_span.data());
  EXPECT_EQ(span.size(), contiguous_span.size());

  it = buf.Chunks().begin();
  buf.InsertChunk(++it, MakeChunk(allocator, 1));
  EXPECT_FALSE(buf.IsContiguous());
}

}  // namespace
}  // namespace pw::multibuf::v1
