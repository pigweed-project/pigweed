// Copyright 2021 The Pigweed Authors
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

// Configuration macros for the unit test module.
#pragma once

// Use <stddef.h> rather than <cstddef> so bindgen can parse this header
// on baremetal targets where C++ standard library headers are not available.
#include <stddef.h>

/// @submodule{pw_unit_test,configuration}

#ifndef PW_UNIT_TEST_CONFIG_EVENT_BUFFER_SIZE
/// The size of the event buffer that the ``UnitTestThread`` contains.
/// This buffer is used to encode events.  By default this is set to
/// 128 bytes.
#define PW_UNIT_TEST_CONFIG_EVENT_BUFFER_SIZE 128
#endif  // PW_UNIT_TEST_CONFIG_EVENT_BUFFER_SIZE

#ifndef PW_UNIT_TEST_CONFIG_MEMORY_POOL_SIZE
/// The size of the memory pool to use for test fixture instances. By default
/// this is set to 16K.
#define PW_UNIT_TEST_CONFIG_MEMORY_POOL_SIZE 16384
#endif  // PW_UNIT_TEST_CONFIG_MEMORY_POOL_SIZE

#ifndef PW_UNIT_TEST_CONFIG_EXPECTATION_BUFFER_SIZE
/// Size of the buffer into which to write the string with the evaluated
/// version of the arguments. This buffer is allocated on the unit test's
/// stack, so it shouldn't be too large.
#define PW_UNIT_TEST_CONFIG_EXPECTATION_BUFFER_SIZE \
  (sizeof(void*) > 4 ? 512 : 192)
#endif  // PW_UNIT_TEST_CONFIG_EXPECTATION_BUFFER_SIZE

/// @}

namespace pw::unit_test::config {

inline constexpr size_t kEventBufferSize =
    PW_UNIT_TEST_CONFIG_EVENT_BUFFER_SIZE;
#undef PW_UNIT_TEST_CONFIG_EVENT_BUFFER_SIZE

inline constexpr size_t kMemoryPoolSize = PW_UNIT_TEST_CONFIG_MEMORY_POOL_SIZE;
#undef PW_UNIT_TEST_CONFIG_MEMORY_POOL_SIZE

inline constexpr size_t kExpectationBufferSizeBytes =
    PW_UNIT_TEST_CONFIG_EXPECTATION_BUFFER_SIZE;
#undef PW_UNIT_TEST_CONFIG_EXPECTATION_BUFFER_SIZE

}  // namespace pw::unit_test::config
