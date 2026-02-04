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

#include "pw_bytes/span.h"
#include "pw_multibuf/v2/internal/chunk_iterator.h"

namespace pw::multibuf::v2 {

namespace test {
class IteratorTest;
}  // namespace test

namespace internal {

/// Type for iterating over the bytes in a multibuf.
///
/// Multibufs can be thought of as a sequence of "layers", where each layer
/// except the bottommost is comprised of subspans of the layer below it, and
/// the bottommost references the actual memory. This type can be used to
/// iterate over the bytes of the topmost layer of a multibuf. It is
/// distinguished from `ChunkIterator`, which iterates over byte spans of
/// the topmost layer.
template <typename SizeType, ChunkMutability kMutability>
class ByteIterator {
 private:
  using ChunkIteratorType =
      ChunkIterator<SizeType, ChunkContiguity::kCoalesce, kMutability>;
  using Deque = typename ChunkIteratorType::Deque;

 public:
  using size_type = SizeType;
  using difference_type = ptrdiff_t;
  using value_type = typename ChunkIteratorType::ByteType;
  using pointer = value_type*;
  using reference = value_type&;
  using iterator_category = std::random_access_iterator_tag;

  constexpr ByteIterator() = default;

  // Support converting non-const iterators to const_iterators.
  constexpr operator ByteIterator<SizeType, ChunkMutability::kConst>() const {
    return {chunk_iterator_, offset_};
  }

  constexpr reference operator*() const {
    return chunk_iterator_->data()[offset_];
  }

  constexpr reference operator[](difference_type n) const {
    return *(*this + n);
  }

  constexpr ByteIterator& operator++() { return operator+=(1); }

  constexpr ByteIterator operator++(int) {
    ByteIterator previous(*this);
    operator++();
    return previous;
  }

  constexpr ByteIterator& operator+=(difference_type n) {
    if (n < 0) {
      return operator-=(-n);
    }
    size_t delta = static_cast<size_t>(n);
    delta += offset_;
    while (delta != 0 && chunk_iterator_->size() <= delta) {
      delta -= chunk_iterator_->size();
      ++chunk_iterator_;
    }
    offset_ = delta;
    return *this;
  }

  constexpr friend ByteIterator operator+(ByteIterator it, difference_type n) {
    return it += n;
  }

  constexpr friend ByteIterator operator+(difference_type n, ByteIterator it) {
    return it += n;
  }

  constexpr ByteIterator& operator--() { return operator-=(1); }

  constexpr ByteIterator operator--(int) {
    ByteIterator previous(*this);
    operator--();
    return previous;
  }

  constexpr ByteIterator& operator-=(difference_type n) {
    if (n < 0) {
      return operator+=(-n);
    }
    size_t delta = static_cast<size_t>(n);
    while (delta > offset_) {
      --chunk_iterator_;
      delta -= offset_;
      offset_ = chunk_iterator_->size();
    }
    offset_ -= delta;
    return *this;
  }

  constexpr friend ByteIterator operator-(ByteIterator it, difference_type n) {
    return it -= n;
  }

  constexpr friend difference_type operator-(const ByteIterator& lhs,
                                             const ByteIterator& rhs) {
    if (lhs < rhs) {
      return -(rhs - lhs);
    }
    difference_type delta = 0;
    auto chunk_iterator = rhs.chunk_iterator_;
    size_t offset = rhs.offset_;
    while (chunk_iterator != lhs.chunk_iterator_) {
      ConstByteSpan bytes = *chunk_iterator;
      delta += static_cast<difference_type>(bytes.size() - offset);
      offset = 0;
      ++chunk_iterator;
    }
    return delta + static_cast<difference_type>(lhs.offset_);
  }

  constexpr friend bool operator==(const ByteIterator& lhs,
                                   const ByteIterator& rhs) {
    return lhs.Compare(rhs) == 0;
  }

  constexpr friend bool operator!=(const ByteIterator& lhs,
                                   const ByteIterator& rhs) {
    return lhs.Compare(rhs) != 0;
  }

  constexpr friend bool operator<(const ByteIterator& lhs,
                                  const ByteIterator& rhs) {
    return lhs.Compare(rhs) < 0;
  }

  constexpr friend bool operator>(const ByteIterator& lhs,
                                  const ByteIterator& rhs) {
    return lhs.Compare(rhs) > 0;
  }

  constexpr friend bool operator<=(const ByteIterator& lhs,
                                   const ByteIterator& rhs) {
    return lhs.Compare(rhs) <= 0;
  }

  constexpr friend bool operator>=(const ByteIterator& lhs,
                                   const ByteIterator& rhs) {
    return lhs.Compare(rhs) >= 0;
  }

 private:
  // Allow non-const iterators to construct const_iterators in conversions.
  template <typename, ChunkMutability>
  friend class ByteIterator;

  // Allow MultiBufs to create iterators.
  friend class GenericMultiBuf;

  // For unit testing.
  friend class ::pw::multibuf::v2::test::IteratorTest;

  constexpr ByteIterator(ChunkIteratorType&& chunk_iterator, size_t offset)
      : chunk_iterator_(std::move(chunk_iterator)), offset_(offset) {}

  constexpr size_type chunk() const { return chunk_iterator_.chunk_; }

  // Compare using a method to allow access to the befriended ChunkIterator.
  constexpr int Compare(const ByteIterator& other) const {
    if (chunk_iterator_ != other.chunk_iterator_) {
      return chunk_iterator_.chunk_ < other.chunk_iterator_.chunk_ ? -1 : 1;
    }
    if (offset_ != other.offset_) {
      return offset_ < other.offset_ ? -1 : 1;
    }
    return 0;
  }

  ChunkIteratorType chunk_iterator_;
  size_t offset_ = 0;
};

}  // namespace internal
}  // namespace pw::multibuf::v2
