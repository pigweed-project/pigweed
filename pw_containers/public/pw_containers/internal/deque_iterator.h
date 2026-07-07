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

#include "pw_assert/assert.h"

namespace pw {

template <typename T, typename SizeType>
class DynamicVector;

namespace containers::internal {

template <typename Derived, typename ValueType, typename SizeType>
class GenericDeque;

template <typename ValueType, typename SizeType>
class PodDeque;

// DequeIterator meets the named requirements for LegacyRandomAccessIterator.
template <typename Container>
class DequeIterator {
 public:
  using value_type = typename Container::value_type;
  using reference =
      typename std::conditional_t<std::is_const_v<Container>,
                                  typename Container::const_reference,
                                  typename Container::reference>;
  using pointer = typename std::conditional_t<std::is_const_v<Container>,
                                              typename Container::const_pointer,
                                              typename Container::pointer>;
  using difference_type = std::ptrdiff_t;
  using iterator_category = std::forward_iterator_tag;

  constexpr DequeIterator() : container_(nullptr), pos_(0) {}

  constexpr DequeIterator(const DequeIterator& other) = default;
  constexpr DequeIterator& operator=(const DequeIterator& other) = default;

  // Support converting non-const iterators to const_iterators.
  constexpr operator DequeIterator<std::add_const_t<Container>>() const {
    DequeIterator<std::add_const_t<Container>> const_it;
    const_it.container_ = container_;
    const_it.pos_ = pos_;
    return const_it;
  }

  constexpr DequeIterator& operator+=(difference_type n) {
    pos_ = static_cast<size_type>(pos_ + n);
    return *this;
  }
  constexpr DequeIterator& operator-=(difference_type n) {
    pos_ = static_cast<size_type>(pos_ - n);
    return *this;
  }
  constexpr DequeIterator& operator++() { return operator+=(1); }
  constexpr DequeIterator operator++(int) {
    DequeIterator it(*this);
    operator++();
    return it;
  }

  constexpr DequeIterator& operator--() { return operator-=(1); }
  constexpr DequeIterator operator--(int) {
    DequeIterator it = *this;
    operator--();
    return it;
  }

  constexpr friend DequeIterator operator+(DequeIterator it,
                                           difference_type n) {
    return it += n;
  }
  constexpr friend DequeIterator operator+(difference_type n,
                                           DequeIterator it) {
    return it += n;
  }

  constexpr friend DequeIterator operator-(DequeIterator it,
                                           difference_type n) {
    return it -= n;
  }

  constexpr friend difference_type operator-(const DequeIterator& a,
                                             const DequeIterator& b) {
    return a.pos_ - b.pos_;
  }

  constexpr reference operator*() const { return container_->at(pos_); }

  constexpr pointer operator->() const { return &operator*(); }

  constexpr reference operator[](difference_type n) { return *(*this + n); }

  constexpr friend bool operator==(DequeIterator a, DequeIterator b) {
    return a.container_ == b.container_ && a.pos_ == b.pos_;
  }

  constexpr friend bool operator!=(DequeIterator a, DequeIterator b) {
    return !(a == b);
  }

  constexpr friend bool operator<(DequeIterator a, DequeIterator b) {
    return a.pos_ < b.pos_;
  }

  constexpr friend bool operator>(DequeIterator a, DequeIterator b) {
    return a.pos_ > b.pos_;
  }

  constexpr friend bool operator<=(DequeIterator a, DequeIterator b) {
    return a.pos_ <= b.pos_;
  }

  constexpr friend bool operator>=(DequeIterator a, DequeIterator b) {
    return a.pos_ >= b.pos_;
  }

 private:
  template <typename T, typename SizeType>
  friend class ::pw::DynamicVector;

  template <typename Derived, typename ValueType, typename SizeType>
  friend class GenericDeque;

  template <typename ValueType, typename SizeType>
  friend class PodDeque;

  // Allow non-const iterators to construct const_iterators in conversions.
  friend class DequeIterator<std::remove_const_t<Container>>;

  using size_type = typename Container::size_type;

  constexpr DequeIterator(Container& container, size_type pos)
      : container_(&container), pos_(pos) {}

  Container* container_;  // pointer to container this iterator is from
  size_type pos_;         // logical index of iterator
};

}  // namespace containers::internal
}  // namespace pw
