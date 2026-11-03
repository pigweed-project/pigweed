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

#include <cstdint>
#include <string_view>
#include <type_traits>

#include "pw_enum/to_string.h"
#include "pw_enum/traits.h"
#include "pw_enum_private/json_base_enum.h"
#include "pw_enum_private/mixed_enum.h"
#include "pw_enum_private/toml_enums.h"
#include "pw_tokenizer/enum.h"
#include "pw_unit_test/constexpr.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::EnumToString;
using ::pw::EnumTraits;
using ::pw::enum_test::JsonBaseEnum;
using ::pw::enum_test::MixedEnum;
using ::pw::enum_test::toml::TomlCode;
using ::pw::enum_test::toml::TomlEnum;
using ::pw::enum_test::toml::TomlStatus;

static_assert(std::is_same_v<std::underlying_type_t<TomlEnum>, int16_t>);
static_assert(std::is_same_v<std::underlying_type_t<TomlCode>, uint8_t>);
static_assert(std::is_same_v<std::underlying_type_t<JsonBaseEnum>, uint8_t>);

PW_CONSTEXPR_TEST(PwEnumJsonTest, Values, {
  PW_TEST_EXPECT_EQ(static_cast<int>(TomlEnum::kNegative), -2);
  PW_TEST_EXPECT_EQ(static_cast<int>(TomlEnum::kFirst), 0);
  PW_TEST_EXPECT_EQ(static_cast<int>(TomlEnum::kSecond), 1);
  PW_TEST_EXPECT_EQ(TomlEnum::kAliasSecond, TomlEnum::kSecond);
  PW_TEST_EXPECT_EQ(static_cast<int>(TomlEnum::kFromBase), 11);
  PW_TEST_EXPECT_EQ(static_cast<int>(TomlEnum::kCustomName), 12);
  PW_TEST_EXPECT_EQ(static_cast<int>(TomlCode::kBroken), 16);
});

PW_CONSTEXPR_TEST(PwEnumJsonTest, ToString, {
  PW_TEST_EXPECT_STREQ(EnumToString(TomlEnum::kNegative), "NEGATIVE");
  PW_TEST_EXPECT_STREQ(EnumToString(TomlEnum::kAliasSecond),
                       "ALIAS_SECOND|SECOND");
  PW_TEST_EXPECT_STREQ(EnumToString(TomlEnum::kCustomName), "custom_name");
  PW_TEST_EXPECT_STREQ(EnumToString(JsonBaseEnum::kTen), "TEN");
});

PW_CONSTEXPR_TEST(PwEnumJsonTest, Traits, {
  using Traits = EnumTraits<TomlEnum>;
  PW_TEST_EXPECT_EQ(Traits::kName, std::string_view("TomlEnum"));
  PW_TEST_EXPECT_EQ(Traits::kFullyQualifiedName,
                    std::string_view("::pw::enum_test::toml::TomlEnum"));
  PW_TEST_EXPECT_EQ(Traits::kTokenDomain,
                    std::string_view(PW_ENUM_TEST_TOML_TOML_ENUM_DOMAIN));
  PW_TEST_EXPECT_EQ(Traits::kDistinctValueCount, 5u);
  PW_TEST_EXPECT_FALSE(Traits::kIsContiguous);
  PW_TEST_EXPECT_EQ(Traits::kMin, TomlEnum::kNegative);
  PW_TEST_EXPECT_EQ(Traits::kMax, TomlEnum::kCustomName);
  PW_TEST_EXPECT_TRUE(Traits::IsValid(-2));
  PW_TEST_EXPECT_FALSE(Traits::IsValid(-1));
  PW_TEST_EXPECT_TRUE(Traits::IsValid(TomlEnum::kFromBase));
});

PW_CONSTEXPR_TEST(PwEnumJsonTest, TokenDomainIsVersioned, {
  constexpr std::string_view domain = EnumTraits<TomlEnum>::kTokenDomain;
  PW_TEST_EXPECT_EQ(domain.find("::pw::enum_test::toml::_pw_enum_"), 0u);
  PW_TEST_EXPECT_NE(pw::tokenizer::EnumDomainToken<TomlEnum>(),
                    pw::tokenizer::EnumDomainToken<TomlCode>());
});

PW_CONSTEXPR_TEST(PwEnumJsonTest, Status, {
  constexpr TomlStatus ok;
  PW_TEST_EXPECT_TRUE(ok.ok());

  constexpr TomlStatus busy = TomlStatus::Busy();
  PW_TEST_EXPECT_FALSE(busy.ok());
  PW_TEST_EXPECT_TRUE(busy.IsBusy());
  PW_TEST_EXPECT_FALSE(busy.IsBroken());
  PW_TEST_EXPECT_EQ(busy.code(), TomlCode::kBusy);
  PW_TEST_EXPECT_STREQ(busy.str(), "BUSY");
});

PW_CONSTEXPR_TEST(PwEnumJsonTest, HeaderEnumReferencesJsonEnum, {
  PW_TEST_EXPECT_EQ(static_cast<int>(MixedEnum::kFromJson), 10);
  PW_TEST_EXPECT_EQ(static_cast<int>(MixedEnum::kNext), 11);
  PW_TEST_EXPECT_STREQ(EnumToString(MixedEnum::kNext), "NEXT");
});

}  // namespace
