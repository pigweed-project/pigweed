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
#include <functional>
#include <iterator>

#include "pw_assert/assert.h"
#include "pw_bytes/span.h"
#include "pw_containers/dynamic_deque.h"
#include "pw_multibuf/v2/internal/entry.h"

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
class BasicByteIterator {
 public:
  using Deque = Entry::Deque;
  using size_type = Entry::size_type;
  using difference_type = Entry::difference_type;
  using iterator_category = std::random_access_iterator_tag;

  ~BasicByteIterator() = default;

  constexpr friend difference_type operator-(const BasicByteIterator& lhs,
                                             const BasicByteIterator& rhs);

 protected:
  constexpr BasicByteIterator() = default;
  constexpr BasicByteIterator(const Deque& deque,
                              size_type chunk,
                              size_type entries_per_chunk,
                              size_type offset)
      : deque_(&deque),
        chunk_(chunk),
        entries_per_chunk_(entries_per_chunk),
        offset_(offset) {}

  [[nodiscard]] constexpr bool has_deque() const { return deque_ != nullptr; }

  [[nodiscard]] constexpr const Deque& deque() const {
    PW_ASSERT(deque_ != nullptr);
    return *deque_;
  }

  constexpr void Increment(difference_type n);

  constexpr void Decrement(difference_type n);

  [[nodiscard]] constexpr int Compare(const BasicByteIterator& other) const;

  const Deque* deque_ = nullptr;
  size_type chunk_ = 0;
  size_type entries_per_chunk_ = 0;
  size_type offset_ = 0;
};

/// Byte iterator templated on the const-ness of the bytes it references.
template <Mutability kMutability>
class ByteIterator : public BasicByteIterator {
 public:
  using BasicByteIterator::Deque;
  using BasicByteIterator::difference_type;
  using BasicByteIterator::iterator_category;
  using BasicByteIterator::size_type;

  using value_type = std::conditional_t<kMutability == Mutability::kConst,
                                        const std::byte,
                                        std::byte>;
  using pointer = value_type*;
  using reference = value_type&;

  constexpr ByteIterator() = default;
  constexpr ByteIterator(const ByteIterator& other) = default;
  ByteIterator& operator=(const ByteIterator& other) = default;

  // Support converting non-const iterators to const_iterators.
  constexpr operator ByteIterator<Mutability::kConst>() const {
    return has_deque() ? ByteIterator<Mutability::kConst>(
                             deque(), chunk_, entries_per_chunk_, offset_)
                       : ByteIterator<Mutability::kConst>();
  }

  constexpr reference operator*() const {
    return Entry::GetView(deque(), chunk_, entries_per_chunk_)[offset_];
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

  template <typename T, typename = std::enable_if_t<std::is_integral_v<T>, int>>
  constexpr ByteIterator& operator+=(T n) {
    if constexpr (std::is_unsigned_v<T>) {
      PW_ASSERT(size_t(n) < std::numeric_limits<difference_type>::max());
    }
    Increment(static_cast<difference_type>(n));
    return *this;
  }

  template <typename T, typename = std::enable_if_t<std::is_integral_v<T>, int>>
  constexpr friend ByteIterator operator+(ByteIterator it, T n) {
    return it += n;
  }

  template <typename T, typename = std::enable_if_t<std::is_integral_v<T>, int>>
  constexpr friend ByteIterator operator+(T n, ByteIterator it) {
    return it += n;
  }

  constexpr ByteIterator& operator--() { return operator-=(1); }

  constexpr ByteIterator operator--(int) {
    ByteIterator previous(*this);
    operator--();
    return previous;
  }

  template <typename T, typename = std::enable_if_t<std::is_integral_v<T>, int>>
  constexpr ByteIterator& operator-=(T n) {
    if constexpr (std::is_unsigned_v<T>) {
      PW_ASSERT(size_t(n) < std::numeric_limits<difference_type>::max());
    }
    Decrement(static_cast<difference_type>(n));
    return *this;
  }

  template <typename T, typename = std::enable_if_t<std::is_integral_v<T>, int>>
  constexpr friend ByteIterator operator-(ByteIterator it, T n) {
    return it -= n;
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
  template <Mutability>
  friend class ByteIterator;

  // Allow MultiBufs to create iterators.
  friend class GenericMultiBuf;

  // For unit testing.
  friend class ::pw::multibuf::v2::test::IteratorTest;

  constexpr ByteIterator(const Deque& deque,
                         size_type chunk,
                         size_type entries_per_chunk,
                         size_type offset)
      : BasicByteIterator(deque, chunk, entries_per_chunk, offset) {}
};

// Constexpr and template method implementations.

constexpr void BasicByteIterator::Increment(difference_type n) {
  if (n < 0) {
    Decrement(-n);
    return;
  }
  PW_ASSERT(deque_ != nullptr);
  PW_ASSERT(n < std::numeric_limits<size_type>::max());
  size_type delta = static_cast<size_type>(n) + offset_;
  size_type length = 0;
  size_type num_chunks = Entry::num_chunks(*deque_, entries_per_chunk_);
  while (chunk_ < num_chunks) {
    length = Entry::GetLength(*deque_, chunk_, entries_per_chunk_);
    if (delta < length) {
      break;
    }
    delta -= length;
    ++chunk_;
  }
  PW_ASSERT(delta < length || delta == 0);
  offset_ = delta;
}

constexpr void BasicByteIterator::Decrement(difference_type n) {
  if (n < 0) {
    return Increment(-n);
  }
  PW_ASSERT(deque_ != nullptr);
  PW_ASSERT(n < std::numeric_limits<size_type>::max());
  size_type delta = static_cast<size_type>(n);
  while (delta > offset_) {
    PW_ASSERT(chunk_ != 0);
    --chunk_;
    delta -= offset_;
    offset_ = Entry::GetLength(*deque_, chunk_, entries_per_chunk_);
  }
  offset_ -= delta;
}

constexpr int BasicByteIterator::Compare(const BasicByteIterator& other) const {
  if (deque_ == nullptr && other.deque_ == nullptr) {
    return 0;
  }
  if (deque_ == nullptr && other.deque_ != nullptr) {
    return -1;
  }
  if (deque_ != nullptr && other.deque_ == nullptr) {
    return 1;
  }
  if (deque_ != other.deque_) {
    return std::less<>()(deque_, other.deque_) ? -1 : 1;
  }
  if (chunk_ != other.chunk_) {
    return chunk_ < other.chunk_ ? -1 : 1;
  }
  if (offset_ != other.offset_) {
    return offset_ < other.offset_ ? -1 : 1;
  }
  return 0;
}

constexpr BasicByteIterator::difference_type operator-(
    const BasicByteIterator& lhs, const BasicByteIterator& rhs) {
  using size_type = BasicByteIterator::size_type;
  PW_ASSERT(lhs.deque_ != nullptr);
  PW_ASSERT(lhs.deque_ == rhs.deque_);
  PW_ASSERT(lhs.entries_per_chunk_ == rhs.entries_per_chunk_);
  int cmp = lhs.Compare(rhs);
  if (cmp < 0) {
    return -(rhs - lhs);
  }
  if (cmp == 0) {
    return 0;
  }
  size_type delta = 0;
  size_type chunk = rhs.chunk_;
  while (chunk < lhs.chunk_) {
    delta += Entry::GetLength(*rhs.deque_, chunk, rhs.entries_per_chunk_);
    ++chunk;
  }
  delta -= rhs.offset_;
  delta += lhs.offset_;
  return static_cast<BasicByteIterator::difference_type>(delta);
}

}  // namespace internal
}  // namespace pw::multibuf::v2
