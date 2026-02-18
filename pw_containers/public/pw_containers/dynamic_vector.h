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

#include <initializer_list>
#include <iterator>

#include "pw_allocator/allocator.h"
#include "pw_containers/dynamic_deque.h"
#include "pw_containers/ptr_iterator.h"

namespace pw {

/// @module{pw_containers}

/// @defgroup pw_containers_vectors Vectors
/// @{

/// Array-backed list similar to `std::vector`, but optimized for embedded.
///
/// Key features of `pw::DynamicVector`.
///
/// - Uses a `pw::Allocator` for memory operations.
/// - Provides the `std::vector` API, but adds `try_*` versions of
///   operations that crash on allocation failure.
///   - `assign()` & `try_assign()`.
///   - `push_back()` & `try_push_back()`
///   - `emplace_back()` & `try_emplace_back()`
///   - `resize()` & `try_resize()`.
/// - Offers `reserve()`/`try_reserve()` and `shrink_to_fit()` to manage memory
///   usage.
/// - Never allocates in the constructor. `constexpr` constructible.
/// - Compact representation when used with a `size_type` of `uint16_t`.
/// - Uses `pw::Allocator::Resize()` when possible to maximize efficiency.
///
/// @note `pw::DynamicVector` is currently implemented as a wrapper around
/// `pw::DynamicDeque`. Some operations are more expensive than they need to be,
/// and `DynamicVector` objects are larger than necessary. This overhead will be
/// eliminated in the future (see <a href="http://pwbug.dev/424613355">
/// b/424613355</a>).
template <typename T, typename SizeType = uint16_t>
class DynamicVector {
 public:
  using value_type = T;
  using size_type = SizeType;
  using reference = value_type&;
  using const_reference = const value_type&;
  using pointer = value_type*;
  using const_pointer = const value_type*;
  using iterator = containers::PtrIterator<DynamicVector>;
  using const_iterator = containers::ConstPtrIterator<DynamicVector>;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;
  using allocator_type = Allocator;

  /// Constructs an empty `DynamicVector` using the provided allocator.
  ///
  /// @param allocator The allocator to use for memory management.
  explicit constexpr DynamicVector(Allocator& allocator) : deque_(allocator) {}

  DynamicVector(const DynamicVector&) = delete;
  DynamicVector& operator=(const DynamicVector&) = delete;

  constexpr DynamicVector(DynamicVector&&) = default;
  constexpr DynamicVector& operator=(DynamicVector&&) = default;

  // Iterators

  iterator begin() { return iterator(data()); }
  const_iterator begin() const { return cbegin(); }
  const_iterator cbegin() const { return const_iterator(data()); }

  iterator end() { return iterator(data() + size()); }
  const_iterator end() const { return cend(); }
  const_iterator cend() const { return const_iterator(data() + size()); }

  reverse_iterator rbegin() { return reverse_iterator(end()); }
  const_reverse_iterator rbegin() const { return crbegin(); }
  const_reverse_iterator crbegin() const {
    return const_reverse_iterator(end());
  }

  reverse_iterator rend() { return reverse_iterator(begin()); }
  const_reverse_iterator rend() const { return crend(); }
  const_reverse_iterator crend() const {
    return const_reverse_iterator(begin());
  }

  // Capacity

  /// Returns the vector's allocator.
  constexpr allocator_type& get_allocator() const {
    return deque_.get_allocator();
  }

  /// Checks if the vector is empty.
  bool empty() const { return deque_.empty(); }

  /// Returns the number of elements in the vector.
  size_type size() const { return deque_.size(); }

  /// Returns the total number of elements that the vector can hold without
  /// requiring reallocation.
  size_type capacity() const { return deque_.capacity(); }

  /// Maximum possible value of `size()`, ignoring allocator limitations.
  size_type max_size() const { return deque_.max_size(); }

  /// Requests that the vector capacity be at least `new_capacity` elements.
  ///
  /// Crashes if allocation fails.
  ///
  /// @param new_capacity The minimum desired capacity.
  void reserve(size_type new_capacity) { deque_.reserve(new_capacity); }

  /// Requests that the vector capacity be exactly `new_capacity` elements.
  ///
  /// Crashes if allocation fails.
  ///
  /// @param new_capacity The minimum desired capacity.
  void reserve_exact(size_type new_capacity) {
    deque_.reserve_exact(new_capacity);
  }

  /// Attempts to request that the vector capacity be at least `new_capacity`
  /// elements.
  ///
  /// Returns `true` on success, or `false` if allocation fails.
  ///
  /// @param new_capacity The minimum desired capacity.
  /// @return `true` if successful, `false` otherwise.
  [[nodiscard]] bool try_reserve(size_type new_capacity) {
    return deque_.try_reserve(new_capacity);
  }

  /// Attempts to set the vector capacity to exactly `new_capacity` elements.
  ///
  /// Returns `true` on success, or `false` if allocation fails.
  ///
  /// @param new_capacity The exact desired capacity.
  /// @return `true` if successful, `false` otherwise.
  [[nodiscard]] bool try_reserve_exact(size_type new_capacity) {
    return deque_.try_reserve_exact(new_capacity);
  }

  /// Reduces memory usage by releasing unused capacity.
  void shrink_to_fit() { deque_.shrink_to_fit(); }

  // Element access

  /// Returns a reference to the element at specified location `pos`.
  ///
  /// No bounds checking is performed.
  ///
  /// @param pos The index of the element to access.
  /// @return Reference to the element.
  reference operator[](size_type pos) { return data()[pos]; }

  /// Returns a const reference to the element at specified location `pos`.
  ///
  /// No bounds checking is performed.
  ///
  /// @param pos The index of the element to access.
  /// @return Const reference to the element.
  const_reference operator[](size_type pos) const { return data()[pos]; }

  /// Returns a reference to the element at specified location `pos`, with
  /// bounds checking.
  ///
  /// Crashes if `pos` is not within the range `[0, size())`.
  ///
  /// @param pos The index of the element to access.
  /// @return Reference to the element.
  reference at(size_type pos) { return deque_.at(pos); }

  /// Returns a const reference to the element at specified location `pos`, with
  /// bounds checking.
  ///
  /// Crashes if `pos` is not within the range `[0, size())`.
  ///
  /// @param pos The index of the element to access.
  /// @return Const reference to the element.
  const_reference at(size_type pos) const { return deque_.at(pos); }

  /// Returns a reference to the first element in the vector.
  ///
  /// Calling `front()` on an empty vector is undefined behavior.
  reference front() { return deque_.front(); }

  /// Returns a const reference to the first element in the vector.
  ///
  /// Calling `front()` on an empty vector is undefined behavior.
  const_reference front() const { return deque_.front(); }

  /// Returns a reference to the last element in the vector.
  ///
  /// Calling `back()` on an empty vector is undefined behavior.
  reference back() { return deque_.back(); }

  /// Returns a const reference to the last element in the vector.
  ///
  /// Calling `back()` on an empty vector is undefined behavior.
  const_reference back() const { return deque_.back(); }

  /// Returns a pointer to the underlying array serving as element storage.
  ///
  /// The pointer is such that `[data(), data() + size())` is a valid range.
  ///
  /// @return A pointer to the first element, or `nullptr` if the vector is
  /// empty.
  pointer data() { return deque_.data(); }

  /// Returns a const pointer to the underlying array serving as element
  /// storage.
  ///
  /// @return A const pointer to the first element, or `nullptr` if the vector
  /// is empty.
  const_pointer data() const { return deque_.data(); }

  // Modifiers

  /// Assigns new contents to the vector, replacing its current contents.
  ///
  /// @param count The number of elements to assign.
  /// @param value The value to copy.
  /// @return `true` if successful, `false` otherwise.
  void assign(size_type count, const value_type& value) {
    deque_.assign(count, value);
  }

  [[nodiscard]] bool try_assign(size_type count, const value_type& value) {
    return deque_.try_assign(count, value);
  }

  /// Assigns new contents to the vector from an initializer list.
  ///
  /// @param init The initializer list to copy elements from.
  /// @return `true` if successful, `false` otherwise.
  void assign(std::initializer_list<T> init) { deque_.assign(init); }

  [[nodiscard]] bool try_assign(std::initializer_list<T> init) {
    return deque_.try_assign(init);
  }

  /// Adds an element to the back of the vector.
  ///
  /// Note: This operation is potentially fallible if memory allocation is
  /// required and fails. Use `try_push_back()` for a fallible version.
  void push_back(const value_type& value) { deque_.push_back(value); }

  /// Adds an element to the back of the vector (move version).
  ///
  /// Note: This operation is potentially fallible if memory allocation is
  /// required and fails.
  void push_back(value_type&& value) { deque_.push_back(std::move(value)); }

  /// Attempts to add an element to the back of the vector.
  ///
  /// Returns `true` on success, or `false` if allocation fails.
  ///
  /// @param value The value to add.
  /// @return `true` if successful, `false` otherwise.
  [[nodiscard]] bool try_push_back(const value_type& value) {
    return deque_.try_push_back(value);
  }

  /// Attempts to add an element to the back of the vector (move version).
  ///
  /// Returns `true` on success, or `false` if allocation fails.
  ///
  /// @param value The value to add.
  /// @return `true` if successful, `false` otherwise.
  [[nodiscard]] bool try_push_back(value_type&& value) {
    return deque_.try_push_back(std::move(value));
  }

  /// Removes the last element from the vector.
  ///
  /// Calling `pop_back()` on an empty vector is undefined behavior.
  void pop_back() { deque_.pop_back(); }

  /// Constructs an element in place at the back of the vector.
  ///
  /// Note: This operation is potentially fallible if memory allocation is
  /// required and fails. Use `try_emplace_back()` for a fallible version.
  template <typename... Args>
  void emplace_back(Args&&... args) {
    deque_.emplace_back(std::forward<Args>(args)...);
  }

  /// Attempts to construct an element in place at the back of the vector.
  ///
  /// Returns `true` on success, or `false` if allocation fails.
  ///
  /// @param args Arguments to forward to the element's constructor.
  /// @return `true` if successful, `false` otherwise.
  template <typename... Args>
  [[nodiscard]] bool try_emplace_back(Args&&... args) {
    return deque_.try_emplace_back(std::forward<Args>(args)...);
  }

  /// Constructs an element in-place at the specified position.
  ///
  /// Crashes if allocation fails.
  ///
  /// @param pos The position at which to construct the element.
  /// @param args Arguments to forward to the element's constructor.
  /// @return Iterator to the emplaced element.
  template <typename... Args>
  iterator emplace(const_iterator pos, Args&&... args) {
    auto deque_it = deque_.try_emplace_shift_right(ToDequeIterator(pos),
                                                   std::forward<Args>(args)...);
    PW_ASSERT(deque_it.has_value());
    return iterator(data() + deque_it->pos_);
  }

  /// Attempts to construct an element in-place at the specified position.
  ///
  /// Returns an iterator to the new element on success, or `std::nullopt` if
  /// allocation fails.
  ///
  /// @param pos The position at which to construct the element.
  /// @param args Arguments to forward to the element's constructor.
  /// @return Iterator to the emplaced element, or `std::nullopt` on failure.
  template <typename... Args>
  [[nodiscard]] std::optional<iterator> try_emplace(const_iterator pos,
                                                    Args&&... args) {
    auto deque_it = deque_.try_emplace_shift_right(ToDequeIterator(pos),
                                                   std::forward<Args>(args)...);
    if (!deque_it.has_value()) {
      return std::nullopt;
    }
    return iterator(data() + deque_it->pos_);
  }

  /// Inserts an element at the specified position.
  ///
  /// Crashes if allocation fails.
  ///
  /// @param pos The position at which to insert the element.
  /// @param value The value to insert.
  /// @return Iterator to the inserted element.
  iterator insert(const_iterator pos, const T& value) {
    return emplace(pos, value);
  }

  /// Inserts an element at the specified position (move version).
  ///
  /// Crashes if allocation fails.
  ///
  /// @param pos The position at which to insert the element.
  /// @param value The value to insert.
  /// @return Iterator to the inserted element.
  iterator insert(const_iterator pos, T&& value) {
    return emplace(pos, std::move(value));
  }

  /// Inserts multiple copies of an element at the specified position.
  ///
  /// Crashes if allocation fails.
  ///
  /// @param pos The position at which to insert the elements.
  /// @param count The number of elements to insert.
  /// @param value The value to insert.
  /// @return Iterator to the first inserted element.
  iterator insert(const_iterator pos, size_type count, const T& value) {
    auto deque_it =
        deque_.try_insert_shift_right(ToDequeIterator(pos), count, value);
    PW_ASSERT(deque_it.has_value());
    return iterator(data() + deque_it->pos_);
  }

  /// Inserts elements from an iterator range at the specified position.
  ///
  /// Crashes if allocation fails.
  ///
  /// @param pos The position at which to insert the elements.
  /// @param first The beginning of the range to insert.
  /// @param last The end of the range to insert.
  /// @return Iterator to the first inserted element.
  template <typename InputIt>
  iterator insert(const_iterator pos, InputIt first, InputIt last) {
    auto deque_it =
        deque_.try_insert_shift_right(ToDequeIterator(pos), first, last);
    PW_ASSERT(deque_it.has_value());
    return iterator(data() + deque_it->pos_);
  }

  /// Inserts elements from an initializer list at the specified position.
  ///
  /// Crashes if allocation fails.
  ///
  /// @param pos The position at which to insert the elements.
  /// @param ilist The initializer list to insert.
  /// @return Iterator to the first inserted element.
  iterator insert(const_iterator pos, std::initializer_list<T> ilist) {
    return insert(pos, ilist.begin(), ilist.end());
  }

  /// Attempts to insert an element at the specified position.
  ///
  /// Returns an iterator to the new element on success, or `std::nullopt` if
  /// allocation fails.
  ///
  /// @param pos The position at which to insert the element.
  /// @param value The value to insert.
  /// @return Iterator to the inserted element, or `std::nullopt` on failure.
  [[nodiscard]] std::optional<iterator> try_insert(const_iterator pos,
                                                   const T& value) {
    return try_emplace(pos, value);
  }

  /// Attempts to insert an element at the specified position (move version).
  ///
  /// Returns an iterator to the new element on success, or `std::nullopt` if
  /// allocation fails.
  ///
  /// @param pos The position at which to insert the element.
  /// @param value The value to insert.
  /// @return Iterator to the inserted element, or `std::nullopt` on failure.
  [[nodiscard]] std::optional<iterator> try_insert(const_iterator pos,
                                                   T&& value) {
    return try_emplace(pos, std::move(value));
  }

  /// Attempts to insert multiple copies of an element at the specified
  /// position.
  ///
  /// Returns an iterator to the first new element on success, or `std::nullopt`
  /// if allocation fails.
  ///
  /// @param pos The position at which to insert the elements.
  /// @param count The number of elements to insert.
  /// @param value The value to insert.
  /// @return Iterator to the first inserted element, or `std::nullopt` on
  /// failure.
  [[nodiscard]] std::optional<iterator> try_insert(const_iterator pos,
                                                   size_type count,
                                                   const T& value) {
    auto deque_it =
        deque_.try_insert_shift_right(ToDequeIterator(pos), count, value);
    if (!deque_it.has_value()) {
      return std::nullopt;
    }
    return iterator(data() + deque_it->pos_);
  }

  /// Attempts to insert elements from an iterator range at the specified
  /// position.
  ///
  /// Returns an iterator to the first new element on success, or `std::nullopt`
  /// if allocation fails.
  ///
  /// @param pos The position at which to insert the elements.
  /// @param first The beginning of the range to insert.
  /// @param last The end of the range to insert.
  /// @return Iterator to the first inserted element, or `std::nullopt` on
  /// failure.
  template <typename InputIt>
  [[nodiscard]] std::optional<iterator> try_insert(const_iterator pos,
                                                   InputIt first,
                                                   InputIt last) {
    auto deque_it =
        deque_.try_insert_shift_right(ToDequeIterator(pos), first, last);
    if (!deque_it.has_value()) {
      return std::nullopt;
    }
    return iterator(data() + deque_it->pos_);
  }

  /// Attempts to insert elements from an initializer list at the specified
  /// position.
  ///
  /// Returns an iterator to the first new element on success, or `std::nullopt`
  /// if allocation fails.
  ///
  /// @param pos The position at which to insert the elements.
  /// @param ilist The initializer list to insert.
  /// @return Iterator to the first inserted element, or `std::nullopt` on
  /// failure.
  [[nodiscard]] std::optional<iterator> try_insert(
      const_iterator pos, std::initializer_list<T> ilist) {
    return try_insert(pos, ilist.begin(), ilist.end());
  }

  // TODO: b/424613355 - Implement insert, emplace

  /// Erases the specified element from the vector.
  ///
  /// @param pos Iterator to the element to remove.
  /// @return Iterator following the last removed element.
  iterator erase(const_iterator pos) { return erase(pos, pos + 1); }

  /// Erases the specified range of elements from the vector.
  ///
  /// @param first The first element to erase.
  /// @param last The last element to erase.
  /// @return Iterator following the last removed element.
  iterator erase(const_iterator first, const_iterator last) {
    auto first_ptr = const_cast<pointer>(&*first);
    if (first != last) {
      std::move(const_cast<pointer>(&*last), data() + size(), first_ptr);
      size_t items_to_pop = static_cast<SizeType>(last - first);
      for (size_t i = 0; i < items_to_pop; ++i) {
        deque_.pop_back();
      }
    }
    return iterator(first_ptr);
  }

  /// Resizes the vector to contain `count` elements.
  ///
  /// If `count` is smaller than the current size, the content is reduced to
  /// the first `count` elements. If `count` is greater than the current size,
  /// new elements are appended and default-constructed.
  ///
  /// @param count New size of the vector.
  void resize(size_type count) { deque_.resize(count); }

  /// Resizes the vector to contain `count` elements, value-initializing new
  /// elements.
  ///
  /// If `count` is smaller than the current size, the content is reduced to
  /// the first `count` elements. If `count` is greater than the current size,
  /// new elements are appended and copy-constructed from `value`.
  ///
  /// @param count New size of the vector.
  /// @param value Value to initialize new elements with.
  void resize(size_type count, const value_type& value) {
    deque_.resize(count, value);
  }

  /// Attempts to resize the vector to contain `count` elements.
  ///
  /// @param count New size of the vector.
  /// @return `true` if successful, `false` otherwise.
  [[nodiscard]] bool try_resize(size_type count) {
    return deque_.try_resize(count);
  }

  /// Attempts to resize the vector to contain `count` elements,
  /// value-initializing new elements.
  ///
  /// @param count New size of the vector.
  /// @param value Value to initialize new elements with.
  /// @return `true` if successful, `false` otherwise.
  [[nodiscard]] bool try_resize(size_type count, const value_type& value) {
    return deque_.try_resize(count, value);
  }

  /// Removes all elements from the vector.
  void clear() { deque_.clear(); }

  /// Swaps the contents with another DynamicVector.
  ///
  /// @param other The other vector to swap with.
  void swap(DynamicVector& other) { deque_.swap(other.deque_); }

 private:
  typename DynamicDeque<T, size_type>::const_iterator ToDequeIterator(
      const_iterator it) const {
    return {deque_, static_cast<size_type>(it - cbegin())};
  }

  // The underlying DynamicDeque instance that provides the storage.
  DynamicDeque<T, size_type> deque_;
};

}  // namespace pw
