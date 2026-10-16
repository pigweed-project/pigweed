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
#include <type_traits>
#include <utility>

#include "pw_containers/internal/intrusive_queue.h"
#include "pw_containers/intrusive_forward_list.h"

namespace pw {

/// An `IntrusiveQueue` is a singly-linked list similar to
/// `IntrusiveForwardList` but tracks the tail element to provide O(1)
/// `push_back` support. It does not support `pop_back()`.
template <typename T>
class IntrusiveQueue {
 private:
  using ItemBase = containers::internal::IntrusiveQueueItem;

 public:
  class Item : public ItemBase {
   protected:
    constexpr explicit Item() = default;

   private:
    template <typename, typename, bool>
    friend struct containers::internal::IntrusiveItem;
    using ItemType = T;
  };

  using element_type = T;
  using value_type = std::remove_cv_t<element_type>;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using reference = value_type&;
  using const_reference = const value_type&;
  using pointer = element_type*;
  using const_pointer = const element_type*;

  using iterator = typename containers::internal::ForwardIterator<T, ItemBase>;
  using const_iterator =
      typename containers::internal::ForwardIterator<std::add_const_t<T>,
                                                     const ItemBase>;

  constexpr IntrusiveQueue() = default;

  // Intrusive queues cannot be copied.
  IntrusiveQueue(const IntrusiveQueue&) = delete;
  IntrusiveQueue& operator=(const IntrusiveQueue&) = delete;

  /// Moves the other queue's contents into this queue.
  IntrusiveQueue(IntrusiveQueue&&) = default;

  /// Clears this queue and moves the other queue's contents into it.
  IntrusiveQueue& operator=(IntrusiveQueue&&) = default;

  /// Constructs a queue from an iterator over items.
  template <typename Iterator>
  IntrusiveQueue(Iterator first, Iterator last) {
    assign(first, last);
  }

  /// Constructs a queue from a std::initializer_list of pointers to items.
  IntrusiveQueue(std::initializer_list<T*> items) {
    assign(items.begin(), items.end());
  }

  template <typename Iterator>
  void assign(Iterator first, Iterator last) {
    CheckItemType();
    generic_queue_.assign(first, last);
  }

  void assign(std::initializer_list<T*> items) {
    assign(items.begin(), items.end());
  }

  // Iterators

  iterator before_begin() noexcept {
    return iterator(generic_queue_.before_begin());
  }
  const_iterator before_begin() const noexcept {
    return const_iterator(generic_queue_.before_begin());
  }
  const_iterator cbefore_begin() const noexcept { return before_begin(); }

  iterator begin() noexcept { return iterator(generic_queue_.begin()); }
  const_iterator begin() const noexcept {
    return const_iterator(generic_queue_.begin());
  }
  const_iterator cbegin() const noexcept { return begin(); }

  iterator end() noexcept { return iterator(generic_queue_.end()); }
  const_iterator end() const noexcept {
    return const_iterator(generic_queue_.end());
  }
  const_iterator cend() const noexcept { return end(); }

  // Element access

  reference front() { return *static_cast<T*>(generic_queue_.begin()); }
  const_reference front() const {
    return *static_cast<const T*>(generic_queue_.begin());
  }

  reference back() { return *static_cast<T*>(generic_queue_.tail()); }
  const_reference back() const {
    return *static_cast<const T*>(generic_queue_.tail());
  }

  // Capacity

  [[nodiscard]] bool empty() const noexcept { return generic_queue_.empty(); }
  constexpr size_type max_size() const noexcept {
    return std::numeric_limits<difference_type>::max();
  }

  // Modifiers

  void clear() { generic_queue_.clear(); }

  /// Inserts an element at the beginning of the queue.
  void push_front(T& item) { generic_queue_.push_front(item); }

  /// Removes the first element of the queue.
  void pop_front() { generic_queue_.pop_front(); }

  /// Inserts an element at the end of the queue in O(1) time.
  void push_back(T& item) { generic_queue_.push_back(item); }

  /// Inserts `item` after the specified `pos`.
  iterator insert_after(iterator pos, T& item) {
    return iterator(generic_queue_.insert_after(pos.item_, item));
  }

  /// Inserts a range after the specified `pos`.
  template <typename Iterator>
  iterator insert_after(iterator pos, Iterator first, Iterator last) {
    return iterator(generic_queue_.insert_after(pos.item_, first, last));
  }

  /// Inserts an initializer list after the specified `pos`.
  iterator insert_after(iterator pos, std::initializer_list<T*> items) {
    return insert_after(pos, items.begin(), items.end());
  }

  /// Removes the item succeeding `pos`.
  iterator erase_after(iterator pos) {
    return iterator(generic_queue_.erase_after(pos.item_));
  }

  /// Removes a range of items.
  iterator erase_after(iterator first, iterator last) {
    return iterator(generic_queue_.erase_after(first.item_, last.item_));
  }

  /// Removes this specific item from the list, if it is present. Unsafe to use
  /// raw `unlist()` if you want the queue `tail_` to be preserved. This method
  /// properly updates the `tail_` iterator if the removed item is the tail.
  bool remove(const T& item_to_remove) {
    return generic_queue_.remove(item_to_remove);
  }

  /// Removes any item for which the given unary predicate evaluates to true.
  template <typename UnaryPredicate>
  size_type remove_if(UnaryPredicate&& pred) {
    return generic_queue_.remove_if<T>(std::forward<UnaryPredicate>(pred));
  }

  void swap(IntrusiveQueue<T>& other) noexcept {
    generic_queue_.swap(other.generic_queue_);
  }

 private:
  // Check that T is an Item in a function, since the class T will not be fully
  // defined when the IntrusiveQueue<T> class is instantiated.
  static constexpr void CheckItemType() {
    static_assert(
        containers::internal::IsIntrusiveItem<ItemBase, T>(),
        "IntrusiveQueue items must be derived from IntrusiveQueue<T>::Item, "
        "where T is the item or one of its bases.");
  }

  containers::internal::GenericIntrusiveQueue generic_queue_;
};

}  // namespace pw
