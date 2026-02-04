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

#include <cstdint>
#include <type_traits>

namespace pw::multibuf::v2 {

/// @submodule{pw_multibuf,v2}

/// Basic properties of a MultiBuf.
enum class Property : uint8_t {
  /// Indicates the data contained within the MultiBuf is read-only. Note the
  /// difference from the MultiBuf itself being `const`, which restricts changes
  /// to its structure, e.g. adding or removing layers.
  kConst = 1 << 0,

  /// Allows adding or removing layers to create different views of the
  /// underlying data. This is useful with a "bottoms-up" approach to building a
  /// high-level application view out of a series of low-level protocol packets.
  kLayerable = 1 << 1,

  /// Allows setting an `Observer` that is notified when bytes or layers are
  /// added or removed. One possible usage is as part of a flow control scheme,
  /// to update the flow control whenever a certain number of bytes are
  /// processed.
  kObservable = 1 << 2,
};

/// @}

template <Property...>
class BasicMultiBuf;

namespace internal {

class GenericMultiBuf;

/// Verifies the template parameters of a MultiBuf are in canonical order.
/// @{
template <Property>
constexpr bool PropertiesAreInOrderWithoutDuplicates() {
  return true;
}
template <Property kLhs, Property kRhs, Property... kOthers>
constexpr bool PropertiesAreInOrderWithoutDuplicates() {
  return (kLhs < kRhs) &&
         PropertiesAreInOrderWithoutDuplicates<kRhs, kOthers...>();
}
/// @}

/// Verifies the template parameters of a MultiBuf are valid.
template <Property... kProperties>
constexpr bool PropertiesAreValid() {
  if constexpr (sizeof...(kProperties) != 0) {
    static_assert(PropertiesAreInOrderWithoutDuplicates<kProperties...>(),
                  "Properties must be specified in the following order, "
                  "without duplicates: kConst, kLayerable, kObservable");
  }
  return true;
}

/// Type trait to identify MultiBuf types.
template <typename>
struct IsBasicMultiBuf : public std::false_type {};

template <Property... kProperties>
struct IsBasicMultiBuf<BasicMultiBuf<kProperties...>> : public std::true_type {
};

/// SFINAE-style type that can be used to enable methods only when a MultiBuf
/// can be converted to another MultiBuf type.
template <typename From, typename To>
using EnableIfConvertible =
    std::enable_if_t<std::is_same_v<To, GenericMultiBuf> ||
                     // Only conversion to other MultiBuf types are supported.
                     (IsBasicMultiBuf<To>::value &&
                      // Read-only data cannot be converted to mutable data.
                      (!From::is_const() || To::is_const()) &&
                      // Flat MultiBufs do not have layer-related methods.
                      (From::is_layerable() || !To::is_layerable()) &&
                      // Untracked MultiBufs do not have observer-related
                      // methods.
                      (From::is_observable() || !To::is_observable()))>;

/// Performs the same checks as EnableIfConvertible, but generates a
/// static_assert with a helpful message if any condition is not met.
template <typename From, typename To>
static constexpr void AssertIsConvertible() {
  if constexpr (!std::is_same_v<To, GenericMultiBuf>) {
    static_assert(IsBasicMultiBuf<To>::value,
                  "Only conversion to other MultiBuf types are supported.");
    static_assert(!From::is_const() || To::is_const(),
                  "Read-only data cannot be converted to mutable data.");
    static_assert(From::is_layerable() || !To::is_layerable(),
                  "Flat MultiBufs do not have layer-related methods.");
    static_assert(From::is_observable() || !To::is_observable(),
                  "Untracked MultiBufs do not have observer-related methods.");
  }
}

}  // namespace internal
}  // namespace pw::multibuf::v2
