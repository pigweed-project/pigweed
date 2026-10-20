// Copyright 2026 The Pigweed Authors
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

#include "pw_numeric/absolute_value.h"

#include <cstdint>
#include <limits>
#include <type_traits>

#include "pw_unit_test/constexpr.h"
#include "pw_unit_test/framework.h"

namespace {

/// Tests that a given `op` produces absolute values.
///
/// @tparam InType  Type of valuesto pass to `op`.
/// @tparam OutType Type of values produced by `op`.
/// @tparam Op      Function type like `pw::Function<OutType(InType)>`. A
///                 template parameter is used as `pw::Function` does not have a
///                 `constexpr` constructor.
template <typename InType, typename OutType, typename Op>
constexpr void TestOne(Op op) {
  PW_TEST_EXPECT_EQ(op(InType(0)), OutType(0));
  PW_TEST_EXPECT_EQ(op(InType(1)), OutType(1));
  PW_TEST_EXPECT_EQ(op(InType(42)), OutType(42));
  PW_TEST_EXPECT_EQ(op(std::numeric_limits<InType>::max()),
                    static_cast<OutType>(std::numeric_limits<InType>::max()));

  PW_TEST_EXPECT_EQ(op(static_cast<InType>(-1)), static_cast<OutType>(1));
  PW_TEST_EXPECT_EQ(op(InType(-42)), OutType(42));
  PW_TEST_EXPECT_EQ(
      op(static_cast<InType>(-std::numeric_limits<InType>::max())),
      static_cast<OutType>(std::numeric_limits<InType>::max()));
}

template <typename... Ts>
constexpr void TestAbs() {
  (TestOne<Ts, Ts>([](Ts t) { return pw::abs(t); }), ...);
}

PW_CONSTEXPR_TEST(AbsoluteValueTest, Abs, {
  TestAbs<int8_t,
          int16_t,
          int32_t,
          int64_t,
          short,
          int,
          long,
          long long,
          float,
          double,
          long double>();

  // `char` is unsigned on ARM.
  if constexpr (std::is_signed_v<char>) {
    TestAbs<char>();
  }
});

template <typename... Ts>
constexpr void TestLabs() {
  (
      [] {
        if constexpr (sizeof(Ts) <= sizeof(long)) {
          TestOne<Ts, long>([](Ts t) { return pw::labs(t); });
        }
      }(),
      ...);
}

PW_CONSTEXPR_TEST(AbsoluteValueTest, Labs, {
  TestLabs<int8_t, int16_t, int32_t, short, int, long, int64_t>();
  // `char` is unsigned on ARM.
  if constexpr (std::is_signed_v<char>) {
    TestLabs<char>();
  }
});

template <typename... Ts>
constexpr void TestLlabs() {
  (TestOne<Ts, long long>([](Ts t) { return pw::llabs(t); }), ...);
}

PW_CONSTEXPR_TEST(AbsoluteValueTest, Llabs, {
  TestLlabs<int8_t, int16_t, int32_t, int64_t, short, int, long, long long>();
  // `char` is unsigned on ARM.
  if constexpr (std::is_signed_v<char>) {
    TestLlabs<char>();
  }
});

template <typename... Ts>
constexpr void TestIMaxAbs() {
  (TestOne<Ts, std::intmax_t>([](Ts t) { return pw::imaxabs(t); }), ...);
}

PW_CONSTEXPR_TEST(AbsoluteValueTest, IMaxAbs, {
  TestIMaxAbs<int8_t, int16_t, int32_t, int64_t, short, int, long, long long>();
  // `char` is unsigned on ARM.
  if constexpr (std::is_signed_v<char>) {
    TestIMaxAbs<char>();
  }
});

template <typename... Ts>
constexpr void TestFabsFloatingPoint() {
  (TestOne<Ts, Ts>([](Ts t) { return pw::fabs(t); }), ...);
}

template <typename... Ts>
constexpr void TestFabsIntegral() {
  (TestOne<Ts, double>([](Ts t) { return pw::fabs(t); }), ...);
}

PW_CONSTEXPR_TEST(AbsoluteValueTest, Fabs, {
  TestFabsFloatingPoint<float, double, long double>();
  TestFabsIntegral<int8_t,
                   int16_t,
                   int32_t,
                   int64_t,
                   short,
                   int,
                   long,
                   long long>();
  // `char` is unsigned on ARM.
  if constexpr (std::is_signed_v<char>) {
    TestFabsIntegral<char>();
  }
});

template <typename... Ts>
constexpr void TestFabsf() {
  (TestOne<Ts, float>([](Ts t) { return pw::fabsf(t); }), ...);
}

PW_CONSTEXPR_TEST(AbsoluteValueTest, Fabsf, { TestFabsf<float>(); });

template <typename... Ts>
constexpr void TestFabsl() {
  (TestOne<Ts, long double>([](Ts t) { return pw::fabsl(t); }), ...);
}

PW_CONSTEXPR_TEST(AbsoluteValueTest, Fabsl, {
  TestFabsl<float, double, long double>();
});

}  // namespace
