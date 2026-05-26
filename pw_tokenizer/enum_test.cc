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

#include "pw_tokenizer/enum.h"

#include "pw_compilation_testing/negative_compilation.h"
#include "pw_unit_test/framework.h"

namespace this_is_a_test {
namespace {

// DOCSTAG: [pw_tokenizer-examples-enum]
enum Thing : int { kAlpha, kBravo, kCharlie };

PW_TOKENIZE_ENUM(::this_is_a_test::Thing, kAlpha, kBravo, kCharlie);
// DOCSTAG: [pw_tokenizer-examples-enum]

// DOCSTAG: [pw_tokenizer-examples-enum-custom]
enum Thing2 { kDelta, kEcho, kFoxtrot };

PW_TOKENIZE_ENUM_CUSTOM(::this_is_a_test::Thing2,
                        (kDelta, "DELTA"),
                        (kEcho, "ECHO"),
                        (kFoxtrot, "FOXTROT"));
// DOCSTAG: [pw_tokenizer-examples-enum-custom]

enum OneThing { kGolf };

PW_TOKENIZE_ENUM(::this_is_a_test::OneThing, kGolf);

enum class ScopedThing { kKilo, kLima, kMike };

PW_TOKENIZE_ENUM(::this_is_a_test::ScopedThing, kKilo, kLima, kMike);

enum class ScopedThing2 { kKilo, kLima, kMike };

PW_TOKENIZE_ENUM_CUSTOM(::this_is_a_test::ScopedThing2,
                        (kKilo, "KILO"),
                        (kLima, "LIMA"),
                        (kMike, "MIKE"));

enum NonTokenizedThing { kNovember, kOscar, kPapa };

enum NamespaceThing { kHotel, kIndia, kJuliett };

TEST(TokenizeEnums, KnownValues_1) {
  constexpr const char* log_value = PwEnumToString(kBravo);
  EXPECT_STREQ("kBravo", log_value);
}

TEST(TokenizeEnums, KnownValues_2) {
  constexpr const char* log_value =
      PwEnumToString(::this_is_a_test::ScopedThing::kLima);
  EXPECT_STREQ("kLima", log_value);
}

TEST(TokenizeEnums, KnownValues_3) {
  constexpr const char* log_value =
      PwEnumToString(::this_is_a_test::ScopedThing2::kLima);
  EXPECT_STREQ("LIMA", log_value);
}

[[maybe_unused]] void TokenizeUnknownValue() {
#if PW_NC_TEST(TokenizeUnknownValue)
  PW_NC_EXPECT("no matching function for call");

  PwEnumToString(kOscar);
#endif  // PW_NC_TEST
}

enum ManyThing { kQuebec, kRomeo, kSierra };

[[maybe_unused]] void MissAValue() {
#if PW_NC_TEST(MissAValue)
  PW_NC_EXPECT("is not allowed here");

  PW_TOKENIZE_ENUM(::this_is_a_test::ManyThing, kQuebec, kRomeo);
#endif  // PW_NC_TEST
}

TEST(TokenizeEnums, BadEnumValue) {
  EXPECT_STREQ("Unknown ::this_is_a_test::Thing value",
               PwEnumToString(static_cast<Thing>(-100)));
}

}  // namespace

[[maybe_unused]] void TokenizeInDifferentNamespace() {
#if PW_NC_TEST(TokenizeInDifferentNamespace)
  PW_NC_EXPECT("no matching function for call");

  PwEnumToString(::this_is_a_test::NamespaceThing::kHotel);
#endif  // PW_NC_TEST
}

}  // namespace this_is_a_test

namespace this_is_also_a_test {

PW_TOKENIZE_ENUM(::this_is_a_test::NamespaceThing, kHotel, kIndia, kJuliett);

}  // namespace this_is_also_a_test
