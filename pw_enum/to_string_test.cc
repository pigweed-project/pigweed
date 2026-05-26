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

#include "pw_enum/to_string.h"

#include "pw_unit_test/framework.h"

namespace my_namespace {

enum class MyEnum {
  kValue1 = 1,
  kValue2 = 2,
};

constexpr const char* PwEnumToString(MyEnum value) {
  switch (value) {
    case MyEnum::kValue1:
      return "Value1";
    case MyEnum::kValue2:
      return "Value2";
  }
  return "Unknown";
}

}  // namespace my_namespace

namespace {

TEST(ToString, ManualImplementation) {
  EXPECT_STREQ(pw::EnumToString(my_namespace::MyEnum::kValue1), "Value1");
  EXPECT_STREQ(pw::EnumToString(my_namespace::MyEnum::kValue2), "Value2");
}

}  // namespace
