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

// DOCSTAG: [pw_enum-examples-toml-cc]
// Generated from pw_enum/examples/enum_example/sensor.toml.
#include "pw_enum/examples/enum_example/sensor.h"
// DOCSTAG: [pw_enum-examples-toml-cc]

#include "pw_enum/to_string.h"
#include "pw_log/log.h"
#include "pw_log/tokenized_args.h"
#include "pw_unit_test/framework.h"

namespace {

using enum_example::SensorErrorCode;
using enum_example::SensorState;
using enum_example::SensorStatus;

// DOCSTAG: [pw_enum-examples-toml-cc-usage]
SensorStatus Sample(SensorState state) {
  PW_LOG_INFO("Sampling in state " ENUM_EXAMPLE_SENSOR_STATE_FMT,
              PW_LOG_ENUM(state));
  if (state != SensorState::kIdle) {
    return SensorStatus::NotReady();
  }
  return SensorStatus();
}
// DOCSTAG: [pw_enum-examples-toml-cc-usage]

TEST(TomlEnumExample, Values) {
  EXPECT_EQ(static_cast<int>(SensorState::kFault), 0x80);
  EXPECT_STREQ(pw::EnumToString(SensorState::kSampling), "SAMPLING");
}

TEST(TomlEnumExample, Status) {
  EXPECT_TRUE(Sample(SensorState::kIdle).ok());

  SensorStatus status = Sample(SensorState::kOff);
  EXPECT_TRUE(status.IsNotReady());
  EXPECT_EQ(status.code(), SensorErrorCode::kNotReady);
}

}  // namespace
