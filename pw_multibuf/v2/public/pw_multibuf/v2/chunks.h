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

// Forward declaration for friending.
namespace test {
class IteratorTest;
}  // namespace test

namespace internal {

/// Base class for ranges of chunks.
template <typename Deque,
          ChunkContiguity kContiguity,
          ChunkMutability kMutability>
class ChunksImpl {
 public:
  using size_type = typename Deque::size_type;
  using value_type = typename Deque::value_type;
  using difference_type = typename Deque::difference_type;
  using iterator = ChunkIterator<size_type, kContiguity, kMutability>;
  using const_iterator =
      ChunkIterator<size_type, kContiguity, ChunkMutability::kConst>;

  constexpr ChunksImpl() = default;

  constexpr size_type size() const {
    if constexpr (kContiguity == ChunkContiguity::kKeepAll) {
      return deque().size() / begin_.entries_per_chunk_;
    }
    return static_cast<size_type>(std::distance(begin_, end_));
  }

  constexpr iterator begin() const { return begin_; }
  constexpr iterator end() const { return end_; }

  constexpr const_iterator cbegin() const { return begin_; }
  constexpr const_iterator cend() const { return end_; }

 private:
  friend class GenericMultiBuf;

  // For unit testing.
  friend class ::pw::multibuf::v2::test::IteratorTest;

  using DequeRefType =
      std::conditional_t<kMutability == ChunkMutability::kConst,
                         const Deque&,
                         Deque&>;

  constexpr ChunksImpl(DequeRefType& deque, size_type entries_per_chunk) {
    begin_.deque_ = &deque;
    begin_.entries_per_chunk_ = entries_per_chunk;
    end_.deque_ = &deque;
    end_.entries_per_chunk_ = entries_per_chunk;
    end_.chunk_ = deque.size() / entries_per_chunk;
  }

  constexpr const Deque& deque() const { return *(begin_.deque_); }

  iterator begin_;
  iterator end_;
};

/// Helper class that allows iterating over mutable contiguous chunks in a
/// MultiBuf.
///
/// This allows using range-based for-loops, e.g.
///
/// @code{.cpp}
/// for (ByteSpan chunk : multibuf.RawChunks()) {
///   ModifyChunk(chunk);
/// }
/// @endcode
///
/// @warning Modifying the structure of a MultiBuf invalidates any outstanding
/// chunk iterators.
template <typename Deque>
using Chunks =
    ChunksImpl<Deque, ChunkContiguity::kCoalesce, ChunkMutability::kMutable>;

/// Helper class that allows iterating over read-only contiguous chunks in a
/// MultiBuf.
///
/// This allows using range-based for-loops, e.g.
///
/// @code{.cpp}
/// for (ConstByteSpan chunk : multibuf.RawConstChunks()) {
///   ReadChunk(chunk);
/// }
/// @endcode
///
/// @warning Modifying the structure of a MultiBuf invalidates any outstanding
/// chunk iterators.
template <typename Deque>
using ConstChunks =
    ChunksImpl<Deque, ChunkContiguity::kCoalesce, ChunkMutability::kConst>;

/// Helper class that allows iterating the raw mutable chunks in a MultiBuf.
///
/// This allows using range-based for-loops, e.g.
///
/// @code{.cpp}
/// for (ByteSpan chunk : multibuf.RawChunks()) {
///   ModifyChunk(chunk);
/// }
/// @endcode
///
/// @warning Modifying the structure of a MultiBuf invalidates any outstanding
/// chunk iterators.
template <typename Deque>
using RawChunks =
    ChunksImpl<Deque, ChunkContiguity::kKeepAll, ChunkMutability::kMutable>;

/// Helper class that allows iterating the raw read-only chunks in a MultiBuf.
///
/// This allows using range-based for-loops, e.g.
///
/// @code{.cpp}
/// for (ConstByteSpan chunk : multibuf.RawConstChunks()) {
///   ReadChunk(chunk);
/// }
/// @endcode
///
/// @warning Modifying the structure of a MultiBuf invalidates any outstanding
/// chunk iterators.
template <typename Deque>
using RawConstChunks =
    ChunksImpl<Deque, ChunkContiguity::kKeepAll, ChunkMutability::kConst>;

}  // namespace internal
}  // namespace pw::multibuf::v2
