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

// DOCSTAG: [pw_enum-examples-status-enum]
#include <cstdint>

#include "pw_enum/generate.h"

namespace my_driver {

enum class I2cStatusCode : uint8_t {
  kOk,
  kNack,
  kBusError,
  kTimeout,
};

}  // namespace my_driver

// Generates the my_driver::I2cStatus class. The second argument is the OK
// enumerator (its namespace is inferred from the status class).
PW_ENUM_STATUS(
    my_driver::I2cStatus, I2cStatusCode::kOk, kOk, kNack, kBusError, kTimeout);
// DOCSTAG: [pw_enum-examples-status-enum]
