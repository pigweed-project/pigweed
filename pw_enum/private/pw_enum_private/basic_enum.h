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
#pragma once

#include <cstdint>

#include "pw_enum/generate.h"
#include "pw_enum_private/base_enum.h"

namespace pw::testing {

enum class TestEnum : uint8_t {
  kFirst = 0,
  kSecond = 1,
  kFromH = ::pw::enum_test::base::kBase,
};

enum class EnumWithComments {
  kFirst = 1,  // First value
  kSecond = 2, /* Second value */
  // Middle comment
  kThird = 3,
};

enum class EnumWithCustomStrings {
  kValueA = 1,
  kValueB = 2,
  kValueC = 3,
  kValueD = 4,
  kValueE = 5,
  kValueF = 6,
  kValueG = 7,
  kValueH = 8,
  kValueI = 9,
  kValueJ = 10,
};

enum class EnumWithCustomAliases {
  kA = 1,
  kB = 2,
  kC = 1,
  kD = 2,
  kE = 3,
  kF = 3,
};

enum class EnumWithPartialAliases {
  kA = 1,
  kB = 1,
};

}  // namespace pw::testing

PW_ENUM(pw::testing::TestEnum, kFirst, kSecond, kFromH);
PW_ENUM(pw::testing::EnumWithComments,
        /* inline comment: */ kFirst,
        // Another comment
        kSecond,
        kThird);
PW_ENUM(pw::testing::EnumWithCustomStrings,
        kValueA = "custom_a",
        kValueB,
        kValueC = "custom nested \"quotes\" here",
        kValueD = "a + b - c",
        kValueE = "spaces are cool",
        kValueF = "value_f",
        kValueG = "line\nbreak",
        kValueH = "control\x1b_character",
        kValueI = "tab\tcharacter",
        kValueJ = "emoji 🚀 character");
PW_ENUM(pw::testing::EnumWithCustomAliases,
        kA = "custom_a",
        kB,
        kC = "custom_c",
        kD,
        kE,
        kF = "custom_f");
PW_ENUM(pw::testing::EnumWithPartialAliases, kA);
