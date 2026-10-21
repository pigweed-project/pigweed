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

// IWYU pragma: private, include "pw_multibuf/multibuf.h"

#include "pw_multibuf/config.h"       // IWYU pragma: export
#include "pw_multibuf/v2/multibuf.h"  // IWYU pragma: export

namespace pw {

template <multibuf::v2::Property... kProperties>
using BasicMultiBuf = multibuf::v2::BasicMultiBuf<kProperties...>;

using multibuf::v2::ConstMultiBuf;
using multibuf::v2::FlatConstMultiBuf;
using multibuf::v2::FlatMultiBuf;
using multibuf::v2::MultiBuf;
using multibuf::v2::TrackedConstMultiBuf;
using multibuf::v2::TrackedFlatConstMultiBuf;
using multibuf::v2::TrackedFlatMultiBuf;
using multibuf::v2::TrackedMultiBuf;

}  // namespace pw

// This adapter will be removed when the migration to v2 is complete.
#if PW_MULTIBUF_INCLUDE_V1_ADAPTERS

#include "pw_multibuf/v1_adapter/multibuf.h"  // IWYU pragma: export

namespace pw::multibuf {

using v1_adapter::MultiBuf;
using v1_adapter::MultiBufChunks;

}  // namespace pw::multibuf

#endif  // PW_MULTIBUF_INCLUDE_V1_ADAPTERS
