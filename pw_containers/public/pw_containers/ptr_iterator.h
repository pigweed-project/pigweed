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
#include <type_traits>

#include "pw_containers/iterator.h"

namespace pw::containers {
namespace internal {

// Generic PtrIterator for const or non-const iterators. Extended by
// pw::containers::PtrIterator and pw::containers::ConstPtrIterator.
template <typename Iterator, typename Container>
class PtrIterator {
 public:
  using value_type = typename Container::value_type;
  using difference_type = std::ptrdiff_t;
  using reference =
      typename std::conditional_t<std::is_const_v<Container>,
                                  typename Container::const_reference,
                                  typename Container::reference>;
  using pointer = typename std::conditional_t<std::is_const_v<Container>,
                                              typename Container::const_pointer,
                                              typename Container::pointer>;
  using iterator_category = containers::contiguous_iterator_tag;

  constexpr reference operator*() const { return *ptr_; }
  constexpr pointer operator->() const { return ptr_; }

  constexpr Iterator& operator++() {
    ++ptr_;
    return static_cast<Iterator&>(*this);
  }

  constexpr Iterator& operator--() {
    --ptr_;
    return static_cast<Iterator&>(*this);
  }

  constexpr Iterator operator++(int) {
    Iterator original(ptr_);
    ++(*this);
    return original;
  }

  constexpr Iterator operator--(int) {
    Iterator original{ptr_};
    --(*this);
    return original;
  }

  constexpr Iterator& operator+=(difference_type n) {
    ptr_ += n;
    return static_cast<Iterator&>(*this);
  }

  constexpr Iterator& operator-=(difference_type n) {
    ptr_ -= n;
    return static_cast<Iterator&>(*this);
  }

  friend constexpr Iterator operator+(Iterator it, difference_type n) {
    return it.Add(n);
  }

  friend constexpr Iterator operator+(difference_type n, Iterator it) {
    return it.Add(n);
  }

  friend constexpr Iterator operator-(Iterator it, difference_type n) {
    return it.Add(-n);
  }

  friend constexpr difference_type operator-(Iterator lhs, Iterator rhs) {
    return lhs.ptr_ - rhs.ptr_;
  }

  friend constexpr bool operator==(Iterator lhs, Iterator rhs) {
    return lhs.ptr_ == rhs.ptr_;
  }
  friend constexpr bool operator!=(Iterator lhs, Iterator rhs) {
    return lhs.ptr_ != rhs.ptr_;
  }
  friend constexpr bool operator<(Iterator lhs, Iterator rhs) {
    return lhs.ptr_ < rhs.ptr_;
  }
  friend constexpr bool operator<=(Iterator lhs, Iterator rhs) {
    return lhs.ptr_ <= rhs.ptr_;
  }
  friend constexpr bool operator>(Iterator lhs, Iterator rhs) {
    return lhs.ptr_ > rhs.ptr_;
  }
  friend constexpr bool operator>=(Iterator lhs, Iterator rhs) {
    return lhs.ptr_ >= rhs.ptr_;
  }

  constexpr reference operator[](difference_type n) const { return ptr_[n]; }

 protected:
  constexpr PtrIterator() : ptr_(nullptr) {}

  constexpr explicit PtrIterator(pointer ptr) : ptr_(ptr) {}

 private:
  constexpr Iterator Add(difference_type n) const { return Iterator(ptr_ + n); }

  pointer ptr_;
};

}  // namespace internal

/// @module{pw_containers}

/// @addtogroup pw_containers_utilities
/// @{

/// Provides an iterator for use with container with contiguous storage. Use
/// this instead of a plain pointer to prevent accidental misuse of iterators as
/// pointers and vice versa.
///
/// References standard aliases including `Container::value_type` and
/// `Container::pointer`, etc.
///
/// Usage:
/// @code{.cpp}
///   using iterator = pw::containers::PtrIterator<MyContainer>;
/// @endcode
template <typename Container>
class PtrIterator
    : public internal::PtrIterator<PtrIterator<Container>, Container> {
 public:
  constexpr PtrIterator() = default;

 private:
  using Base = internal::PtrIterator<PtrIterator, Container>;

  friend Base;
  friend Container;

  explicit constexpr PtrIterator(typename Base::pointer ptr) : Base(ptr) {}
};

/// Version of `pw::containers::PtrIterator` for `const_iterator`.
///
/// Usage:
/// @code{.cpp}
///   using const_iterator = pw::containers::ConstPtrIterator<MyContainer>;
/// @endcode
template <typename Container>
class ConstPtrIterator
    : public internal::PtrIterator<ConstPtrIterator<Container>,
                                   const Container> {
 public:
  constexpr ConstPtrIterator() = default;

  // Implicit conversion from non-const iterators.
  constexpr ConstPtrIterator(PtrIterator<Container> other)
      : Base(other.operator->()) {}

 private:
  using Base = internal::PtrIterator<ConstPtrIterator, const Container>;

  friend Base;
  friend Container;

  explicit constexpr ConstPtrIterator(typename Base::pointer ptr) : Base(ptr) {}
};

}  // namespace pw::containers
