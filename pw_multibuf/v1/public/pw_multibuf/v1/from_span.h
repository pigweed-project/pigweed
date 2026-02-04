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

#include "pw_allocator/allocator.h"
#include "pw_function/function.h"
#include "pw_multibuf/v1/multibuf.h"

namespace pw::multibuf::v1 {

/// @submodule{pw_multibuf,v1}

/// Creates a multibuf from an existing span and a ``deleter`` callback.
///
/// The provided allocator is used to allocate storage for the chunk-tracking
/// metadata. The allocator's lifetime must outlive the returned ``MultiBuf``.
///
/// Returns ``nullopt`` if the ``metadata_allocator`` fails to allocate a
/// metadata region to track the provided buffer. In this case, ``deleter``
/// will not be invoked and the caller will retain ownership of the provided
/// region.
std::optional<MultiBuf> FromSpan(pw::Allocator& metadata_allocator,
                                 ByteSpan region,
                                 pw::Function<void(ByteSpan)>&& deleter);

/// @}

}  // namespace pw::multibuf::v1
