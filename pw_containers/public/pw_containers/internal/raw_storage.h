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

#include <array>
#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>

#include "pw_containers/internal/tag.h"

namespace pw::containers::internal {

// Used as max_size in generic-sized interfaces using RawStorage<T>.
inline constexpr size_t kGenericSized = static_cast<size_t>(-1);

template <typename T>
using EnableIfIterable =
    std::enable_if_t<true, decltype(T().begin(), T().end())>;

template <typename Base,
          typename ValueType,
          size_t kCapacity,
          bool kIsTriviallyDestructible>
class RawStorageImpl;

/// Storage for a queue's data and that ensures entries are `clear`'d before
/// the storage is removed.
template <typename Base, typename ValueType, size_t kCapacity>
using RawStorage = RawStorageImpl<Base,
                                  ValueType,
                                  kCapacity,
                                  std::is_trivially_destructible_v<ValueType>>;

// Container similar to std::array that provides an array of Elements which are
// stored as uninitialized memory blocks aligned correctly for the type.
//
// The caller is responsible for constructing, accessing, and destructing
// elements. In addition, the caller is responsible for element access and all
// associated bounds checking.
template <typename Base, typename ValueType, size_t kCapacity>
class BasicRawStorage : public Base {
 public:
  using value_type = ValueType;
  using size_type = size_t;
  using difference_type = ptrdiff_t;
  using reference = value_type&;
  using const_reference = const value_type&;
  using pointer = value_type*;
  using const_pointer = const value_type*;

  static_assert(kCapacity <= std::numeric_limits<size_type>::max());

  // Construct
  constexpr BasicRawStorage() noexcept : Base(kCapacity), null_bits_() {}

  // Do not permit copying and move for now.
  BasicRawStorage(const BasicRawStorage&) = delete;
  BasicRawStorage& operator=(const BasicRawStorage&) = delete;
  BasicRawStorage(BasicRawStorage&&) = delete;
  BasicRawStorage&& operator=(BasicRawStorage&&) = delete;

  pointer data() noexcept {
    return std::launder(reinterpret_cast<pointer>(&bytes_));
  }
  const_pointer data() const noexcept {
    return std::launder(reinterpret_cast<const_pointer>(&bytes_));
  }

  constexpr size_type max_size() const noexcept { return kCapacity; }

 private:
  union {
    // Elements must initialized on demand with placement new, this array
    // provides the right amount of storage needed for the elements, however it
    // is never constructed nor destructed by the RawStorage container, as
    // null_bits_ is used instead.
    //
    // This uses std::array instead of a C array to support zero-length arrays.
    // Zero-length C arrays are non-standard, but std::array<T, 0> is valid.
    // The alignas specifier is required ensure that a zero-length array is
    // aligned the same as an array with elements.
    alignas(value_type)
        std::array<std::byte, sizeof(value_type) * kCapacity> bytes_;

    // Empty struct used when initializing the storage in the constexpr
    // constructor.
    Empty null_bits_;
  };
};

/// Specialization of `BasicRawStorage` for trivially-destructible `ValueType`.
///
/// This specialization ensures that no destructor is generated.
template <typename Base, typename ValueType, size_t kCapacity>
class RawStorageImpl<Base, ValueType, kCapacity, true>
    : public BasicRawStorage<Base, ValueType, kCapacity> {};

/// Specialization of `BasicRawStorage` for non-trivially-destructible
/// `ValueType`.
///
/// This specialization ensures that the queue is cleared during destruction.
template <typename Base, typename ValueType, size_t kCapacity>
class RawStorageImpl<Base, ValueType, kCapacity, false>
    : public BasicRawStorage<Base, ValueType, kCapacity> {
 public:
  ~RawStorageImpl() { Base::clear(); }
};

}  // namespace pw::containers::internal
