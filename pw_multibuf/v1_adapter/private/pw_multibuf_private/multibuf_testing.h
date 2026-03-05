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

// Test fixture that aliases v2 names to the v1 adapter type so that the v2
// unit tests can be applied to the v1 adapter.
using ConstMultiBufInstance = v1_adapter::MultiBuf;
using FlatConstMultiBufInstance = v1_adapter::MultiBuf;
using FlatMultiBufInstance = v1_adapter::MultiBuf;
using MultiBufInstance = v1_adapter::MultiBuf;
using TrackedConstMultiBufInstance = v1_adapter::MultiBuf;
using TrackedFlatConstMultiBufInstance = v1_adapter::MultiBuf;
using TrackedFlatMultiBufInstance = v1_adapter::MultiBuf;
using TrackedMultiBufInstance = v1_adapter::MultiBuf;

}  // namespace pw::multibuf::test
