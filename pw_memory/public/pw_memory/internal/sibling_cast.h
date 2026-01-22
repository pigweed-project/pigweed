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

#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace pw::internal {

// Equivalent to C++20's std::derived_from concept.
template <typename Derived, typename Base>
inline constexpr bool kDerivedFrom =
    std::is_base_of_v<Base, Derived> &&
    std::is_convertible_v<std::add_const_t<std::add_volatile_t<Derived>>*,
                          std::add_const_t<std::add_volatile_t<Base>>*>;

// Converts a pointer or reference between two compatible sibling types: types
// that share a common base with no additional data members.
//
// Do NOT use SiblingCast in new code. It is deprecated. Classes that use
// SiblingCast will be migrated to the inheritance pattern used in pw_channel.
//
// A "sibling cast" can be accomplished with a static_cast to the base type
// followed by a static_cast to the sibling type. However, this results in
// undefined behavior since the cast from the base to the new type is not valid.
// This helper ensures that the types are actually compatible and uses
// std::launder prevent undefined behavior.
//
// This function facilitates providing different interfaces for an object
// without requiring multiple inheritance. For virtual classes, this reduces
// overhead since each virtual base would have its own vtable. For non-virtual
// classes, consider instead using multiple private bases to provide alternate
// APIs. The derived class holds all data members and returns references to its
// private bases to provide different APIs. The bases down cast to the derived
// type to access data.
template <typename Dest, typename BaseType, typename Source>
[[nodiscard]] Dest SiblingCast(Source&& source) {
  using SourceType = std::remove_pointer_t<std::remove_reference_t<Source>>;
  using DestType = std::remove_pointer_t<std::remove_reference_t<Dest>>;

  static_assert((std::is_pointer_v<Source> && std::is_pointer_v<Dest>) ||
                    std::is_lvalue_reference_v<Dest> ||
                    std::is_rvalue_reference_v<Dest>,
                "May only SiblingCast to a pointer or reference type");

  static_assert(std::is_pointer_v<Source> == std::is_pointer_v<Dest>,
                "Cannot cast between pointer and non-pointer types");

  static_assert(std::is_class_v<BaseType> && !std::is_const_v<BaseType> &&
                    !std::is_volatile_v<BaseType>,
                "BaseType must be an unqualified class type");

  static_assert(
      kDerivedFrom<SourceType, BaseType> && kDerivedFrom<DestType, BaseType>,
      "The source and destination must unambiguously derive from the base");

  static_assert(sizeof(SourceType) == sizeof(BaseType),
                "The source type cannot add any members to the base");
  static_assert(sizeof(DestType) == sizeof(BaseType),
                "The destination type cannot add any members to the base");

#ifdef __clang__
  if constexpr (std::is_pointer_v<Source>) {
    return std::launder(reinterpret_cast<Dest>(std::forward<Source>(source)));
  } else {
    Dest dest = reinterpret_cast<Dest>(std::forward<Source>(source));
    return static_cast<Dest>(*std::launder(std::addressof(dest)));
  }
#else   // Alternate implementation for GCC
  // TODO: b/322910273 - GCC 12 doesn't seem to respect std::launder, resulting
  //     in undesirable optimizations with SiblingCast. Use static_cast for now,
  //     which works as intended, though it is UB.

  // Incrementally add the destination's qualifiers and */&/&& to the base type
  // for the intermediate static_cast.
  using Base1 =
      std::conditional_t<std::is_const_v<DestType>, const BaseType, BaseType>;
  using Base2 =
      std::conditional_t<std::is_volatile_v<DestType>, volatile Base1, Base1>;
  using Base3 = std::conditional_t<std::is_pointer_v<Dest>, Base2*, Base2>;
  using Base4 =
      std::conditional_t<std::is_lvalue_reference_v<Dest>, Base3&, Base3>;
  using Base5 =
      std::conditional_t<std::is_rvalue_reference_v<Dest>, Base4&&, Base4>;

  return static_cast<Dest>(static_cast<Base5>(source));
#endif  // __clang__
}

}  // namespace pw::internal
