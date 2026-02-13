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

#include "pw_multibuf/allocator.h"
#include "pw_multibuf/config.h"
#include "pw_multibuf/multibuf.h"

#if PW_MULTIBUF_VERSION == 1

#include "pw_multibuf/v1/simple_allocator.h"

namespace pw::multibuf {

using v1::SimpleAllocator;

}  // namespace pw::multibuf

#elif PW_MULTIBUF_VERSION == 2

// This adapter will be removed when the migration to v2 is complete.
#if PW_MULTIBUF_INCLUDE_V1_ADAPTERS

#include "pw_multibuf/v1_adapter/simple_allocator.h"

namespace pw::multibuf {

using v1_adapter::SimpleAllocator;

}  // namespace pw::multibuf

#endif  // PW_MULTIBUF_INCLUDE_V1_ADAPTERS

#else

#error "Unsupported PW_MULTIBUF_VERSION"

#endif  // PW_MULTIBUF_VERSION
