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
#include <string_view>

#include "pw_bytes/span.h"
#include "pw_string/string.h"

namespace examples {

class NamedU32 final {
 public:
  NamedU32(std::string_view name, uint32_t value)
      : name_(name), value_(value) {}

  std::string_view name() const { return name_; }
  uint32_t value() const { return value_; }

 private:
  pw::InlineString<16> name_;
  uint32_t value_;
};

}  // namespace examples
