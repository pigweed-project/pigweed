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
#include <iterator>
#include <type_traits>
#include <utility>

#include "pw_assert/assert.h"
#include "pw_bytes/span.h"
#include "pw_containers/dynamic_deque.h"
#include "pw_multibuf/v2/internal/entry.h"

namespace pw::multibuf::v2::internal {

// Forward declaration.
template <Mutability kMutability>
class ChunksImpl;

/// Type for iterating over the chunks added to a multibuf.
///
/// MultiBufs can be thought of as a sequence of "layers", where each layer
/// except the bottommost is comprised of subspans of the layer below it, and
/// the bottommost references the actual memory. This type can be used to
/// retrieve the contiguous byte spans of the topmost layer of a multibuf. It is
/// distinguished from `ByteIterator`, which iterates over individual bytes of
/// the topmost layer.
///
/// The iteration can be over the raw chunks for each layer, or it can be over
/// the contiguous non-empty chunks. The iteration can allow mutation of the
/// data in the chunk, or it can be a const iteration.
template <Mutability kMutability>
class ChunkIterator {
 private:
  using SpanType = std::
      conditional_t<kMutability == Mutability::kConst, ConstByteSpan, ByteSpan>;
  using ByteType = typename SpanType::element_type;

 public:
  using size_type = Entry::size_type;
  using difference_type = Entry::difference_type;
  using value_type = SpanType;
  using pointer = value_type*;
  using const_pointer = const value_type*;
  using reference = value_type&;
  using const_reference = const value_type&;
  using iterator_category = std::bidirectional_iterator_tag;

  constexpr ChunkIterator() = default;
  ~ChunkIterator() = default;
  constexpr ChunkIterator(const ChunkIterator& other) { *this = other; }
  constexpr ChunkIterator& operator=(const ChunkIterator& other);
  constexpr ChunkIterator(ChunkIterator&& other) = default;
  constexpr ChunkIterator& operator=(ChunkIterator&& other) = default;

  // Support converting non-const iterators to const_iterators.
  constexpr operator ChunkIterator<Mutability::kConst>() const {
    return {deque_, chunk_, entries_per_chunk_};
  }

  constexpr reference operator*() {
    PW_ASSERT(is_valid());
    return current_;
  }

  constexpr const_reference operator*() const {
    PW_ASSERT(is_valid());
    return current_;
  }

  constexpr pointer operator->() {
    PW_ASSERT(is_valid());
    return &current_;
  }

  constexpr const_pointer operator->() const {
    PW_ASSERT(is_valid());
    return &current_;
  }

  constexpr ChunkIterator& operator++();

  constexpr ChunkIterator operator++(int) {
    ChunkIterator previous(*this);
    operator++();
    return previous;
  }

  constexpr ChunkIterator& operator--();

  constexpr ChunkIterator operator--(int) {
    ChunkIterator previous(*this);
    operator--();
    return previous;
  }

  constexpr friend bool operator==(const ChunkIterator& lhs,
                                   const ChunkIterator& rhs) {
    return lhs.deque_ == rhs.deque_ &&
           lhs.entries_per_chunk_ == rhs.entries_per_chunk_ &&
           lhs.chunk_ == rhs.chunk_;
  }

  constexpr friend bool operator!=(const ChunkIterator& lhs,
                                   const ChunkIterator& rhs) {
    return !(lhs == rhs);
  }

 private:
  using Deque = Entry::Deque;

  // Iterators that point to something are created `Chunks` or `ConstChunks`.
  template <Mutability>
  friend class ChunksImpl;

  // Allow internal conversions between iterator subtypes
  template <Mutability>
  friend class ChunkIterator;

  // Allow MultiBufs to create iterators.
  friend class GenericMultiBuf;

  constexpr ChunkIterator(const Deque* deque,
                          size_type chunk,
                          size_type entries_per_chunk)
      : deque_(deque), chunk_(chunk), entries_per_chunk_(entries_per_chunk) {
    ResetCurrent();
  }

  [[nodiscard]] constexpr bool is_valid() const {
    return deque_ != nullptr && chunk_ < num_chunks();
  }

  constexpr size_type num_chunks() const {
    return Entry::num_chunks(*deque_, entries_per_chunk_);
  }

  constexpr void ResetCurrent();

  const Deque* deque_ = nullptr;
  size_type chunk_ = 0;
  size_type entries_per_chunk_ = Entry::kMinEntriesPerChunk;
  SpanType current_;
};

// Template method implementations.

template <Mutability kMutability>
constexpr ChunkIterator<kMutability>& ChunkIterator<kMutability>::operator=(
    const ChunkIterator& other) {
  deque_ = other.deque_;
  chunk_ = other.chunk_;
  entries_per_chunk_ = other.entries_per_chunk_;
  ResetCurrent();
  return *this;
}

template <Mutability kMutability>
constexpr ChunkIterator<kMutability>& ChunkIterator<kMutability>::operator++() {
  PW_ASSERT(is_valid());
  size_t left = current_.size();
  while (left != 0) {
    left -= Entry::GetLength(*deque_, chunk_, entries_per_chunk_);
    ++chunk_;
  }
  while (chunk_ < num_chunks() &&
         Entry::GetLength(*deque_, chunk_, entries_per_chunk_) == 0) {
    ++chunk_;
  }
  ResetCurrent();
  return *this;
}

template <Mutability kMutability>
constexpr ChunkIterator<kMutability>& ChunkIterator<kMutability>::operator--() {
  PW_ASSERT(deque_ != nullptr);
  PW_ASSERT(chunk_ != 0);
  current_ = SpanType();
  while (chunk_ != 0) {
    SpanType prev = Entry::GetView(*deque_, chunk_ - 1, entries_per_chunk_);
    if (!current_.empty() && prev.data() + prev.size() != current_.data()) {
      break;
    }
    current_ = SpanType(prev.data(), prev.size() + current_.size());
    --chunk_;
  }
  return *this;
}

template <Mutability kMutability>
constexpr void ChunkIterator<kMutability>::ResetCurrent() {
  if (!is_valid()) {
    current_ = SpanType();
    chunk_ = num_chunks();
    return;
  }

  current_ = Entry::GetView(*deque_, chunk_, entries_per_chunk_);
  for (size_type i = chunk_ + 1; i < num_chunks(); ++i) {
    SpanType next = Entry::GetView(*deque_, i, entries_per_chunk_);
    if (next.empty()) {
      continue;
    }
    if (current_.empty()) {
      current_ = next;
      chunk_ = i;
      continue;
    }
    if (current_.data() + current_.size() != next.data()) {
      break;
    }
    current_ = SpanType(current_.data(), current_.size() + next.size());
  }
  if (current_.empty()) {
    chunk_ = num_chunks();
  }
}

}  // namespace pw::multibuf::v2::internal
