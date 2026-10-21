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

// IWYU pragma: private, include "pw_multibuf/from_span.h"

#include "pw_multibuf/config.h"               // IWYU pragma: export
#include "pw_multibuf/v1_adapter/multibuf.h"  // IWYU pragma: export

// This adapter will be removed when the migration to v2 is complete.
#if PW_MULTIBUF_INCLUDE_V1_ADAPTERS

#include "pw_multibuf/v1_adapter/from_span.h"  // IWYU pragma: export

namespace pw::multibuf {

inline std::optional<v1_adapter::MultiBuf> FromSpan(
    Allocator& metadata_allocator,
    ByteSpan region,
    Function<void(ByteSpan)>&& deleter) {
  return v1_adapter::FromSpan(metadata_allocator, region, std::move(deleter));
}

}  // namespace pw::multibuf

#endif  // PW_MULTIBUF_INCLUDE_V1_ADAPTERS
