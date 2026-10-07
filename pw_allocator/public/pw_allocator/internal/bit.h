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

#include <cstddef>

#include "lib/stdcompat/bit.h"

namespace pw::allocator::internal {

/// @submodule{pw_allocator}

/// Like cpp20:countr_zero, but returns an unsigned type.
///
/// Useful for managing the bitmaps that several allocators use to track empty
/// buckets.
template <typename T, typename U = size_t>
[[nodiscard]] constexpr U CountRZero(T t) {
  return static_cast<U>(cpp20::countr_zero(t));
}

/// Like cpp20:countl_zero, but returns an unsigned type.
///
/// Useful for managing the bitmaps that several allocators use to track empty
/// buckets.
template <typename T, typename U = size_t>
[[nodiscard]] constexpr U CountLZero(T t) {
  return static_cast<U>(cpp20::countl_zero(t));
}

/// @}

}  // namespace pw::allocator::internal
