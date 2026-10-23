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

#include "pw_allocator/fragmentation.h"

#include <cstddef>
#include <limits>

#include "pw_allocator/testing.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::allocator::Fragmentation;

TEST(FragmentationTest, ValuesAreCorrect) {
  Fragmentation fragmentation;
  fragmentation.AddFragment(867);
  fragmentation.AddFragment(5309);
  EXPECT_EQ(fragmentation.sum_of_squares.hi, 0U);
  EXPECT_EQ(fragmentation.sum_of_squares.lo, 867U * 867U + 5309U * 5309U);
  EXPECT_EQ(fragmentation.sum, 867U + 5309U);
}

TEST(FragmentationTest, HandlesOverflow) {
  constexpr size_t kHalfWord = size_t(1) << sizeof(size_t) * 4;
  Fragmentation fragmentation;
  fragmentation.AddFragment(kHalfWord);
  fragmentation.AddFragment(kHalfWord);
  fragmentation.AddFragment(kHalfWord);
  fragmentation.AddFragment(kHalfWord);
  EXPECT_EQ(fragmentation.sum_of_squares.hi, 4U);
  EXPECT_EQ(fragmentation.sum_of_squares.lo, 0U);
  EXPECT_EQ(fragmentation.sum, 4 * kHalfWord);
}

TEST(FragmentationTest, CalculateFragmentation) {
  // Add `n^2` fragments of size `n`, so that the sum of squares is just `n^4`.
  // Then the root is `n^2`, the sum is `n^3`, and the result is `1 - 1/n`.
  for (size_t n = 2; n < 20; ++n) {
    Fragmentation fragmentation;
    for (size_t i = 0; i < n * n; ++i) {
      fragmentation.AddFragment(n);
    }
    auto expected = 1.f - (1.f / static_cast<float>(n));
    EXPECT_FLOAT_EQ(CalculateFragmentation(fragmentation), expected);
  }
}

TEST(FragmentationTest, AdditionIsCorrect) {
  Fragmentation frag1;
  frag1.AddFragment(100);
  frag1.AddFragment(200);

  Fragmentation frag2;
  frag2.AddFragment(300);
  frag2.AddFragment(400);

  Fragmentation combined = frag1 + frag2;
  EXPECT_EQ(combined.sum, 100U + 200U + 300U + 400U);
  EXPECT_EQ(combined.sum_of_squares.lo,
            100U * 100U + 200U * 200U + 300U * 300U + 400U * 400U);
  EXPECT_EQ(combined.sum_of_squares.hi, 0U);

  frag1 += frag2;
  EXPECT_EQ(frag1.sum, combined.sum);
  EXPECT_EQ(frag1.sum_of_squares.lo, combined.sum_of_squares.lo);
  EXPECT_EQ(frag1.sum_of_squares.hi, combined.sum_of_squares.hi);
}

TEST(FragmentationTest, AdditionHandlesOverflow) {
  constexpr size_t kHalfWord = size_t(1) << sizeof(size_t) * 4;
  Fragmentation frag1;
  frag1.AddFragment(kHalfWord);
  frag1.AddFragment(kHalfWord);  // sum_of_squares.hi = 2, lo = 0

  Fragmentation frag2;
  frag2.AddFragment(kHalfWord);
  frag2.AddFragment(kHalfWord);  // sum_of_squares.hi = 2, lo = 0

  frag1 += frag2;
  EXPECT_EQ(frag1.sum_of_squares.hi, 4U);
  EXPECT_EQ(frag1.sum_of_squares.lo, 0U);
  EXPECT_EQ(frag1.sum, 4 * kHalfWord);
}

TEST(FragmentationTest, AdditionHandlesLoToHiOverflow) {
  Fragmentation frag1;
  frag1.sum_of_squares.lo = std::numeric_limits<size_t>::max();
  frag1.sum_of_squares.hi = 1;
  frag1.sum = 100;

  Fragmentation frag2;
  frag2.sum_of_squares.lo = 1;
  frag2.sum_of_squares.hi = 1;
  frag2.sum = 100;

  frag1 += frag2;
  EXPECT_EQ(frag1.sum_of_squares.lo, 0U);
  EXPECT_EQ(frag1.sum_of_squares.hi, 3U);  // 1 + 1 + 1 (carry)
  EXPECT_EQ(frag1.sum, 200U);
}

TEST(FragmentationTest, SubtractionOperatorIsCorrect) {
  Fragmentation frag1;
  frag1.AddFragment(100);
  frag1.AddFragment(200);

  Fragmentation frag2;
  frag2.AddFragment(300);
  frag2.AddFragment(400);

  Fragmentation total = frag1 + frag2;
  Fragmentation result = total - frag2;
  EXPECT_EQ(result, frag1);

  total -= frag1;
  EXPECT_EQ(total, frag2);
}

TEST(FragmentationTest, SubtractionHandlesHiLoBorrow) {
  Fragmentation frag1;
  frag1.sum_of_squares.hi = 1;
  frag1.sum_of_squares.lo = 0;
  frag1.sum = 100;

  Fragmentation frag2;
  frag2.sum_of_squares.hi = 0;
  frag2.sum_of_squares.lo = 1;
  frag2.sum = 10;

  frag1 -= frag2;
  EXPECT_EQ(frag1.sum_of_squares.hi, 0U);
  EXPECT_EQ(frag1.sum_of_squares.lo, std::numeric_limits<size_t>::max());
  EXPECT_EQ(frag1.sum, 90U);
}

TEST(FragmentationTest, Equality) {
  Fragmentation frag1;
  frag1.AddFragment(100);
  frag1.AddFragment(200);

  Fragmentation frag2;
  frag2.AddFragment(100);
  frag2.AddFragment(200);

  EXPECT_TRUE(frag1 == frag2);
  EXPECT_FALSE(frag1 != frag2);

  frag2.AddFragment(300);
  EXPECT_FALSE(frag1 == frag2);
  EXPECT_TRUE(frag1 != frag2);
}

TEST(FragmentationTest, SubtractionIsCorrect) {
  Fragmentation frag;
  frag.AddFragment(100);
  frag.AddFragment(200);
  frag.AddFragment(300);

  frag.SubtractFragment(200);
  EXPECT_EQ(frag.sum, 100U + 300U);
  EXPECT_EQ(frag.sum_of_squares.lo, 100U * 100U + 300U * 300U);
  EXPECT_EQ(frag.sum_of_squares.hi, 0U);
}

TEST(FragmentationTest, SubtractionHandlesOverflow) {
  constexpr size_t kHalfWord = size_t(1) << sizeof(size_t) * 4;
  Fragmentation frag;
  frag.AddFragment(kHalfWord);
  frag.AddFragment(kHalfWord);
  frag.AddFragment(kHalfWord);
  frag.AddFragment(kHalfWord);

  frag.SubtractFragment(kHalfWord);
  EXPECT_EQ(frag.sum_of_squares.hi, 3U);
  EXPECT_EQ(frag.sum_of_squares.lo, 0U);
  EXPECT_EQ(frag.sum, 3 * kHalfWord);

  frag.SubtractFragment(kHalfWord);
  EXPECT_EQ(frag.sum_of_squares.hi, 2U);
  EXPECT_EQ(frag.sum_of_squares.lo, 0U);
  EXPECT_EQ(frag.sum, 2 * kHalfWord);
}

TEST(FragmentationTest, SubtractionToZero) {
  Fragmentation frag;
  frag.AddFragment(100);
  frag.AddFragment(200);

  frag.SubtractFragment(100);
  frag.SubtractFragment(200);

  EXPECT_EQ(frag.sum, 0U);
  EXPECT_EQ(frag.sum_of_squares.lo, 0U);
  EXPECT_EQ(frag.sum_of_squares.hi, 0U);
  EXPECT_EQ(frag, Fragmentation());
}

}  // namespace
