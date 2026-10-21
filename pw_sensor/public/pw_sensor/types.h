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
#pragma once

#include <cstdint>

#include "pw_tokenizer/tokenize.h"

/// Sensor codegen library
namespace pw::sensor {

#define PW_SENSOR_UNIT_TYPE(_unit_name, _domain, _name_str, _symbol_str) \
  struct _unit_name {                                                    \
    [[maybe_unused]] static constexpr uint16_t kUnitName =               \
        PW_TOKENIZE_STRING_MASK(_domain, 0xFFFF, _name_str);             \
    [[maybe_unused]] static constexpr uint16_t kUnitSymbol =             \
        PW_TOKENIZE_STRING_MASK(_domain, 0xFFFF, _symbol_str);           \
    [[maybe_unused]] static constexpr uint32_t kUnitType =               \
        (static_cast<uint32_t>(kUnitName) << 16) | kUnitSymbol;          \
  }

#define PW_SENSOR_MEASUREMENT_TYPE(                                  \
    _measurement_name, _domain, _name_str, _unit_name)               \
  struct _measurement_name {                                         \
    [[maybe_unused]] static constexpr uint32_t kMeasurementName =    \
        PW_TOKENIZE_STRING_DOMAIN(_domain, _name_str);               \
    [[maybe_unused]] static constexpr uint32_t kUnitType =           \
        _unit_name::kUnitType;                                       \
    [[maybe_unused]] static constexpr uint64_t kMeasurementType =    \
        (static_cast<uint64_t>(kMeasurementName) << 32) | kUnitType; \
  }

constexpr inline uint32_t GetMeasurementNameFromType(
    uint64_t measurement_type) {
  return static_cast<uint32_t>(measurement_type >> 32);
}

constexpr inline uint32_t GetMeasurementUnitFromType(
    uint64_t measurement_type) {
  return static_cast<uint32_t>(measurement_type & 0xffffffff);
}

constexpr inline uint32_t GetMeasurementUnitNameFromType(
    uint64_t measurement_type) {
  return GetMeasurementUnitFromType(measurement_type) >> 16;
}

#define PW_SENSOR_ATTRIBUTE_TYPE(_attribute_name, _domain, _name_str) \
  struct _attribute_name {                                            \
    [[maybe_unused]] static constexpr uint32_t kAttributeType =       \
        PW_TOKENIZE_STRING_DOMAIN(_domain, _name_str);                \
  }

#define PW_SENSOR_ATTRIBUTE_INSTANCE(                                         \
    _inst_name, _measurement_name, _attribute_name, _unit_name)               \
  struct _inst_name {                                                         \
    [[maybe_unused]] static constexpr auto kMeasurementType =                 \
        _measurement_name::kMeasurementType;                                  \
    [[maybe_unused]] static constexpr auto kAttributeType =                   \
        _attribute_name::kAttributeType;                                      \
    [[maybe_unused]] static constexpr auto kUnitType = _unit_name::kUnitType; \
  }

#define PW_SENSOR_TRIGGER_TYPE(var, _domain, _name)           \
  struct var {                                                \
    [[maybe_unused]] static constexpr uint32_t kTriggerType = \
        PW_TOKENIZE_STRING_DOMAIN(_domain, _name);            \
  }

}  // namespace pw::sensor
