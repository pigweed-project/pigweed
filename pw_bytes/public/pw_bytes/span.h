// Copyright 2020 The Pigweed Authors
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
#include <memory>
#include <type_traits>

#include "pw_span/span.h"

namespace pw {

/// @submodule{pw_bytes,span}

/// Alias for a mutable `pw::span` of bytes.
using ByteSpan = span<std::byte>;

/// Alias for a read-only `pw::span` of bytes.
using ConstByteSpan = span<const std::byte>;

/// Gets a read-only `pw::ConstByteSpan` (`pw::span<const std::byte>`) view of a
/// type-erased memory buffer (`const void*` or `void*`) of a given size in
/// bytes.
///
/// This function is constrained strictly to `const void*` and `void*` so that
/// typed pointers (`T*`) cannot implicitly convert to `const void*` and
/// accidentally interpret an element count as a byte count. For typed pointers,
/// construct a `pw::span` first: `pw::as_bytes(pw::span(ptr, count))`.
template <typename VoidPtr>
ConstByteSpan AsBytes(VoidPtr data, size_t size_bytes) noexcept {
  if constexpr (std::is_same_v<VoidPtr, const void*> ||
                std::is_same_v<VoidPtr, void*>) {
    return ConstByteSpan(static_cast<const std::byte*>(data), size_bytes);
  } else {
    static_assert(
        !std::is_same_v<VoidPtr, VoidPtr>,
        "cannot pass typed pointer to 2-argument pw::AsBytes: "
        "only void* or const void* is accepted to prevent treating element "
        "counts as byte counts; construct a pw::span first: "
        "pw::as_bytes(pw::span(ptr, count))");
  }
}

/// Gets a writable `pw::ByteSpan` (`pw::span<std::byte>`) view of a mutable
/// type-erased memory buffer (`void*`) of a given size in bytes.
///
/// This function is constrained strictly to `void*` so that typed pointers
/// (`T*`) cannot implicitly convert to `void*` and accidentally interpret an
/// element count as a byte count. For typed pointers, construct a `pw::span`
/// first: `pw::as_writable_bytes(pw::span(ptr, count))`.
template <typename VoidPtr>
ByteSpan AsWritableBytes(VoidPtr data, size_t size_bytes) noexcept {
  if constexpr (std::is_same_v<VoidPtr, void*>) {
    return ByteSpan(static_cast<std::byte*>(data), size_bytes);
  } else {
    static_assert(
        !std::is_same_v<VoidPtr, VoidPtr>,
        "cannot pass typed or const pointer to 2-argument "
        "pw::AsWritableBytes: only mutable void* is accepted to prevent "
        "treating element counts as byte counts; construct a pw::span first: "
        "pw::as_writable_bytes(pw::span(ptr, count))");
  }
}

/// Gets a read-only `pw::span<const std::byte>` (`ConstByteSpan`) view of an
/// object.
///
/// This function is only available for types where it always safe to rely on
/// the underlying bytes of the object, i.e. serializable objects designed to be
/// sent over the wire. It cannot be used with, for example, types that include
/// padding bytes, since those are indeterminate and may leak information.
///
/// For types that do not meet these criteria, it is still possible to represent
/// them as bytes using `pw::as_bytes`, albeit in a manner that is unsafe for
/// serialization.
///
/// The returned span has a static extent.
template <typename T>
span<const std::byte, sizeof(T)> ObjectAsBytes(const T& obj) {
  static_assert(std::is_trivially_copyable_v<T>,
                "cannot treat object as bytes: "
                "copying bytes may result in an invalid object");
  static_assert(std::has_unique_object_representations_v<T>,
                "cannot treat object as bytes: "
                "type includes indeterminate bytes which may leak information "
                "or result in incorrect object hashing");

  auto s = pw::span<const T, 1>(std::addressof(obj), 1);
  return pw::as_bytes(s);
}

/// Gets a writable `pw::span<std::byte>` (`ByteSpan`) view of an object.
///
/// This function is only available for types where it always safe to rely on
/// the underlying bytes of the object, i.e. serializable objects designed to be
/// sent over the wire. It cannot be used with, for example, types that include
/// padding bytes, since those are indeterminate and may leak information.
///
/// For types that do not meet these criteria, it is still possible to represent
/// them as bytes using `pw::as_writable_bytes`, albeit in a manner that is
/// unsafe for serialization.
///
/// The returned span has a static extent.
template <typename T>
span<std::byte, sizeof(T)> ObjectAsWritableBytes(T& obj) {
  static_assert(std::is_trivially_copyable_v<T>,
                "cannot treat object as bytes: "
                "copying bytes may result in an invalid object");
  static_assert(std::has_unique_object_representations_v<T>,
                "cannot treat object as bytes: "
                "type includes indeterminate bytes which may leak information "
                "or result in incorrect object hashing");

  auto s = pw::span<T, 1>(std::addressof(obj), 1);
  return pw::as_writable_bytes(s);
}

/// @endsubmodule

}  // namespace pw
