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

using pw::multibuf::v2::internal::Mutability;

template <Mutability kMutability>
using ByteIterator = ::pw::multibuf::v2::internal::ByteIterator<kMutability>;

using ::pw::multibuf::v2::test::IteratorTest;

// Test fixture.
class ByteIteratorTest : public IteratorTest {
 protected:
  using iterator = ByteIterator<Mutability::kMutable>;
  using const_iterator = ByteIterator<Mutability::kConst>;

  ByteIteratorTest() {
    second_ = MakeIterator(0);
    flipped_ = MakeConstIterator(0);
    first_ = second_++;
    last_ = MakeIterator(kNumChunks);
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

  iterator first_;
  const_iterator flipped_;
  iterator second_;
  iterator last_;
  iterator past_the_end_;
};

// Template method implementations.

TEST_F(ByteIteratorTest, CanDereferenceToByte) {
  pw::ConstByteSpan first_span = GetContiguous(0);
  pw::ConstByteSpan last_span = GetContiguous(kNumContiguous - 1);
  EXPECT_EQ(&(*first_), first_span.data());
  EXPECT_EQ(&(*flipped_), first_span.data());
  EXPECT_EQ(&(*last_), last_span.data() + last_span.size() - 1);
}

TEST_F(ByteIteratorTest, CanDereferenceWithArrayIndex) {
  pw::ConstByteSpan expected = GetContiguous(0);
  for (uint16_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(first_[i], expected[i]);
    EXPECT_EQ(flipped_[i], expected[i]);
  }
}

TEST_F(ByteIteratorTest, CanIterateUsingPrefixIncrement) {
  iterator mut_iter = first_;
  const_iterator const_iter = first_;
  for (size_t i = 0; mut_iter != past_the_end_; ++i) {
    pw::ConstByteSpan expected = GetContiguous(i);
    for (auto b : expected) {
      EXPECT_EQ(*mut_iter, b);
      ++mut_iter;

      EXPECT_EQ(*const_iter, b);
      ++const_iter;
    }
  }
}

TEST_F(ByteIteratorTest, CanIterateUsingPostfixIncrement) {
  iterator mut_iter = first_;
  const_iterator const_iter = first_;
  for (size_t i = 0; mut_iter != past_the_end_; ++i) {
    for (auto expected : GetContiguous(i)) {
      iterator mut_copy = mut_iter++;
      EXPECT_EQ(*mut_copy, expected);

      const_iterator const_copy = const_iter++;
      EXPECT_EQ(*const_copy, expected);
    }
  }
}

TEST_F(ByteIteratorTest, CanIterateUsingAddition) {
  uint16_t offset = 0;
  iterator mut_iter = first_;
  const_iterator const_iter = first_;
  for (size_t i = 0; mut_iter != past_the_end_; ++i) {
    for (auto expected : GetContiguous(i)) {
      iterator mut_copy = first_ + offset;
      EXPECT_EQ(mut_copy, mut_iter);
      EXPECT_EQ(*mut_copy, expected);
      ++mut_iter;

      const_iterator const_copy = first_ + offset;
      EXPECT_EQ(const_copy, const_iter);
      EXPECT_EQ(*const_copy, expected);
      ++const_iter;

      ++offset;
    }
  }
}

TEST_F(ByteIteratorTest, CanIterateUsingCompoundAddition) {
  uint16_t offset = 0;
  iterator mut_iter = first_;
  const_iterator const_iter = first_;
  for (size_t i = 0; mut_iter != past_the_end_; ++i) {
    for (auto expected : GetContiguous(i)) {
      iterator mut_copy = first_;
      mut_copy += offset;
      EXPECT_EQ(mut_copy, mut_iter);
      EXPECT_EQ(*mut_copy, expected);
      ++mut_iter;

      const_iterator const_copy = first_;
      const_copy += offset;
      EXPECT_EQ(const_copy, const_iter);
      EXPECT_EQ(*const_copy, expected);
      ++const_iter;

      ++offset;
    }
  }
}

TEST_F(ByteIteratorTest, CanIterateUsingPrefixDecrement) {
  iterator mut_iter = last_;
  const_iterator const_iter = last_;
  for (size_t i = 1; i <= kNumContiguous; ++i) {
    auto expected = GetContiguous(kNumContiguous - i);
    for (auto b = expected.rbegin(); b != expected.rend(); ++b) {
      EXPECT_EQ(*mut_iter, *b);
      EXPECT_EQ(*const_iter, *b);
      if (mut_iter != first_) {
        --mut_iter;
        --const_iter;
      }
    }
  }
}

TEST_F(ByteIteratorTest, CanIterateUsingPostfixDecrement) {
  iterator mut_iter = last_;
  const_iterator const_iter = last_;
  for (size_t i = 1; i <= kNumContiguous; ++i) {
    auto expected = GetContiguous(kNumContiguous - i);
    for (auto b = expected.rbegin(); b != expected.rend(); ++b) {
      if (mut_iter == first_) {
        EXPECT_EQ(*mut_iter, *b);
        EXPECT_EQ(*const_iter, *b);
      } else {
        iterator mut_copy = mut_iter--;
        EXPECT_EQ(*mut_copy, *b);

        const_iterator const_copy = const_iter--;
        EXPECT_EQ(*const_copy, *b);
      }
    }
  }
}

TEST_F(ByteIteratorTest, CanIterateUsingSubtraction) {
  uint16_t offset = 1;
  iterator mut_iter = past_the_end_;
  const_iterator const_iter = past_the_end_;
  for (size_t i = 1; i <= kNumContiguous; ++i) {
    auto expected = GetContiguous(kNumContiguous - i);
    for (auto b = expected.rbegin(); b != expected.rend(); ++b) {
      iterator mut_copy = past_the_end_ - offset;
      --mut_iter;
      EXPECT_EQ(mut_copy, mut_iter);
      EXPECT_EQ(*mut_copy, *b);

      const_iterator const_copy = past_the_end_ - offset;
      --const_iter;
      EXPECT_EQ(const_copy, const_iter);
      EXPECT_EQ(*const_copy, *b);

      ++offset;
    }
  }
}

TEST_F(ByteIteratorTest, CanIterateUsingCompoundSubtraction) {
  uint16_t offset = 1;
  iterator mut_iter = past_the_end_;
  const_iterator const_iter = past_the_end_;
  for (size_t i = 1; i <= kNumContiguous; ++i) {
    auto expected = GetContiguous(kNumContiguous - i);
    for (auto b = expected.rbegin(); b != expected.rend(); ++b) {
      iterator mut_copy = past_the_end_;
      mut_copy -= offset;
      --mut_iter;
      EXPECT_EQ(mut_copy, mut_iter);
      EXPECT_EQ(*mut_copy, *b);

      const_iterator const_copy = past_the_end_;
      const_copy -= offset;
      --const_iter;
      EXPECT_EQ(const_copy, const_iter);
      EXPECT_EQ(*const_copy, *b);

      ++offset;
    }
  }
}

TEST_F(ByteIteratorTest, CanCalculateDistanceBetweenIterators) {
  uint16_t total = 0;
  for (size_t i = 0; i < kNumContiguous; ++i) {
    total += GetContiguous(i).size();
  }
  EXPECT_EQ(first_ - first_, 0);
  EXPECT_EQ(std::distance(first_, first_), 0);

  EXPECT_EQ(first_ - flipped_, 0);

  EXPECT_EQ(second_ - first_, 1);
  EXPECT_EQ(std::distance(first_, second_), 1);

  EXPECT_EQ(first_ - second_, -1);

  EXPECT_EQ(last_ - first_, total - 1);
  EXPECT_EQ(std::distance(first_, last_), total - 1);

  EXPECT_EQ(past_the_end_ - first_, total);
  EXPECT_EQ(std::distance(first_, past_the_end_), total);
}

TEST_F(ByteIteratorTest, CanCompareIteratorsUsingEqual) {
  EXPECT_EQ(first_, first_);
  EXPECT_EQ(first_, flipped_);
  EXPECT_EQ(past_the_end_, past_the_end_);
}

TEST_F(ByteIteratorTest, CanCompareIteratorsUsingNotEqual) {
  EXPECT_NE(first_, second_);
  EXPECT_NE(flipped_, second_);
  EXPECT_NE(first_, past_the_end_);
}

TEST_F(ByteIteratorTest, CanCompareIteratorsUsingLessThan) {
  EXPECT_LT(first_, second_);
  EXPECT_LT(flipped_, second_);
  EXPECT_LT(first_, past_the_end_);
}

TEST_F(ByteIteratorTest, CanCompareIteratorsUsingLessThanOrEqual) {
  EXPECT_LE(first_, first_);
  EXPECT_LE(first_, flipped_);
  EXPECT_LE(first_, second_);
  EXPECT_LE(first_, past_the_end_);
}

TEST_F(ByteIteratorTest, CanCompareIteratorsUsingGreaterThan) {
  EXPECT_GT(last_, second_);
  EXPECT_GT(last_, flipped_);
  EXPECT_GT(past_the_end_, last_);
}

TEST_F(ByteIteratorTest, CanCompareIteratorsUsingGreaterThanOrEqual) {
  EXPECT_GE(past_the_end_, past_the_end_);
  EXPECT_GE(last_, second_);
  EXPECT_GE(last_, flipped_);
  EXPECT_GE(past_the_end_, last_);
}

}  // namespace
