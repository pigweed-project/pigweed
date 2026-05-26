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

#include "pw_enum/generate.h"

namespace pw::enum_test::base {

#define VALUE 50

constexpr int value = [] { return VALUE * 2; }();

enum BaseEnum {
  kBase = value,
};

}  // namespace pw::enum_test::base

PW_ENUM(pw::enum_test::base::BaseEnum, kBase);
