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

#include <cstddef>
#include <type_traits>

#include "pw_assert/assert.h"

namespace pw {
namespace internal {

template <typename T>
struct PointerToMemberTraits;

template <typename T, typename C>
struct PointerToMemberTraits<T C::*> {
  using value_type = T;
  using class_type = C;
};

template <typename T>
using MemberValueType = typename PointerToMemberTraits<T>::value_type;

}  // namespace internal

/// Returns the offset of a member within a class.
///
/// This is similar to the `offsetof` macro, but works with member pointers and
/// is type-safe.
///
/// @param member A pointer to the member.
/// @return The offset of the member in bytes.
template <typename Member, typename Class>
size_t OffsetOf(Member Class::* member) {
  alignas(Class) std::byte class_memory[sizeof(Class)];
  const Class* c = reinterpret_cast<const Class*>(class_memory);
  return reinterpret_cast<size_t>(&(c->*member)) - reinterpret_cast<size_t>(c);
}

/// Returns a reference to the container of a member.
///
/// @param ref A reference to the member.
/// @param member The pointer-to-member for that member (`&Foo::member`).
/// @return A reference to the container class.
template <typename Member, typename ClassMember, typename Class>
const Class& ContainerOf(const Member& ref, ClassMember Class::* member) {
#ifdef __cpp_lib_is_pointer_interconvertible
  static_assert(
      std::is_same_v<Member, ClassMember> ||
      std::is_pointer_interconvertible_base_of_v<Member, ClassMember>);
#else
  static_assert(std::is_convertible_v<ClassMember*, Member*>);
  static_assert(std::is_same_v<Member, ClassMember> ||
                std::is_base_of_v<Member, ClassMember>);
#endif  // __cpp_lib_is_pointer_interconvertible
  return *reinterpret_cast<const Class*>(
      reinterpret_cast<const std::byte*>(&ref) - OffsetOf(member));
}

/// @copydoc ContainerOf
template <typename Member, typename ClassMember, typename Class>
Class& ContainerOf(Member& ref, ClassMember Class::* member) {
  return const_cast<Class&>(
      ContainerOf(const_cast<const Member&>(ref), member));
}

/// Returns a pointer to the container of a member.
///
/// @param ptr A pointer to the member.
/// @param member The pointer-to-member for that member (`&Foo::member`).
/// @return A pointer to the container class.
template <typename Member, typename ClassMember, typename Class>
const Class* ContainerOf(const Member* ptr, ClassMember Class::* member) {
  PW_ASSERT(ptr != nullptr);
  return &ContainerOf(*ptr, member);
}

/// @copydoc ContainerOf
template <typename Member, typename ClassMember, typename Class>
Class* ContainerOf(Member* ptr, ClassMember Class::* member) {
  PW_ASSERT(ptr != nullptr);
  return &ContainerOf(*ptr, member);
}

/// Returns a pointer to the container of a member.
///
/// @tparam kMemberPtr A pointer to the member within the class.
/// @param ptr A pointer to the member.
/// @return A pointer to the container class.
template <auto kMemberPtr,
          typename Member = internal::MemberValueType<decltype(kMemberPtr)>>
auto ContainerOf(Member* ptr) {
  return ContainerOf(ptr, kMemberPtr);
}

/// Returns a reference to the container of a member.
///
/// @tparam kMemberPtr A pointer to the member within the class.
/// @param ref A reference to the member.
/// @return A reference to the container class.
template <auto kMemberPtr,
          typename Member = internal::MemberValueType<decltype(kMemberPtr)>>
auto& ContainerOf(Member& ref) {
  return ContainerOf(ref, kMemberPtr);
}

/// Function that dereferences a pointer to a member.
///
/// This is a just wrapper around a pointer-to-member dereference, but is useful
/// to create a function that complements `ContainerOf`.
///
/// @tparam kMemberPtr A pointer to the member within the class.
/// @param ptr A pointer to the containing class.
/// @return A pointer to the member.
template <auto kMemberPtr,
          typename Class = typename internal::PointerToMemberTraits<
              decltype(kMemberPtr)>::class_type>
constexpr internal::MemberValueType<decltype(kMemberPtr)>& MemberOf(
    Class& ptr) {
  return ptr.*kMemberPtr;
}

}  // namespace pw
