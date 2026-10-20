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

#include <iterator>
#include <type_traits>

namespace pw {

// Forward declaration for friending.
template <typename>
class IntrusiveForwardList;

template <typename>
class IntrusiveQueue;

namespace containers {
namespace future {

template <typename>
class IntrusiveList;

}  // namespace future

namespace internal {

// Mixin for iterators that can dereference and compare items.
template <typename Derived, typename T, typename I>
class IteratorBase {
 public:
  constexpr const T& operator*() const { return downcast(); }
  constexpr T& operator*() { return downcast(); }

  constexpr const T* operator->() const { return &downcast(); }
  constexpr T* operator->() { return &downcast(); }

  template <typename D2,
            typename T2,
            typename I2,
            typename = std::enable_if_t<
                std::is_same_v<std::remove_cv_t<T>, std::remove_cv_t<T2>>>>
  constexpr bool operator==(const IteratorBase<D2, T2, I2>& rhs) const {
    return static_cast<const I*>(derived().item_) ==
           static_cast<const I2*>(rhs.derived().item_);
  }

  template <typename D2,
            typename T2,
            typename I2,
            typename = std::enable_if_t<
                std::is_same_v<std::remove_cv_t<T>, std::remove_cv_t<T2>>>>
  constexpr bool operator!=(const IteratorBase<D2, T2, I2>& rhs) const {
    return !operator==(rhs);
  }

 private:
  template <typename, typename, typename>
  friend class IteratorBase;

  Derived& derived() { return static_cast<Derived&>(*this); }
  const Derived& derived() const { return static_cast<const Derived&>(*this); }

  T& downcast() { return static_cast<T&>(*derived().item_); }
  const T& downcast() const { return static_cast<const T&>(*derived().item_); }
};

// Mixin for iterators that can advance to a subsequent item.
template <typename Derived, typename I>
class Incrementable {
 public:
  constexpr Derived& operator++() {
    auto* derived = static_cast<Derived*>(this);
    derived->item_ = static_cast<I*>(derived->item_->next_);
    return *derived;
  }

  constexpr Derived operator++(int) {
    Derived copy(static_cast<Derived&>(*this));
    operator++();
    return copy;
  }
};

// Mixin for iterators that can reverse to a previous item.
template <typename Derived, typename I>
class Decrementable {
 public:
  constexpr Derived& operator--() {
    auto* derived = static_cast<Derived*>(this);
    derived->item_ = static_cast<I*>(derived->item_->previous());
    return *derived;
  }

  constexpr Derived operator--(int) {
    Derived copy(static_cast<Derived&>(*this));
    operator--();
    return copy;
  }
};

/// Forward iterator that has the ability to advance over a sequence of items.
///
/// This is roughly equivalent to std::forward_iterator<T>, but for intrusive
/// lists.
template <typename T, typename I>
class ForwardIterator : public IteratorBase<ForwardIterator<T, I>, T, I>,
                        public Incrementable<ForwardIterator<T, I>, I> {
 public:
  using difference_type = std::ptrdiff_t;
  using value_type = std::remove_cv_t<T>;
  using pointer = T*;
  using reference = T&;
  using iterator_category = std::forward_iterator_tag;

  constexpr ForwardIterator() = default;

  // Allow creating const_iterators from iterators.
  template <typename U>
  ForwardIterator(const ForwardIterator<U, std::remove_cv_t<I>>& other)
      : item_(other.item_) {}

 private:
  template <typename, typename, typename>
  friend class IteratorBase;

  friend class Incrementable<ForwardIterator<T, I>, I>;

  template <typename, typename>
  friend class ForwardIterator;

  template <typename>
  friend class ::pw::IntrusiveForwardList;

  template <typename>
  friend class ::pw::IntrusiveQueue;

  // Only allow IntrusiveForwardList to create iterators that point to
  // something.
  constexpr explicit ForwardIterator(I* item) : item_{item} {}

  I* item_ = nullptr;
};

/// Iterator that has the ability to advance forwards or backwards over a
/// sequence of items.
///
/// This is roughly equivalent to std::bidirectional_iterator<T>, but for
/// intrusive lists.
template <typename T, typename I>
class BidirectionalIterator
    : public IteratorBase<BidirectionalIterator<T, I>, T, I>,
      public Decrementable<BidirectionalIterator<T, I>, I>,
      public Incrementable<BidirectionalIterator<T, I>, I> {
 public:
  using difference_type = std::ptrdiff_t;
  using value_type = std::remove_cv_t<T>;
  using pointer = T*;
  using reference = T&;
  using iterator_category = std::bidirectional_iterator_tag;

  constexpr BidirectionalIterator() = default;

  // Allow creating const_iterators from iterators.
  template <typename U>
  BidirectionalIterator(
      const BidirectionalIterator<U, std::remove_cv_t<I>>& other)
      : item_(other.item_) {}

 private:
  template <typename, typename, typename>
  friend class IteratorBase;

  friend class Decrementable<BidirectionalIterator<T, I>, I>;
  friend class Incrementable<BidirectionalIterator<T, I>, I>;

  template <typename, typename>
  friend class BidirectionalIterator;

  template <typename>
  friend class ::pw::containers::future::IntrusiveList;

  // Only allow IntrusiveList to create iterators that point to something.
  constexpr explicit BidirectionalIterator(I* item) : item_{item} {}

  I* item_ = nullptr;
};

}  // namespace internal
}  // namespace containers
}  // namespace pw
