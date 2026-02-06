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

#include <type_traits>

#include "pw_multibuf/v2/internal/chunk_iterator.h"

namespace pw::multibuf::v2 {

// Forward declarations.
namespace test {
class IteratorTest;
}  // namespace test

namespace internal {

/// Helper class that allows iterating over contiguous chunks in a MultiBuf.
///
/// This allows using range-based for-loops, e.g.
///
/// @code{.cpp}
/// for (ByteSpan chunk : multibuf.Chunks()) {
///   ModifyChunk(chunk);
/// }
/// for (ConstByteSpan chunk : multibuf.ConstChunks()) {
///   ReadChunk(chunk);
/// }
/// @endcode
///
/// @warning Modifying the structure of a MultiBuf invalidates any outstanding
/// chunk iterators.
template <Mutability kMutability>
class ChunksImpl {
 private:
  using Deque = internal::Entry::Deque;

 public:
  using size_type = Deque::size_type;
  using value_type = Deque::value_type;
  using difference_type = Deque::difference_type;
  using iterator = internal::ChunkIterator<kMutability>;
  using const_iterator = internal::ChunkIterator<internal::Mutability::kConst>;

  constexpr ChunksImpl() = default;

  constexpr size_type size() const {
    return static_cast<size_type>(std::distance(begin_, end_));
  }

  constexpr iterator begin() { return begin_; }
  constexpr const_iterator begin() const { return begin_; }
  constexpr const_iterator cbegin() const { return begin_; }

  constexpr iterator end() { return end_; }
  constexpr const_iterator end() const { return end_; }
  constexpr const_iterator cend() const { return end_; }

 private:
  friend class internal::GenericMultiBuf;

  // For unit testing.
  friend class test::IteratorTest;

  constexpr ChunksImpl(const Deque& deque, size_type entries_per_chunk) {
    begin_.deque_ = &deque;
    begin_.entries_per_chunk_ = entries_per_chunk;
    end_.deque_ = &deque;
    end_.entries_per_chunk_ = entries_per_chunk;
    end_.chunk_ = deque.size() / entries_per_chunk;
  }

  iterator begin_;
  iterator end_;
};

}  // namespace internal

using Chunks = internal::ChunksImpl<internal::Mutability::kMutable>;
using ConstChunks = internal::ChunksImpl<internal::Mutability::kConst>;

}  // namespace pw::multibuf::v2
