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
#include <type_traits>

#include "pw_allocator/hardening.h"
#include "pw_result/result.h"

namespace pw {

// Helper variables to determine when a template parameter is an array type.
// Based on the sample implementation found at
// https://en.cppreference.com/w/cpp/memory/unique_ptr/make_unique.
template <typename>
struct is_unbounded_array : std::false_type {};

template <typename T>
struct is_unbounded_array<T[]> : std::true_type {};

template <typename T>
constexpr bool is_unbounded_array_v = is_unbounded_array<T>::value;

template <typename>
struct is_bounded_array : std::false_type {};

template <typename T, size_t kN>
struct is_bounded_array<T[kN]> : std::true_type {};

template <typename T>
constexpr bool is_bounded_array_v = is_bounded_array<T>::value;

namespace allocator {

/// @submodule{pw_allocator,core}

/// Describes the layout of a block of memory.
///
/// Layouts are passed to allocators, and consist of a (possibly padded) size
/// and a power-of-two alignment no larger than the size. Layouts can be
/// constructed for a type `T` using `Layout::Of`.
///
/// Example:
///
/// @code{.cpp}
///    struct MyStruct {
///      uint8_t field1[3];
///      uint32_t field2[3];
///    };
///    constexpr Layout layout_for_struct = Layout::Of<MyStruct>();
/// @endcode
class Layout {
 public:
  constexpr Layout() : Layout(0) {}
  constexpr explicit Layout(size_t size)
      : Layout(size, alignof(std::max_align_t)) {}
  constexpr Layout(size_t size, size_t alignment)
      : size_(size), alignment_(alignment) {}

  constexpr size_t size() const { return size_; }
  constexpr size_t alignment() const { return alignment_; }

  /// Creates a Layout for the given type.
  template <typename T, std::enable_if_t<std::is_same_v<T, void>, int> = 0>
  static constexpr Layout Of() {
    return Layout(0, 1);
  }

  template <typename T,
            std::enable_if_t<!std::is_same_v<T, void> && !std::is_array_v<T>,
                             int> = 0>
  static constexpr Layout Of() {
    return Layout(sizeof(T), alignof(T));
  }

  /// Creates a Layout for the given bounded array type, e.g. Foo[kN].
  template <typename T, std::enable_if_t<is_bounded_array_v<T>, int> = 0>
  static constexpr Layout Of() {
    return Layout(sizeof(T), alignof(std::remove_extent_t<T>));
  }

  /// Creates a Layout for the given array type, e.g. Foo[].
  template <typename T, std::enable_if_t<is_unbounded_array_v<T>, int> = 0>
  static constexpr Layout Of(size_t count);

  /// If the result is okay, returns its contained layout; otherwise, returns a
  /// default layout.
  static constexpr Layout Unwrap(const Result<Layout>& result) {
    return result.ok() ? (*result) : Layout();
  }

  /// Returns a new layout from this object increased by the given size.
  constexpr Layout Extend(size_t size) const;

  /// Returns a new layout from this object with at least the given alignment.
  constexpr Layout Align(size_t alignment) const {
    return Layout(size_, std::max(alignment, alignment_));
  }

  /// Returns a new layout from this object that is at least aligned to the
  /// given type.
  template <typename T>
  constexpr Layout AlignTo() const {
    return Align(alignof(T));
  }

 private:
  size_t size_;
  size_t alignment_;
};

inline bool operator==(const Layout& lhs, const Layout& rhs) {
  return lhs.size() == rhs.size() && lhs.alignment() == rhs.alignment();
}

inline bool operator!=(const Layout& lhs, const Layout& rhs) {
  return !(lhs == rhs);
}

/// @}

// Template and inline method implementations.

template <typename T, std::enable_if_t<is_unbounded_array_v<T>, int>>
constexpr Layout Layout::Of(size_t count) {
  using U = std::remove_extent_t<T>;
  size_t size = sizeof(U);
  Hardening::Multiply(size, count);
  return Layout(size, alignof(U));
}

constexpr Layout Layout::Extend(size_t size) const {
  Hardening::Increment(size, size_);
  return Layout(size, alignment_);
}

}  // namespace allocator
}  // namespace pw
