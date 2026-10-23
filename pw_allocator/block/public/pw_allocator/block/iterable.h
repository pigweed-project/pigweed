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

#include <type_traits>

#include "pw_allocator/block/contiguous.h"

namespace pw::allocator {
namespace internal {

// Trivial base class for trait support.
struct IterableBase {};

}  // namespace internal

/// @submodule{pw_allocator,block_mixins}

/// Mix-in for blocks that allows creating forward iterators over block ranges.
///
/// Block mix-ins are stateless and trivially constructible. See `BasicBlock`
/// for details on how mix-ins can be combined to implement blocks.
///
/// This mix-in requires its derived type also derive from `ContiguousBlock`.
template <typename Derived>
class IterableBlock : public internal::IterableBase {
 protected:
  constexpr explicit IterableBlock() {
    static_assert(is_contiguous_v<Derived>,
                  "Types derived from IterableBlock must also derive "
                  "from ContiguousBlock");
  }

 public:
  /// Represents an iterator that moves forward through a list of blocks.
  ///
  /// This class is not typically instantiated directly, but rather using a
  /// range-based for-loop using `Block::Range`.
  ///
  /// Allocating or freeing blocks invalidates the iterator.
  class Iterator final {
   public:
    constexpr explicit Iterator(Derived* block) : block_(block) {}
    constexpr Iterator& operator++();
    constexpr bool operator!=(const Iterator& other);
    constexpr Derived* operator*() { return block_; }

   private:
    Derived* block_;
  };

  /// Represents a range of blocks that can be iterated over.
  ///
  /// The typical usage of this class is in a range-based for-loop, e.g.
  /// @code{.cpp}
  ///   for (auto* block : Range(first, last)) { ... }
  /// @endcode
  ///
  /// Allocating or freeing blocks invalidates the range.
  class Range final {
   public:
    /// Constructs a range including `begin` and all valid following blocks.
    constexpr explicit Range(Derived* begin) : begin_(begin), end_(nullptr) {}

    /// Constructs a range of blocks from `begin` to `end`, inclusively.
    constexpr Range(Derived* begin_inclusive, Derived* end_inclusive)
        : begin_(begin_inclusive), end_(end_inclusive->Next()) {}

    constexpr Iterator& begin() { return begin_; }
    constexpr Iterator& end() { return end_; }

   private:
    Iterator begin_;
    Iterator end_;
  };
};

/// Trait type that allow interrogating a block as to whether it is forward-
/// iterable.
template <typename BlockType>
struct is_iterable : std::is_base_of<internal::IterableBase, BlockType> {};

/// Helper variable template for `is_iterable<BlockType>::value`.
template <typename BlockType>
constexpr bool is_iterable_v = is_iterable<BlockType>::value;

/// @}

// Template method implementations.

template <typename Derived>
constexpr auto IterableBlock<Derived>::Iterator::operator++() -> Iterator& {
  block_ = block_ != nullptr ? block_->Next() : nullptr;
  return *this;
}

template <typename Derived>
constexpr bool IterableBlock<Derived>::Iterator::operator!=(
    const Iterator& other) {
  return block_ != other.block_;
}

}  // namespace pw::allocator
