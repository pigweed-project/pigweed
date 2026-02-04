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

#include "pw_multibuf/v2/internal/byte_iterator.h"

#include "pw_multibuf/v2/internal/chunk_iterator.h"
#include "pw_multibuf_private/iterator_testing.h"
#include "pw_unit_test/framework.h"

namespace {

using ByteIterator = ::pw::multibuf::v2::internal::ByteIterator<
    uint16_t,
    ::pw::multibuf::v2::internal::ChunkMutability::kMutable>;
using ConstByteIterator = ::pw::multibuf::v2::internal::ByteIterator<
    uint16_t,
    ::pw::multibuf::v2::internal::ChunkMutability::kConst>;
using ::pw::multibuf::v2::test::IteratorTest;

// Test fixture.
template <typename IteratorType>
class ByteIteratorTestImpl : public IteratorTest {
 private:
  using Base = IteratorTest;
  using FlippedType =
      std::conditional_t<std::is_same_v<IteratorType, ByteIterator>,
                         ConstByteIterator,
                         ByteIterator>;

 protected:
  ByteIteratorTestImpl() {
    auto byte_iters = GetByteIterators();
    second_ = IteratorType(byte_iters.first);
    flipped_ = FlippedType(byte_iters.first);
    first_ = second_++;
    last_ = IteratorType(byte_iters.second);
    past_the_end_ = last_--;
  }

  // Unit tests.
  void CanDereferenceToByte();
  void CanDereferenceWithArrayIndex();
  void CanIterateUsingPrefixIncrement();
  void CanIterateUsingPostfixIncrement();
  void CanIterateUsingAddition();
  void CanIterateUsingCompoundAddition();
  void CanIterateUsingPrefixDecrement();
  void CanIterateUsingPostfixDecrement();
  void CanIterateUsingSubtraction();
  void CanIterateUsingCompoundSubtraction();
  void CanCalculateDistanceBetweenIterators();
  void CanCompareIteratorsUsingEqual();
  void CanCompareIteratorsUsingNotEqual();
  void CanCompareIteratorsUsingLessThan();
  void CanCompareIteratorsUsingLessThanOrEqual();
  void CanCompareIteratorsUsingGreaterThan();
  void CanCompareIteratorsUsingGreaterThanOrEqual();

  IteratorType first_;
  FlippedType flipped_;
  IteratorType second_;
  IteratorType last_;
  IteratorType past_the_end_;
};

using ByteIteratorTest = ByteIteratorTestImpl<ByteIterator>;
using ConstByteIteratorTest = ByteIteratorTestImpl<ConstByteIterator>;

// Template method implementations.

template <typename IteratorType>
void ByteIteratorTestImpl<IteratorType>::CanDereferenceToByte() {
  pw::ConstByteSpan first_span = GetContiguous(0);
  pw::ConstByteSpan last_span = GetContiguous(kNumContiguous - 1);
  EXPECT_EQ(&(*first_), first_span.data());
  EXPECT_EQ(&(*last_), last_span.data() + last_span.size() - 1);
}
TEST_F(ByteIteratorTest, CanDereferenceToByte) { CanDereferenceToByte(); }
TEST_F(ConstByteIteratorTest, CanDereferenceToByte) { CanDereferenceToByte(); }

template <typename IteratorType>
void ByteIteratorTestImpl<IteratorType>::CanDereferenceWithArrayIndex() {
  pw::ConstByteSpan expected = GetContiguous(0);
  for (uint16_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(first_[i], expected[i]);
  }
}
TEST_F(ByteIteratorTest, CanDereferenceWithArrayIndex) {
  CanDereferenceWithArrayIndex();
}
TEST_F(ConstByteIteratorTest, CanDereferenceWithArrayIndex) {
  CanDereferenceWithArrayIndex();
}

template <typename IteratorType>
void ByteIteratorTestImpl<IteratorType>::CanIterateUsingPrefixIncrement() {
  IteratorType iter = first_;
  for (size_t i = 0; iter != past_the_end_; ++i) {
    pw::ConstByteSpan expected = GetContiguous(i);
    for (auto b : expected) {
      EXPECT_EQ(*iter, b);
      ++iter;
    }
  }
}
TEST_F(ByteIteratorTest, CanIterateUsingPrefixIncrement) {
  CanIterateUsingPrefixIncrement();
}
TEST_F(ConstByteIteratorTest, CanIterateUsingPrefixIncrement) {
  CanIterateUsingPrefixIncrement();
}

template <typename IteratorType>
void ByteIteratorTestImpl<IteratorType>::CanIterateUsingPostfixIncrement() {
  IteratorType iter = first_;
  IteratorType copy;
  for (size_t i = 0; iter != past_the_end_; ++i) {
    for (auto expected : GetContiguous(i)) {
      copy = iter++;
      EXPECT_EQ(*copy, expected);
    }
  }
}
TEST_F(ByteIteratorTest, CanIterateUsingPostfixIncrement) {
  CanIterateUsingPostfixIncrement();
}
TEST_F(ConstByteIteratorTest, CanIterateUsingPostfixIncrement) {
  CanIterateUsingPostfixIncrement();
}

template <typename IteratorType>
void ByteIteratorTestImpl<IteratorType>::CanIterateUsingAddition() {
  uint16_t offset = 0;
  IteratorType iter = first_;
  for (size_t i = 0; iter != past_the_end_; ++i) {
    for (auto expected : GetContiguous(i)) {
      auto copy = first_ + offset;
      EXPECT_EQ(copy, iter);
      EXPECT_EQ(*copy, expected);
      ++iter;
      ++offset;
    }
  }
}
TEST_F(ByteIteratorTest, CanIterateUsingAddition) { CanIterateUsingAddition(); }
TEST_F(ConstByteIteratorTest, CanIterateUsingAddition) {
  CanIterateUsingAddition();
}

template <typename IteratorType>
void ByteIteratorTestImpl<IteratorType>::CanIterateUsingCompoundAddition() {
  uint16_t offset = 0;
  IteratorType iter = first_;
  for (size_t i = 0; iter != past_the_end_; ++i) {
    for (auto expected : GetContiguous(i)) {
      auto copy = first_;
      copy += offset;
      EXPECT_EQ(copy, iter);
      EXPECT_EQ(*copy, expected);
      ++iter;
      ++offset;
    }
  }
}
TEST_F(ByteIteratorTest, CanIterateUsingCompoundAddition) {
  CanIterateUsingCompoundAddition();
}
TEST_F(ConstByteIteratorTest, CanIterateUsingCompoundAddition) {
  CanIterateUsingCompoundAddition();
}

template <typename IteratorType>
void ByteIteratorTestImpl<IteratorType>::CanIterateUsingPrefixDecrement() {
  IteratorType iter = last_;
  for (size_t i = 1; i <= kNumContiguous; ++i) {
    auto expected = GetContiguous(kNumContiguous - i);
    for (auto b = expected.rbegin(); b != expected.rend(); ++b) {
      EXPECT_EQ(*iter, *b);
      if (iter != first_) {
        --iter;
      }
    }
  }
}
TEST_F(ByteIteratorTest, CanIterateUsingPrefixDecrement) {
  CanIterateUsingPrefixDecrement();
}
TEST_F(ConstByteIteratorTest, CanIterateUsingPrefixDecrement) {
  CanIterateUsingPrefixDecrement();
}

template <typename IteratorType>
void ByteIteratorTestImpl<IteratorType>::CanIterateUsingPostfixDecrement() {
  IteratorType iter = last_;
  for (size_t i = 1; i <= kNumContiguous; ++i) {
    auto expected = GetContiguous(kNumContiguous - i);
    for (auto b = expected.rbegin(); b != expected.rend(); ++b) {
      if (iter == first_) {
        EXPECT_EQ(*iter, *b);
      } else {
        auto copy = iter--;
        EXPECT_EQ(*copy, *b);
      }
    }
  }
}
TEST_F(ByteIteratorTest, CanIterateUsingPostfixDecrement) {
  CanIterateUsingPostfixDecrement();
}
TEST_F(ConstByteIteratorTest, CanIterateUsingPostfixDecrement) {
  CanIterateUsingPostfixDecrement();
}

template <typename IteratorType>
void ByteIteratorTestImpl<IteratorType>::CanIterateUsingSubtraction() {
  uint16_t offset = 1;
  IteratorType iter = past_the_end_;
  for (size_t i = 1; i <= kNumContiguous; ++i) {
    auto expected = GetContiguous(kNumContiguous - i);
    for (auto b = expected.rbegin(); b != expected.rend(); ++b) {
      auto copy = past_the_end_ - offset;
      --iter;
      EXPECT_EQ(copy, iter);
      EXPECT_EQ(*copy, *b);
      ++offset;
    }
  }
}
TEST_F(ByteIteratorTest, CanIterateUsingSubtraction) {
  CanIterateUsingSubtraction();
}
TEST_F(ConstByteIteratorTest, CanIterateUsingSubtraction) {
  CanIterateUsingSubtraction();
}

template <typename IteratorType>
void ByteIteratorTestImpl<IteratorType>::CanIterateUsingCompoundSubtraction() {
  uint16_t offset = 1;
  IteratorType iter = past_the_end_;
  for (size_t i = 1; i <= kNumContiguous; ++i) {
    auto expected = GetContiguous(kNumContiguous - i);
    for (auto b = expected.rbegin(); b != expected.rend(); ++b) {
      auto copy = past_the_end_;
      copy -= offset;
      --iter;
      EXPECT_EQ(copy, iter);
      EXPECT_EQ(*copy, *b);
      ++offset;
    }
  }
}
TEST_F(ByteIteratorTest, CanIterateUsingCompoundSubtraction) {
  CanIterateUsingCompoundSubtraction();
}
TEST_F(ConstByteIteratorTest, CanIterateUsingCompoundSubtraction) {
  CanIterateUsingCompoundSubtraction();
}

template <typename IteratorType>
void ByteIteratorTestImpl<
    IteratorType>::CanCalculateDistanceBetweenIterators() {
  uint16_t total = 0;
  for (size_t i = 0; i < kNumContiguous; ++i) {
    total += GetContiguous(i).size();
  }
  EXPECT_EQ(first_ - first_, 0);
  EXPECT_EQ(first_ - flipped_, 0);
  EXPECT_EQ(second_ - first_, 1);
  EXPECT_EQ(first_ - second_, -1);
  EXPECT_EQ(last_ - first_, total - 1);
  EXPECT_EQ(past_the_end_ - first_, total);
}
TEST_F(ByteIteratorTest, CanCalculateDistanceBetweenIterators) {
  CanCalculateDistanceBetweenIterators();
}
TEST_F(ConstByteIteratorTest, CanCalculateDistanceBetweenIterators) {
  CanCalculateDistanceBetweenIterators();
}

template <typename IteratorType>
void ByteIteratorTestImpl<IteratorType>::CanCompareIteratorsUsingEqual() {
  EXPECT_EQ(first_, first_);
  EXPECT_EQ(first_, flipped_);
  EXPECT_EQ(past_the_end_, past_the_end_);
}
TEST_F(ByteIteratorTest, CanCompareIteratorsUsingEqual) {
  CanCompareIteratorsUsingEqual();
}
TEST_F(ConstByteIteratorTest, CanCompareIteratorsUsingEqual) {
  CanCompareIteratorsUsingEqual();
}

template <typename IteratorType>
void ByteIteratorTestImpl<IteratorType>::CanCompareIteratorsUsingNotEqual() {
  EXPECT_NE(first_, second_);
  EXPECT_NE(flipped_, second_);
  EXPECT_NE(first_, past_the_end_);
}
TEST_F(ByteIteratorTest, CanCompareIteratorsUsingNotEqual) {
  CanCompareIteratorsUsingNotEqual();
}
TEST_F(ConstByteIteratorTest, CanCompareIteratorsUsingNotEqual) {
  CanCompareIteratorsUsingNotEqual();
}

template <typename IteratorType>
void ByteIteratorTestImpl<IteratorType>::CanCompareIteratorsUsingLessThan() {
  EXPECT_LT(first_, second_);
  EXPECT_LT(flipped_, second_);
  EXPECT_LT(first_, past_the_end_);
}
TEST_F(ByteIteratorTest, CanCompareIteratorsUsingLessThan) {
  CanCompareIteratorsUsingLessThan();
}
TEST_F(ConstByteIteratorTest, CanCompareIteratorsUsingLessThan) {
  CanCompareIteratorsUsingLessThan();
}

template <typename IteratorType>
void ByteIteratorTestImpl<
    IteratorType>::CanCompareIteratorsUsingLessThanOrEqual() {
  EXPECT_LE(first_, first_);
  EXPECT_LE(first_, flipped_);
  EXPECT_LE(first_, second_);
  EXPECT_LE(first_, past_the_end_);
}
TEST_F(ByteIteratorTest, CanCompareIteratorsUsingLessThanOrEqual) {
  CanCompareIteratorsUsingLessThanOrEqual();
}
TEST_F(ConstByteIteratorTest, CanCompareIteratorsUsingLessThanOrEqual) {
  CanCompareIteratorsUsingLessThanOrEqual();
}

template <typename IteratorType>
void ByteIteratorTestImpl<IteratorType>::CanCompareIteratorsUsingGreaterThan() {
  EXPECT_GT(last_, second_);
  EXPECT_GT(last_, flipped_);
  EXPECT_GT(past_the_end_, last_);
}
TEST_F(ByteIteratorTest, CanCompareIteratorsUsingGreaterThan) {
  CanCompareIteratorsUsingGreaterThan();
}
TEST_F(ConstByteIteratorTest, CanCompareIteratorsUsingGreaterThan) {
  CanCompareIteratorsUsingGreaterThan();
}

template <typename IteratorType>
void ByteIteratorTestImpl<
    IteratorType>::CanCompareIteratorsUsingGreaterThanOrEqual() {
  EXPECT_GE(past_the_end_, past_the_end_);
  EXPECT_GE(last_, second_);
  EXPECT_GE(last_, flipped_);
  EXPECT_GE(past_the_end_, last_);
}
TEST_F(ByteIteratorTest, CanCompareIteratorsUsingGreaterThanOrEqual) {
  CanCompareIteratorsUsingGreaterThanOrEqual();
}
TEST_F(ConstByteIteratorTest, CanCompareIteratorsUsingGreaterThanOrEqual) {
  CanCompareIteratorsUsingGreaterThanOrEqual();
}

}  // namespace
