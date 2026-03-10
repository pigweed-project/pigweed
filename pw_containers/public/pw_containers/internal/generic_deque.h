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

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

#include "lib/stdcompat/utility.h"
#include "pw_assert/assert.h"
#include "pw_containers/internal/deque_iterator.h"
#include "pw_containers/internal/traits.h"
#include "pw_containers/internal/wrap.h"
#include "pw_numeric/checked_arithmetic.h"
#include "pw_span/span.h"

namespace pw::containers::internal {

template <typename T>
using EnableIfIterable =
    std::enable_if_t<true, decltype(T().begin(), T().end())>;

/// @module{pw_containers}

/// @addtogroup pw_containers_queues
/// @{

/// Base class for deques.
///
/// This type does not depend on the type of the elements being stored in the
/// container.
///
/// @tparam SizeType Type for the size and capacity members. Determines the
///     maximum capacity of the deque.
template <typename SizeType>
class GenericDequeBase {
 public:
  using size_type = SizeType;

  static_assert(std::is_unsigned_v<SizeType>, "SizeType must be unsigned.");

  // Size

  [[nodiscard]] constexpr bool empty() const noexcept { return size() == 0; }

  [[nodiscard]] constexpr bool full() const noexcept {
    return size() == capacity();
  }

  /// Returns the number of elements in the deque.
  constexpr size_type size() const noexcept { return count_; }

  /// Returns the maximum number of elements in the deque.
  constexpr size_type capacity() const noexcept { return capacity_; }

 protected:
  // Functions used by deque implementations

  constexpr void MoveAssignIndices(GenericDequeBase& other) noexcept {
    capacity_ = std::exchange(other.capacity_, 0);
    count_ = std::exchange(other.count_, 0);
    head_ = cpp20::exchange(other.head_, 0);
    tail_ = cpp20::exchange(other.tail_, 0);
  }

  void SwapIndices(GenericDequeBase& other) noexcept {
    std::swap(capacity_, other.capacity_);
    std::swap(count_, other.count_);
    std::swap(head_, other.head_);
    std::swap(tail_, other.tail_);
  }

  // Returns if buffer can be resized larger without moving any items. Can
  // extend if not wrapped or if tail_ wrapped but no elements were added.
  bool CanExtendBuffer() const { return tail_ > head_ || tail_ == 0; }

  // Can only shrink if there are no empty slots at the start of the buffer.
  bool CanShrinkBuffer() const { return head_ == 0; }

  void HandleNewBuffer(size_type new_capacity) {
    capacity_ = new_capacity;
    head_ = 0;
    tail_ = size() == new_capacity ? 0 : count_;  // handle full buffers
  }

  void HandleExtendedBuffer(size_type new_capacity) {
    capacity_ = new_capacity;
    if (tail_ == 0) {  // "unwrap" the tail if needed
      tail_ = head_ + count_;
    }
  }

  void HandleShrunkBuffer(size_type new_capacity) {
    capacity_ = new_capacity;
    if (tail_ == new_capacity) {  // wrap the tail if needed
      tail_ = 0;
    }
  }

 private:
  // Functions needed by GenericDeque only
  template <typename Derived, typename ValueType, typename S>
  friend class GenericDeque;

  explicit constexpr GenericDequeBase(size_type initial_capacity) noexcept
      : capacity_(initial_capacity), count_(0), head_(0), tail_(0) {}

  constexpr void ClearIndices() {
    count_ = 0;
    head_ = tail_ = 0;
  }

  // Returns the absolute index based on the relative index beyond the
  // head offset.
  //
  // Precondition: The relative index must be valid, i.e. < size().
  constexpr size_type AbsoluteIndex(const size_type relative_index) const {
    const size_type absolute_index = head_ + relative_index;
    if (absolute_index < capacity()) {
      return absolute_index;
    }
    // Offset wrapped across the end of the circular buffer.
    return absolute_index - capacity();
  }

  constexpr size_type AbsoluteIndexChecked(
      const size_type relative_index) const {
    PW_ASSERT(relative_index < size());
    return AbsoluteIndex(relative_index);
  }

  constexpr void PushBack(size_type count = 1) {
    IncrementWithWrap(tail_, count, capacity());
    count_ += count;
  }
  constexpr void PushFront(size_type count = 1) {
    DecrementWithWrap(head_, count, capacity());
    count_ += count;
  }
  constexpr void PopFront() {
    IncrementWithWrap(head_, size_type(1), capacity());
    count_ -= 1;
  }
  constexpr void PopBack() {
    DecrementWithWrap(tail_, size_type(1), capacity());
    count_ -= 1;
  }

  size_type capacity_;
  size_type count_;
  size_type head_;  // Inclusive offset for the front.
  size_type tail_;  // Non-inclusive offset for the back.
};

/// Generic array-based deque class.
///
/// Uses CRTP to access the underlying array and handle potentially resizing it.
/// Extended by pw::InlineDeque and pw::DynamicDeque.
template <typename Derived, typename ValueType, typename SizeType>
class GenericDeque : public GenericDequeBase<SizeType> {
 private:
  using Base = GenericDequeBase<SizeType>;

 public:
  using value_type = ValueType;
  using size_type = SizeType;
  using difference_type = ptrdiff_t;
  using reference = value_type&;
  using const_reference = const value_type&;
  using pointer = value_type*;
  using const_pointer = const value_type*;
  using iterator = containers::internal::DequeIterator<Derived>;
  using const_iterator = containers::internal::DequeIterator<const Derived>;

  // Copy/assign are implemented in derived classes.
  GenericDeque(const GenericDeque&) = delete;
  GenericDeque(GenericDeque&&) = delete;

  GenericDeque& operator=(const GenericDeque&) = delete;
  GenericDeque&& operator=(GenericDeque&&) = delete;

  // Size

  using Base::capacity;
  using Base::empty;
  using Base::size;

  // Infallible assign

  /// Sets the contents to `count` copies of `value`. Crashes if cannot fit.
  void assign(size_type count, const value_type& value) {
    PW_ASSERT(try_assign(count, value));
  }

  /// Sets the contents to copies of the items from the iterator. Crashes if
  /// cannot fit.
  template <typename It,
            int&...,
            typename = containers::internal::EnableIfInputIterator<It>>
  void assign(It start, It finish);

  /// Sets contents to copies of the items from the list. Crashes if cannot fit.
  void assign(const std::initializer_list<value_type>& list) {
    assign(list.begin(), list.end());
  }

  // Access

  constexpr reference at(size_type index) {
    return data()[Base::AbsoluteIndexChecked(index)];
  }
  constexpr const_reference at(size_type index) const {
    return data()[Base::AbsoluteIndexChecked(index)];
  }

  constexpr reference operator[](size_type index) {
    PW_DASSERT(index < size());
    return data()[Base::AbsoluteIndex(index)];
  }
  constexpr const_reference operator[](size_type index) const {
    PW_DASSERT(index < size());
    return data()[Base::AbsoluteIndex(index)];
  }

  constexpr reference front() {
    PW_DASSERT(!empty());
    return data()[head()];
  }
  constexpr const_reference front() const {
    PW_DASSERT(!empty());
    return data()[head()];
  }

  constexpr reference back() {
    PW_DASSERT(!empty());
    return data()[Base::AbsoluteIndex(size() - 1)];
  }
  constexpr const_reference back() const {
    PW_DASSERT(!empty());
    return data()[Base::AbsoluteIndex(size() - 1)];
  }

  /// Provides access to the valid data in a contiguous form.
  constexpr std::pair<span<const value_type>, span<const value_type>>
  contiguous_data() const;
  constexpr std::pair<span<value_type>, span<value_type>> contiguous_data() {
    auto [first, second] = std::as_const(*this).contiguous_data();
    return {{const_cast<pointer>(first.data()), first.size()},
            {const_cast<pointer>(second.data()), second.size()}};
  }

  // Iterate

  constexpr iterator begin() noexcept {
    if (empty()) {
      return end();
    }

    return iterator(derived(), 0);
  }
  constexpr const_iterator begin() const noexcept { return cbegin(); }
  constexpr const_iterator cbegin() const noexcept {
    if (empty()) {
      return cend();
    }
    return const_iterator(derived(), 0);
  }

  constexpr iterator end() noexcept { return iterator(derived(), size()); }
  constexpr const_iterator end() const noexcept { return cend(); }
  constexpr const_iterator cend() const noexcept {
    return const_iterator(derived(), size());
  }

  // Infallible modify

  constexpr void clear() {
    DestroyAll();
    Base::ClearIndices();
  }

  /// Erases the item at `pos`, which must be a dereferenceable iterator.
  iterator erase(const_iterator pos) {
    PW_DASSERT(pos.pos_ < size());
    return erase(pos, pos + 1);
  }

  /// Erases the items in the range `[first, last)`. Does nothing if `first ==
  /// last`.
  iterator erase(const_iterator first, const_iterator last);

  void push_back(const value_type& value) { PW_ASSERT(try_push_back(value)); }

  void push_back(value_type&& value) {
    PW_ASSERT(try_push_back(std::move(value)));
  }

  template <typename... Args>
  void emplace_back(Args&&... args) {
    PW_ASSERT(try_emplace_back(std::forward<Args>(args)...));
  }

  void pop_back();

  void push_front(const value_type& value) { PW_ASSERT(try_push_front(value)); }

  void push_front(value_type&& value) {
    PW_ASSERT(try_push_front(std::move(value)));
  }

  template <typename... Args>
  void emplace_front(Args&&... args) {
    PW_ASSERT(try_emplace_front(std::forward<Args>(args)...));
  }

  void pop_front();

  /// Constructs an item in place at `pos`. Crashes if memory allocation
  /// fails.
  ///
  /// @returns an iterator to the emplaced item.
  template <typename... Args>
  iterator emplace(const_iterator pos, Args&&... args) {
    std::optional<iterator> result =
        try_emplace(pos, std::forward<Args>(args)...);
    PW_ASSERT(result.has_value());
    return *result;
  }

  /// Inserts a copy of an item at `pos`. Crashes if memory allocation fails.
  ///
  /// @returns an iterator to the inserted item.
  iterator insert(const_iterator pos, const value_type& value) {
    std::optional<iterator> result = try_insert(pos, value);
    PW_ASSERT(result.has_value());
    return *result;
  }

  /// Inserts an item at `pos` using move semanatics. Crashes if memory
  /// allocation fails.
  ///
  /// @returns an iterator to the inserted item.
  iterator insert(const_iterator pos, value_type&& value) {
    std::optional<iterator> result = try_insert(pos, std::move(value));
    PW_ASSERT(result.has_value());
    return *result;
  }

  /// Inserts `count` copies of `value` at `pos`. Crashes if memory allocation
  /// fails.
  ///
  /// @returns an iterator to the first inserted item.
  iterator insert(const_iterator pos,
                  size_type count,
                  const value_type& value) {
    std::optional<iterator> result = try_insert(pos, count, value);
    PW_ASSERT(result.has_value());
    return *result;
  }

  /// Inserts the contents of an iterator at `pos`. Crashes if memory
  /// allocation fails.
  ///
  /// @returns an iterator to the first inserted item.
  template <typename InputIt,
            typename = containers::internal::EnableIfInputIterator<InputIt>>
  iterator insert(const_iterator pos, InputIt first, InputIt last);

  /// Inserts an initializer list at `pos`. Crashes if memory allocation fails.
  ///
  /// @returns an iterator to the first inserted item.
  iterator insert(const_iterator pos, std::initializer_list<value_type> ilist) {
    std::optional<iterator> result = try_insert(pos, ilist);
    PW_ASSERT(result.has_value());
    return *result;
  }

  void resize(size_type new_size) { resize(new_size, value_type()); }

  void resize(size_type new_size, const value_type& value) {
    PW_ASSERT(try_resize(new_size, value));
  }

 protected:
  explicit constexpr GenericDeque(size_type initial_capacity) noexcept
      : GenericDequeBase<SizeType>(initial_capacity) {}

  constexpr void DestroyAll() {
    if constexpr (!std::is_trivially_destructible_v<value_type>) {
      auto [first, second] = contiguous_data();
      std::destroy(first.begin(), first.end());
      std::destroy(second.begin(), second.end());
    }
  }

  // Infallible assignment operators

  // NOLINTBEGIN(misc-unconventional-assign-operator);
  Derived& operator=(const std::initializer_list<value_type>& list) {
    assign(list);
    return derived();
  }

  template <typename T, typename = containers::internal::EnableIfIterable<T>>
  Derived& operator=(const T& other) {
    assign(other.begin(), other.end());
    return derived();
  }
  // NOLINTEND(misc-unconventional-assign-operator);

  // Fallible assign

  /// Attempts to replace the contents with `count` copies of `value`. If `count
  /// > capacity()` and allocation is supported, attempts to allocate to
  /// increase capacity. Does nothing if unable to accommodate `count` items.
  [[nodiscard]] bool try_assign(size_type count, const value_type& value);

  /// Replaces the container with copies of items from an iterator. Does nothing
  /// if unable to accommodate all items.
  ///
  /// `try_assign()` requires a forward iterator so that the capacity can be
  /// checked upfront to avoid partial assignments. Input iterators are only
  /// suitable for one pass, so could be exhausted by a `std::distance` check.
  template <typename It,
            int&...,
            typename = containers::internal::EnableIfForwardIterator<It>>
  [[nodiscard]] bool try_assign(It start, It finish);

  /// Replaces the container with copies of items from an initializer list. Does
  /// nothing if unable to accommodate all items.
  [[nodiscard]] bool try_assign(const std::initializer_list<value_type>& list) {
    return try_assign(list.begin(), list.end());
  }

  // Fallible modify

  /// Tries to construct an item in place at `pos`.
  ///
  /// @returns an `std::optional` iterator to the emplaced item. If memory
  ///          allocation fails, returns `std::nullopt` and the container is
  ///          left unchanged.
  template <typename... Args>
  [[nodiscard]] std::optional<iterator> try_emplace(const_iterator pos,
                                                    Args&&... args);

  /// Tries to insert a copy of an item at `pos`.
  ///
  /// @returns an `std::optional` iterator to the inserted item. If memory
  ///          allocation fails, returns `std::nullopt` and the container is
  ///          left unchanged.
  [[nodiscard]] std::optional<iterator> try_insert(const_iterator pos,
                                                   const value_type& value) {
    return try_emplace(pos, value);
  }

  /// Tries to insert an item at `pos` using move semanatics.
  ///
  /// @returns an `std::optional` iterator to the inserted item. If memory
  ///          allocation fails, returns `std::nullopt` and the container is
  ///          left unchanged.
  [[nodiscard]] std::optional<iterator> try_insert(const_iterator pos,
                                                   value_type&& value) {
    return try_emplace(pos, std::move(value));
  }

  /// Tries to insert `count` copies of `value` at `pos`.
  ///
  /// @returns an `std::optional` iterator to the first inserted item. If memory
  ///          allocation fails, returns `std::nullopt` and the container is
  ///          left unchanged.
  [[nodiscard]] std::optional<iterator> try_insert(const_iterator pos,
                                                   size_type count,
                                                   const value_type& value);

  /// Tries to insert the contents of an iterator at `pos`.
  ///
  /// @returns an `std::optional` iterator to the first inserted item. If memory
  ///          allocation fails, returns `std::nullopt` and the container is
  ///          left unchanged.
  template <typename ForwardIt,
            typename = containers::internal::EnableIfForwardIterator<ForwardIt>>
  [[nodiscard]] std::optional<iterator> try_insert(const_iterator pos,
                                                   ForwardIt first,
                                                   ForwardIt last);

  /// Tries to insert an initializer list at `pos`.
  ///
  /// @returns an `std::optional` iterator to the first inserted item. If memory
  ///          allocation fails, returns `std::nullopt` and the container is
  ///          left unchanged.
  [[nodiscard]] std::optional<iterator> try_insert(
      const_iterator pos, std::initializer_list<value_type> ilist) {
    return try_insert(pos, ilist.begin(), ilist.end());
  }

  [[nodiscard]] bool try_push_back(const value_type& value) {
    return try_emplace_back(value);
  }

  [[nodiscard]] bool try_push_back(value_type&& value) {
    return try_emplace_back(std::move(value));
  }

  template <typename... Args>
  [[nodiscard]] bool try_emplace_back(Args&&... args);

  [[nodiscard]] bool try_push_front(const value_type& value) {
    return try_emplace_front(value);
  }

  [[nodiscard]] bool try_push_front(value_type&& value) {
    return try_emplace_front(std::move(value));
  }

  template <typename... Args>
  [[nodiscard]] bool try_emplace_front(Args&&... args);

  [[nodiscard]] bool try_resize(size_type new_size) {
    return try_resize(new_size, value_type());
  }

  [[nodiscard]] bool try_resize(size_type new_size, const value_type& value);

  template <typename... Args>
  [[nodiscard]] std::optional<iterator> try_emplace_shift_right(
      const_iterator pos, Args&&... args);

  [[nodiscard]] std::optional<iterator> try_insert_shift_right(
      const_iterator pos, size_type count, const value_type& value);

  template <typename ForwardIt,
            typename = containers::internal::EnableIfForwardIterator<ForwardIt>>
  [[nodiscard]] std::optional<iterator> try_insert_shift_right(
      const_iterator pos, ForwardIt first, ForwardIt last);

 private:
  constexpr Derived& derived() { return static_cast<Derived&>(*this); }
  constexpr const Derived& derived() const {
    return static_cast<const Derived&>(*this);
  }

  constexpr size_type head() const { return Base::head_; }
  constexpr size_type tail() const { return Base::tail_; }

  // Accessed the underlying array in the derived class.
  constexpr pointer data() { return derived().data(); }
  constexpr const_pointer data() const { return derived().data(); }

  // Creates an uninitialized slot of `new_items` at `insert_index`.
  bool ShiftForInsert(size_type insert_index, size_type new_items);

  // Called by ShiftForInsert depending on where the insert happens.
  void ShiftLeft(size_type insert_index, size_type new_items);
  void ShiftRight(size_type insert_index, size_type new_items);

  // Make sure the container can hold count additional items.
  bool CheckCapacityAdd(size_type count) {
    size_type new_size;
    return CheckedAdd(size(), count, new_size) && CheckCapacity(new_size);
  }

  // Ensures that the container can hold at least this many elements.
  constexpr bool CheckCapacity(size_type new_size) {
    if constexpr (Derived::kFixedCapacity) {
      return new_size <= capacity();
    } else {
      return derived().try_reserve(new_size);
    }
  }

  // Appends items without checking the capacity. Capacity MUST be large enough.
  template <typename... Args>
  void EmplaceBackUnchecked(Args&&... args) {
    new (&data()[tail()]) value_type(std::forward<Args>(args)...);
    Base::PushBack();
  }
};

// Function implementations

template <typename Derived, typename ValueType, typename SizeType>
template <typename It, int&..., typename>
void GenericDeque<Derived, ValueType, SizeType>::assign(It start, It finish) {
  // Can't safely check std::distance for InputIterator, so use push_back().
  if constexpr (Derived::kFixedCapacity ||
                std::is_same_v<
                    typename std::iterator_traits<It>::iterator_category,
                    std::input_iterator_tag>) {
    clear();
    while (start != finish) {
      push_back(*start);
      ++start;
    }
  } else {
    PW_ASSERT(try_assign(start, finish));
  }
}

template <typename Derived, typename ValueType, typename SizeType>
bool GenericDeque<Derived, ValueType, SizeType>::try_assign(
    size_type count, const value_type& value) {
  if (!CheckCapacity(count)) {
    return false;
  }
  clear();
  Base::PushBack(count);
  std::uninitialized_fill_n(data(), count, value);
  return true;
}

template <typename Derived, typename ValueType, typename SizeType>
template <typename It, int&..., typename>
bool GenericDeque<Derived, ValueType, SizeType>::try_assign(It start,
                                                            It finish) {
  // Requires at least a forward iterator to safely check std::distance.
  static_assert(std::is_convertible_v<
                typename std::iterator_traits<It>::iterator_category,
                std::forward_iterator_tag>);
  const auto items = std::distance(start, finish);
  PW_DASSERT(items >= 0);
  if (static_cast<std::make_unsigned_t<decltype(items)>>(items) >
      std::numeric_limits<size_type>::max()) {
    return false;
  }
  const size_type count = static_cast<size_type>(items);
  if (!CheckCapacity(count)) {
    return false;
  }

  clear();
  Base::PushBack(count);
  std::uninitialized_move(start, finish, data());
  return true;
}

template <typename Derived, typename ValueType, typename SizeType>
constexpr std::pair<span<const ValueType>, span<const ValueType>>
GenericDeque<Derived, ValueType, SizeType>::contiguous_data() const {
  if (empty()) {
    return {span<const value_type>(), span<const value_type>()};
  }
  if (tail() > head()) {
    // If the newest entry is after the oldest entry, we have not wrapped:
    //     [  |head()|...more_entries...|tail()|  ]
    return {span<const value_type>(&data()[head()], size()),
            span<const value_type>()};
  } else {
    // If the newest entry is before or at the oldest entry and we know we are
    // not empty, ergo we have wrapped:
    //     [..more_entries...|tail()|  |head()|...more_entries...]
    return {span<const value_type>(&data()[head()], capacity() - head()),
            span<const value_type>(&data()[0], tail())};
  }
}

template <typename Derived, typename ValueType, typename SizeType>
template <typename... Args>
bool GenericDeque<Derived, ValueType, SizeType>::try_emplace_back(
    Args&&... args) {
  if (!CheckCapacityAdd(1)) {
    return false;
  }
  EmplaceBackUnchecked(std::forward<Args>(args)...);
  return true;
}

template <typename Derived, typename ValueType, typename SizeType>
void GenericDeque<Derived, ValueType, SizeType>::pop_back() {
  PW_ASSERT(!empty());
  if constexpr (!std::is_trivially_destructible_v<value_type>) {
    std::destroy_at(&back());
  }
  Base::PopBack();
}

template <typename Derived, typename ValueType, typename SizeType>
template <typename... Args>
bool GenericDeque<Derived, ValueType, SizeType>::try_emplace_front(
    Args&&... args) {
  if (!CheckCapacityAdd(1)) {
    return false;
  }
  Base::PushFront();
  new (&data()[head()]) value_type(std::forward<Args>(args)...);
  return true;
}

template <typename Derived, typename ValueType, typename SizeType>
void GenericDeque<Derived, ValueType, SizeType>::pop_front() {
  PW_ASSERT(!empty());
  if constexpr (!std::is_trivially_destructible_v<value_type>) {
    std::destroy_at(&front());
  }
  Base::PopFront();
}

template <typename Derived, typename ValueType, typename SizeType>
bool GenericDeque<Derived, ValueType, SizeType>::try_resize(
    size_type new_size, const value_type& value) {
  if (size() < new_size) {
    if (!CheckCapacity(new_size)) {
      return false;
    }
    const size_type new_items = new_size - size();
    for (size_type i = 0; i < new_items; ++i) {
      EmplaceBackUnchecked(value);
    }
  } else {
    while (size() > new_size) {
      pop_back();
    }
  }
  return true;
}

template <typename Derived, typename ValueType, typename SizeType>
typename GenericDeque<Derived, ValueType, SizeType>::iterator
GenericDeque<Derived, ValueType, SizeType>::erase(const_iterator first,
                                                  const_iterator last) {
  PW_DASSERT(first <= last);
  const iterator first_it(derived(), first.pos_);
  const iterator last_it(derived(), last.pos_);

  const size_type items_to_erase = static_cast<size_type>(last - first);
  if (items_to_erase == 0) {
    return first_it;
  }

  const size_type items_after = static_cast<size_type>(size() - last.pos_);
  // Shift head forward or tail backwards -- whichever involves fewer moves.
  if (first.pos_ < items_after) {  // fewer before than after
    std::move_backward(begin(), first_it, last_it);
    if constexpr (!std::is_trivially_destructible_v<value_type>) {
      std::destroy(begin(), begin() + items_to_erase);
    }
    Base::head_ = Base::AbsoluteIndex(items_to_erase);
  } else {  // fewer after than before
    std::move(last_it, end(), first_it);
    if constexpr (!std::is_trivially_destructible_v<value_type>) {
      std::destroy(first_it + items_after, end());
    }
    Base::tail_ = Base::AbsoluteIndex(first.pos_ + items_after);
  }
  Base::count_ = size() - items_to_erase;
  return first_it;
}

template <typename Derived, typename ValueType, typename SizeType>
template <typename... Args>
std::optional<typename GenericDeque<Derived, ValueType, SizeType>::iterator>
GenericDeque<Derived, ValueType, SizeType>::try_emplace(const_iterator pos,
                                                        Args&&... args) {
  // Insert in the middle of the deque, shifting other items out of the way.
  if (!ShiftForInsert(pos.pos_, 1)) {
    return std::nullopt;
  }
  iterator it(derived(), pos.pos_);
  new (std::addressof(*it)) value_type(std::forward<Args>(args)...);
  return it;
}

template <typename Derived, typename ValueType, typename SizeType>
bool GenericDeque<Derived, ValueType, SizeType>::ShiftForInsert(
    const size_type insert_index, size_type new_items) {
  if (!CheckCapacityAdd(new_items)) {
    return false;
  }

  if (insert_index < size() / 2) {  // Fewer items before than after.
    ShiftLeft(insert_index, new_items);
  } else {
    ShiftRight(insert_index, new_items);
  }
  return true;
}

template <typename Derived, typename ValueType, typename SizeType>
void GenericDeque<Derived, ValueType, SizeType>::ShiftLeft(
    const size_type insert_index, size_type new_items) {
  Base::PushFront(new_items);
  iterator original_begin(derived(), new_items);

  const size_type move_to_new_slots = std::min(new_items, insert_index);
  auto [next_src, next_dst] =
      std::uninitialized_move_n(original_begin, move_to_new_slots, begin());

  const size_type move_to_existing_slots = insert_index - move_to_new_slots;
  std::move(next_src, next_src + move_to_existing_slots, next_dst);

  // For consistency, ensure the whole opening is uninitialized.
  // TODO: b/429231491 - Consider having callers handle moved vs uninitialized
  // slots or doing the insert in this function.
  if constexpr (!std::is_trivially_destructible_v<value_type>) {
    std::destroy(
        iterator(derived(), std::max(insert_index, original_begin.pos_)),
        iterator(derived(), insert_index + new_items));
  }
}

template <typename Derived, typename ValueType, typename SizeType>
void GenericDeque<Derived, ValueType, SizeType>::ShiftRight(
    const size_type insert_index, size_type new_items) {
  const size_type items_after = size() - insert_index;
  iterator original_end = end();
  Base::PushBack(new_items);

  const size_type move_to_new_slots = std::min(new_items, items_after);
  std::uninitialized_move(original_end - move_to_new_slots,
                          original_end,
                          end() - move_to_new_slots);

  const size_type move_to_existing_slots = items_after - move_to_new_slots;
  iterator pos(derived(), insert_index);
  std::move_backward(pos, pos + move_to_existing_slots, original_end);

  // For consistency, ensure the whole opening is uninitialized.
  // TODO: b/429231491 - Consider having callers handle moved vs uninitialized
  // slots or doing the insert in this function.
  if constexpr (!std::is_trivially_destructible_v<value_type>) {
    std::destroy(
        iterator(derived(), insert_index),
        iterator(derived(),
                 std::min(static_cast<size_type>(insert_index + new_items),
                          original_end.pos_)));
  }
}

template <typename Derived, typename ValueType, typename SizeType>
std::optional<typename GenericDeque<Derived, ValueType, SizeType>::iterator>
GenericDeque<Derived, ValueType, SizeType>::try_insert(
    const_iterator pos, size_type count, const value_type& value) {
  if (count == 0) {
    return iterator(derived(), pos.pos_);
  }
  if (!ShiftForInsert(pos.pos_, count)) {
    return std::nullopt;
  }

  iterator it(derived(), pos.pos_);
  std::uninitialized_fill_n(it, count, value);
  return it;
}

template <typename Derived, typename ValueType, typename SizeType>
template <typename InputIt, typename>
typename GenericDeque<Derived, ValueType, SizeType>::iterator
GenericDeque<Derived, ValueType, SizeType>::insert(const_iterator pos,
                                                   InputIt first,
                                                   InputIt last) {
  // Can't safely check std::distance for InputIterator, so repeatedly emplace()
  if constexpr (std::is_same_v<
                    typename std::iterator_traits<InputIt>::iterator_category,
                    std::input_iterator_tag>) {
    // Inserting M items into an N-item deque is O(N*M) due to repeated
    // emplace() calls. DynamicDeque reads into a temporary deque instead.
    iterator insert_pos = iterator(derived(), pos.pos_);
    while (first != last) {
      insert_pos = emplace(insert_pos, *first);
      ++first;
      ++insert_pos;
    }
    return iterator(derived(), pos.pos_);
  } else {
    std::optional<iterator> result = try_insert(pos, first, last);
    PW_ASSERT(result.has_value());
    return *result;
  }
}

template <typename Derived, typename ValueType, typename SizeType>
template <typename ForwardIt, typename>
std::optional<typename GenericDeque<Derived, ValueType, SizeType>::iterator>
GenericDeque<Derived, ValueType, SizeType>::try_insert(const_iterator pos,
                                                       ForwardIt first,
                                                       ForwardIt last) {
  static_assert(std::is_convertible_v<
                typename std::iterator_traits<ForwardIt>::iterator_category,
                std::forward_iterator_tag>);
  const auto distance = std::distance(first, last);
  PW_DASSERT(distance >= 0);
  const size_type count = static_cast<size_type>(distance);

  const iterator it(derived(), pos.pos_);
  if (count == 0) {
    return it;
  }
  if (!ShiftForInsert(pos.pos_, count)) {
    return std::nullopt;
  }
  std::uninitialized_move(first, last, it);
  return it;
}

template <typename Derived, typename ValueType, typename SizeType>
template <typename... Args>
std::optional<typename GenericDeque<Derived, ValueType, SizeType>::iterator>
GenericDeque<Derived, ValueType, SizeType>::try_emplace_shift_right(
    const_iterator pos, Args&&... args) {
  if (!CheckCapacityAdd(1)) {
    return std::nullopt;
  }
  ShiftRight(pos.pos_, 1);
  iterator it(derived(), pos.pos_);
  new (std::addressof(*it)) value_type(std::forward<Args>(args)...);
  return it;
}

template <typename Derived, typename ValueType, typename SizeType>
std::optional<typename GenericDeque<Derived, ValueType, SizeType>::iterator>
GenericDeque<Derived, ValueType, SizeType>::try_insert_shift_right(
    const_iterator pos, size_type count, const value_type& value) {
  if (count == 0) {
    return iterator(derived(), pos.pos_);
  }
  if (!CheckCapacityAdd(count)) {
    return std::nullopt;
  }

  ShiftRight(pos.pos_, count);
  iterator it(derived(), pos.pos_);
  std::uninitialized_fill_n(it, count, value);
  return it;
}

template <typename Derived, typename ValueType, typename SizeType>
template <typename ForwardIt, typename>
std::optional<typename GenericDeque<Derived, ValueType, SizeType>::iterator>
GenericDeque<Derived, ValueType, SizeType>::try_insert_shift_right(
    const_iterator pos, ForwardIt first, ForwardIt last) {
  static_assert(std::is_convertible_v<
                typename std::iterator_traits<ForwardIt>::iterator_category,
                std::forward_iterator_tag>);
  const auto distance = std::distance(first, last);
  PW_DASSERT(distance >= 0);
  const size_type count = static_cast<size_type>(distance);

  const iterator it(derived(), pos.pos_);
  if (count == 0) {
    return it;
  }
  if (!CheckCapacityAdd(count)) {
    return std::nullopt;
  }
  ShiftRight(pos.pos_, count);
  std::uninitialized_move(first, last, it);
  return it;
}

}  // namespace pw::containers::internal
