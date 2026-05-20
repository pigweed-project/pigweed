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

#include <cstddef>

#include "pw_allocator/chunk_pool.h"
#include "pw_allocator/hardening.h"
#include "pw_bytes/span.h"

namespace pw::allocator {

/// @submodule{pw_allocator,concrete}

/// Typed pool that can be used for slab allocation.
///
/// This class is a special purpose pool designed to allocate objects of one
/// specific type, ``T``. It is useful when you need a dynamic pool of objects
/// with very low performance and memory overhead costs. For example, a
/// dispatcher might use such an allocator to manage memory for a set of task
/// objects.
///
/// @tparam   T   The type of object to allocate memory for.
template <typename T>
class TypedPool : public ChunkPool {
 public:
  /// Returns the amount of memory needed to allocate ``num_objects``.
  static constexpr size_t SizeNeeded(size_t num_objects);

  /// Returns the optimal alignment for the backing memory region.
  static constexpr size_t AlignmentNeeded() {
    return std::max(alignof(T), ChunkPool::kMinAlignment);
  }

  /// Provides aligned storage for `kNumObjects` of type `T`.
  template <size_t kNumObjects>
  struct Buffer {
    static_assert(kNumObjects != 0);
    alignas(
        AlignmentNeeded()) std::array<std::byte, SizeNeeded(kNumObjects)> data;
  };

  /// Construct a ``TypedPool``.
  ///
  /// This constructor uses the `Buffer` type to minimize wasted memory. For
  /// example:
  ///
  /// @code{.cpp}
  /// TypedPool<MyObject>::Buffer<100> buffer;
  /// TypedPool<MyObject> my_pool(buffer);
  /// @endcode
  ///
  /// @param  buffer  The memory to allocate from.
  template <size_t kNumObjects>
  TypedPool(Buffer<kNumObjects>& buffer)
      : ChunkPool(buffer.data, Layout::Of<T>()) {}

  /// Construct a ``TypedPool``.
  ///
  /// To minimize wasted memory, align the buffer provided to the allocator to
  /// the object type's alignment. For example:
  ///
  /// @code{.cpp}
  /// alignas(MyObject) std::array<std::byte, 0x1000> buffer;
  /// TypedPool<MyObject> my_pool(buffer);
  /// @endcode
  ///
  /// @param  region  The memory to allocate from. Must be large enough to
  ///                 allocate memory for at least one object.
  TypedPool(ByteSpan region) : ChunkPool(region, Layout::Of<T>()) {}

  /// Constructs and object from the given `args`
  ///
  /// This method is similar to ``Allocator::New``, except that it is specific
  /// to the pool's object type.
  ///
  /// @param[in]  args        Arguments passed to the object constructor.
  template <int&... kExplicitGuard, typename... Args>
  T* New(Args&&... args);

  /// Constructs and object from the given `args`, and wraps it in a `UniquePtr`
  ///
  /// This method is similar to ``Allocator::MakeUnique``, except that it is
  /// specific to the pool's object type.
  ///
  /// @param[in]  args        Arguments passed to the object constructor.
  template <int&... kExplicitGuard, typename... Args>
  UniquePtr<T> MakeUnique(Args&&... args) {
    return UniquePtr<T>(New(std::forward<Args>(args)...), *this);
  }
};

/// @}

// Template method implementations.

template <typename T>
constexpr size_t TypedPool<T>::SizeNeeded(size_t num_objects) {
  size_t needed = std::max(sizeof(T), ChunkPool::kMinSize);
  Hardening::Multiply(needed, num_objects);
  return needed;
}

template <typename T>
template <int&... kExplicitGuard, typename... Args>
T* TypedPool<T>::New(Args&&... args) {
  void* ptr = Allocate();
  return ptr != nullptr ? new (ptr) T(std::forward<Args>(args)...) : nullptr;
}

}  // namespace pw::allocator
