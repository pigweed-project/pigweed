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

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "pw_assert/assert.h"
#include "pw_containers/algorithm.h"
#include "pw_span/span.h"

namespace pw::containers {
namespace internal {

// Span that is known to be aligned as ValueType.
template <typename ValueType>
class Aligned {
 public:
  // Construct from an UNALIGNED span. Adjusts alignment if necessary.
  static constexpr Aligned Align(span<std::byte> unaligned_buffer) {
    Aligned buffer(unaligned_buffer.data(), unaligned_buffer.size());

    if constexpr (alignof(ValueType) > alignof(std::byte)) {
      void* data_void = buffer.data_;
      buffer.data_ = static_cast<std::byte*>(std::align(
          alignof(ValueType), sizeof(ValueType), data_void, buffer.size_));
      if (buffer.data_ == nullptr) {
        buffer.size_ = 0;  // cannot fit any items
      }
    }

    return buffer;
  }

  // Asserts that a buffer is aligned at least as ValueType.
  static constexpr Aligned Assert(std::byte* data, size_t size) {
    void* buffer_start = data;
    // Avoid "%" in the PW_ASSERT condition being misinterpreted as a format
    // specifier: https://pigweed.dev/pw_assert/#in-conditions
    uintptr_t misalignment =
        reinterpret_cast<uintptr_t>(buffer_start) % alignof(ValueType);
    PW_ASSERT(misalignment == 0);
    return Aligned(data, size);
  }

  constexpr Aligned(std::byte* aligned_data, size_t size_bytes)
      : data_(aligned_data), size_(size_bytes) {}

  constexpr std::byte* data() const { return data_; }
  constexpr size_t capacity() const { return size_ / sizeof(ValueType); }

 private:
  std::byte* data_;
  size_t size_;
};

}  // namespace internal

/// @submodule{pw_containers,utilities}

/// Declares aligned storage as a `std::byte` array.
///
/// `Storage` provides aligned external storage for containers such as
/// `pw::Deque`, avoiding the need for alignment checks.
///
/// Use `StorageFor` to declare `Storage` for objects of a particular type.
///
/// @note `sizeof(Storage)` may be larger than `kSizeBytes` due to padding for
/// alignment.
///
/// @tparam kAlignment How to align the storage; must be valid for use as
///     `alignas(kAlignment)`.
/// @tparam kSizeBytes Storage size.
template <size_t kAlignment, size_t kSizeBytes>
class Storage final {
 public:
  using value_type = std::byte;
  using size_type = size_t;
  using pointer = value_type*;
  using const_pointer = const value_type*;

  constexpr Storage() : buffer_{} {}

  Storage(const Storage&) = delete;
  Storage& operator=(const Storage&) = delete;

  Storage(Storage&&) = delete;
  Storage& operator=(Storage&&) = delete;

  constexpr pointer data() { return buffer_.data(); }
  constexpr const_pointer data() const { return buffer_.data(); }

  /// The size of the storage in bytes.
  constexpr size_type size() const { return buffer_.size(); }

  [[nodiscard]] constexpr bool empty() const { return buffer_.empty(); }

  constexpr void fill(std::byte value) {
    pw::fill_n(buffer_.data(), buffer_.size(), value);
  }

 private:
  alignas(kAlignment) std::array<std::byte, kSizeBytes> buffer_;
};

/// Declares `Storage` for `kCount` items of type `T` in a `std::byte`
/// array.
///
/// @tparam T The type of items to store.
/// @tparam kCount The number of items for which to reserve space.
template <typename T, size_t kCount = 1>
using StorageFor = Storage<alignof(T), sizeof(T) * kCount>;

/// Reserved capacity value for container specializations with external storage.
inline constexpr size_t kExternalStorage = static_cast<size_t>(-1);

/// `StorageBase` is intended to serve as a base class when combining a
/// non-owning container with its storage using inheritance. The storage must be
/// in a base class before the container base so that the storage outlives the
/// container using it.
template <size_t kAlignment, size_t kSizeBytes>
class StorageBase {
 protected:
  constexpr StorageBase() = default;

  Storage<kAlignment, kSizeBytes>& storage() { return storage_; }
  const Storage<kAlignment, kSizeBytes>& storage() const { return storage_; }

 private:
  Storage<kAlignment, kSizeBytes> storage_;
};

/// Declares `StorageBase` for `kCount` items of type `T` in a `std::byte`
/// array.
template <typename T, size_t kCount = 1>
using StorageBaseFor = StorageBase<alignof(T), sizeof(T) * kCount>;

}  // namespace pw::containers
