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

#include "pw_multibuf/multibuf.h"

namespace pw::multibuf::test {

// Test fixture that aliases v2 names This allows the v1_adapter to provide
// different aliases and reuse the unit tests.
using ConstMultiBufInstance = v2::ConstMultiBuf::Instance;
using FlatConstMultiBufInstance = v2::FlatConstMultiBuf::Instance;
using FlatMultiBufInstance = v2::FlatMultiBuf::Instance;
using MultiBufInstance = v2::MultiBuf::Instance;
using TrackedConstMultiBufInstance = v2::TrackedConstMultiBuf::Instance;
using TrackedFlatConstMultiBufInstance = v2::TrackedFlatConstMultiBuf::Instance;
using TrackedFlatMultiBufInstance = v2::TrackedFlatMultiBuf::Instance;
using TrackedMultiBufInstance = v2::TrackedMultiBuf::Instance;

}  // namespace pw::multibuf::test
