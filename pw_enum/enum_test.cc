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

#include "pw_tokenizer/enum.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>

#include "pw_enum/to_string.h"
#include "pw_enum/traits.h"
#include "pw_enum_private/basic_enum.h"
#include "pw_enum_private/complex_enum.h"
#include "pw_enum_private/enum_with_deps.h"
#include "pw_enum_private/standalone_enum.h"
#include "pw_log/tokenized_args.h"
#include "pw_tokenizer/hash.h"
#include "pw_unit_test/constexpr.h"
#include "pw_unit_test/framework.h"

namespace {

enum class HandwrittenTestEnum : uint8_t {
  kFirst = 0,
  kSecond = 1,
  kFromH = ::pw::enum_test::base::kBase,
};

PW_CONSTEXPR_TEST(PwEnumTest, GeneratesCorrectValues, {
  PW_TEST_EXPECT_EQ(::pw::testing::TestEnum::kFirst,
                    static_cast<::pw::testing::TestEnum>(0));
  PW_TEST_EXPECT_EQ(::pw::testing::TestEnum::kSecond,
                    static_cast<::pw::testing::TestEnum>(1));
  PW_TEST_EXPECT_EQ(::pw::testing::TestEnum::kFromH,
                    static_cast<::pw::testing::TestEnum>(100));
});

PW_CONSTEXPR_TEST(PwEnumTest, CompareToHandwritten, {
  PW_TEST_EXPECT_EQ(static_cast<uint8_t>(::pw::testing::TestEnum::kFirst),
                    static_cast<uint8_t>(HandwrittenTestEnum::kFirst));
  PW_TEST_EXPECT_EQ(static_cast<uint8_t>(::pw::testing::TestEnum::kSecond),
                    static_cast<uint8_t>(HandwrittenTestEnum::kSecond));
  PW_TEST_EXPECT_EQ(static_cast<uint8_t>(::pw::testing::TestEnum::kFromH),
                    static_cast<uint8_t>(HandwrittenTestEnum::kFromH));
});

PW_CONSTEXPR_TEST(PwEnumTest, EnumWithComments, {
  PW_TEST_EXPECT_EQ(::pw::testing::EnumWithComments::kFirst,
                    static_cast<::pw::testing::EnumWithComments>(1));
  PW_TEST_EXPECT_EQ(::pw::testing::EnumWithComments::kSecond,
                    static_cast<::pw::testing::EnumWithComments>(2));
  PW_TEST_EXPECT_EQ(::pw::testing::EnumWithComments::kThird,
                    static_cast<::pw::testing::EnumWithComments>(3));
});

PW_CONSTEXPR_TEST(PwEnumTest, TokenizedEnumString, {
  constexpr const char* log_value =
      ::pw::EnumToString(::pw::testing::TestEnum::kFirst);
  PW_TEST_EXPECT_STREQ("FIRST", log_value);
});

PW_CONSTEXPR_TEST(PwEnumTest, TokenizedEnumStringUnspecified, {
  constexpr const char* log_value =
      ::pw::EnumToString(::pw::testing::TestEnum::kSecond);
  PW_TEST_EXPECT_STREQ("SECOND", log_value);
});

PW_CONSTEXPR_TEST(PwEnumTest, StandaloneEnum, {
  PW_TEST_EXPECT_EQ(::pw::enum_test::Standalone::kOne,
                    static_cast<::pw::enum_test::Standalone>(1));
  PW_TEST_EXPECT_EQ(::pw::enum_test::Standalone::kTwo,
                    static_cast<::pw::enum_test::Standalone>(2));
  PW_TEST_EXPECT_EQ(
      ::pw::tokenizer::EnumDomainToken<::pw::enum_test::Standalone>(),
      ::pw::tokenizer::Hash(PW_ENUM_TEST_STANDALONE_DOMAIN));
});

PW_CONSTEXPR_TEST(PwEnumTest, WithDepsEnum, {
  PW_TEST_EXPECT_EQ(::pw::enum_test::WithDeps::kOk,
                    static_cast<::pw::enum_test::WithDeps>(42));
});

PW_CONSTEXPR_TEST(PwEnumTest, ComplexEnums, {
  PW_TEST_EXPECT_EQ(::a::b::c::d::ComplexEnum::kNeg,
                    static_cast<::a::b::c::d::ComplexEnum>(-1));
  PW_TEST_EXPECT_EQ(::a::b::c::d::ComplexEnum::kZero,
                    static_cast<::a::b::c::d::ComplexEnum>(0));
  PW_TEST_EXPECT_EQ(::a::b::c::d::ComplexEnum::kOne,
                    static_cast<::a::b::c::d::ComplexEnum>(1));
  PW_TEST_EXPECT_EQ(::a::b::c::d::ComplexEnum::kTwo,
                    static_cast<::a::b::c::d::ComplexEnum>(2));
  PW_TEST_EXPECT_EQ(::a::b::c::d::ComplexEnum::kBitwise,
                    static_cast<::a::b::c::d::ComplexEnum>(17));
  PW_TEST_EXPECT_EQ(::a::b::c::d::ComplexEnum::kShift,
                    static_cast<::a::b::c::d::ComplexEnum>(4));
  PW_TEST_EXPECT_EQ(::a::b::c::d::ComplexEnum::kFunctionCall,
                    static_cast<::a::b::c::d::ComplexEnum>(9));

  PW_TEST_EXPECT_EQ(::pw::testing::ReferencesComplex::kVal,
                    static_cast<::pw::testing::ReferencesComplex>(2));

  PW_TEST_EXPECT_STREQ("NEG",
                       ::pw::EnumToString(::a::b::c::d::ComplexEnum::kNeg));
  PW_TEST_EXPECT_STREQ("ZERO",
                       ::pw::EnumToString(::a::b::c::d::ComplexEnum::kZero));
  PW_TEST_EXPECT_STREQ("ONE",
                       ::pw::EnumToString(::a::b::c::d::ComplexEnum::kOne));
  PW_TEST_EXPECT_STREQ("TWO",
                       ::pw::EnumToString(::a::b::c::d::ComplexEnum::kTwo));
  PW_TEST_EXPECT_STREQ("BITWISE",
                       ::pw::EnumToString(::a::b::c::d::ComplexEnum::kBitwise));
  PW_TEST_EXPECT_STREQ("SHIFT",
                       ::pw::EnumToString(::a::b::c::d::ComplexEnum::kShift));
  PW_TEST_EXPECT_STREQ(
      "FUNCTION_CALL",
      ::pw::EnumToString(::a::b::c::d::ComplexEnum::kFunctionCall));
});

PW_CONSTEXPR_TEST(PwEnumTest, NestedEnumInStruct, {
  PW_TEST_EXPECT_EQ(::pw::testing::OuterStruct::NestedEnum::kValA,
                    static_cast<::pw::testing::OuterStruct::NestedEnum>(5));
  PW_TEST_EXPECT_EQ(::pw::testing::OuterStruct::NestedEnum::kValB,
                    static_cast<::pw::testing::OuterStruct::NestedEnum>(10));

  PW_TEST_EXPECT_STREQ(
      "VAL_A",
      ::pw::EnumToString(::pw::testing::OuterStruct::NestedEnum::kValA));
  PW_TEST_EXPECT_STREQ(
      "VAL_B",
      ::pw::EnumToString(::pw::testing::OuterStruct::NestedEnum::kValB));
});

PW_CONSTEXPR_TEST(PwEnumTest, CustomStrings, {
  PW_TEST_EXPECT_EQ(::pw::testing::EnumWithCustomStrings::kValueA,
                    static_cast<::pw::testing::EnumWithCustomStrings>(1));
  PW_TEST_EXPECT_EQ(::pw::testing::EnumWithCustomStrings::kValueB,
                    static_cast<::pw::testing::EnumWithCustomStrings>(2));
  PW_TEST_EXPECT_EQ(::pw::testing::EnumWithCustomStrings::kValueC,
                    static_cast<::pw::testing::EnumWithCustomStrings>(3));
  PW_TEST_EXPECT_EQ(::pw::testing::EnumWithCustomStrings::kValueD,
                    static_cast<::pw::testing::EnumWithCustomStrings>(4));
  PW_TEST_EXPECT_EQ(::pw::testing::EnumWithCustomStrings::kValueE,
                    static_cast<::pw::testing::EnumWithCustomStrings>(5));
  PW_TEST_EXPECT_EQ(::pw::testing::EnumWithCustomStrings::kValueF,
                    static_cast<::pw::testing::EnumWithCustomStrings>(6));
  PW_TEST_EXPECT_EQ(::pw::testing::EnumWithCustomStrings::kValueG,
                    static_cast<::pw::testing::EnumWithCustomStrings>(7));
  PW_TEST_EXPECT_EQ(::pw::testing::EnumWithCustomStrings::kValueH,
                    static_cast<::pw::testing::EnumWithCustomStrings>(8));
  PW_TEST_EXPECT_EQ(::pw::testing::EnumWithCustomStrings::kValueI,
                    static_cast<::pw::testing::EnumWithCustomStrings>(9));
  PW_TEST_EXPECT_EQ(::pw::testing::EnumWithCustomStrings::kValueJ,
                    static_cast<::pw::testing::EnumWithCustomStrings>(10));

  PW_TEST_EXPECT_STREQ(
      "custom_a",
      ::pw::EnumToString(::pw::testing::EnumWithCustomStrings::kValueA));
  PW_TEST_EXPECT_STREQ(
      "VALUE_B",
      ::pw::EnumToString(::pw::testing::EnumWithCustomStrings::kValueB));
  PW_TEST_EXPECT_STREQ(
      "custom nested \"quotes\" here",
      ::pw::EnumToString(::pw::testing::EnumWithCustomStrings::kValueC));
  PW_TEST_EXPECT_STREQ(
      "a + b - c",
      ::pw::EnumToString(::pw::testing::EnumWithCustomStrings::kValueD));
  PW_TEST_EXPECT_STREQ(
      "spaces are cool",
      ::pw::EnumToString(::pw::testing::EnumWithCustomStrings::kValueE));
  PW_TEST_EXPECT_STREQ(
      "value_f",
      ::pw::EnumToString(::pw::testing::EnumWithCustomStrings::kValueF));
  PW_TEST_EXPECT_STREQ(
      "line\nbreak",
      ::pw::EnumToString(::pw::testing::EnumWithCustomStrings::kValueG));
  PW_TEST_EXPECT_STREQ(
      "control\x1b_character",
      ::pw::EnumToString(::pw::testing::EnumWithCustomStrings::kValueH));
  PW_TEST_EXPECT_STREQ(
      "tab\tcharacter",
      ::pw::EnumToString(::pw::testing::EnumWithCustomStrings::kValueI));
  PW_TEST_EXPECT_STREQ(
      "emoji 🚀 character",
      ::pw::EnumToString(::pw::testing::EnumWithCustomStrings::kValueJ));
});

PW_CONSTEXPR_TEST(PwEnumTest, CustomAliases, {
  PW_TEST_EXPECT_EQ(::pw::testing::EnumWithCustomAliases::kA,
                    static_cast<::pw::testing::EnumWithCustomAliases>(1));
  PW_TEST_EXPECT_EQ(::pw::testing::EnumWithCustomAliases::kB,
                    static_cast<::pw::testing::EnumWithCustomAliases>(2));
  PW_TEST_EXPECT_EQ(::pw::testing::EnumWithCustomAliases::kC,
                    static_cast<::pw::testing::EnumWithCustomAliases>(1));
  PW_TEST_EXPECT_EQ(::pw::testing::EnumWithCustomAliases::kD,
                    static_cast<::pw::testing::EnumWithCustomAliases>(2));
  PW_TEST_EXPECT_EQ(::pw::testing::EnumWithCustomAliases::kE,
                    static_cast<::pw::testing::EnumWithCustomAliases>(3));
  PW_TEST_EXPECT_EQ(::pw::testing::EnumWithCustomAliases::kF,
                    static_cast<::pw::testing::EnumWithCustomAliases>(3));

  PW_TEST_EXPECT_STREQ(
      "custom_a|custom_c",
      ::pw::EnumToString(::pw::testing::EnumWithCustomAliases::kA));
  PW_TEST_EXPECT_STREQ(
      "custom_a|custom_c",
      ::pw::EnumToString(::pw::testing::EnumWithCustomAliases::kC));
  PW_TEST_EXPECT_STREQ(
      "B|D", ::pw::EnumToString(::pw::testing::EnumWithCustomAliases::kB));
  PW_TEST_EXPECT_STREQ(
      "B|D", ::pw::EnumToString(::pw::testing::EnumWithCustomAliases::kD));
  PW_TEST_EXPECT_STREQ(
      "E|custom_f",
      ::pw::EnumToString(::pw::testing::EnumWithCustomAliases::kE));
  PW_TEST_EXPECT_STREQ(
      "E|custom_f",
      ::pw::EnumToString(::pw::testing::EnumWithCustomAliases::kF));
});

PW_CONSTEXPR_TEST(PwEnumTest, PartialAliases, {
  PW_TEST_EXPECT_EQ(::pw::testing::EnumWithPartialAliases::kA,
                    static_cast<::pw::testing::EnumWithPartialAliases>(1));
  PW_TEST_EXPECT_EQ(::pw::testing::EnumWithPartialAliases::kB,
                    static_cast<::pw::testing::EnumWithPartialAliases>(1));

  // kB is not registered, but it has the same value as kA (1), so it maps
  // to the same generated string "A".
  PW_TEST_EXPECT_STREQ(
      "A", ::pw::EnumToString(::pw::testing::EnumWithPartialAliases::kA));
  PW_TEST_EXPECT_STREQ(
      "A", ::pw::EnumToString(::pw::testing::EnumWithPartialAliases::kB));
});

// Aliases for the generated traits used throughout the tests below.
using TestTraits = ::pw::EnumTraits<::pw::testing::TestEnum>;
using ComplexTraits = ::pw::EnumTraits<::a::b::c::d::ComplexEnum>;
using NestedTraits = ::pw::EnumTraits<::pw::testing::OuterStruct::NestedEnum>;
using StandaloneTraits = ::pw::EnumTraits<::pw::enum_test::Standalone>;
using AliasTraits = ::pw::EnumTraits<::pw::testing::EnumWithCustomAliases>;
using SingleValueTraits = ::pw::EnumTraits<::pw::testing::ReferencesComplex>;
using BoolTraits = ::pw::EnumTraits<::pw::testing::BoolEnum>;

static_assert(std::is_same_v<TestTraits::enum_type, ::pw::testing::TestEnum>);
static_assert(std::is_same_v<TestTraits::underlying_type,
                             std::underlying_type_t<::pw::testing::TestEnum>>);
static_assert(std::is_same_v<TestTraits::underlying_type, uint8_t>);

// The underlying type is reported faithfully for signed enums and for the
// narrowest possible enum.
static_assert(
    std::is_same_v<ComplexTraits::enum_type, ::a::b::c::d::ComplexEnum>);
static_assert(std::is_same_v<ComplexTraits::underlying_type, int32_t>);
static_assert(std::is_same_v<BoolTraits::underlying_type, bool>);

PW_CONSTEXPR_TEST(EnumTraitsTest, MinAndMax, {
  PW_TEST_EXPECT_EQ(TestTraits::kMin, ::pw::testing::TestEnum::kFirst);
  PW_TEST_EXPECT_EQ(TestTraits::kMax, ::pw::testing::TestEnum::kFromH);

  PW_TEST_EXPECT_EQ(ComplexTraits::kMin, ::a::b::c::d::ComplexEnum::kNeg);
  PW_TEST_EXPECT_EQ(ComplexTraits::kMax, ::a::b::c::d::ComplexEnum::kBitwise);

  PW_TEST_EXPECT_EQ(NestedTraits::kMin,
                    ::pw::testing::OuterStruct::NestedEnum::kValA);
  PW_TEST_EXPECT_EQ(NestedTraits::kMax,
                    ::pw::testing::OuterStruct::NestedEnum::kValB);

  PW_TEST_EXPECT_EQ(StandaloneTraits::kMin, ::pw::enum_test::Standalone::kOne);
  PW_TEST_EXPECT_EQ(StandaloneTraits::kMax, ::pw::enum_test::Standalone::kTwo);
});

PW_CONSTEXPR_TEST(EnumTraitsTest, DistinctValueCountIgnoresAliases, {
  PW_TEST_EXPECT_EQ(TestTraits::kDistinctValueCount, 3u);
  PW_TEST_EXPECT_EQ(ComplexTraits::kDistinctValueCount, 7u);
  PW_TEST_EXPECT_EQ(NestedTraits::kDistinctValueCount, 2u);
  PW_TEST_EXPECT_EQ(StandaloneTraits::kDistinctValueCount, 2u);

  // kA/kC, kB/kD, and kE/kF are aliases, so only three values are distinct.
  PW_TEST_EXPECT_EQ(AliasTraits::kDistinctValueCount, 3u);
});

PW_CONSTEXPR_TEST(EnumTraitsTest, IsContiguous, {
  PW_TEST_EXPECT_FALSE(TestTraits::kIsContiguous);
  PW_TEST_EXPECT_FALSE(ComplexTraits::kIsContiguous);
  PW_TEST_EXPECT_FALSE(NestedTraits::kIsContiguous);
  PW_TEST_EXPECT_TRUE(StandaloneTraits::kIsContiguous);
  PW_TEST_EXPECT_TRUE(AliasTraits::kIsContiguous);
  PW_TEST_EXPECT_TRUE(SingleValueTraits::kIsContiguous);
});

PW_CONSTEXPR_TEST(EnumTraitsTest, Names, {
  PW_TEST_EXPECT_EQ(TestTraits::kName, "TestEnum");
  PW_TEST_EXPECT_EQ(TestTraits::kFullyQualifiedName, "::pw::testing::TestEnum");

  PW_TEST_EXPECT_EQ(ComplexTraits::kName, "ComplexEnum");
  PW_TEST_EXPECT_EQ(ComplexTraits::kFullyQualifiedName,
                    "::a::b::c::d::ComplexEnum");

  PW_TEST_EXPECT_EQ(NestedTraits::kName, "NestedEnum");
  PW_TEST_EXPECT_EQ(NestedTraits::kFullyQualifiedName,
                    "::pw::testing::OuterStruct::NestedEnum");

  PW_TEST_EXPECT_EQ(StandaloneTraits::kName, "Standalone");
  PW_TEST_EXPECT_EQ(StandaloneTraits::kFullyQualifiedName,
                    "::pw::enum_test::Standalone");
});

PW_CONSTEXPR_TEST(EnumTraitsTest, TokenDomainMatchesTokenizedDomain, {
  // The domain is versioned, so only its prefix is stable.
  PW_TEST_EXPECT_EQ(TestTraits::kTokenDomain.find("::pw::testing::_pw_enum_"),
                    0u);
  PW_TEST_EXPECT_EQ(StandaloneTraits::kTokenDomain,
                    PW_ENUM_TEST_STANDALONE_DOMAIN);
});

PW_CONSTEXPR_TEST(EnumTraitsTest, ValuesAreDistinctAndAscending, {
  PW_TEST_EXPECT_EQ(TestTraits::kValues.size(),
                    TestTraits::kDistinctValueCount);
  PW_TEST_EXPECT_EQ(TestTraits::kValues[0], ::pw::testing::TestEnum::kFirst);
  PW_TEST_EXPECT_EQ(TestTraits::kValues[1], ::pw::testing::TestEnum::kSecond);
  PW_TEST_EXPECT_EQ(TestTraits::kValues[2], ::pw::testing::TestEnum::kFromH);

  PW_TEST_EXPECT_EQ(StandaloneTraits::kValues.size(), 2u);
  PW_TEST_EXPECT_EQ(StandaloneTraits::kValues[0],
                    ::pw::enum_test::Standalone::kOne);
  PW_TEST_EXPECT_EQ(StandaloneTraits::kValues[1],
                    ::pw::enum_test::Standalone::kTwo);

  // Aliased enumerators appear exactly once.
  PW_TEST_EXPECT_EQ(AliasTraits::kValues.size(), 3u);
  PW_TEST_EXPECT_EQ(AliasTraits::kValues[0],
                    ::pw::testing::EnumWithCustomAliases::kA);
  PW_TEST_EXPECT_EQ(AliasTraits::kValues[1],
                    ::pw::testing::EnumWithCustomAliases::kB);
  PW_TEST_EXPECT_EQ(AliasTraits::kValues[2],
                    ::pw::testing::EnumWithCustomAliases::kE);

  // ComplexEnum is signed and starts negative, so it is the case most likely
  // to break if values are ever sorted as unsigned.
  PW_TEST_EXPECT_EQ(ComplexTraits::kValues.size(),
                    ComplexTraits::kDistinctValueCount);
  PW_TEST_EXPECT_EQ(ComplexTraits::kValues[0], ComplexTraits::kMin);
  PW_TEST_EXPECT_EQ(ComplexTraits::kValues[ComplexTraits::kValues.size() - 1],
                    ComplexTraits::kMax);
  for (size_t i = 1; i < ComplexTraits::kValues.size(); ++i) {
    PW_TEST_EXPECT_TRUE(
        static_cast<ComplexTraits::underlying_type>(
            ComplexTraits::kValues[i - 1]) <
        static_cast<ComplexTraits::underlying_type>(ComplexTraits::kValues[i]));
  }
});

PW_CONSTEXPR_TEST(EnumTraitsTest, IsValidEnumerator, {
  // Sparse enum; generated as a switch.
  PW_TEST_EXPECT_TRUE(TestTraits::IsValid(::pw::testing::TestEnum::kFirst));
  PW_TEST_EXPECT_TRUE(TestTraits::IsValid(::pw::testing::TestEnum::kSecond));
  PW_TEST_EXPECT_TRUE(TestTraits::IsValid(::pw::testing::TestEnum::kFromH));
  PW_TEST_EXPECT_FALSE(
      TestTraits::IsValid(static_cast<::pw::testing::TestEnum>(50)));

  // Contiguous enum; generated as a range check.
  PW_TEST_EXPECT_TRUE(
      StandaloneTraits::IsValid(::pw::enum_test::Standalone::kOne));
  PW_TEST_EXPECT_TRUE(
      StandaloneTraits::IsValid(::pw::enum_test::Standalone::kTwo));
  PW_TEST_EXPECT_FALSE(
      StandaloneTraits::IsValid(static_cast<::pw::enum_test::Standalone>(0)));
  PW_TEST_EXPECT_FALSE(
      StandaloneTraits::IsValid(static_cast<::pw::enum_test::Standalone>(3)));

  // Single-value enum; generated as an equality check.
  PW_TEST_EXPECT_TRUE(
      SingleValueTraits::IsValid(::pw::testing::ReferencesComplex::kVal));
  PW_TEST_EXPECT_FALSE(SingleValueTraits::IsValid(
      static_cast<::pw::testing::ReferencesComplex>(0)));

  // Every listed value is valid.
  for (::a::b::c::d::ComplexEnum value : ComplexTraits::kValues) {
    PW_TEST_EXPECT_TRUE(ComplexTraits::IsValid(value));
  }
});

PW_CONSTEXPR_TEST(EnumTraitsTest, IsValidInteger, {
  // TestEnum has an unsigned underlying type.
  PW_TEST_EXPECT_TRUE(TestTraits::IsValid(0));
  PW_TEST_EXPECT_TRUE(TestTraits::IsValid(1));
  PW_TEST_EXPECT_TRUE(TestTraits::IsValid(100));
  PW_TEST_EXPECT_FALSE(TestTraits::IsValid(50));
  PW_TEST_EXPECT_FALSE(TestTraits::IsValid(200));
  // Negative and out-of-range values must not wrap into the valid range.
  PW_TEST_EXPECT_FALSE(TestTraits::IsValid(-1));
  PW_TEST_EXPECT_FALSE(TestTraits::IsValid(-156));
  PW_TEST_EXPECT_FALSE(TestTraits::IsValid(356));
  PW_TEST_EXPECT_FALSE(TestTraits::IsValid(int64_t{-1}));
  PW_TEST_EXPECT_FALSE(TestTraits::IsValid(uint64_t{1} << 40));

  PW_TEST_EXPECT_TRUE(StandaloneTraits::IsValid(1));
  PW_TEST_EXPECT_TRUE(StandaloneTraits::IsValid(2));
  PW_TEST_EXPECT_FALSE(StandaloneTraits::IsValid(0));
  PW_TEST_EXPECT_FALSE(StandaloneTraits::IsValid(3));
  PW_TEST_EXPECT_FALSE(StandaloneTraits::IsValid(-1));

  // ComplexEnum has a signed underlying type and a negative enumerator.
  PW_TEST_EXPECT_TRUE(ComplexTraits::IsValid(-1));
  PW_TEST_EXPECT_TRUE(ComplexTraits::IsValid(0));
  PW_TEST_EXPECT_TRUE(ComplexTraits::IsValid(1));
  PW_TEST_EXPECT_TRUE(ComplexTraits::IsValid(2));
  PW_TEST_EXPECT_TRUE(ComplexTraits::IsValid(17));
  PW_TEST_EXPECT_TRUE(ComplexTraits::IsValid(uint8_t{17}));
  PW_TEST_EXPECT_FALSE(ComplexTraits::IsValid(-2));
  PW_TEST_EXPECT_FALSE(ComplexTraits::IsValid(3));
  PW_TEST_EXPECT_FALSE(ComplexTraits::IsValid(100));
  PW_TEST_EXPECT_FALSE(ComplexTraits::IsValid(uint64_t{1} << 40));
});

PW_CONSTEXPR_TEST(EnumTraitsTest, BoolUnderlyingType, {
  // An enum backed by `bool` gets ordinary traits.
  PW_TEST_EXPECT_EQ(BoolTraits::kDistinctValueCount, 2u);
  PW_TEST_EXPECT_TRUE(BoolTraits::kIsContiguous);
  PW_TEST_EXPECT_EQ(BoolTraits::kMin, ::pw::testing::BoolEnum::kFalse);
  PW_TEST_EXPECT_EQ(BoolTraits::kMax, ::pw::testing::BoolEnum::kTrue);

  PW_TEST_EXPECT_TRUE(BoolTraits::IsValid(::pw::testing::BoolEnum::kFalse));
  PW_TEST_EXPECT_TRUE(BoolTraits::IsValid(::pw::testing::BoolEnum::kTrue));

  // Integers are bounds checked against a `bool` [kMin, kMax] pair. This
  // exercises every CmpLess branch that can be reached with an unsigned,
  // one-bit underlying type.
  PW_TEST_EXPECT_TRUE(BoolTraits::IsValid(0));
  PW_TEST_EXPECT_TRUE(BoolTraits::IsValid(1));
  PW_TEST_EXPECT_FALSE(BoolTraits::IsValid(2));
  PW_TEST_EXPECT_FALSE(BoolTraits::IsValid(-1));
  PW_TEST_EXPECT_TRUE(BoolTraits::IsValid(1u));
  PW_TEST_EXPECT_TRUE(BoolTraits::IsValid(uint64_t{1}));
  PW_TEST_EXPECT_FALSE(BoolTraits::IsValid(uint64_t{1} << 40));
  PW_TEST_EXPECT_FALSE(BoolTraits::IsValid(int64_t{-1}));
});

PW_CONSTEXPR_TEST(EnumTraitsTest, IsValidEnumHelper, {
  // The enum type is deduced from an enumerator.
  PW_TEST_EXPECT_TRUE(::pw::IsValidEnum(::pw::enum_test::Standalone::kOne));
  PW_TEST_EXPECT_TRUE(::pw::IsValidEnum(::pw::testing::TestEnum::kFirst));
  PW_TEST_EXPECT_FALSE(
      ::pw::IsValidEnum(static_cast<::pw::testing::TestEnum>(50)));

  // The enum type is specified explicitly for integers.
  PW_TEST_EXPECT_TRUE(::pw::IsValidEnum<::pw::enum_test::Standalone>(1));
  PW_TEST_EXPECT_FALSE(::pw::IsValidEnum<::pw::enum_test::Standalone>(99));
  PW_TEST_EXPECT_TRUE(::pw::IsValidEnum<::pw::testing::TestEnum>(100));
  PW_TEST_EXPECT_FALSE(::pw::IsValidEnum<::pw::testing::TestEnum>(50));
  PW_TEST_EXPECT_FALSE(::pw::IsValidEnum<::pw::testing::TestEnum>(-1));
});

template <typename Enum, typename Arg, typename = void>
struct CanCallTraitsIsValid : std::false_type {};

template <typename Enum, typename Arg>
struct CanCallTraitsIsValid<
    Enum,
    Arg,
    std::void_t<decltype(::pw::EnumTraits<Enum>::IsValid(std::declval<Arg>()))>>
    : std::true_type {};

template <typename Enum, typename Arg, typename = void>
struct CanCallIsValidEnum : std::false_type {};

template <typename Enum, typename Arg>
struct CanCallIsValidEnum<
    Enum,
    Arg,
    std::void_t<decltype(::pw::IsValidEnum<Enum>(std::declval<Arg>()))>>
    : std::true_type {};

TEST(EnumTraitsTest, RejectsNonIntegerAndUnrelatedTypes) {
  static_assert(CanCallTraitsIsValid<::pw::testing::TestEnum,
                                     ::pw::testing::TestEnum>::value);
  static_assert(CanCallTraitsIsValid<::pw::testing::TestEnum, int>::value);
  static_assert(CanCallTraitsIsValid<::pw::testing::TestEnum, uint8_t>::value);
  static_assert(!CanCallTraitsIsValid<::pw::testing::TestEnum, bool>::value);
  static_assert(
      !CanCallTraitsIsValid<::pw::testing::TestEnum, const bool>::value);
  static_assert(!CanCallTraitsIsValid<::pw::testing::TestEnum, double>::value);
  static_assert(!CanCallTraitsIsValid<::pw::testing::TestEnum,
                                      ::pw::enum_test::Standalone>::value);
  static_assert(!CanCallTraitsIsValid<::pw::testing::TestEnum,
                                      ::pw::enum_test::base::BaseEnum>::value);

  static_assert(CanCallIsValidEnum<::pw::testing::TestEnum,
                                   ::pw::testing::TestEnum>::value);
  static_assert(CanCallIsValidEnum<::pw::testing::TestEnum, int>::value);
  static_assert(CanCallIsValidEnum<::pw::testing::TestEnum, uint8_t>::value);
  static_assert(!CanCallIsValidEnum<::pw::testing::TestEnum, bool>::value);
  static_assert(!CanCallIsValidEnum<::pw::testing::TestEnum, double>::value);
  static_assert(!CanCallIsValidEnum<::pw::testing::TestEnum,
                                    ::pw::enum_test::Standalone>::value);
  static_assert(!CanCallIsValidEnum<::pw::testing::TestEnum,
                                    ::pw::enum_test::base::BaseEnum>::value);

  // `bool` is rejected as an argument even when the enum's underlying type is
  // `bool`. Pass an enumerator instead.
  static_assert(CanCallTraitsIsValid<::pw::testing::BoolEnum, int>::value);
  static_assert(!CanCallTraitsIsValid<::pw::testing::BoolEnum, bool>::value);
  static_assert(!CanCallIsValidEnum<::pw::testing::BoolEnum, bool>::value);
}

TEST(EnumTraitsTest, HasEnumTraits) {
  static_assert(::pw::has_enum_traits_v<::pw::testing::TestEnum>);
  static_assert(::pw::has_enum_traits_v<::a::b::c::d::ComplexEnum>);
  static_assert(
      ::pw::has_enum_traits_v<::pw::testing::OuterStruct::NestedEnum>);
  static_assert(::pw::has_enum_traits_v<::pw::enum_test::Standalone>);
  static_assert(::pw::has_enum_traits_v<::pw::testing::BoolEnum>);

  static_assert(!::pw::has_enum_traits_v<HandwrittenTestEnum>);
  static_assert(!::pw::has_enum_traits_v<int>);
}

}  // namespace

// Enum used to verify that a hand-written `pw::EnumTraits` specialization is
// not accepted. It lives at namespace scope because a specialization must be
// declared in the namespace enclosing the primary template.
//
// Do not copy this pattern: specializing `pw::EnumTraits` by hand is exactly
// what this test exists to reject. Use PW_ENUM instead.
enum class UntaggedTraitsEnum { kValue = 0 };

namespace pw {

// Provides the whole documented API, but is missing the generated tag.
template <>
struct EnumTraits<UntaggedTraitsEnum> {
  using enum_type = UntaggedTraitsEnum;
  using underlying_type = std::underlying_type_t<enum_type>;

  static constexpr std::string_view kName = "UntaggedTraitsEnum";
  static constexpr std::string_view kFullyQualifiedName =
      "::UntaggedTraitsEnum";
  static constexpr std::string_view kTokenDomain = "";

  static constexpr size_t kDistinctValueCount = 1;
  static constexpr bool kIsContiguous = true;

  static constexpr enum_type kMin = UntaggedTraitsEnum::kValue;
  static constexpr enum_type kMax = UntaggedTraitsEnum::kValue;

  static constexpr std::array<enum_type, kDistinctValueCount> kValues = {{
      UntaggedTraitsEnum::kValue,
  }};

  static constexpr bool IsValid(enum_type value) { return value == kMin; }

  template <typename Integer,
            typename = std::enable_if_t<internal::kIsInteger<Integer>>>
  static constexpr bool IsValid(Integer value) {
    return internal::IsValidInteger<enum_type>(value);
  }
};

}  // namespace pw

namespace {

TEST(EnumTraitsTest, RejectsHandWrittenSpecializations) {
  // Only specializations tagged by the code generator are recognized, even if
  // they otherwise provide the entire API.
  static_assert(!::pw::has_enum_traits_v<UntaggedTraitsEnum>);
}

}  // namespace
