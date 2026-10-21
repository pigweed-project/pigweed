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

#include "pw_thread/internal/priority.h"

#include <array>
#include <cstdint>

#include "pw_compilation_testing/negative_compilation.h"
#include "pw_unit_test/constexpr.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::thread::internal::Priority;

PW_CONSTEXPR_TEST(ThreadPriority, PriorityNotSuppoerted, {
  using NoPriority = Priority<int, 0, 0, 0>;

  PW_TEST_EXPECT_FALSE(NoPriority::IsSupported());
  PW_TEST_EXPECT_EQ(NoPriority::Medium().NextHigherClamped(),
                    NoPriority::Medium());
});

PW_CONSTEXPR_TEST(ThreadPriority, Default, {
  using Zero = Priority<int, 0, 0, 0>;
  PW_TEST_EXPECT_EQ(Zero::Default().native(), 0);

  using MidDefault = Priority<signed char, -100, 100, 0>;
  PW_TEST_EXPECT_EQ(MidDefault::Default().native(), 0);
  PW_TEST_EXPECT_EQ(MidDefault::Default(), MidDefault());

  using HighDefault = Priority<signed char, -100, 100, 100>;
  PW_TEST_EXPECT_EQ(HighDefault::Default().native(), 100);
  PW_TEST_EXPECT_EQ(HighDefault::Default(), HighDefault());

  using LowDefault = Priority<signed char, -100, 100, -100>;
  PW_TEST_EXPECT_EQ(LowDefault::Default().native(), -100);
  PW_TEST_EXPECT_EQ(LowDefault::Default(), LowDefault());
});

// Table of expected priority values where the columns are the priority tiers.
// lowest=lt, very low = vl, low = l, etc.
template <typename T>
constexpr std::array<T, 9> kExpectedPriorities[] = {
    // clang-format off
//  lt,vl, l,ml, m,mh, h,vh,ht
    // Fewer native priorities than named priorities.
    {0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 1, 1, 1, 1, 1},
    {0, 0, 1, 1, 1, 1, 2, 2, 2},
    {0, 0, 1, 1, 2, 2, 2, 3, 3},
    {0, 1, 1, 2, 2, 3, 3, 4, 4},
    {0, 1, 1, 2, 3, 3, 4, 4, 5},
    {0, 1, 2, 2, 3, 4, 5, 5, 6},
    {0, 1, 2, 3, 4, 4, 5, 6, 7},
    // One native level per named priority.
    {0, 1, 2, 3, 4, 5, 6, 7, 8},
    // More native priorities than named priorities.
    {0,  1,  2,  3,  5,  6,  7,  8,  9},
    {0,  1,  3,  4,  5,  6,  8,  9, 10},
    {0,  1,  3,  4,  6,  7,  8, 10, 11},
    {0,  2,  3,  5,  6,  8,  9, 11, 12},
    {0,  2,  3,  5,  7,  8, 10, 11, 13},
    {0,  2,  4,  5,  7,  9, 11, 12, 14},
    {0,  2,  4,  6,  8,  9, 11, 13, 15},
    {0,  2,  4,  6,  8, 10, 12, 14, 16},
};  // clang-format on

template <typename T, T kMax>
constexpr void TestNamedPrioritiesWithMax() {
  using ThreadPriority = Priority<T, 0, kMax, 0>;
  constexpr const auto& expected = kExpectedPriorities<T>[kMax];

  PW_TEST_EXPECT_EQ(ThreadPriority::Lowest().native(), expected[0]);
  PW_TEST_EXPECT_EQ(ThreadPriority::VeryLow().native(), expected[1]);
  PW_TEST_EXPECT_EQ(ThreadPriority::Low().native(), expected[2]);
  PW_TEST_EXPECT_EQ(ThreadPriority::MediumLow().native(), expected[3]);
  PW_TEST_EXPECT_EQ(ThreadPriority::Medium().native(), expected[4]);
  PW_TEST_EXPECT_EQ(ThreadPriority::MediumHigh().native(), expected[5]);
  PW_TEST_EXPECT_EQ(ThreadPriority::High().native(), expected[6]);
  PW_TEST_EXPECT_EQ(ThreadPriority::VeryHigh().native(), expected[7]);
  PW_TEST_EXPECT_EQ(ThreadPriority::Highest().native(), expected[8]);
}

template <typename T>
constexpr void TestNamedPrioritiesZeroLowest() {
  TestNamedPrioritiesWithMax<T, 0>();
  TestNamedPrioritiesWithMax<T, 1>();
  TestNamedPrioritiesWithMax<T, 2>();
  TestNamedPrioritiesWithMax<T, 3>();
  TestNamedPrioritiesWithMax<T, 4>();
  TestNamedPrioritiesWithMax<T, 5>();
  TestNamedPrioritiesWithMax<T, 6>();
  TestNamedPrioritiesWithMax<T, 7>();
  TestNamedPrioritiesWithMax<T, 8>();
  TestNamedPrioritiesWithMax<T, 9>();
  TestNamedPrioritiesWithMax<T, 10>();
  TestNamedPrioritiesWithMax<T, 11>();
  TestNamedPrioritiesWithMax<T, 12>();
  TestNamedPrioritiesWithMax<T, 13>();
  TestNamedPrioritiesWithMax<T, 14>();
  TestNamedPrioritiesWithMax<T, 15>();
  TestNamedPrioritiesWithMax<T, 16>();
}

PW_CONSTEXPR_TEST(ThreadPriority, NamedPriorities0ToMax, {
  TestNamedPrioritiesZeroLowest<int8_t>();
  TestNamedPrioritiesZeroLowest<uint8_t>();
  TestNamedPrioritiesZeroLowest<int32_t>();
  TestNamedPrioritiesZeroLowest<uint32_t>();
});

PW_CONSTEXPR_TEST(ThreadPriority, OffsetLowToHigh, {
  using ThreadPriority = Priority<int16_t, 1, 9, 1>;

  PW_TEST_EXPECT_EQ(ThreadPriority::Lowest().native(), 1);
  PW_TEST_EXPECT_EQ(ThreadPriority::VeryLow().native(), 2);
  PW_TEST_EXPECT_EQ(ThreadPriority::Low().native(), 3);
  PW_TEST_EXPECT_EQ(ThreadPriority::MediumLow().native(), 4);
  PW_TEST_EXPECT_EQ(ThreadPriority::Medium().native(), 5);
  PW_TEST_EXPECT_EQ(ThreadPriority::MediumHigh().native(), 6);
  PW_TEST_EXPECT_EQ(ThreadPriority::High().native(), 7);
  PW_TEST_EXPECT_EQ(ThreadPriority::VeryHigh().native(), 8);
  PW_TEST_EXPECT_EQ(ThreadPriority::Highest().native(), 9);
});

enum class HighPriorityIsLowValue {
  kMaximum = -1,
  kSuperHigh = 0,
  kHigh = 1,
  kPrettyHigh = 2,
  kJustRight = 3,
  kKindaLow = 4,
  kLow = 5,
  kLowLowLow = 6,
  kAsLowAsItGoes = 7,
};
using HighIsLow = Priority<HighPriorityIsLowValue,
                           HighPriorityIsLowValue::kAsLowAsItGoes,
                           HighPriorityIsLowValue::kMaximum,
                           HighPriorityIsLowValue::kLow>;

PW_CONSTEXPR_TEST(ThreadPriority, HighIsLowEnumMapsCorrectlyToNamedPriorities, {
  PW_TEST_EXPECT_EQ(HighIsLow::Lowest().native(),
                    HighPriorityIsLowValue::kAsLowAsItGoes);
  PW_TEST_EXPECT_EQ(HighIsLow::VeryLow().native(),
                    HighPriorityIsLowValue::kLowLowLow);
  PW_TEST_EXPECT_EQ(HighIsLow::Low().native(), HighPriorityIsLowValue::kLow);
  PW_TEST_EXPECT_EQ(HighIsLow::MediumLow().native(),
                    HighPriorityIsLowValue::kKindaLow);
  PW_TEST_EXPECT_EQ(HighIsLow::Medium().native(),
                    HighPriorityIsLowValue::kJustRight);
  PW_TEST_EXPECT_EQ(HighIsLow::MediumHigh().native(),
                    HighPriorityIsLowValue::kPrettyHigh);
  PW_TEST_EXPECT_EQ(HighIsLow::High().native(), HighPriorityIsLowValue::kHigh);
  PW_TEST_EXPECT_EQ(HighIsLow::VeryHigh().native(),
                    HighPriorityIsLowValue::kSuperHigh);
  PW_TEST_EXPECT_EQ(HighIsLow::Highest().native(),
                    HighPriorityIsLowValue::kMaximum);
});

PW_CONSTEXPR_TEST(ThreadPriority, HighIsLowEnumFromNative, {
  PW_TEST_EXPECT_EQ(
      HighIsLow::FromNative(HighPriorityIsLowValue::kMaximum).native(),
      HighPriorityIsLowValue::kMaximum);
  PW_TEST_EXPECT_EQ(
      HighIsLow::FromNative(HighPriorityIsLowValue::kKindaLow).native(),
      HighPriorityIsLowValue::kKindaLow);
  PW_TEST_EXPECT_EQ(
      HighIsLow::FromNative(HighPriorityIsLowValue::kAsLowAsItGoes).native(),
      HighPriorityIsLowValue::kAsLowAsItGoes);
});

PW_CONSTEXPR_TEST(ThreadPriority, HighIsLowComparisons, {
  PW_TEST_EXPECT_LT(HighIsLow::Lowest(), HighIsLow::Highest());
  PW_TEST_EXPECT_LT(HighIsLow::Lowest(), HighIsLow::VeryLow());
  PW_TEST_EXPECT_LT(HighIsLow::VeryLow(), HighIsLow::Low());
  PW_TEST_EXPECT_LT(HighIsLow::Low(), HighIsLow::MediumLow());
  PW_TEST_EXPECT_LT(HighIsLow::MediumLow(), HighIsLow::Medium());
  PW_TEST_EXPECT_LT(HighIsLow::Medium(), HighIsLow::MediumHigh());
  PW_TEST_EXPECT_LT(HighIsLow::MediumHigh(), HighIsLow::High());
  PW_TEST_EXPECT_LT(HighIsLow::High(), HighIsLow::VeryHigh());
  PW_TEST_EXPECT_LT(HighIsLow::VeryHigh(), HighIsLow::Highest());

  PW_TEST_EXPECT_LE(HighIsLow::Lowest(), HighIsLow::Highest());
  PW_TEST_EXPECT_LE(HighIsLow::Lowest(), HighIsLow::Lowest());
  PW_TEST_EXPECT_LE(HighIsLow::Lowest(), HighIsLow::VeryLow());
  PW_TEST_EXPECT_LE(HighIsLow::VeryLow(), HighIsLow::VeryLow());
  PW_TEST_EXPECT_LE(HighIsLow::VeryLow(), HighIsLow::Low());
  PW_TEST_EXPECT_LE(HighIsLow::Low(), HighIsLow::Low());
  PW_TEST_EXPECT_LE(HighIsLow::Low(), HighIsLow::MediumLow());
  PW_TEST_EXPECT_LE(HighIsLow::MediumLow(), HighIsLow::MediumLow());
  PW_TEST_EXPECT_LE(HighIsLow::MediumLow(), HighIsLow::Medium());
  PW_TEST_EXPECT_LE(HighIsLow::Medium(), HighIsLow::Medium());
  PW_TEST_EXPECT_LE(HighIsLow::Medium(), HighIsLow::MediumHigh());
  PW_TEST_EXPECT_LE(HighIsLow::MediumHigh(), HighIsLow::MediumHigh());
  PW_TEST_EXPECT_LE(HighIsLow::MediumHigh(), HighIsLow::High());
  PW_TEST_EXPECT_LE(HighIsLow::High(), HighIsLow::High());
  PW_TEST_EXPECT_LE(HighIsLow::High(), HighIsLow::VeryHigh());
  PW_TEST_EXPECT_LE(HighIsLow::VeryHigh(), HighIsLow::VeryHigh());
  PW_TEST_EXPECT_LE(HighIsLow::VeryHigh(), HighIsLow::Highest());
  PW_TEST_EXPECT_LE(HighIsLow::Highest(), HighIsLow::Highest());

  PW_TEST_EXPECT_GT(HighIsLow::Highest(), HighIsLow::Lowest());
  PW_TEST_EXPECT_GT(HighIsLow::VeryLow(), HighIsLow::Lowest());
  PW_TEST_EXPECT_GT(HighIsLow::Low(), HighIsLow::VeryLow());
  PW_TEST_EXPECT_GT(HighIsLow::MediumLow(), HighIsLow::Low());
  PW_TEST_EXPECT_GT(HighIsLow::Medium(), HighIsLow::MediumLow());
  PW_TEST_EXPECT_GT(HighIsLow::MediumHigh(), HighIsLow::Medium());
  PW_TEST_EXPECT_GT(HighIsLow::High(), HighIsLow::MediumHigh());
  PW_TEST_EXPECT_GT(HighIsLow::VeryHigh(), HighIsLow::High());
  PW_TEST_EXPECT_GT(HighIsLow::Highest(), HighIsLow::VeryHigh());

  PW_TEST_EXPECT_GE(HighIsLow::Highest(), HighIsLow::Lowest());
  PW_TEST_EXPECT_GE(HighIsLow::Lowest(), HighIsLow::Lowest());
  PW_TEST_EXPECT_GE(HighIsLow::VeryLow(), HighIsLow::Lowest());
  PW_TEST_EXPECT_GE(HighIsLow::VeryLow(), HighIsLow::VeryLow());
  PW_TEST_EXPECT_GE(HighIsLow::Low(), HighIsLow::VeryLow());
  PW_TEST_EXPECT_GE(HighIsLow::Low(), HighIsLow::Low());
  PW_TEST_EXPECT_GE(HighIsLow::MediumLow(), HighIsLow::Low());
  PW_TEST_EXPECT_GE(HighIsLow::MediumLow(), HighIsLow::MediumLow());
  PW_TEST_EXPECT_GE(HighIsLow::Medium(), HighIsLow::MediumLow());
  PW_TEST_EXPECT_GE(HighIsLow::Medium(), HighIsLow::Medium());
  PW_TEST_EXPECT_GE(HighIsLow::MediumHigh(), HighIsLow::Medium());
  PW_TEST_EXPECT_GE(HighIsLow::MediumHigh(), HighIsLow::MediumHigh());
  PW_TEST_EXPECT_GE(HighIsLow::High(), HighIsLow::MediumHigh());
  PW_TEST_EXPECT_GE(HighIsLow::High(), HighIsLow::High());
  PW_TEST_EXPECT_GE(HighIsLow::VeryHigh(), HighIsLow::High());
  PW_TEST_EXPECT_GE(HighIsLow::VeryHigh(), HighIsLow::VeryHigh());
  PW_TEST_EXPECT_GE(HighIsLow::Highest(), HighIsLow::VeryHigh());
  PW_TEST_EXPECT_GE(HighIsLow::Highest(), HighIsLow::Highest());
});

PW_CONSTEXPR_TEST(ThreadPriority, LowIsHighComparisons, {
  using ThreadPriority = Priority<int, 0, 100, 0>;

  PW_TEST_EXPECT_LT(ThreadPriority::Lowest(), ThreadPriority::Highest());
  PW_TEST_EXPECT_LT(ThreadPriority::Lowest(), ThreadPriority::VeryLow());
  PW_TEST_EXPECT_LT(ThreadPriority::VeryLow(), ThreadPriority::Low());
  PW_TEST_EXPECT_LT(ThreadPriority::Low(), ThreadPriority::MediumLow());
  PW_TEST_EXPECT_LT(ThreadPriority::MediumLow(), ThreadPriority::Medium());
  PW_TEST_EXPECT_LT(ThreadPriority::Medium(), ThreadPriority::MediumHigh());
  PW_TEST_EXPECT_LT(ThreadPriority::MediumHigh(), ThreadPriority::High());
  PW_TEST_EXPECT_LT(ThreadPriority::High(), ThreadPriority::VeryHigh());
  PW_TEST_EXPECT_LT(ThreadPriority::VeryHigh(), ThreadPriority::Highest());

  PW_TEST_EXPECT_LE(ThreadPriority::Lowest(), ThreadPriority::Highest());
  PW_TEST_EXPECT_LE(ThreadPriority::Lowest(), ThreadPriority::Lowest());
  PW_TEST_EXPECT_LE(ThreadPriority::Lowest(), ThreadPriority::VeryLow());
  PW_TEST_EXPECT_LE(ThreadPriority::VeryLow(), ThreadPriority::VeryLow());
  PW_TEST_EXPECT_LE(ThreadPriority::VeryLow(), ThreadPriority::Low());
  PW_TEST_EXPECT_LE(ThreadPriority::Low(), ThreadPriority::Low());
  PW_TEST_EXPECT_LE(ThreadPriority::Low(), ThreadPriority::MediumLow());
  PW_TEST_EXPECT_LE(ThreadPriority::MediumLow(), ThreadPriority::MediumLow());
  PW_TEST_EXPECT_LE(ThreadPriority::MediumLow(), ThreadPriority::Medium());
  PW_TEST_EXPECT_LE(ThreadPriority::Medium(), ThreadPriority::Medium());
  PW_TEST_EXPECT_LE(ThreadPriority::Medium(), ThreadPriority::MediumHigh());
  PW_TEST_EXPECT_LE(ThreadPriority::MediumHigh(), ThreadPriority::MediumHigh());
  PW_TEST_EXPECT_LE(ThreadPriority::MediumHigh(), ThreadPriority::High());
  PW_TEST_EXPECT_LE(ThreadPriority::High(), ThreadPriority::High());
  PW_TEST_EXPECT_LE(ThreadPriority::High(), ThreadPriority::VeryHigh());
  PW_TEST_EXPECT_LE(ThreadPriority::VeryHigh(), ThreadPriority::VeryHigh());
  PW_TEST_EXPECT_LE(ThreadPriority::VeryHigh(), ThreadPriority::Highest());
  PW_TEST_EXPECT_LE(ThreadPriority::Highest(), ThreadPriority::Highest());

  PW_TEST_EXPECT_GT(ThreadPriority::Highest(), ThreadPriority::Lowest());
  PW_TEST_EXPECT_GT(ThreadPriority::VeryLow(), ThreadPriority::Lowest());
  PW_TEST_EXPECT_GT(ThreadPriority::Low(), ThreadPriority::VeryLow());
  PW_TEST_EXPECT_GT(ThreadPriority::MediumLow(), ThreadPriority::Low());
  PW_TEST_EXPECT_GT(ThreadPriority::Medium(), ThreadPriority::MediumLow());
  PW_TEST_EXPECT_GT(ThreadPriority::MediumHigh(), ThreadPriority::Medium());
  PW_TEST_EXPECT_GT(ThreadPriority::High(), ThreadPriority::MediumHigh());
  PW_TEST_EXPECT_GT(ThreadPriority::VeryHigh(), ThreadPriority::High());
  PW_TEST_EXPECT_GT(ThreadPriority::Highest(), ThreadPriority::VeryHigh());

  PW_TEST_EXPECT_GE(ThreadPriority::Highest(), ThreadPriority::Lowest());
  PW_TEST_EXPECT_GE(ThreadPriority::Lowest(), ThreadPriority::Lowest());
  PW_TEST_EXPECT_GE(ThreadPriority::VeryLow(), ThreadPriority::Lowest());
  PW_TEST_EXPECT_GE(ThreadPriority::VeryLow(), ThreadPriority::VeryLow());
  PW_TEST_EXPECT_GE(ThreadPriority::Low(), ThreadPriority::VeryLow());
  PW_TEST_EXPECT_GE(ThreadPriority::Low(), ThreadPriority::Low());
  PW_TEST_EXPECT_GE(ThreadPriority::MediumLow(), ThreadPriority::Low());
  PW_TEST_EXPECT_GE(ThreadPriority::MediumLow(), ThreadPriority::MediumLow());
  PW_TEST_EXPECT_GE(ThreadPriority::Medium(), ThreadPriority::MediumLow());
  PW_TEST_EXPECT_GE(ThreadPriority::Medium(), ThreadPriority::Medium());
  PW_TEST_EXPECT_GE(ThreadPriority::MediumHigh(), ThreadPriority::Medium());
  PW_TEST_EXPECT_GE(ThreadPriority::MediumHigh(), ThreadPriority::MediumHigh());
  PW_TEST_EXPECT_GE(ThreadPriority::High(), ThreadPriority::MediumHigh());
  PW_TEST_EXPECT_GE(ThreadPriority::High(), ThreadPriority::High());
  PW_TEST_EXPECT_GE(ThreadPriority::VeryHigh(), ThreadPriority::High());
  PW_TEST_EXPECT_GE(ThreadPriority::VeryHigh(), ThreadPriority::VeryHigh());
  PW_TEST_EXPECT_GE(ThreadPriority::Highest(), ThreadPriority::VeryHigh());
  PW_TEST_EXPECT_GE(ThreadPriority::Highest(), ThreadPriority::Highest());
});

PW_CONSTEXPR_TEST(ThreadPriority, LowIsLowEnum, {
  enum LowIsLowPriority {
    kLowest = 99,
    kLow = 100,
    kMedium = 101,
    kHigh = 102,
    kVeryHigh = 103,
    kHighest = 104,
  };

  using ThreadPriority = Priority<LowIsLowPriority, kLowest, kHighest, kMedium>;

  PW_TEST_EXPECT_EQ(ThreadPriority::Lowest().native(), kLowest);
  PW_TEST_EXPECT_EQ(ThreadPriority::VeryLow().native(), kLow);
  PW_TEST_EXPECT_EQ(ThreadPriority::Low().native(), kLow);
  PW_TEST_EXPECT_EQ(ThreadPriority::MediumLow().native(), kMedium);
  PW_TEST_EXPECT_EQ(ThreadPriority::Medium().native(), kHigh);
  PW_TEST_EXPECT_EQ(ThreadPriority::MediumHigh().native(), kHigh);
  PW_TEST_EXPECT_EQ(ThreadPriority::High().native(), kVeryHigh);
  PW_TEST_EXPECT_EQ(ThreadPriority::VeryHigh().native(), kVeryHigh);
  PW_TEST_EXPECT_EQ(ThreadPriority::Highest().native(), kHighest);

  for (int i = kLowest; i <= kHighest; ++i) {
    PW_TEST_EXPECT_EQ(
        ThreadPriority::FromNative(static_cast<LowIsLowPriority>(i)).native(),
        static_cast<LowIsLowPriority>(i));
  }
});

PW_CONSTEXPR_TEST(ThreadPriority, NextHigher, {
  using ThreadPriority = Priority<int, -1, 1, 0>;
  PW_TEST_EXPECT_EQ(ThreadPriority::Lowest().native(), -1);
  PW_TEST_EXPECT_EQ(ThreadPriority::Lowest().NextHigher().native(), 0);
  PW_TEST_EXPECT_EQ(ThreadPriority::Lowest().NextHigher().NextHigher().native(),
                    1);

  PW_TEST_EXPECT_EQ(ThreadPriority::Medium().NextLower().NextHigher(),
                    ThreadPriority::Medium());
});

PW_CONSTEXPR_TEST(ThreadPriority, NextLower, {
  using ThreadPriority = Priority<int, -1, 1, 0>;
  PW_TEST_EXPECT_EQ(ThreadPriority::Highest().native(), 1);
  PW_TEST_EXPECT_EQ(ThreadPriority::Highest().NextLower().native(), 0);
  PW_TEST_EXPECT_EQ(ThreadPriority::Highest().NextLower().NextLower().native(),
                    -1);
});

#if PW_NC_TEST(NextHigherTooLarge)
PW_NC_EXPECT("Priority cannot exceed the maximum value");
[[maybe_unused]] constexpr auto TooLarge =
    Priority<int, -1, 1, 0>::Highest().NextHigher();
#elif PW_NC_TEST(NextLowerTooLow)
PW_NC_EXPECT("Priority cannot subceed the minimum value");
[[maybe_unused]] constexpr auto TooLarge =
    Priority<int, -1, 1, 0>::Lowest().NextLower();
#endif  // PW_NC_TEST

PW_CONSTEXPR_TEST(ThreadPriority, NextHigherClamped, {
  using ThreadPriority = Priority<int, 0, 100, 0>;
  PW_TEST_EXPECT_EQ(ThreadPriority::Highest().NextHigherClamped(),
                    ThreadPriority::Highest());
  PW_TEST_EXPECT_EQ(
      ThreadPriority::Medium().NextHigherClamped(ThreadPriority::Medium()),
      ThreadPriority::Medium());
  PW_TEST_EXPECT_EQ(ThreadPriority::Medium().NextHigherClamped(),
                    ThreadPriority::Medium().NextHigher());
});

PW_CONSTEXPR_TEST(ThreadPriority, NextLowerClamped, {
  using ThreadPriority = Priority<int, 0, 100, 0>;
  PW_TEST_EXPECT_EQ(ThreadPriority::Lowest().NextLowerClamped(),
                    ThreadPriority::Lowest());
  PW_TEST_EXPECT_EQ(
      ThreadPriority::Medium().NextLowerClamped(ThreadPriority::Medium()),
      ThreadPriority::Medium());
  PW_TEST_EXPECT_EQ(ThreadPriority::Medium().NextLowerClamped(),
                    ThreadPriority::Medium().NextLower());
});

PW_CONSTEXPR_TEST(ThreadPriority, LargeUnsigned, {
  using P = Priority<uint64_t, 0, 100, 50>;
  PW_TEST_EXPECT_TRUE(P::IsSupported());
  PW_TEST_EXPECT_EQ(P::Lowest().native(), 0u);
  PW_TEST_EXPECT_EQ(P::Highest().native(), 100u);
  PW_TEST_EXPECT_GT(P::Highest(), P::Lowest());
  PW_TEST_EXPECT_EQ(P::Highest().NextLowerClamped(), P::Highest().NextLower());
  PW_TEST_EXPECT_EQ(P::Highest().NextHigherClamped(), P::Highest());
  PW_TEST_EXPECT_EQ(P::Lowest().NextLowerClamped(), P::Lowest());
  PW_TEST_EXPECT_EQ(P::FromNative(42u).native(), 42u);
});

PW_CONSTEXPR_TEST(ThreadPriority, LargeSigned, {
  using P = Priority<int64_t, -50, 50, 0>;
  PW_TEST_EXPECT_TRUE(P::IsSupported());
  PW_TEST_EXPECT_EQ(P::Lowest().native(), -50);
  PW_TEST_EXPECT_EQ(P::Highest().native(), 50);
  PW_TEST_EXPECT_GT(P::Highest(), P::Lowest());
  PW_TEST_EXPECT_EQ(P::Highest().NextLowerClamped(), P::Highest().NextLower());
  PW_TEST_EXPECT_EQ(P::Highest().NextHigherClamped(), P::Highest());
  PW_TEST_EXPECT_EQ(P::Lowest().NextLowerClamped(), P::Lowest());
  PW_TEST_EXPECT_EQ(P::FromNative(42).native(), 42);
});

}  // namespace
