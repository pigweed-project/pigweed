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

#include "pw_containers/bitset.h"

#include <cstdint>
#include <type_traits>
#include <utility>

#include "pw_compilation_testing/negative_compilation.h"
#include "pw_unit_test/constexpr.h"
#include "pw_unit_test/framework.h"

namespace {

static_assert(std::is_same_v<pw::BitSet<0>::value_type, uint8_t>);
static_assert(std::is_same_v<pw::BitSet<1>::value_type, uint8_t>);
static_assert(std::is_same_v<pw::BitSet<8>::value_type, uint8_t>);
static_assert(std::is_same_v<pw::BitSet<9>::value_type, uint16_t>);
static_assert(std::is_same_v<pw::BitSet<16>::value_type, uint16_t>);
static_assert(std::is_same_v<pw::BitSet<17>::value_type, uint32_t>);
static_assert(std::is_same_v<pw::BitSet<32>::value_type, uint32_t>);
static_assert(std::is_same_v<pw::BitSet<33>::value_type, uint64_t>);
static_assert(std::is_same_v<pw::BitSet<64>::value_type, uint64_t>);

PW_CONSTEXPR_TEST(BitSet, DefaultConstructor, {
  pw::BitSet<8> bits;
  PW_TEST_EXPECT_TRUE(bits.none());
  PW_TEST_EXPECT_EQ(bits.count(), 0u);
});

PW_CONSTEXPR_TEST(BitSet, Of, {
  auto bits = pw::BitSet<5>::Of<0b10101>();
  PW_TEST_EXPECT_TRUE(bits.test<0>());
  PW_TEST_EXPECT_FALSE(bits.test<1>());
  PW_TEST_EXPECT_TRUE(bits.test<2>());
  PW_TEST_EXPECT_FALSE(bits.test<3>());
  PW_TEST_EXPECT_TRUE(bits.test<4>());
});

PW_CONSTEXPR_TEST(BitSet, InitialzieFromBools, {
  PW_TEST_EXPECT_EQ(pw::BitSet<0>::LittleEndian().to_integer(), 0u);
  PW_TEST_EXPECT_EQ(pw::BitSet<1>::LittleEndian(true).to_integer(), 1u);
  PW_TEST_EXPECT_EQ(pw::BitSet<3>::LittleEndian(true, true, false).to_integer(),
                    0b011);
  PW_TEST_EXPECT_EQ(
      pw::BitSet<6>::LittleEndian(false, true, true, true, false, false)
          .to_integer(),
      0b001110);
});

PW_CONSTEXPR_TEST(BitSet, Size, {
  pw::BitSet<0> zero;
  PW_TEST_EXPECT_EQ(zero.size(), 0u);

  pw::BitSet<1> one;
  PW_TEST_EXPECT_EQ(one.size(), 1u);

  pw::BitSet<64> sixty_four;
  PW_TEST_EXPECT_EQ(sixty_four.size(), 64u);
});

PW_CONSTEXPR_TEST(BitSet, Equality, {
  auto bits1 = pw::BitSet<4>::Of<0b1010>();
  auto bits2 = pw::BitSet<4>::Of<0b1010>();
  auto bits3 = pw::BitSet<4>::Of<0b0101>();

  PW_TEST_EXPECT_EQ(bits1, bits2);
  PW_TEST_EXPECT_NE(bits1, bits3);
});

PW_CONSTEXPR_TEST(BitSet, Test, {
  auto bits = pw::BitSet<8>::Of<0b10010001>();
  PW_TEST_EXPECT_TRUE(bits.test<0>());
  PW_TEST_EXPECT_FALSE(bits.test<1>());
  PW_TEST_EXPECT_FALSE(bits.test<2>());
  PW_TEST_EXPECT_FALSE(bits.test<3>());
  PW_TEST_EXPECT_TRUE(bits.test<4>());
  PW_TEST_EXPECT_FALSE(bits.test<5>());
  PW_TEST_EXPECT_FALSE(bits.test<6>());
  PW_TEST_EXPECT_TRUE(bits.test<7>());
});

PW_CONSTEXPR_TEST(BitSet, AllAnyNone, {
  pw::BitSet<4> all_set;
  all_set.set();
  PW_TEST_EXPECT_TRUE(all_set.all());
  PW_TEST_EXPECT_TRUE(all_set.any());
  PW_TEST_EXPECT_FALSE(all_set.none());

  pw::BitSet<4> none_set;
  none_set.reset();
  PW_TEST_EXPECT_FALSE(none_set.all());
  PW_TEST_EXPECT_FALSE(none_set.any());
  PW_TEST_EXPECT_TRUE(none_set.none());

  auto partial_set = pw::BitSet<4>::Of<0b0100>();
  PW_TEST_EXPECT_FALSE(partial_set.all());
  PW_TEST_EXPECT_TRUE(partial_set.any());
  PW_TEST_EXPECT_FALSE(partial_set.none());
});

PW_CONSTEXPR_TEST(BitSet, Count, {
  PW_TEST_EXPECT_EQ(pw::BitSet<8>().count(), 0u);
  PW_TEST_EXPECT_EQ(pw::BitSet<8>::Of<0b1>().count(), 1u);
  PW_TEST_EXPECT_EQ(pw::BitSet<8>::Of<0b10101010>().count(), 4u);
  PW_TEST_EXPECT_EQ(pw::BitSet<8>::Of<0b11111111>().count(), 8u);
});

PW_CONSTEXPR_TEST(BitSet, SetAll, {
  pw::BitSet<6> bits;
  bits.set();
  PW_TEST_EXPECT_TRUE(bits.all());
  PW_TEST_EXPECT_EQ(bits.to_integer(), 0b111111);
});

PW_CONSTEXPR_TEST(BitSet, SetIndividual, {
  pw::BitSet<8> bits;
  bits.set<1, 3, 5>();
  PW_TEST_EXPECT_EQ(bits.to_integer(), 0b00101010);
});

PW_CONSTEXPR_TEST(BitSet, ResetAll, {
  auto bits = pw::BitSet<6>::Of<0b111111>();
  bits.reset();
  PW_TEST_EXPECT_TRUE(bits.none());
  PW_TEST_EXPECT_EQ(bits.to_integer(), 0u);
});

PW_CONSTEXPR_TEST(BitSet, ResetIndividual, {
  auto bits = pw::BitSet<8>::Of<0b11111111>();
  bits.reset<0, 2, 4, 6>();
  PW_TEST_EXPECT_EQ(bits.to_integer(), 0b10101010);
});

PW_CONSTEXPR_TEST(BitSet, FlipAll, {
  auto bits = pw::BitSet<8>::Of<0b11001100>();
  bits.flip();
  PW_TEST_EXPECT_EQ(bits.to_integer(), 0b00110011);
});

PW_CONSTEXPR_TEST(BitSet, FlipIndividual, {
  auto bits = pw::BitSet<8>::Of<0b11001100>();
  bits.flip<0, 1, 2, 3>();
  PW_TEST_EXPECT_EQ(bits.to_integer(), 0b11000011);
});

PW_CONSTEXPR_TEST(BitSet, BitwiseAnd, {
  auto a = pw::BitSet<4>::Of<0b1100>();
  auto b = pw::BitSet<4>::Of<0b1010>();
  PW_TEST_EXPECT_EQ((a & b).to_integer(), 0b1000);
  a &= b;
  PW_TEST_EXPECT_EQ(a.to_integer(), 0b1000);
});

PW_CONSTEXPR_TEST(BitSet, BitwiseOr, {
  auto a = pw::BitSet<4>::Of<0b1100>();
  auto b = pw::BitSet<4>::Of<0b1010>();
  PW_TEST_EXPECT_EQ((a | b).to_integer(), 0b1110);
  a |= b;
  PW_TEST_EXPECT_EQ(a.to_integer(), 0b1110);
});

PW_CONSTEXPR_TEST(BitSet, BitwiseXor, {
  auto a = pw::BitSet<4>::Of<0b1100>();
  auto b = pw::BitSet<4>::Of<0b1010>();
  PW_TEST_EXPECT_EQ((a ^ b).to_integer(), 0b0110);
  a ^= b;
  PW_TEST_EXPECT_EQ(a.to_integer(), 0b0110);
});

PW_CONSTEXPR_TEST(BitSet, BitwiseNot, {
  auto bits = pw::BitSet<5>::Of<0b10110>();
  PW_TEST_EXPECT_EQ((~bits).to_integer(), 0b01001);
});

PW_CONSTEXPR_TEST(BitSet, LeftShift, {
  auto bits = pw::BitSet<8>::Of<0b00001101>();
  PW_TEST_EXPECT_EQ((bits << 2).to_integer(), 0b00110100);
  bits <<= 3;
  PW_TEST_EXPECT_EQ(bits.to_integer(), 0b01101000);
  // Shift out all bits.
  PW_TEST_EXPECT_EQ((bits << 8).to_integer(), 0u);
});

PW_CONSTEXPR_TEST(BitSet, RightShift, {
  auto bits = pw::BitSet<8>::Of<0b11010000>();
  PW_TEST_EXPECT_EQ((bits >> 2).to_integer(), 0b00110100);
  bits >>= 3;
  PW_TEST_EXPECT_EQ(bits.to_integer(), 0b00011010);
  // Shift out all bits.
  PW_TEST_EXPECT_EQ((bits >> 8).to_integer(), 0u);
});

PW_CONSTEXPR_TEST(BitSet, ToInteger, {
  auto bits = pw::BitSet<5>::Of<0b10101>();
  PW_TEST_EXPECT_EQ(bits.to_integer(), 0b10101);
  static_assert(std::is_same_v<decltype(bits)::value_type, uint8_t>);

  pw::BitSet<12> bits12 = pw::BitSet<12>::Of<0b111100001111>();
  PW_TEST_EXPECT_EQ(bits12.to_integer(), 0b111100001111);
  static_assert(std::is_same_v<decltype(bits12)::value_type, uint16_t>);

  pw::BitSet<32> bits32;
  bits32.set();
  PW_TEST_EXPECT_EQ(bits32.to_integer(), 0xFFFFFFFF);
  static_assert(std::is_same_v<decltype(bits32)::value_type, uint32_t>);

  pw::BitSet<64> bits64;
  bits64.set();
  PW_TEST_EXPECT_EQ(bits64.to_integer(), 0xFFFFFFFFFFFFFFFF);
  static_assert(std::is_same_v<decltype(bits64)::value_type, uint64_t>);
});

PW_CONSTEXPR_TEST(BitSet0, DefaultConstructor, {
  pw::BitSet<0> bits;
  PW_TEST_EXPECT_EQ(bits.size(), 0u);
});

PW_CONSTEXPR_TEST(BitSet0, Equality, {
  pw::BitSet<0> a, b;
  PW_TEST_EXPECT_EQ(a, b);
  PW_TEST_EXPECT_FALSE(a != b);
});

PW_CONSTEXPR_TEST(BitSet0, AllAnyNoneCount, {
  pw::BitSet<0> bits;
  PW_TEST_EXPECT_TRUE(bits.all());
  PW_TEST_EXPECT_FALSE(bits.any());
  PW_TEST_EXPECT_TRUE(bits.none());
  PW_TEST_EXPECT_EQ(bits.count(), 0u);
});

PW_CONSTEXPR_TEST(BitSet0, Modifiers, {
  pw::BitSet<0> bits;
  bits.set();
  PW_TEST_EXPECT_EQ(bits, pw::BitSet<0>());
  bits.reset();
  PW_TEST_EXPECT_EQ(bits, pw::BitSet<0>());
  bits.flip();
  PW_TEST_EXPECT_EQ(bits, pw::BitSet<0>());
});

PW_CONSTEXPR_TEST(BitSet0, BitwiseOperators, {
  pw::BitSet<0> a, b;
  PW_TEST_EXPECT_EQ(a & b, pw::BitSet<0>());
  PW_TEST_EXPECT_EQ(a | b, pw::BitSet<0>());
  PW_TEST_EXPECT_EQ(a ^ b, pw::BitSet<0>());
  PW_TEST_EXPECT_EQ(~a, pw::BitSet<0>());
  PW_TEST_EXPECT_EQ(a << 5, pw::BitSet<0>());
  PW_TEST_EXPECT_EQ(a >> 5, pw::BitSet<0>());

  a &= b;
  PW_TEST_EXPECT_EQ(a, pw::BitSet<0>());
  a |= b;
  PW_TEST_EXPECT_EQ(a, pw::BitSet<0>());
  a ^= b;
  PW_TEST_EXPECT_EQ(a, pw::BitSet<0>());
  a <<= 5;
  PW_TEST_EXPECT_EQ(a, pw::BitSet<0>());
  a >>= 5;
  PW_TEST_EXPECT_EQ(a, pw::BitSet<0>());
});

[[maybe_unused]] void OutOfRangeAccess() {
  [[maybe_unused]] pw::BitSet<10> bits;
#if PW_NC_TEST(TestOutOfRange)
  PW_NC_EXPECT("out of range");
  std::ignore = bits.test<10>();
#elif PW_NC_TEST(SetOutOfRange)
  PW_NC_EXPECT("out of range");
  bits.set<10>();
#elif PW_NC_TEST(ResetOutOfRange)
  PW_NC_EXPECT("out of range");
  bits.reset<10>();
#elif PW_NC_TEST(FlipOutOfRange)
  PW_NC_EXPECT("out of range");
  bits.flip<100>();
#endif  // PW_NC_TEST
}

[[maybe_unused]] void BoolConstructor() {
#if PW_NC_TEST(LittleEndianBoolConstructorRequiresAllBits_1)
  PW_NC_EXPECT("One bool argument must be provided for each bit");
  [[maybe_unused]] auto bits = pw::BitSet<1>::LittleEndian();
#elif PW_NC_TEST(LittleEndianBoolConstructorRequiresAllBits_TooFew)
  PW_NC_EXPECT("One bool argument must be provided for each bit");
  [[maybe_unused]] auto bits = pw::BitSet<3>::LittleEndian(true, false);
#elif PW_NC_TEST(LittleEndianBoolConstructorRequiresAllBits_TooMany)
  PW_NC_EXPECT("One bool argument must be provided for each bit");
  [[maybe_unused]] auto bits = pw::BitSet<2>::LittleEndian(true, true, true);
#endif  // PW_NC_TEST
}

[[maybe_unused]] void OfTooLarge() {
#if PW_NC_TEST(OfMustFitWithinBitSet)
  PW_NC_EXPECT("value must fit within the BitSet");
  [[maybe_unused]] auto bits = pw::BitSet<3>::Of<0b1111>();
#endif  // PW_NC_TEST
}

[[maybe_unused]] void Only64Bits() {
#if PW_NC_TEST(OnlyUpTo64Bits)
  PW_NC_EXPECT("64 bits");
  [[maybe_unused]] pw::BitSet<65> bits;
#endif  // PW_NC_TEST
}

}  // namespace
