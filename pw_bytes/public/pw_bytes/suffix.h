// Copyright 2023 The Pigweed Authors
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

#include "pw_polyfill/language_feature_macros.h"

namespace pw {
namespace internal {

// This function is not intended for use by end-users.
//
// This function does nothing, but attempting to call it in a constexpr
// context will result in a compilation error.
//
// If it appears in an error like the following:
//
// ```
// error: call to consteval function 'pw::operator""_b' is not a constant
// expression
// ...
// note: non-constexpr function 'ByteLiteralIsTooLarge' cannot be used in a
// constant expression
// ```
//
// Then a byte-literal with a value exceeding 255 has been written.
// For example, ``255_b`` is okay, but ``256_b`` will result in an error.
inline void ByteLiteralIsTooLarge() {}

}  // namespace internal

/// @submodule{pw_bytes,suffix}

/// Returns a ``std::byte`` when used as a ``_b`` suffix.
///
/// This is useful for writing byte literals, particularly in tests.
/// To use, add:
/// @code
/// using ::pw::operator""_b;
/// @endcode
/// and then use like ``5_b``
/// in order to create a ``std::byte`` with the contents ``5``.
///
/// This should not be used in header files, as it requires a ``using``
/// declaration that will be publicly exported at whatever level it is
/// used.
PW_CONSTEVAL std::byte operator""_b(unsigned long long value) {
  if (value > 255) {
    internal::ByteLiteralIsTooLarge();
  }
  return std::byte(value);
}

/// @}

}  // namespace pw
