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

namespace pw::multibuf::v2 {

// Forward declarations.
template <typename, typename>
class ChunksImpl;

class MultiBufV1Adapter;

namespace internal {

enum class ChunkContiguity {
  kKeepAll,
  kCoalesce,
};

enum class ChunkMutability {
  kMutable,
  kConst,
};

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
template <typename SizeType,
          ChunkContiguity kContiguity,
          ChunkMutability kMutability>
class ChunkIterator {
 private:
  using SpanType = std::conditional_t<kMutability == ChunkMutability::kConst,
                                      ConstByteSpan,
                                      ByteSpan>;
  using ByteType = typename SpanType::element_type;
  using Deque = std::conditional_t<kMutability == ChunkMutability::kConst,
                                   const DynamicDeque<Entry, SizeType>,
                                   DynamicDeque<Entry, SizeType>>;

 public:
  using size_type = SizeType;
  using difference_type = std::ptrdiff_t;
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
  constexpr
  operator ChunkIterator<SizeType, kContiguity, ChunkMutability::kConst>()
      const {
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
  // Iterators that point to something are created `Chunks` or `ConstChunks`.
  template <typename, ChunkContiguity, ChunkMutability>
  friend class ChunksImpl;

  // Allow internal conversions between iterator subtypes
  template <typename, ChunkContiguity, ChunkMutability>
  friend class ChunkIterator;

  // Byte iterators use chunk iterators to get contiguous spans.
  template <typename, ChunkMutability>
  friend class ByteIterator;

  friend MultiBufV1Adapter;

  constexpr ChunkIterator(Deque* deque,
                          size_type chunk,
                          size_type entries_per_chunk)
      : deque_(deque), chunk_(chunk), entries_per_chunk_(entries_per_chunk) {
    ResetCurrent();
  }

  [[nodiscard]] constexpr bool is_valid() const {
    return deque_ != nullptr && chunk_ < num_chunks();
  }

  constexpr size_type num_chunks() const {
    return deque_ == nullptr ? 0 : deque_->size() / entries_per_chunk_;
  }

  constexpr ByteType* data(size_type chunk) const {
    size_type data_index = Entry::data_index(chunk, entries_per_chunk_);
    size_type view_index = Entry::top_view_index(chunk, entries_per_chunk_);
    size_type offset = entries_per_chunk_ == Entry::kMinEntriesPerChunk
                           ? (*deque_)[view_index].base_view.offset
                           : (*deque_)[view_index].view.offset;
    return (*deque_)[data_index].data + offset;
  }

  constexpr size_t size(size_type chunk) const {
    size_type view_index = Entry::top_view_index(chunk, entries_per_chunk_);
    return entries_per_chunk_ == Entry::kMinEntriesPerChunk
               ? (*deque_)[view_index].base_view.length
               : (*deque_)[view_index].view.length;
  }

  constexpr void ResetCurrent();

  Deque* deque_ = nullptr;
  size_type chunk_ = 0;
  size_type entries_per_chunk_ = Entry::kMinEntriesPerChunk;
  SpanType current_;
};

// Template method implementations.

template <typename SizeType,
          ChunkContiguity kContiguity,
          ChunkMutability kMutability>
constexpr ChunkIterator<SizeType, kContiguity, kMutability>&
ChunkIterator<SizeType, kContiguity, kMutability>::operator=(
    const ChunkIterator& other) {
  deque_ = other.deque_;
  chunk_ = other.chunk_;
  entries_per_chunk_ = other.entries_per_chunk_;
  ResetCurrent();
  return *this;
}

template <typename SizeType,
          ChunkContiguity kContiguity,
          ChunkMutability kMutability>
constexpr ChunkIterator<SizeType, kContiguity, kMutability>&
ChunkIterator<SizeType, kContiguity, kMutability>::operator++() {
  PW_ASSERT(is_valid());

  if constexpr (kContiguity == ChunkContiguity::kKeepAll) {
    ++chunk_;
    ResetCurrent();
    return *this;
  }

  size_t left = current_.size();
  while (left != 0) {
    left -= size(chunk_);
    ++chunk_;
  }
  while (chunk_ < num_chunks() && size(chunk_) == 0) {
    ++chunk_;
  }
  ResetCurrent();
  return *this;
}

template <typename SizeType,
          ChunkContiguity kContiguity,
          ChunkMutability kMutability>
constexpr ChunkIterator<SizeType, kContiguity, kMutability>&
ChunkIterator<SizeType, kContiguity, kMutability>::operator--() {
  PW_ASSERT(deque_ != nullptr);
  PW_ASSERT(chunk_ != 0);

  if constexpr (kContiguity == ChunkContiguity::kKeepAll) {
    --chunk_;
    current_ = SpanType(data(chunk_), size(chunk_));
    return *this;
  }

  current_ = SpanType();
  while (chunk_ != 0) {
    SpanType prev(data(chunk_ - 1), size(chunk_ - 1));
    if (!current_.empty() && prev.data() + prev.size() != current_.data()) {
      break;
    }
    current_ = SpanType(prev.data(), prev.size() + current_.size());
    --chunk_;
  }
  return *this;
}

template <typename SizeType,
          ChunkContiguity kContiguity,
          ChunkMutability kMutability>
constexpr void
ChunkIterator<SizeType, kContiguity, kMutability>::ResetCurrent() {
  if (!is_valid()) {
    current_ = SpanType();
    chunk_ = num_chunks();
    return;
  }

  current_ = SpanType(data(chunk_), size(chunk_));
  if constexpr (kContiguity == ChunkContiguity::kKeepAll) {
    return;
  }

  for (size_type i = chunk_ + 1; i < num_chunks(); ++i) {
    size_t next_size = size(i);
    if (next_size == 0) {
      continue;
    }
    auto* next_data = data(i);
    if (current_.empty()) {
      current_ = SpanType(next_data, next_size);
      chunk_ = i;
      continue;
    }
    if (current_.data() + current_.size() != next_data) {
      break;
    }
    current_ = SpanType(current_.data(), current_.size() + next_size);
  }
  if (current_.empty()) {
    chunk_ = num_chunks();
  }
}

}  // namespace internal
}  // namespace pw::multibuf::v2
