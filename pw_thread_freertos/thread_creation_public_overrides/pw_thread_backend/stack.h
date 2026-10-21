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

#include <algorithm>  // IWYU pragma: export
#include <cstddef>    // IWYU pragma: export

#include "FreeRTOS.h"                       // IWYU pragma: export
#include "pw_thread/stack_not_supported.h"  // IWYU pragma: export
#include "pw_thread_freertos/config.h"      // IWYU pragma: export

namespace pw::thread::freertos {

constexpr size_t BytesToWords(size_t bytes) {
  return (bytes + sizeof(StackType_t) - 1) / sizeof(StackType_t);
}

// Returns a stack size in words.
constexpr size_t StackSizeBytesToWords(size_t stack_size_bytes) {
  return std::max(BytesToWords(stack_size_bytes),
                  config::kMinimumStackSizeWords);
}

}  // namespace pw::thread::freertos

namespace pw::thread::backend {

inline constexpr size_t kDefaultStackSizeBytes =
    ::pw::thread::freertos::config::kDefaultStackSizeWords *
    sizeof(StackType_t);

template <size_t kStackSizeBytes>
using Stack =
#if PW_THREAD_FREERTOS_CONFIG_DYNAMIC_ALLOCATION_ENABLED
    ::pw::thread::StackNotSupported;
#else
    std::array<StackType_t, freertos::StackSizeBytesToWords(kStackSizeBytes)>;
#endif  // PW_THREAD_FREERTOS_CONFIG_DYNAMIC_ALLOCATION_ENABLED

}  // namespace pw::thread::backend
