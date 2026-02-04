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

#include "pw_multibuf/config.h"
#include "pw_multibuf/simple_allocator.h"

// Class template argument deduction for alias templates is not available in
// C++17. As a workaround, this file creates an alias template for a versioned
// base type and inherits from it to define an explicit CTAD guide.

#if PW_MULTIBUF_VERSION == 1

#include "pw_multibuf/v1/simple_allocator_for_test.h"

namespace pw::multibuf::test {

template <size_t kDataSizeBytes, size_t kMetaSizeBytes>
using SimpleAllocatorForTestBase =
    v1::test::SimpleAllocatorForTest<kDataSizeBytes, kMetaSizeBytes>;

}  // namespace pw::multibuf::test

#elif PW_MULTIBUF_VERSION == 2

#else

#error "Unsupported PW_MULTIBUF_VERSION"

#endif  // PW_MULTIBUF_VERSION

namespace pw::multibuf::test {

template <size_t kDataSizeBytes = 1024, size_t kMetaSizeBytes = kDataSizeBytes>
class SimpleAllocatorForTest
    : public SimpleAllocatorForTestBase<kDataSizeBytes, kMetaSizeBytes> {};

SimpleAllocatorForTest() -> SimpleAllocatorForTest<>;

}  // namespace pw::multibuf::test
