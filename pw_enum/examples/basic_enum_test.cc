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

#include "enum_example/basic_enum.h"

#include "enum_example/other_enum.h"
#include "pw_log/log.h"
#include "pw_log/tokenized_args.h"
#include "pw_unit_test/framework.h"

namespace my::nested::pkg {

const char* HandleEnum(MyEnum value);

}  // namespace my::nested::pkg

namespace {

TEST(BasicEnumTest, RunCode) {
  EXPECT_STREQ("ALPHA",
               my::nested::pkg::HandleEnum(my::nested::pkg::MyEnum::kAlpha));
  EXPECT_STREQ("ALIASED_BETA|BETA",
               my::nested::pkg::HandleEnum(my::nested::pkg::MyEnum::kBeta));

  constexpr auto state = my::nested::pkg::OtherEnum::kSecond;
  // DOCSTAG: [pw_enum-examples-basic-cc-log]
  PW_LOG_INFO("State " MY_NESTED_PKG_OTHER_ENUM_FMT ": received packet",
              PW_LOG_ENUM(state));
  // DOCSTAG: [pw_enum-examples-basic-cc-log]

  // DOCSTAG: [pw_enum-examples-versioned-cc-log]
  PW_LOG_INFO("State " PW_LOG_ENUM_VERSIONED_FMT() ": received packet",
              PW_LOG_ENUM_VERSIONED(state));
  // DOCSTAG: [pw_enum-examples-versioned-cc-log]
}

}  // namespace
