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

namespace a::b::c::d {

constexpr int GetComplexValue() { return 3; }

enum class ComplexEnum : int32_t {
  kNeg = -1,
  kZero = 0,
  kOne = 1,
  kTwo = kOne + 1,
  kBitwise = kOne | 0x10,
  kShift = kOne << 2,
  kFunctionCall = GetComplexValue() * 3,
};

}  // namespace a::b::c::d

namespace pw::testing {

enum class ReferencesComplex {
  kVal = static_cast<int>(::a::b::c::d::ComplexEnum::kTwo),
};

struct OuterStruct {
  enum class NestedEnum {
    kValA = 5,
    kValB = 10,
  };
};

}  // namespace pw::testing

PW_ENUM(a::b::c::d::ComplexEnum,
        kNeg,
        kZero,
        kOne,
        kTwo,
        kBitwise,
        kShift,
        kFunctionCall);
PW_ENUM(pw::testing::ReferencesComplex, kVal);
PW_ENUM(pw::testing::OuterStruct::NestedEnum, kValA, kValB);
