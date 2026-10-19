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

#include <atomic>

#include "lib/stdcompat/bit.h"

namespace pw {

using cpp20::bit_ceil;

/// @module{pw_alignment}

/// Ensures the object is naturally aligned to a power of 2 bytes greater
/// than or equal to its size. `NaturallyAligned` is a wrapper class.
///
/// Example:
///
/// @code{.cpp}
///   #include "pw_alignment/alignment.h"
///
///   std::atomic<pw::NaturallyAligned<std::optional<bool>>> nat_aligned_obj;
/// @endcode
template <typename T>
struct [[gnu::aligned(bit_ceil(sizeof(T)))]] NaturallyAligned : public T {
  NaturallyAligned() : T() {}
  NaturallyAligned(const T& t) : T(t) {}
  template <class U>
  NaturallyAligned(const U& u) : T(u) {}
  NaturallyAligned& operator=(T other) {
    T::operator=(other);
    return *this;
  }  // namespace pw
};

/// Ensures the object held by `std::atomic` is naturally aligned. This
/// enables the compiler to replace libcalls to atomic functions with native
/// instructions when appropriate. `AlignedAtomic` is a convenience wrapper.
///
/// Example:
///
/// @code{.cpp}
///   #include "pw_alignment/alignment.h"
///
///   pw::AlignedAtomic<std::optional<bool>> mute_enable{};
/// @endcode
///
/// `std::optional<bool>` has a size of 2 but alignment of 1, which would
/// normally lower to an `__atomic_*` libcall, but `pw::NaturallyAligned` in
/// `std::atomic` tells the compiler to align the object to 2 bytes, which
/// satisfies the requirements for replacing `__atomic_*` with native
/// instructions.
template <typename T>
using AlignedAtomic = std::atomic<NaturallyAligned<T>>;

}  // namespace pw
