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

#include <string_view>

#include "pw_enum/to_string.h"
#include "pw_enum_private/basic_enum.h"
#include "pw_enum_private/complex_enum.h"
#include "pw_enum_private/enum_with_deps.h"
#include "pw_enum_private/standalone_enum.h"
#include "pw_log/tokenized_args.h"
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

}  // namespace
