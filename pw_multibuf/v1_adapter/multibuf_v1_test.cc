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

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <tuple>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_allocator/bump_allocator.h"
#include "pw_allocator/testing.h"
#include "pw_allocator/unique_ptr.h"
#include "pw_assert/check.h"
#include "pw_bytes/array.h"
#include "pw_bytes/span.h"
#include "pw_bytes/suffix.h"
#include "pw_multibuf/multibuf.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"
#include "pw_unit_test/framework.h"

namespace pw::multibuf::v1_adapter {

#if __cplusplus >= 202002L
static_assert(std::forward_iterator<MultiBuf::iterator>);
static_assert(std::forward_iterator<MultiBuf::const_iterator>);
#endif  // __cplusplus >= 202002L

// Test fixture
class MultiBufV1Test : public ::testing::Test {
 private:
  // Arbitrary sizes intended to be large enough to store the Chunk and data
  // slices. These may be increased if `MakeChunk` fails.
  static constexpr size_t kMetaSizeBytes = 512;
  static constexpr size_t kDataSizeBytes = 2048;

 protected:
  static constexpr size_t kArbitraryChunkSize = 32;

  size_t num_data_allocations() const {
    const auto& metrics = data_allocator_.metrics();
    return metrics.num_allocations.value() - metrics.num_deallocations.value();
  }

  OwnedChunk MakeChunk(size_t size, std::byte initializer = std::byte(0)) {
    OwnedChunk chunk = DoMakeChunk(size);
    if (size != 0) {
      std::memset(
          chunk.data(), static_cast<uint8_t>(initializer), chunk.size());
    }
    return chunk;
  }

  OwnedChunk MakeChunk(std::initializer_list<std::byte> data) {
    OwnedChunk chunk = DoMakeChunk(data.size());
    std::copy(data.begin(), data.end(), chunk.data());
    return chunk;
  }

  OwnedChunk MakeChunk(ConstByteSpan data) {
    OwnedChunk chunk = DoMakeChunk(data.size());
    std::copy(data.begin(), data.end(), chunk.data());
    return chunk;
  }

 private:
  OwnedChunk DoMakeChunk(size_t size) {
    SharedPtr<std::byte[]> shared;
    if (size != 0) {
      shared = data_allocator_.MakeShared<std::byte[]>(size);
      PW_CHECK(shared != nullptr);
    }
    return OwnedChunk(meta_allocator_, shared);
  }

  allocator::test::AllocatorForTest<kDataSizeBytes> data_allocator_;
  allocator::test::AllocatorForTest<kMetaSizeBytes> meta_allocator_;
};

template <typename ActualIterable, typename ExpectedIterable>
void ExpectElementsEqual(const ActualIterable& actual,
                         const ExpectedIterable& expected) {
  auto actual_iter = actual.begin();
  auto expected_iter = expected.begin();
  for (; expected_iter != expected.end(); ++actual_iter, ++expected_iter) {
    ASSERT_NE(actual_iter, actual.end());
    EXPECT_EQ(*actual_iter, *expected_iter);
  }
}

template <typename ActualIterable, typename T>
void ExpectElementsEqual(const ActualIterable& actual,
                         std::initializer_list<T> expected) {
  ExpectElementsEqual<ActualIterable, std::initializer_list<T>>(actual,
                                                                expected);
}

template <typename ActualIterable,
          typename T = typename ActualIterable::iterator::value_type>
void ExpectElementsAre(const ActualIterable& actual, T value) {
  auto actual_iter = actual.begin();
  for (; actual_iter != actual.end(); ++actual_iter) {
    ASSERT_NE(actual_iter, actual.end());
    EXPECT_EQ(*actual_iter, value);
  }
}

// LINT.IfChange
// Keep these tests in sync with those in ../v1/multibuf_test.cc
TEST_F(MultiBufV1Test, WithOneChunkReleases) {
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(kArbitraryChunkSize));
  EXPECT_EQ(num_data_allocations(), 1U);
  buf.Release();
  EXPECT_EQ(num_data_allocations(), 0U);
}

TEST_F(MultiBufV1Test, WithOneChunkReleasesOnDestruction) {
  {
    MultiBuf buf;
    buf.PushFrontChunk(MakeChunk(kArbitraryChunkSize));
    EXPECT_EQ(num_data_allocations(), 1U);
  }
  EXPECT_EQ(num_data_allocations(), 0U);
}

TEST_F(MultiBufV1Test, WithMultipleChunksReleasesAllOnDestruction) {
  {
    MultiBuf buf;
    buf.PushFrontChunk(MakeChunk(kArbitraryChunkSize));
    buf.PushFrontChunk(MakeChunk(kArbitraryChunkSize));
    EXPECT_EQ(num_data_allocations(), 2U);
  }
  EXPECT_EQ(num_data_allocations(), 0U);
}

TEST_F(MultiBufV1Test, SizeReturnsNumberOfBytes) {
  MultiBuf buf;
  EXPECT_EQ(buf.size(), 0U);
  buf.PushFrontChunk(MakeChunk(kArbitraryChunkSize));
  EXPECT_EQ(buf.size(), kArbitraryChunkSize);
  buf.PushFrontChunk(MakeChunk(kArbitraryChunkSize));
  EXPECT_EQ(buf.size(), kArbitraryChunkSize * 2);
}

TEST_F(MultiBufV1Test, EmptyIfNoChunks) {
  MultiBuf buf;
  EXPECT_EQ(buf.size(), 0U);
  EXPECT_TRUE(buf.empty());
  buf.PushFrontChunk(MakeChunk(kArbitraryChunkSize));
  EXPECT_NE(buf.size(), 0U);
  EXPECT_FALSE(buf.empty());
}

TEST_F(MultiBufV1Test, EmptyIfOnlyEmptyChunks) {
  MultiBuf buf;
  EXPECT_TRUE(buf.empty());
  buf.PushFrontChunk(MakeChunk(0));
  EXPECT_TRUE(buf.empty());
  buf.PushFrontChunk(MakeChunk(0));
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(buf.size(), 0U);
}

TEST_F(MultiBufV1Test, EmptyIsFalseIfAnyNonEmptyChunks) {
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(0));
  EXPECT_TRUE(buf.empty());
  buf.PushFrontChunk(MakeChunk(kArbitraryChunkSize));
  EXPECT_FALSE(buf.empty());
  EXPECT_EQ(buf.size(), kArbitraryChunkSize);
}

TEST_F(MultiBufV1Test, ClaimPrefixReclaimsFirstChunkPrefix) {
  MultiBuf buf;
  OwnedChunk chunk = MakeChunk(16);
  buf.PushFrontChunk(std::move(chunk));
  buf.DiscardPrefix(7);
  EXPECT_EQ(buf.size(), 9U);
  EXPECT_EQ(buf.ClaimPrefix(7), true);
  EXPECT_EQ(buf.size(), 16U);
}

TEST_F(MultiBufV1Test, ClaimPrefixOnFirstChunkWithoutPrefixReturnsFalse) {
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(16));
  EXPECT_EQ(buf.size(), 16U);
  EXPECT_EQ(buf.ClaimPrefix(7), false);
  EXPECT_EQ(buf.size(), 16U);
}

TEST_F(MultiBufV1Test, ClaimPrefixWithoutChunksReturnsFalse) {
  MultiBuf buf;
  EXPECT_EQ(buf.size(), 0U);
  EXPECT_EQ(buf.ClaimPrefix(7), false);
  EXPECT_EQ(buf.size(), 0U);
}

TEST_F(MultiBufV1Test, ClaimSuffixReclaimsLastChunkSuffix) {
  MultiBuf buf;
  OwnedChunk chunk = MakeChunk(16U);
  buf.PushFrontChunk(std::move(chunk));
  buf.Truncate(9U);
  buf.PushFrontChunk(MakeChunk(4U));
  EXPECT_EQ(buf.size(), 13U);
  EXPECT_EQ(buf.ClaimSuffix(7U), true);
  EXPECT_EQ(buf.size(), 20U);
}

TEST_F(MultiBufV1Test, ClaimSuffixOnLastChunkWithoutSuffixReturnsFalse) {
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(16U));
  EXPECT_EQ(buf.size(), 16U);
  EXPECT_EQ(buf.ClaimPrefix(7U), false);
  EXPECT_EQ(buf.size(), 16U);
}

TEST_F(MultiBufV1Test, ClaimSuffixWithoutChunksReturnsFalse) {
  MultiBuf buf;
  EXPECT_EQ(buf.size(), 0U);
  EXPECT_EQ(buf.ClaimSuffix(7U), false);
  EXPECT_EQ(buf.size(), 0U);
}

TEST_F(MultiBufV1Test, DiscardPrefixWithZeroDoesNothing) {
  MultiBuf buf;
  buf.DiscardPrefix(0);
  EXPECT_EQ(buf.size(), 0U);
}

TEST_F(MultiBufV1Test, DiscardPrefixDiscardsPartialChunk) {
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(16U));
  buf.DiscardPrefix(5U);
  EXPECT_EQ(buf.size(), 11U);
}

TEST_F(MultiBufV1Test, DiscardPrefixDiscardsWholeChunk) {
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(16U));
  buf.PushFrontChunk(MakeChunk(3U));
  buf.DiscardPrefix(16U);
  EXPECT_EQ(buf.size(), 3U);
}

TEST_F(MultiBufV1Test, DiscardPrefixDiscardsMultipleChunks) {
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(16U));
  buf.PushFrontChunk(MakeChunk(4U));
  buf.PushFrontChunk(MakeChunk(3U));
  buf.DiscardPrefix(21U);
  EXPECT_EQ(buf.size(), 2U);
}

TEST_F(MultiBufV1Test, SliceDiscardsPrefixAndSuffixWholeAndPartialChunks) {
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk({1_b, 1_b, 1_b}));
  buf.PushBackChunk(MakeChunk({2_b, 2_b, 2_b}));
  buf.PushBackChunk(MakeChunk({3_b, 3_b, 3_b}));
  buf.PushBackChunk(MakeChunk({4_b, 4_b, 4_b}));
  buf.Slice(4, 7);
  ExpectElementsEqual(buf, {2_b, 2_b, 3_b});
}

TEST_F(MultiBufV1Test, SliceDoesNotModifyChunkMemory) {
  MultiBuf buf;
  std::array<std::byte, 4> kBytes = {1_b, 2_b, 3_b, 4_b};
  OwnedChunk chunk = MakeChunk(kBytes);
  const ConstByteSpan span = *chunk;
  buf.PushFrontChunk(std::move(chunk));
  buf.Slice(2, 3);
  ExpectElementsEqual(span, kBytes);
}

TEST_F(MultiBufV1Test, TruncateRemovesFinalEmptyChunk) {
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(3U));
  buf.PushFrontChunk(MakeChunk(3U));
  buf.Truncate(3U);
  EXPECT_EQ(buf.size(), 3U);
  EXPECT_EQ(buf.Chunks().size(), 1U);
}

TEST_F(MultiBufV1Test, TruncateRemovesWholeAndPartialChunks) {
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(3U));
  buf.PushFrontChunk(MakeChunk(3U));
  buf.Truncate(2U);
  EXPECT_EQ(buf.size(), 2U);
}

TEST_F(MultiBufV1Test, TruncateAfterRemovesWholeAndPartialChunks) {
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(3U));
  buf.PushFrontChunk(MakeChunk(0U));
  buf.PushFrontChunk(MakeChunk(1U));
  auto it = buf.begin();
  ++it;
  buf.TruncateAfter(it);
  EXPECT_EQ(buf.size(), 2U);
}

TEST_F(MultiBufV1Test, TruncateEmptyBuffer) {
  MultiBuf buf;
  buf.Truncate(0);
  EXPECT_TRUE(buf.empty());
}

TEST_F(MultiBufV1Test, TakePrefixWithNoBytesDoesNothing) {
  MultiBuf buf;
  std::optional<MultiBuf> empty_front = buf.TakePrefix(0);
  ASSERT_TRUE(empty_front.has_value());
  EXPECT_EQ(buf.size(), 0U);
  EXPECT_EQ(empty_front->size(), 0U);
}

TEST_F(MultiBufV1Test, TakePrefixReturnsPartialChunk) {
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk({1_b, 2_b, 3_b}));
  std::optional<MultiBuf> old_front = buf.TakePrefix(2);
  ASSERT_TRUE(old_front.has_value());
  ExpectElementsEqual(*old_front, {1_b, 2_b});
  ExpectElementsEqual(buf, {3_b});
}

TEST_F(MultiBufV1Test, TakePrefixReturnsWholeAndPartialChunks) {
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk({1_b, 2_b, 3_b}));
  buf.PushBackChunk(MakeChunk({4_b, 5_b, 6_b}));
  std::optional<MultiBuf> old_front = buf.TakePrefix(4);
  ASSERT_TRUE(old_front.has_value());
  ExpectElementsEqual(*old_front, {1_b, 2_b, 3_b, 4_b});
  ExpectElementsEqual(buf, {5_b, 6_b});
}

TEST_F(MultiBufV1Test, TakeSuffixReturnsWholeAndPartialChunks) {
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk({1_b, 2_b, 3_b}));
  buf.PushBackChunk(MakeChunk({4_b, 5_b, 6_b}));
  std::optional<MultiBuf> old_tail = buf.TakeSuffix(4);
  ASSERT_TRUE(old_tail.has_value());
  ExpectElementsEqual(buf, {1_b, 2_b});
  ExpectElementsEqual(*old_tail, {3_b, 4_b, 5_b, 6_b});
}

TEST_F(MultiBufV1Test, PushPrefixPrependsData) {
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk({1_b, 2_b, 3_b}));
  buf.PushBackChunk(MakeChunk({4_b, 5_b, 6_b}));
  MultiBuf buf2;
  buf2.PushBackChunk(MakeChunk({7_b, 8_b}));
  buf2.PushPrefix(std::move(buf));
  ExpectElementsEqual(buf2, {1_b, 2_b, 3_b, 4_b, 5_b, 6_b, 7_b, 8_b});
}

TEST_F(MultiBufV1Test, PushSuffixAppendsData) {
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk({1_b, 2_b, 3_b}));
  buf.PushBackChunk(MakeChunk({4_b, 5_b, 6_b}));
  MultiBuf buf2;
  buf2.PushBackChunk(MakeChunk({7_b, 8_b}));
  buf2.PushSuffix(std::move(buf));
  ExpectElementsEqual(buf2, {7_b, 8_b, 1_b, 2_b, 3_b, 4_b, 5_b, 6_b});
}

TEST_F(MultiBufV1Test, PushFrontChunkAddsBytesToFront) {
  MultiBuf buf;

  const std::array<std::byte, 3> kBytesOne = {0_b, 1_b, 2_b};
  auto chunk_one = MakeChunk(kBytesOne);
  buf.PushFrontChunk(std::move(chunk_one));
  ExpectElementsEqual(buf, kBytesOne);

  const std::array<std::byte, 4> kBytesTwo = {9_b, 10_b, 11_b, 12_b};
  auto chunk_two = MakeChunk(kBytesTwo);
  buf.PushFrontChunk(std::move(chunk_two));

  // clang-format off
  ExpectElementsEqual(buf, {
      9_b, 10_b, 11_b, 12_b,
      0_b, 1_b, 2_b,
  });
  // clang-format on
}

TEST_F(MultiBufV1Test, InsertChunkOnEmptyBufAddsFirstChunk) {
  MultiBuf buf;

  const std::array<std::byte, 3> kBytes = {0_b, 1_b, 2_b};
  auto chunk = MakeChunk(kBytes);
  auto inserted_iter = buf.InsertChunk(buf.Chunks().begin(), std::move(chunk));
  EXPECT_EQ(inserted_iter, buf.Chunks().begin());
  EXPECT_EQ(inserted_iter->size(), 3u);

  EXPECT_EQ(inserted_iter, buf.Chunks().begin());
  ExpectElementsEqual(buf, kBytes);
  EXPECT_EQ(++inserted_iter, buf.Chunks().end());
}

TEST_F(MultiBufV1Test, InsertChunkAtEndOfBufAddsLastChunk) {
  MultiBuf buf;

  // Add a chunk to the beginning
  buf.PushFrontChunk(MakeChunk(kArbitraryChunkSize));

  const std::array<std::byte, 3> kBytes = {0_b, 1_b, 2_b};
  auto chunk = MakeChunk(kBytes);
  auto inserted_iter = buf.InsertChunk(buf.Chunks().end(), std::move(chunk));

  EXPECT_EQ(inserted_iter, ++buf.Chunks().begin());
  auto next_iter = inserted_iter;
  EXPECT_EQ(++next_iter, buf.Chunks().end());
  const Chunk& second_chunk = *inserted_iter;
  ExpectElementsEqual(second_chunk, kBytes);
}

TEST_F(MultiBufV1Test, TakeChunkAtBeginRemovesAndReturnsFirstChunk) {
  MultiBuf buf;
  auto insert_iter = buf.Chunks().begin();
  insert_iter = buf.InsertChunk(insert_iter, MakeChunk(2));
  insert_iter = buf.InsertChunk(++insert_iter, MakeChunk(4));

  auto [chunk_iter, chunk] = buf.TakeChunk(buf.Chunks().begin());
  EXPECT_EQ(chunk.size(), 2U);
  EXPECT_EQ(chunk_iter->size(), 4U);
  ++chunk_iter;
  EXPECT_EQ(chunk_iter, buf.Chunks().end());
}

TEST_F(MultiBufV1Test, TakeChunkOnLastInsertedIterReturnsLastInserted) {
  MultiBuf buf;
  auto iter = buf.Chunks().begin();
  iter = buf.InsertChunk(iter, MakeChunk(42));
  EXPECT_NE(iter, buf.Chunks().end());
  ++iter;
  ASSERT_EQ(iter, buf.Chunks().end());
  iter = buf.InsertChunk(iter, MakeChunk(11));
  EXPECT_NE(iter, buf.Chunks().end());
  ++iter;
  ASSERT_EQ(iter, buf.Chunks().end());
  iter = buf.InsertChunk(iter, MakeChunk(65));
  EXPECT_NE(iter, buf.Chunks().end());

  OwnedChunk chunk;
  std::tie(iter, chunk) = buf.TakeChunk(iter);
  EXPECT_EQ(iter, buf.Chunks().end());
  EXPECT_EQ(chunk.size(), 65U);
}

TEST_F(MultiBufV1Test, RangeBasedForLoopsCompile) {
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

TEST_F(MultiBufV1Test, IteratorAdvancesNAcrossChunks) {
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk({1_b, 2_b, 3_b}));
  buf.PushBackChunk(MakeChunk({4_b, 5_b, 6_b}));

  MultiBuf::iterator iter = buf.begin();
  iter += 4;
  EXPECT_EQ(*iter, 5_b);
}

TEST_F(MultiBufV1Test, IteratorAdvancesNAcrossZeroLengthChunk) {
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk(0));
  buf.PushBackChunk(MakeChunk({1_b, 2_b, 3_b}));
  buf.PushBackChunk(MakeChunk(0));
  buf.PushBackChunk(MakeChunk({4_b, 5_b, 6_b}));

  MultiBuf::iterator iter = buf.begin();
  iter += 4;
  EXPECT_EQ(*iter, 5_b);
}

TEST_F(MultiBufV1Test, ConstIteratorAdvancesNAcrossChunks) {
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk({1_b, 2_b, 3_b}));
  buf.PushBackChunk(MakeChunk({4_b, 5_b, 6_b}));

  MultiBuf::const_iterator iter = buf.cbegin();
  iter += 4;
  EXPECT_EQ(*iter, 5_b);
}

TEST_F(MultiBufV1Test, IteratorSkipsEmptyChunks) {
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk(0));
  buf.PushBackChunk(MakeChunk(0));
  buf.PushBackChunk(MakeChunk({1_b}));
  buf.PushBackChunk(MakeChunk(0));
  buf.PushBackChunk(MakeChunk({2_b, 3_b}));
  buf.PushBackChunk(MakeChunk(0));

  MultiBuf::iterator it = buf.begin();
  ASSERT_EQ(*it++, 1_b);
  ASSERT_EQ(*it++, 2_b);
  ASSERT_EQ(*it++, 3_b);
  ASSERT_EQ(it, buf.end());
}

constexpr auto kSequentialBytes =
    bytes::Initialized<6>([](size_t i) { return i + 1; });

TEST_F(MultiBufV1Test, CopyToFromEmptyMultiBuf) {
  const MultiBuf buf;
  std::array<std::byte, 6> buffer = {};
  StatusWithSize result = buf.CopyTo(buffer);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.size(), 0U);

  result = buf.CopyTo({});
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.size(), 0U);
}

TEST_F(MultiBufV1Test, CopyToEmptyDestination) {
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk({1_b, 2_b, 3_b, 4_b}));
  const StatusWithSize result = buf.CopyTo({});
  ASSERT_EQ(result.status(), Status::ResourceExhausted());
  EXPECT_EQ(result.size(), 0U);
}

TEST_F(MultiBufV1Test, CopyToOneChunk) {
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk({1_b, 2_b, 3_b, 4_b}));

  std::array<std::byte, 4> buffer = {};
  const StatusWithSize result = buf.CopyTo(buffer);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.size(), 4U);
  EXPECT_TRUE(
      std::equal(buffer.begin(), buffer.end(), kSequentialBytes.begin()));
}

TEST_F(MultiBufV1Test, CopyToVariousChunks) {
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk({1_b}));
  buf.PushBackChunk(MakeChunk({2_b, 3_b}));
  buf.PushBackChunk(MakeChunk({}));
  buf.PushBackChunk(MakeChunk({4_b, 5_b, 6_b}));

  std::array<std::byte, 6> buffer = {};
  const StatusWithSize result = buf.CopyTo(buffer);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.size(), 6U);
  EXPECT_TRUE(
      std::equal(buffer.begin(), buffer.end(), kSequentialBytes.begin()));
}

TEST_F(MultiBufV1Test, CopyToInTwoParts) {
  constexpr size_t kMultiBufSize = 6;
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk({1_b}));
  buf.PushBackChunk(MakeChunk({2_b, 3_b}));
  buf.PushBackChunk(MakeChunk({}));
  buf.PushBackChunk(MakeChunk({4_b, 5_b, 6_b}));
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

TEST_F(MultiBufV1Test, CopyToPositionIsEnd) {
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk({1_b}));
  buf.PushBackChunk(MakeChunk({2_b, 3_b}));
  buf.PushBackChunk(MakeChunk({}));

  const StatusWithSize result = buf.CopyTo({}, 3U);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.size(), 0U);
}

TEST_F(MultiBufV1Test, CopyFromIntoOneChunk) {
  MultiBuf mb;
  mb.PushBackChunk(MakeChunk(6));

  const StatusWithSize result = mb.CopyFrom(kSequentialBytes);
  EXPECT_EQ(result.status(), OkStatus());
  ASSERT_EQ(result.size(), 6U);
  EXPECT_TRUE(std::equal(mb.begin(), mb.end(), kSequentialBytes.begin()));
}

TEST_F(MultiBufV1Test, CopyFromIntoMultipleChunks) {
  MultiBuf mb;
  mb.PushBackChunk(MakeChunk(2));
  mb.PushBackChunk(MakeChunk(0));
  mb.PushBackChunk(MakeChunk(3));
  mb.PushBackChunk(MakeChunk(1));
  mb.PushBackChunk(MakeChunk(0));

  const StatusWithSize result = mb.CopyFrom(kSequentialBytes);
  EXPECT_EQ(result.status(), OkStatus());
  ASSERT_EQ(result.size(), 6U);
  EXPECT_TRUE(std::equal(mb.begin(), mb.end(), kSequentialBytes.begin()));
}

TEST_F(MultiBufV1Test, CopyFromInTwoParts) {
  for (size_t first = 0; first < kSequentialBytes.size(); ++first) {
    MultiBuf mb;
    mb.PushBackChunk(MakeChunk(1));
    mb.PushBackChunk(MakeChunk(0));
    mb.PushBackChunk(MakeChunk(0));
    mb.PushBackChunk(MakeChunk(2));
    mb.PushBackChunk(MakeChunk(3));
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

TEST_F(MultiBufV1Test, CopyFromAndTruncate) {
  for (size_t to_copy = 0; to_copy < kSequentialBytes.size(); ++to_copy) {
    MultiBuf mb;
    mb.PushBackChunk(MakeChunk(1));
    mb.PushBackChunk(MakeChunk(0));
    mb.PushBackChunk(MakeChunk(0));
    mb.PushBackChunk(MakeChunk(2));
    mb.PushBackChunk(MakeChunk(3));
    mb.PushBackChunk(MakeChunk(0));
    ASSERT_EQ(mb.size(), kSequentialBytes.size());

    const StatusWithSize result =
        mb.CopyFromAndTruncate(span(kSequentialBytes).first(to_copy));

    ASSERT_EQ(result.status(), OkStatus());
    ASSERT_EQ(result.size(), to_copy);
    ASSERT_EQ(mb.size(), result.size());
    ASSERT_TRUE(std::equal(mb.begin(), mb.end(), kSequentialBytes.begin()));
  }
}

TEST_F(MultiBufV1Test, CopyFromAndTruncateFromOffset) {
  static constexpr std::array<std::byte, 6> kZeroes = {};

  // Sweep offsets 0–6 (inclusive), and copy 0–all bytes for each offset.
  for (size_t num = 0; num <= kSequentialBytes.size(); ++num) {
    for (size_t to_copy = 0; to_copy <= kSequentialBytes.size() - num;
         ++to_copy) {
      auto offset = static_cast<MultiBuf::difference_type>(num);
      MultiBuf mb;
      mb.PushBackChunk(MakeChunk(2));
      mb.PushBackChunk(MakeChunk(0));
      mb.PushBackChunk(MakeChunk(3));
      mb.PushBackChunk(MakeChunk(0));
      mb.PushBackChunk(MakeChunk(0));
      mb.PushBackChunk(MakeChunk(1));
      ASSERT_EQ(mb.size(), kSequentialBytes.size());

      const StatusWithSize result =
          mb.CopyFromAndTruncate(span(kSequentialBytes).first(to_copy), num);
      ASSERT_EQ(result.status(), OkStatus());
      ASSERT_EQ(result.size(), to_copy);
      ASSERT_EQ(mb.size(), num + to_copy);

      // MultiBuf contains to_copy 0s followed by to_copy sequential bytes.
      ASSERT_TRUE(std::equal(mb.begin(), mb.begin() + offset, kZeroes.begin()));
      ASSERT_TRUE(
          std::equal(mb.begin() + offset, mb.end(), kSequentialBytes.begin()));
    }
  }
}

TEST_F(MultiBufV1Test, CopyFromIntoEmptyMultibuf) {
  MultiBuf mb;

  StatusWithSize result = mb.CopyFrom({});
  EXPECT_EQ(result.status(), OkStatus());  // empty source, so copy succeeded
  EXPECT_EQ(result.size(), 0U);

  result = mb.CopyFrom(kSequentialBytes);
  EXPECT_EQ(result.status(), Status::ResourceExhausted());
  EXPECT_EQ(result.size(), 0U);

  mb.PushBackChunk(MakeChunk(0));  // add an empty chunk

  result = mb.CopyFrom({});
  EXPECT_EQ(result.status(), OkStatus());  // empty source, so copy succeeded
  EXPECT_EQ(result.size(), 0U);

  result = mb.CopyFrom(kSequentialBytes);
  EXPECT_EQ(result.status(), Status::ResourceExhausted());
  EXPECT_EQ(result.size(), 0U);
}

TEST_F(MultiBufV1Test, IsContiguousTrueForEmptyBuffer) {
  MultiBuf buf;
  EXPECT_TRUE(buf.IsContiguous());

  buf.PushBackChunk(MakeChunk({}));
  EXPECT_TRUE(buf.IsContiguous());
  buf.PushBackChunk(MakeChunk({}));
  EXPECT_TRUE(buf.IsContiguous());
  buf.PushBackChunk(MakeChunk({}));
  EXPECT_TRUE(buf.IsContiguous());
}

TEST_F(MultiBufV1Test, IsContiguousTrueForSingleNonEmptyChunk) {
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk({1_b}));
  EXPECT_TRUE(buf.IsContiguous());
  buf.PushBackChunk(MakeChunk({}));
  EXPECT_TRUE(buf.IsContiguous());
  buf.PushFrontChunk(MakeChunk({}));
  EXPECT_TRUE(buf.IsContiguous());
}

TEST_F(MultiBufV1Test, IsContiguousFalseIfMultipleNonEmptyChunks) {
  MultiBuf buf;
  buf.PushBackChunk(MakeChunk({1_b}));
  buf.PushBackChunk(MakeChunk({2_b}));
  EXPECT_FALSE(buf.IsContiguous());
}

TEST_F(MultiBufV1Test, ContiguousSpanAcrossMultipleChunks) {
  OwnedChunk chunk_1 = MakeChunk(10);
  const ConstByteSpan contiguous_span = chunk_1;
  OwnedChunk chunk_2 = chunk_1->TakeSuffix(5).value();
  OwnedChunk chunk_3 = chunk_2->TakeSuffix(5).value();
  OwnedChunk chunk_4 = chunk_3->TakeSuffix(1).value();

  MultiBuf buf;
  buf.PushBackChunk(std::move(chunk_1));  // 5 bytes
  buf.PushBackChunk(std::move(chunk_2));  // 0 bytes
  buf.PushBackChunk(std::move(chunk_3));  // 4 bytes
  buf.PushBackChunk(std::move(chunk_4));  // 1 byte
  buf.PushBackChunk(MakeChunk(0));        // empty

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
  buf.InsertChunk(++it, MakeChunk(1));
  EXPECT_FALSE(buf.IsContiguous());
}
// LINT.ThenChange(../v1/multibuf_test.cc)

}  // namespace pw::multibuf::v1_adapter
