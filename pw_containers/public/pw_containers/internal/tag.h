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
#include <utility>

namespace pw::containers::internal {

// Trivial empty placeholder used for inactive union storage.
struct Empty {};

// Internal tag for leaving union storage uninitialized before a helper method
// constructs the active value.
struct UninitializedTag {};

// Empty tag type to indicate that an optional container element (such as in
// `pw::OptionalTuple`) is not set. Equivalent to `std::nullopt_t`. Use
// `pw::kTupleNull` to refer to a value of this type.
//
// This type is for the exclusive use of Pigweed containers.
class NullPlaceholder {
 public:
  static constexpr NullPlaceholder InternalUseOnly() noexcept { return {}; }

 private:
  constexpr NullPlaceholder() noexcept {}
};

// Trait detecting `std::in_place_t`, `std::in_place_type_t<T>`, and
// `std::in_place_index_t<kIndex>`.
template <typename T>
inline constexpr bool kIsInPlaceTag = false;

template <>
inline constexpr bool kIsInPlaceTag<std::in_place_t> = true;

template <typename T>
inline constexpr bool kIsInPlaceTag<std::in_place_type_t<T>> = true;

template <size_t kIndex>
inline constexpr bool kIsInPlaceTag<std::in_place_index_t<kIndex>> = true;

}  // namespace pw::containers::internal
