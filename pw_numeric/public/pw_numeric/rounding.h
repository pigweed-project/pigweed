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

#include "pw_assert/assert.h"
#include "pw_numeric/checked_arithmetic.h"

namespace pw {

template <typename T, typename U>
constexpr T RoundUp(T value, U multiple);

/// Returns the value rounded down to the nearest multiple.
template <typename T, typename U = T>
constexpr T RoundDown(T value, U multiple) {
  PW_ASSERT(multiple > 0);
  if constexpr (std::is_signed_v<T>) {
    if (value < T{0}) {
      return static_cast<T>(-RoundUp(-value, multiple));
    }
  }
  if (multiple > std::numeric_limits<T>::max()) {
    return T{0};
  }
  T t_multiple = static_cast<T>(multiple);
  return static_cast<T>((value / t_multiple) * t_multiple);
}

/// Returns the value rounded up to the nearest multiple.
template <typename T, typename U = T>
constexpr T RoundUp(T value, U multiple) {
  PW_ASSERT(multiple > 0);
  if constexpr (std::is_signed_v<T>) {
    if (value < T{0}) {
      return static_cast<T>(-RoundDown(-value, multiple));
    }
  }
  PW_ASSERT(multiple <= std::numeric_limits<T>::max());
  T t_multiple = static_cast<T>(multiple);
  PW_ASSERT(CheckedAdd(value, t_multiple - 1, value));
  return static_cast<T>((value / t_multiple) * t_multiple);
}

}  // namespace pw
