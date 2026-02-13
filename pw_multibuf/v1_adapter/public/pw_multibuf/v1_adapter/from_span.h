// Copyright 2025 The Pigweed Authors
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

#include "pw_allocator/allocator.h"
#include "pw_bytes/span.h"
#include "pw_function/function.h"
#include "pw_multibuf/v1_adapter/multibuf.h"

namespace pw::multibuf::v1_adapter {

/// @submodule{pw_multibuf,v1_adapter}

/// Creates a multibuf from an existing span and a `deleter` callback.
///
/// This function can be used as a drop-in replacement for `v1::FromSpan` while
/// migrating to using pw_multibuf/v2.
std::optional<MultiBuf> FromSpan(Allocator& metadata_allocator,
                                 ByteSpan region,
                                 Function<void(ByteSpan)>&& deleter);

/// @}

}  // namespace pw::multibuf::v1_adapter
