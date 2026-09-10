// Copyright 2026 The Pigweed Authors
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
#include <limits>
#include <type_traits>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_assert/assert.h"
#include "pw_containers/intrusive_forward_list.h"

namespace pw {

/// @submodule{pw_containers,lists}

/// A singly-linked list that owns its elements and allocates them using a
/// `pw::Allocator`. It provides an interface similar to `std::forward_list`.
///
/// `ForwardList` is an owning wrapper around `IntrusiveForwardList` that
/// manages the lifetime and dynamic memory allocation of its elements. It
/// provides both asserting operations (such as `push_front` and `resize`) and
/// fallible `try_*` operations (such as `try_push_front` and `try_resize`) for
/// allocation-failure handling without exceptions.
///
/// @warning The container's allocator MUST outlive the container.
///
/// @tparam T The type of elements in the list.
template <typename T>
class ForwardList {
 public:
  using value_type = T;
  using allocator_type = Allocator;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using reference = value_type&;
  using const_reference = const value_type&;
  using pointer = value_type*;
  using const_pointer = const value_type*;

 private:
  // Node type used internally.
  using Node = IntrusiveForwardListItem<T>;
  using InternalList = IntrusiveForwardList<Node>;

  // Iterator wrapper.
  template <bool kIsConst>
  class Iterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using iterator_concept = std::forward_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = std::conditional_t<kIsConst, const T*, T*>;
    using reference = std::conditional_t<kIsConst, const T&, T&>;

    using BaseIterator = typename InternalList::iterator;

    constexpr Iterator() = default;
    constexpr explicit Iterator(BaseIterator it) noexcept : it_(it) {}

    // Convert non-const iterator to const iterator.
    template <bool kOtherConst,
              typename = std::enable_if_t<kIsConst && !kOtherConst>>
    constexpr Iterator(const Iterator<kOtherConst>& other) noexcept
        : it_(other.it_) {}

    reference operator*() const noexcept {
      return const_cast<Node&>(*it_).item();
    }

    pointer operator->() const noexcept {
      return &const_cast<Node&>(*it_).item();
    }

    Iterator& operator++() noexcept {
      ++it_;
      return *this;
    }

    Iterator operator++(int) noexcept {
      Iterator copy = *this;
      ++it_;
      return copy;
    }

    template <bool kOtherConst>
    friend constexpr bool operator==(Iterator lhs,
                                     Iterator<kOtherConst> rhs) noexcept {
      return lhs.it_ == rhs.it_;
    }

    template <bool kOtherConst>
    friend constexpr bool operator!=(Iterator lhs,
                                     Iterator<kOtherConst> rhs) noexcept {
      return lhs.it_ != rhs.it_;
    }

   private:
    friend class ForwardList;
    BaseIterator it_;
  };

 public:
  using iterator = Iterator<false>;
  using const_iterator = Iterator<true>;

  /// Constructs an empty `ForwardList` backed by the provided allocator.
  ///
  /// @param[in] allocator The allocator to use for node allocations.
  explicit constexpr ForwardList(Allocator& allocator) noexcept
      : allocator_(&allocator) {}

  /// Destroys the `ForwardList` and deallocates all stored elements.
  ~ForwardList() { clear(); }

  ForwardList(const ForwardList&) = delete;
  ForwardList& operator=(const ForwardList&) = delete;

  /// Moves the elements and allocator from `other` into this list.
  ForwardList(ForwardList&& other) noexcept
      : list_(std::move(other.list_)), allocator_(other.allocator_) {}

  /// Move-assigns `other` into this list, clearing existing elements first.
  ForwardList& operator=(ForwardList&& other) noexcept {
    if (this != &other) {
      clear();
      list_ = std::move(other.list_);
      allocator_ = other.allocator_;
    }
    return *this;
  }

  // Iterators

  /// Returns an iterator to the element before the first element of the list.
  iterator before_begin() noexcept { return iterator(list_.before_begin()); }

  /// Returns a const iterator to the element before the first element of the
  /// list.
  const_iterator before_begin() const noexcept {
    return const_iterator(const_cast<InternalList&>(list_).before_begin());
  }

  /// Returns a const iterator to the element before the first element of the
  /// list.
  const_iterator cbefore_begin() const noexcept { return before_begin(); }

  /// Returns an iterator to the first element in the list.
  iterator begin() noexcept { return iterator(list_.begin()); }

  /// Returns a const iterator to the first element in the list.
  const_iterator begin() const noexcept {
    return const_iterator(const_cast<InternalList&>(list_).begin());
  }

  /// Returns a const iterator to the first element in the list.
  const_iterator cbegin() const noexcept { return begin(); }

  /// Returns an iterator to the end of the list.
  iterator end() noexcept { return iterator(list_.end()); }

  /// Returns a const iterator to the end of the list.
  const_iterator end() const noexcept {
    return const_iterator(const_cast<InternalList&>(list_).end());
  }

  /// Returns a const iterator to the end of the list.
  const_iterator cend() const noexcept { return end(); }

  // Allocator & Capacity

  /// Returns a reference to the allocator associated with this container.
  allocator_type& get_allocator() const noexcept { return *allocator_; }

  /// Checks whether the container is empty.
  [[nodiscard]] bool empty() const noexcept { return list_.empty(); }

  /// Returns the maximum possible number of elements the list can hold.
  constexpr size_type max_size() const noexcept {
    return static_cast<size_type>(std::numeric_limits<difference_type>::max());
  }

  // Access

  /// Returns a reference to the first element.
  reference front() noexcept {
    PW_DASSERT(!empty());
    return static_cast<Node&>(list_.front()).item();
  }

  /// Returns a const reference to the first element.
  const_reference front() const noexcept {
    PW_DASSERT(!empty());
    return static_cast<const Node&>(list_.front()).item();
  }

  // Modifiers

  /// Constructs an element in-place at the beginning of the list.
  /// Crashes if allocation fails.
  template <typename... Args>
  reference emplace_front(Args&&... args) {
    PW_ASSERT(try_emplace_front(std::forward<Args>(args)...));
    return front();
  }

  /// Attempts to construct an element in-place at the beginning of the list.
  ///
  /// @returns `true` if allocation succeeded, `false` otherwise.
  template <typename... Args>
  [[nodiscard]] bool try_emplace_front(Args&&... args) {
    Node* node = allocator_->New<Node>(std::forward<Args>(args)...);
    if (node == nullptr) {
      return false;
    }
    list_.push_front(*node);
    return true;
  }

  /// Prepends the given element to the beginning of the list.
  /// Crashes if allocation fails.
  void push_front(const T& value) { PW_ASSERT(try_push_front(value)); }

  /// Prepends the given element to the beginning of the list via move.
  /// Crashes if allocation fails.
  void push_front(T&& value) { PW_ASSERT(try_push_front(std::move(value))); }

  /// Attempts to prepend the given element to the beginning of the list.
  ///
  /// @returns `true` if allocation succeeded, `false` otherwise.
  [[nodiscard]] bool try_push_front(const T& value) {
    return try_emplace_front(value);
  }

  /// Attempts to prepend the given element to the beginning of the list via
  /// move.
  ///
  /// @returns `true` if allocation succeeded, `false` otherwise.
  [[nodiscard]] bool try_push_front(T&& value) {
    return try_emplace_front(std::move(value));
  }

  /// Removes the first element of the container. If the container is empty,
  /// this function does nothing.
  void pop_front() noexcept {
    if (empty()) {
      return;
    }
    Node* node = &static_cast<Node&>(list_.front());
    list_.pop_front();
    allocator_->Delete(node);
  }

  /// Erases all elements from the container and frees their allocated nodes.
  void clear() noexcept {
    while (!empty()) {
      pop_front();
    }
  }

  /// Exchanges the contents and allocator of the container with those of
  /// `other`.
  void swap(ForwardList& other) noexcept {
    list_.swap(other.list_);
    std::swap(allocator_, other.allocator_);
  }

  friend void swap(ForwardList& lhs, ForwardList& rhs) noexcept {
    lhs.swap(rhs);
  }

  /// Constructs an element in-place directly after `pos`.
  /// Crashes if allocation fails.
  ///
  /// @returns An iterator pointing to the newly inserted element.
  template <typename... Args>
  iterator emplace_after(const_iterator pos, Args&&... args) {
    Node* node = allocator_->New<Node>(std::forward<Args>(args)...);
    PW_ASSERT(node != nullptr);
    return iterator(list_.insert_after(pos.it_, *node));
  }

  /// Attempts to construct an element in-place directly after `pos`.
  ///
  /// @returns `true` on success, `false` if allocation failed.
  template <typename... Args>
  [[nodiscard]] bool try_emplace_after(const_iterator pos, Args&&... args) {
    Node* node = allocator_->New<Node>(std::forward<Args>(args)...);
    if (node == nullptr) {
      return false;
    }
    list_.insert_after(pos.it_, *node);
    return true;
  }

  /// Inserts a copy of `value` after `pos`.
  /// Crashes if allocation fails.
  iterator insert_after(const_iterator pos, const T& value) {
    return emplace_after(pos, value);
  }

  /// Inserts `value` after `pos` via move.
  /// Crashes if allocation fails.
  iterator insert_after(const_iterator pos, T&& value) {
    return emplace_after(pos, std::move(value));
  }

  /// Attempts to insert a copy of `value` after `pos`.
  [[nodiscard]] bool try_insert_after(const_iterator pos, const T& value) {
    return try_emplace_after(pos, value);
  }

  /// Attempts to insert `value` after `pos` via move.
  [[nodiscard]] bool try_insert_after(const_iterator pos, T&& value) {
    return try_emplace_after(pos, std::move(value));
  }

  /// Inserts `count` copies of `value` after `pos`.
  /// Crashes if allocation fails.
  iterator insert_after(const_iterator pos, size_type count, const T& value) {
    iterator it = iterator(pos.it_);
    for (size_type i = 0; i < count; ++i) {
      it = insert_after(it, value);
    }
    return it;
  }

  /// Attempts to insert `count` copies of `value` after `pos`. If allocation
  /// fails, all partially inserted elements are erased (strong guarantee).
  [[nodiscard]] bool try_insert_after(const_iterator pos,
                                      size_type count,
                                      const T& value) {
    iterator it = iterator(pos.it_);
    for (size_type i = 0; i < count; ++i) {
      Node* node = allocator_->New<Node>(value);
      if (node == nullptr) {
        erase_after(pos, std::next(it));
        return false;
      }
      it = iterator(list_.insert_after(it.it_, *node));
    }
    return true;
  }

  /// Inserts elements from range `[first, last)` after `pos`.
  /// Crashes if allocation fails.
  template <typename InputIt,
            typename = std::enable_if_t<!std::is_integral_v<InputIt>>>
  iterator insert_after(const_iterator pos, InputIt first, InputIt last) {
    iterator it = iterator(pos.it_);
    while (first != last) {
      it = insert_after(it, *first++);
    }
    return it;
  }

  /// Attempts to insert elements from range `[first, last)` after `pos`.
  /// If allocation fails, all partially inserted elements are erased.
  template <typename InputIt,
            typename = std::enable_if_t<!std::is_integral_v<InputIt>>>
  [[nodiscard]] bool try_insert_after(const_iterator pos,
                                      InputIt first,
                                      InputIt last) {
    iterator it = iterator(pos.it_);
    while (first != last) {
      Node* node = allocator_->New<Node>(*first++);
      if (node == nullptr) {
        erase_after(pos, std::next(it));
        return false;
      }
      it = iterator(list_.insert_after(it.it_, *node));
    }
    return true;
  }

  /// Inserts elements from `ilist` after `pos`.
  /// Crashes if allocation fails.
  iterator insert_after(const_iterator pos, std::initializer_list<T> ilist) {
    return insert_after(pos, ilist.begin(), ilist.end());
  }

  /// Attempts to insert elements from `ilist` after `pos`.
  [[nodiscard]] bool try_insert_after(const_iterator pos,
                                      std::initializer_list<T> ilist) {
    return try_insert_after(pos, ilist.begin(), ilist.end());
  }

  /// Replaces the contents with `count` copies of `value`.
  /// Crashes if allocation fails.
  void assign(size_type count, const T& value) {
    PW_ASSERT(try_assign(count, value));
  }

  /// Attempts to replace the contents with `count` copies of `value`.
  /// If allocation fails, the list contents remain unchanged.
  [[nodiscard]] bool try_assign(size_type count, const T& value) {
    ForwardList temp(*allocator_);
    if (!temp.try_resize(count, value)) {
      return false;
    }
    *this = std::move(temp);
    return true;
  }

  /// Replaces the contents with elements from `[first, last)`.
  /// Crashes if allocation fails.
  template <typename InputIt,
            typename = std::enable_if_t<!std::is_integral_v<InputIt>>>
  void assign(InputIt first, InputIt last) {
    PW_ASSERT(try_assign(first, last));
  }

  /// Attempts to replace contents with elements from `[first, last)`.
  /// If allocation fails, the list contents remain unchanged.
  template <typename InputIt,
            typename = std::enable_if_t<!std::is_integral_v<InputIt>>>
  [[nodiscard]] bool try_assign(InputIt first, InputIt last) {
    ForwardList temp(*allocator_);
    if (!temp.try_insert_after(temp.before_begin(), first, last)) {
      return false;
    }
    *this = std::move(temp);
    return true;
  }

  /// Replaces the contents with elements from `ilist`.
  /// Crashes if allocation fails.
  void assign(std::initializer_list<T> ilist) { PW_ASSERT(try_assign(ilist)); }

  /// Attempts to replace contents with elements from `ilist`.
  /// If allocation fails, the list contents remain unchanged.
  [[nodiscard]] bool try_assign(std::initializer_list<T> ilist) {
    return try_assign(ilist.begin(), ilist.end());
  }

  /// Removes the element following `pos`.
  ///
  /// @returns Iterator to the element following the erased element, or `end()`.
  iterator erase_after(const_iterator pos) {
    PW_DASSERT(std::next(pos.it_) != list_.end());
    Node* node = &static_cast<Node&>(*std::next(pos.it_));
    auto next_it = list_.erase_after(pos.it_);
    allocator_->Delete(node);
    return iterator(next_it);
  }

  /// Removes the elements in the range `(first, last)`.
  ///
  /// @returns Iterator to `last`.
  iterator erase_after(const_iterator first, const_iterator last) {
    if (first.it_ == last.it_ && first.it_ != list_.end()) {
      return iterator(last.it_);
    }
    while (std::next(first.it_) != last.it_) {
      erase_after(first);
    }
    return iterator(last.it_);
  }

  /// Resizes the container to contain `count` elements. Default-inserts
  /// additional elements if growing. Crashes on allocation failure.
  void resize(size_type count) { PW_ASSERT(try_resize(count)); }

  /// Resizes the container to contain `count` elements, copy-inserting `value`
  /// if growing. Crashes on allocation failure.
  void resize(size_type count, const T& value) {
    PW_ASSERT(try_resize(count, value));
  }

  /// Attempts to resize the container to `count` elements with default-inserted
  /// values. If allocation fails, the container is restored to its prior state.
  [[nodiscard]] bool try_resize(size_type count) {
    return try_resize_impl(count);
  }

  /// Attempts to resize the container to `count` elements with copies of
  /// `value`. If allocation fails, the container is restored to its prior
  /// state.
  [[nodiscard]] bool try_resize(size_type count, const T& value) {
    return try_resize_impl(count, value);
  }

  /// Moves all elements from `other` into this list after `pos`.
  void splice_after(const_iterator pos, ForwardList& other) noexcept {
    if (this == &other || other.empty()) {
      return;
    }
    PW_ASSERT(get_allocator().IsEqual(other.get_allocator()));
    list_.splice_after(pos.it_, other.list_);
  }

  /// Moves all elements from `other` into this list after `pos`.
  void splice_after(const_iterator pos, ForwardList&& other) noexcept {
    splice_after(pos, other);
  }

  /// Moves the element pointed to after `it` from `other` to after `pos`.
  void splice_after(const_iterator pos,
                    ForwardList& other,
                    const_iterator it) noexcept {
    if (this == &other && (pos.it_ == it.it_ || pos.it_ == std::next(it.it_))) {
      return;
    }
    PW_ASSERT(this == &other || get_allocator().IsEqual(other.get_allocator()));
    list_.splice_after(pos.it_, other.list_, it.it_);
  }

  /// Moves the element pointed to after `it` from `other` to after `pos`.
  void splice_after(const_iterator pos,
                    ForwardList&& other,
                    const_iterator it) noexcept {
    splice_after(pos, other, it);
  }

  /// Moves the elements in the range `(first, last)` from `other` to after
  /// `pos`.
  void splice_after(const_iterator pos,
                    ForwardList& other,
                    const_iterator first,
                    const_iterator last) noexcept {
    if (first.it_ == last.it_ && first.it_ != other.list_.end()) {
      return;
    }
    if (this == &other && pos.it_ == first.it_) {
      return;
    }
    PW_ASSERT(this == &other || get_allocator().IsEqual(other.get_allocator()));
    list_.splice_after(pos.it_, other.list_, first.it_, last.it_);
  }

  /// Moves the elements in the range `(first, last)` from `other` to after
  /// `pos`.
  void splice_after(const_iterator pos,
                    ForwardList&& other,
                    const_iterator first,
                    const_iterator last) noexcept {
    splice_after(pos, other, first, last);
  }

  /// Removes all elements equal to `value`.
  ///
  /// @returns The number of elements removed.
  size_type remove(const T& value) {
    if constexpr (std::is_copy_constructible_v<T>) {
      T val_copy = value;
      return remove_if([&val_copy](const T& item) { return item == val_copy; });
    } else {
      // Defer node deallocation so that if `value` refers to an element in this
      // list, it remains valid throughout the traversal.
      InternalList to_delete;
      size_type removed = 0;
      iterator it = before_begin();
      while (std::next(it.it_) != list_.end()) {
        if (static_cast<Node&>(*std::next(it.it_)).item() == value) {
          Node* node = &static_cast<Node&>(*std::next(it.it_));
          list_.erase_after(it.it_);
          to_delete.push_front(*node);
          ++removed;
        } else {
          ++it;
        }
      }
      while (!to_delete.empty()) {
        Node* node = &static_cast<Node&>(to_delete.front());
        to_delete.pop_front();
        allocator_->Delete(node);
      }
      return removed;
    }
  }

  /// Removes all elements that satisfy the predicate `pred`.
  ///
  /// @returns The number of elements removed.
  template <typename UnaryPredicate>
  size_type remove_if(UnaryPredicate pred) {
    size_type removed = 0;
    iterator it = before_begin();
    while (std::next(it.it_) != list_.end()) {
      if (pred(*std::next(it))) {
        erase_after(it);
        ++removed;
      } else {
        ++it;
      }
    }
    return removed;
  }

  /// Removes consecutive duplicate elements.
  ///
  /// @returns The number of elements removed.
  size_type unique() { return unique(std::equal_to<T>()); }

  /// Removes consecutive duplicate elements according to `binary_pred`.
  ///
  /// @returns The number of elements removed.
  template <typename BinaryPredicate>
  size_type unique(BinaryPredicate binary_pred) {
    iterator it = begin();
    if (it == end()) {
      return 0;
    }
    size_type removed = 0;
    while (std::next(it.it_) != list_.end()) {
      if (binary_pred(*it, *std::next(it))) {
        erase_after(it);
        ++removed;
      } else {
        ++it;
      }
    }
    return removed;
  }

  /// Merges two sorted lists into one using `operator<`.
  void merge(ForwardList& other) {
    merge(other, [](const T& a, const T& b) { return a < b; });
  }

  /// Merges two sorted lists into one using `operator<`.
  void merge(ForwardList&& other) { merge(other); }

  /// Merges two sorted lists into one using the comparator `comp`.
  template <typename Compare>
  void merge(ForwardList& other, Compare comp) {
    if (this == &other) {
      return;
    }
    PW_ASSERT(get_allocator().IsEqual(other.get_allocator()));
    list_.merge(other.list_, [&comp](const Node& a, const Node& b) {
      return comp(a.item(), b.item());
    });
  }

  /// Merges two sorted lists into one using the comparator `comp`.
  template <typename Compare>
  void merge(ForwardList&& other, Compare comp) {
    merge(other, comp);
  }

  /// Sorts the elements in non-descending order using `operator<`.
  void sort() {
    list_.sort(
        [](const Node& a, const Node& b) { return a.item() < b.item(); });
  }

  /// Sorts the elements in non-descending order using the comparator `comp`.
  template <typename Compare>
  void sort(Compare comp) {
    list_.sort([&comp](const Node& a, const Node& b) {
      return comp(a.item(), b.item());
    });
  }

  /// Reverses the order of the elements in the list.
  void reverse() noexcept { list_.reverse(); }

 private:
  // Shared implementation for resizing. Uses a variadic template to support
  // in-place default construction as well as copy construction.
  template <typename... Args>
  [[nodiscard]] bool try_resize_impl(size_type count, const Args&... args) {
    iterator it = before_begin();
    size_type current_size = 0;
    while (std::next(it.it_) != list_.end() && current_size < count) {
      ++current_size;
      ++it;
    }
    if (current_size == count) {
      while (std::next(it.it_) != list_.end()) {
        erase_after(it);
      }
      return true;
    }
    iterator rollback_pos = it;
    while (current_size < count) {
      Node* node = allocator_->New<Node>(args...);
      if (node == nullptr) {
        erase_after(rollback_pos, end());
        return false;
      }
      it = iterator(list_.insert_after(it.it_, *node));
      ++current_size;
    }
    return true;
  }

  InternalList list_;
  Allocator* allocator_;
};

/// @endsubmodule

}  // namespace pw
