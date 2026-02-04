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

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "pw_bytes/span.h"
#include "pw_containers/dynamic_deque.h"
#include "pw_multibuf/v2/chunks.h"
#include "pw_multibuf/v2/internal/entry.h"
#include "pw_multibuf_private/iterator_testing.h"
#include "pw_unit_test/framework.h"

// Test fixtures.

namespace {

using ::pw::ByteSpan;
using ::pw::multibuf::v2::internal::Chunks;
using ::pw::multibuf::v2::internal::Entry;
using ::pw::multibuf::v2::internal::RawChunks;
using ::pw::multibuf::v2::test::IteratorTest;
using Deque = pw::DynamicDeque<Entry>;

template <typename _ChunksType, typename _IteratorType>
struct IteratorTraits {
  using ChunksType = _ChunksType;
  using IteratorType = _IteratorType;
  using FlippedType = std::conditional_t<
      std::is_same_v<IteratorType, typename ChunksType::iterator>,
      typename ChunksType::const_iterator,
      typename ChunksType::iterator>;
};

using ChunksIteratorTraits =
    IteratorTraits<Chunks<Deque>, Chunks<Deque>::iterator>;
using ConstChunksIteratorTraits =
    IteratorTraits<Chunks<Deque>, Chunks<Deque>::const_iterator>;
using RawChunksIteratorTraits =
    IteratorTraits<RawChunks<Deque>, RawChunks<Deque>::iterator>;
using RawConstChunksIteratorTraits =
    IteratorTraits<RawChunks<Deque>, RawChunks<Deque>::const_iterator>;

template <typename IteratorTraits>
class ChunkIteratorTestBase : public IteratorTest {
 protected:
  using ChunksType = typename IteratorTraits::ChunksType;
  using IteratorType = typename IteratorTraits::IteratorType;
  using FlippedType = typename IteratorTraits::FlippedType;

  explicit ChunkIteratorTestBase(const ChunksType& chunks)
      : first_(chunks.begin()),
        flipped_(chunks.begin()),
        second_(++chunks.begin()),
        last_(--chunks.end()),
        past_the_end_(chunks.end()) {}

  [[nodiscard]] ByteSpan GetChunk(size_t index) {
    if constexpr (std::is_same_v<ChunksType, Chunks<Deque>>) {
      return this->GetContiguous(index);
    } else if constexpr (std::is_same_v<ChunksType, RawChunks<Deque>>) {
      return this->GetRaw(index);
    }
  }

  [[nodiscard]] uint16_t GetNumChunks() const {
    if constexpr (std::is_same_v<ChunksType, Chunks<Deque>>) {
      return this->kNumContiguous;
    } else if constexpr (std::is_same_v<ChunksType, RawChunks<Deque>>) {
      return this->kNumRaw;
    }
  }

  // Unit tests.
  void IndirectionOperatorDereferencesToByteSpan();
  void MemberOfOperatorDereferencesToByteSpan();
  void CanIterateUsingPrefixIncrement();
  void CanIterateUsingPostfixIncrement();
  void CanIterateUsingPrefixDecrement();
  void CanIterateUsingPostfixDecrement();
  void DistanceFromFirstToLastMatchesSize();
  void CanCompareIteratorsUsingEqual();
  void CanCompareIteratorsUsingNotEqual();

 private:
  IteratorType first_;
  FlippedType flipped_;
  IteratorType second_;
  IteratorType last_;
  IteratorType past_the_end_;
};

template <typename IteratorTraits>
class ChunkIteratorTestImpl : public ChunkIteratorTestBase<IteratorTraits> {
 protected:
  ChunkIteratorTestImpl()
      : ChunkIteratorTestBase<IteratorTraits>(this->chunks()) {}
};

template <typename IteratorTraits>
class RawChunkIteratorTestImpl : public ChunkIteratorTestBase<IteratorTraits> {
 protected:
  RawChunkIteratorTestImpl()
      : ChunkIteratorTestBase<IteratorTraits>(this->raw_chunks()) {}
};

using ChunkIteratorTest = ChunkIteratorTestImpl<ChunksIteratorTraits>;
using ChunkConstIteratorTest = ChunkIteratorTestImpl<ConstChunksIteratorTraits>;
using RawChunkIteratorTest = RawChunkIteratorTestImpl<RawChunksIteratorTraits>;
using RawChunkConstIteratorTest =
    RawChunkIteratorTestImpl<RawConstChunksIteratorTraits>;
using ChunksTest = IteratorTest;

// Template method implementations.

TEST_F(ChunkIteratorTest, CheckFixture) {}
TEST_F(ChunkConstIteratorTest, CheckFixture) {}
TEST_F(RawChunkIteratorTest, CheckFixture) {}
TEST_F(RawChunkConstIteratorTest, CheckFixture) {}

static_assert(sizeof(pw::multibuf::v2::internal::Entry) == sizeof(std::byte*));

template <typename IteratorTraits>
void ChunkIteratorTestBase<
    IteratorTraits>::IndirectionOperatorDereferencesToByteSpan() {
  const pw::ConstByteSpan actual = *first_;
  const pw::ConstByteSpan expected = GetChunk(0);
  EXPECT_EQ(actual.data(), expected.data());
  EXPECT_EQ(actual.size(), expected.size());
}
TEST_F(ChunkIteratorTest, IndirectionOperatorDereferencesToByteSpan) {
  IndirectionOperatorDereferencesToByteSpan();
}
TEST_F(ChunkConstIteratorTest, IndirectionOperatorDereferencesToByteSpan) {
  IndirectionOperatorDereferencesToByteSpan();
}
TEST_F(RawChunkIteratorTest, IndirectionOperatorDereferencesToByteSpan) {
  IndirectionOperatorDereferencesToByteSpan();
}
TEST_F(RawChunkConstIteratorTest, IndirectionOperatorDereferencesToByteSpan) {
  IndirectionOperatorDereferencesToByteSpan();
}

template <typename IteratorTraits>
void ChunkIteratorTestBase<
    IteratorTraits>::MemberOfOperatorDereferencesToByteSpan() {
  const pw::ConstByteSpan expected = GetChunk(0);
  EXPECT_EQ(first_->data(), expected.data());
  EXPECT_EQ(first_->size(), expected.size());
}
TEST_F(ChunkIteratorTest, MemberOfOperatorDereferencesToByteSpan) {
  MemberOfOperatorDereferencesToByteSpan();
}
TEST_F(ChunkConstIteratorTest, MemberOfOperatorDereferencesToByteSpan) {
  MemberOfOperatorDereferencesToByteSpan();
}
TEST_F(RawChunkIteratorTest, MemberOfOperatorDereferencesToByteSpan) {
  MemberOfOperatorDereferencesToByteSpan();
}
TEST_F(RawChunkConstIteratorTest, MemberOfOperatorDereferencesToByteSpan) {
  MemberOfOperatorDereferencesToByteSpan();
}

template <typename IteratorTraits>
void ChunkIteratorTestBase<IteratorTraits>::CanIterateUsingPrefixIncrement() {
  IteratorType iter = first_;
  for (size_t i = 0; i < GetNumChunks(); ++i) {
    EXPECT_EQ(size_t(std::distance(first_, iter)), i);
    const pw::ConstByteSpan expected = GetChunk(i);
    EXPECT_EQ(iter->data(), expected.data());
    EXPECT_EQ(iter->size(), expected.size());
    ++iter;
  }
  EXPECT_EQ(iter, past_the_end_);
}
TEST_F(ChunkIteratorTest, CanIterateUsingPrefixIncrement) {
  CanIterateUsingPrefixIncrement();
}
TEST_F(ChunkConstIteratorTest, CanIterateUsingPrefixIncrement) {
  CanIterateUsingPrefixIncrement();
}
TEST_F(RawChunkIteratorTest, CanIterateUsingPrefixIncrement) {
  CanIterateUsingPrefixIncrement();
}
TEST_F(RawChunkConstIteratorTest, CanIterateUsingPrefixIncrement) {
  CanIterateUsingPrefixIncrement();
}

template <typename IteratorTraits>
void ChunkIteratorTestBase<IteratorTraits>::CanIterateUsingPostfixIncrement() {
  IteratorType iter = first_;
  IteratorType copy;
  for (size_t i = 0; i < GetNumChunks(); ++i) {
    EXPECT_EQ(size_t(std::distance(first_, iter)), i);
    const pw::ConstByteSpan expected = GetChunk(i);
    copy = iter++;
    EXPECT_EQ(copy->data(), expected.data());
    EXPECT_EQ(copy->size(), expected.size());
  }
  EXPECT_EQ(copy, last_);
  EXPECT_EQ(iter, past_the_end_);
}
TEST_F(ChunkIteratorTest, CanIterateUsingPostfixIncrement) {
  CanIterateUsingPostfixIncrement();
}
TEST_F(ChunkConstIteratorTest, CanIterateUsingPostfixIncrement) {
  CanIterateUsingPostfixIncrement();
}
TEST_F(RawChunkIteratorTest, CanIterateUsingPostfixIncrement) {
  CanIterateUsingPostfixIncrement();
}
TEST_F(RawChunkConstIteratorTest, CanIterateUsingPostfixIncrement) {
  CanIterateUsingPostfixIncrement();
}

template <typename IteratorTraits>
void ChunkIteratorTestBase<IteratorTraits>::CanIterateUsingPrefixDecrement() {
  IteratorType iter = past_the_end_;
  for (size_t i = 1; i <= GetNumChunks(); ++i) {
    const pw::ConstByteSpan expected = GetChunk(GetNumChunks() - i);
    --iter;
    EXPECT_EQ(iter->data(), expected.data());
    EXPECT_EQ(iter->size(), expected.size());
    EXPECT_EQ(size_t(std::distance(iter, past_the_end_)), i);
  }
  EXPECT_EQ(iter, first_);
}
TEST_F(ChunkIteratorTest, CanIterateUsingPrefixDecrement) {
  CanIterateUsingPrefixDecrement();
}
TEST_F(ChunkConstIteratorTest, CanIterateUsingPrefixDecrement) {
  CanIterateUsingPrefixDecrement();
}
TEST_F(RawChunkIteratorTest, CanIterateUsingPrefixDecrement) {
  CanIterateUsingPrefixDecrement();
}
TEST_F(RawChunkConstIteratorTest, CanIterateUsingPrefixDecrement) {
  CanIterateUsingPrefixDecrement();
}

template <typename IteratorTraits>
void ChunkIteratorTestBase<IteratorTraits>::CanIterateUsingPostfixDecrement() {
  IteratorType iter = last_;
  for (size_t i = 1; i < GetNumChunks(); ++i) {
    const pw::ConstByteSpan expected = GetChunk(GetNumChunks() - i);
    auto copy = iter--;
    EXPECT_EQ(copy->data(), expected.data());
    EXPECT_EQ(copy->size(), expected.size());
    EXPECT_EQ(size_t(std::distance(iter, last_)), i);
  }
  EXPECT_EQ(iter, first_);
}
TEST_F(ChunkIteratorTest, CanIterateUsingPostfixDecrement) {
  CanIterateUsingPostfixDecrement();
}
TEST_F(ChunkConstIteratorTest, CanIterateUsingPostfixDecrement) {
  CanIterateUsingPostfixDecrement();
}
TEST_F(RawChunkIteratorTest, CanIterateUsingPostfixDecrement) {
  CanIterateUsingPostfixDecrement();
}
TEST_F(RawChunkConstIteratorTest, CanIterateUsingPostfixDecrement) {
  CanIterateUsingPostfixDecrement();
}

TEST_F(ChunkIteratorTest, DistanceFromFirstToLastMatchesSize) {
  ChunksType chunks = this->chunks();
  EXPECT_EQ(kNumContiguous, chunks.size());
  EXPECT_EQ(kNumContiguous,
            size_t(std::distance(chunks.begin(), chunks.end())));
}
TEST_F(ChunkConstIteratorTest, DistanceFromFirstToLastMatchesSize) {
  ChunksType chunks = this->chunks();
  EXPECT_EQ(kNumContiguous, chunks.size());
  EXPECT_EQ(kNumContiguous,
            size_t(std::distance(chunks.begin(), chunks.end())));
}
TEST_F(RawChunkIteratorTest, DistanceFromFirstToLastMatchesSize) {
  ChunksType chunks = this->raw_chunks();
  EXPECT_EQ(kNumRaw, chunks.size());
  EXPECT_EQ(kNumRaw, size_t(std::distance(chunks.begin(), chunks.end())));
}
TEST_F(RawChunkConstIteratorTest, DistanceFromFirstToLastMatchesSize) {
  ChunksType chunks = this->raw_chunks();
  EXPECT_EQ(kNumRaw, chunks.size());
  EXPECT_EQ(kNumRaw, size_t(std::distance(chunks.begin(), chunks.end())));
}

template <typename IteratorTraits>
void ChunkIteratorTestBase<IteratorTraits>::CanCompareIteratorsUsingEqual() {
  EXPECT_EQ(first_, first_);
  EXPECT_EQ(first_, flipped_);
  EXPECT_EQ(past_the_end_, past_the_end_);
}
TEST_F(ChunkIteratorTest, CanCompareIteratorsUsingEqual) {
  CanCompareIteratorsUsingEqual();
}
TEST_F(ChunkConstIteratorTest, CanCompareIteratorsUsingEqual) {
  CanCompareIteratorsUsingEqual();
}
TEST_F(RawChunkIteratorTest, CanCompareIteratorsUsingEqual) {
  CanCompareIteratorsUsingEqual();
}
TEST_F(RawChunkConstIteratorTest, CanCompareIteratorsUsingEqual) {
  CanCompareIteratorsUsingEqual();
}

template <typename IteratorTraits>
void ChunkIteratorTestBase<IteratorTraits>::CanCompareIteratorsUsingNotEqual() {
  EXPECT_NE(first_, second_);
  EXPECT_NE(flipped_, second_);
  EXPECT_NE(first_, past_the_end_);
}
TEST_F(ChunkIteratorTest, CanCompareIteratorsUsingNotEqual) {
  CanCompareIteratorsUsingNotEqual();
}
TEST_F(ChunkConstIteratorTest, CanCompareIteratorsUsingNotEqual) {
  CanCompareIteratorsUsingNotEqual();
}
TEST_F(RawChunkIteratorTest, CanCompareIteratorsUsingNotEqual) {
  CanCompareIteratorsUsingNotEqual();
}
TEST_F(RawChunkConstIteratorTest, CanCompareIteratorsUsingNotEqual) {
  CanCompareIteratorsUsingNotEqual();
}

TEST_F(ChunksTest, CanIterateUsingRangeBasedForLoop) {
  size_t i = 0;
  for (auto actual : chunks()) {
    const pw::ConstByteSpan expected = GetContiguous(i++);
    EXPECT_EQ(actual.data(), expected.data());
    EXPECT_EQ(actual.size(), expected.size());
  }
}

TEST_F(ChunksTest, CanIterateRawUsingRangeBasedForLoop) {
  size_t i = 0;
  for (auto actual : raw_chunks()) {
    const pw::ConstByteSpan expected = GetRaw(i++);
    EXPECT_EQ(actual.data(), expected.data());
    EXPECT_EQ(actual.size(), expected.size());
  }
}

}  // namespace
