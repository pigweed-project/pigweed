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
using ::pw::multibuf::v2::Chunks;
using ::pw::multibuf::v2::test::IteratorTest;

class ChunkIteratorTest : public IteratorTest {
 protected:
  explicit ChunkIteratorTest()
      : first_(chunks().begin()),
        flipped_(chunks().begin()),
        second_(++(chunks().begin())),
        last_(--(chunks().end())),
        past_the_end_(chunks().end()) {}

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

  Chunks::iterator first_;
  Chunks::const_iterator flipped_;
  Chunks::iterator second_;
  Chunks::iterator last_;
  Chunks::iterator past_the_end_;
};

// Template method implementations.

TEST_F(ChunkIteratorTest, IndirectionOperatorDereferencesToByteSpan) {
  const pw::ConstByteSpan actual = *first_;
  const pw::ConstByteSpan expected = GetContiguous(0);
  EXPECT_EQ(actual.data(), expected.data());
  EXPECT_EQ(actual.size(), expected.size());
}

TEST_F(ChunkIteratorTest, MemberOfOperatorDereferencesToByteSpan) {
  const pw::ConstByteSpan expected = GetContiguous(0);
  EXPECT_EQ(first_->data(), expected.data());
  EXPECT_EQ(first_->size(), expected.size());
}

TEST_F(ChunkIteratorTest, CanIterateUsingPrefixIncrement) {
  Chunks::iterator iter = first_;
  for (size_t i = 0; i < kNumContiguous; ++i) {
    EXPECT_EQ(size_t(std::distance(first_, iter)), i);
    const pw::ConstByteSpan expected = GetContiguous(i);
    EXPECT_EQ(iter->data(), expected.data());
    EXPECT_EQ(iter->size(), expected.size());
    ++iter;
  }
  EXPECT_EQ(iter, past_the_end_);
}

TEST_F(ChunkIteratorTest, CanIterateUsingPostfixIncrement) {
  Chunks::iterator iter = first_;
  Chunks::iterator copy;
  for (size_t i = 0; i < kNumContiguous; ++i) {
    EXPECT_EQ(size_t(std::distance(first_, iter)), i);
    const pw::ConstByteSpan expected = GetContiguous(i);
    copy = iter++;
    EXPECT_EQ(copy->data(), expected.data());
    EXPECT_EQ(copy->size(), expected.size());
  }
  EXPECT_EQ(copy, last_);
  EXPECT_EQ(iter, past_the_end_);
}

TEST_F(ChunkIteratorTest, CanIterateUsingPrefixDecrement) {
  Chunks::iterator iter = past_the_end_;
  for (size_t i = 1; i <= kNumContiguous; ++i) {
    const pw::ConstByteSpan expected = GetContiguous(kNumContiguous - i);
    --iter;
    EXPECT_EQ(iter->data(), expected.data());
    EXPECT_EQ(iter->size(), expected.size());
    EXPECT_EQ(size_t(std::distance(iter, past_the_end_)), i);
  }
  EXPECT_EQ(iter, first_);
}

TEST_F(ChunkIteratorTest, CanIterateUsingPostfixDecrement) {
  Chunks::iterator iter = last_;
  for (size_t i = 1; i < kNumContiguous; ++i) {
    const pw::ConstByteSpan expected = GetContiguous(kNumContiguous - i);
    auto copy = iter--;
    EXPECT_EQ(copy->data(), expected.data());
    EXPECT_EQ(copy->size(), expected.size());
    EXPECT_EQ(size_t(std::distance(iter, last_)), i);
  }
  EXPECT_EQ(iter, first_);
}

TEST_F(ChunkIteratorTest, DistanceFromFirstToLastMatchesSize) {
  Chunks chunks = this->chunks();
  EXPECT_EQ(kNumContiguous, chunks.size());
  EXPECT_EQ(kNumContiguous,
            size_t(std::distance(chunks.begin(), chunks.end())));
  EXPECT_EQ(kNumContiguous,
            size_t(std::distance(chunks.cbegin(), chunks.cend())));
}

TEST_F(ChunkIteratorTest, CanCompareIteratorsUsingEqual) {
  EXPECT_EQ(first_, first_);
  EXPECT_EQ(first_, flipped_);
  EXPECT_EQ(past_the_end_, past_the_end_);
}

TEST_F(ChunkIteratorTest, CanCompareIteratorsUsingNotEqual) {
  EXPECT_NE(first_, second_);
  EXPECT_NE(flipped_, second_);
  EXPECT_NE(first_, past_the_end_);
}

TEST_F(ChunkIteratorTest, CanIterateUsingRangeBasedForLoop) {
  size_t i = 0;
  for (auto actual : chunks()) {
    const pw::ConstByteSpan expected = GetContiguous(i++);
    EXPECT_EQ(actual.data(), expected.data());
    EXPECT_EQ(actual.size(), expected.size());
  }
  i = 0;
  for (auto actual : const_chunks()) {
    const pw::ConstByteSpan expected = GetContiguous(i++);
    EXPECT_EQ(actual.data(), expected.data());
    EXPECT_EQ(actual.size(), expected.size());
  }
}

}  // namespace
