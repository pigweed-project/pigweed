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

#include <cstddef>
#include <limits>
#include <type_traits>

#include "pw_containers/internal/intrusive_item.h"
#include "pw_containers/internal/intrusive_list_item.h"

namespace pw::containers::internal {

/// @module{pw_containers}

/// @addtogroup pw_containers_lists
/// @{

/// Generic intrusive list implementation.
///
/// This implementation relies on the `Item` type to provide details of how to
/// navigate the list. It provides methods similar to `std::forward_list` and
/// `std::list`.
///
/// @tparam   Item  The type used to intrusive store list pointers.
template <typename Item>
class GenericIntrusiveList {
 public:
  static_assert(std::is_same_v<Item, IntrusiveForwardListItem> ||
                std::is_same_v<Item, IntrusiveListItem>);

  constexpr GenericIntrusiveList() : head_() {}

  template <typename Iterator>
  GenericIntrusiveList(Iterator first, Iterator last) : GenericIntrusiveList() {
    insert_after(&head_, first, last);
  }

  // Intrusive lists cannot be copied, since each Item can only be in one list.
  GenericIntrusiveList(const GenericIntrusiveList&) = delete;
  GenericIntrusiveList& operator=(const GenericIntrusiveList&) = delete;

  // Moves the other list's contents into this list.
  GenericIntrusiveList(GenericIntrusiveList&& other) {
    head_.replace(other.head_);
  }

  // Clears this list and moves the other list's contents into it.
  GenericIntrusiveList& operator=(GenericIntrusiveList&& other) {
    clear();
    head_.replace(other.head_);
    return *this;
  }

  ~GenericIntrusiveList() {
    if (!empty()) {
      CrashIntrusiveContainerIsNotEmpty();
    }
  }

  template <typename Iterator>
  void assign(Iterator first, Iterator last) {
    clear();
    insert_after(&head_, first, last);
  }

  // Iterators

  /// Returns a pointer to the sentinel item.
  constexpr Item* before_begin() noexcept { return &head_; }
  constexpr const Item* before_begin() const noexcept { return &head_; }

  /// Returns a pointer to the first item.
  constexpr Item* begin() noexcept { return head_.next_; }
  constexpr const Item* begin() const noexcept { return head_.next_; }

  /// Returns a pointer to the last item.
  Item* before_end() noexcept { return before_begin()->previous(); }

  /// Returns a pointer to the sentinel item.
  constexpr Item* end() noexcept { return &head_; }
  constexpr const Item* end() const noexcept { return &head_; }

  // Capacity

  bool empty() const noexcept { return begin() == end(); }

  /// Returns how many items can be added.
  ///
  /// As an intrusive container, this is effectively unbounded.
  constexpr size_t max_size() const noexcept {
    return std::numeric_limits<ptrdiff_t>::max();
  }

  // Modifiers

  /// Removes all items from the list.
  void clear() {
    while (!empty()) {
      erase_after(before_begin());
    }
  }

  /// Inserts an element at the end of the list.
  void push_back(Item& item) { insert_after(before_end(), item); }

  /// Removes the last item in the list. The list must not be empty.
  void pop_back() { erase_after(before_end()->previous()); }

  /// Inserts an element at the beginning of the list.
  void push_front(Item& item) { insert_after(before_begin(), item); }

  /// Removes the first item in the list. The list must not be empty.
  void pop_front() { erase_after(before_begin()); }

  /// Inserts an item into a list.
  ///
  /// The item given by `prev` is updated to point to the item as being next in
  /// the list, while the item itself points to what `prev` previously pointed
  /// to as next.
  ///
  /// This is O(1). The ownership of the item is not changed.
  ///
  /// @return The item that was added.
  static Item* insert_after(Item* prev, Item& item) {
    if (!item.unlisted()) {
      CrashIntrusiveItemIsContained();
    }
    item.next_ = prev->next_;
    item.set_previous(prev);
    prev->next_ = &item;
    item.next_->set_previous(&item);
    return &item;
  }

  /// Adds items to the list from the provided range after the given item.
  ///
  /// This is O(n), where "n" is the number of items in the range.
  ///
  /// @return The last item that was added.
  template <typename Iterator>
  static Item* insert_after(Item* prev, Iterator first, Iterator last) {
    for (Iterator it = first; it != last; ++it) {
      prev = insert_after(prev, *(FromIterator(it)));
    }
    return prev;
  }

  /// Removes an item from a list.
  ///
  /// The item after the given `item` is unlisted, and the item following it is
  /// returned.
  ///
  /// This is O(1). The item is not destroyed..
  static Item* erase_after(Item* item) {
    item->next_->unlist(item);
    return item->next_;
  }

  /// Removes the range of items exclusively between `first` and `last`.
  static Item* erase_after(Item* first, Item* last) {
    while (first->next_ != last) {
      erase_after(first);
    }
    return last;
  }

  // Exchanges this list's items with the `other` list's items. O(1) for
  // IntrusiveList, O(n) for IntrusiveForwardList.
  void swap(GenericIntrusiveList<Item>& other) {
    Item tmp;
    tmp.replace(head_);
    head_.replace(other.head_);
    other.head_.replace(tmp);
  }

  // Operations

  /// Merges the given `other` list into this one.
  ///
  /// After the call, the list will be sorted according to `comp`. The sort is
  /// stable, and equivalent items in each list will remain in the same order
  /// relative to each other.
  template <typename Compare>
  void merge(GenericIntrusiveList<Item>& other, Compare comp) {
    Item* prev = before_begin();
    Item* item = begin();
    Item* other_item = other.begin();
    while (!other.empty()) {
      if (item == end() || !comp(*item, *other_item)) {
        Item* tmp = other_item;
        other_item = other.erase_after(other.before_begin());
        prev = insert_after(prev, *tmp);
      } else {
        prev = item;
        item = item->next_;
      }
    }
  }

  /// Moves the items exclusively between `first` and `last` from `other` to
  /// after `pos` in this list.
  static void splice_after(Item* pos,
                           GenericIntrusiveList<Item>& other,
                           Item* first,
                           Item* last) {
    // Return if the range is empty, unless it is from before_begin to end.
    if (first == last && first != &other.head_) {
      return;
    }
    Item* first_next = first->next_;
    if (first_next == last) {
      return;
    }
    Item* last_prev = last->previous();
    Item* pos_next = pos->next_;

    first->next_ = last;
    last->set_previous(first);

    pos->next_ = first_next;
    first_next->set_previous(pos);

    pos_next->set_previous(last_prev);
    last_prev->next_ = pos_next;
  }

  // Removes this specific item from the list, if it is present. Finds the item
  // by identity (address comparison) rather than value equality. Returns true
  // if the item was removed; false if it was not present.
  bool remove(const Item& item_to_remove) {
    Item* prev = find_previous(item_to_remove);
    if (prev == nullptr) {
      return false;
    }
    erase_after(prev);
    return true;
  }

  /// Finds the item that precedes `item` in the list by identity (address
  /// comparison). Since an item can only be in one list at a time, the search
  /// stops at the first match.
  ///
  /// This is O(n).
  ///
  /// @returns The item before `item`, which may be `before_begin()`, or
  /// `nullptr` if `item` is not in this list.
  Item* find_previous(const Item& item) {
    for (Item* prev = before_begin(); prev->next_ != end();
         prev = prev->next_) {
      if (prev->next_ == &item) {
        return prev;
      }
    }
    return nullptr;
  }

  /// Removes any item for which the given unary predicate function `p`
  /// evaluates to true when passed that item.
  ///
  /// @tparam UnaryPredicate    Function with the signature `bool(const Item&)`
  ///
  /// @returns The number of items removed.
  template <typename UnaryPredicate>
  size_t remove_if(UnaryPredicate pred,
                   size_t max = std::numeric_limits<size_t>::max()) {
    size_t removed = 0;
    Item* prev = before_begin();
    while (true) {
      Item* item = prev->next_;
      if (item == end()) {
        break;
      }
      if (pred(*item)) {
        erase_after(prev);
        ++removed;
        if (removed == max) {
          break;
        }
      } else {
        prev = item;
      }
    }
    return removed;
  }

  /// Reverses the order of items in the list.
  void reverse() {
    GenericIntrusiveList<Item> list;
    while (!empty()) {
      Item* item = begin();
      erase_after(before_begin());
      list.insert_after(list.before_begin(), *item);
    }
    splice_after(before_begin(), list, list.before_begin(), list.end());
  }

  /// Removes consecutive ietms that are equivalent accroding to the given
  /// binary predicate `p`, leaving only the first item in the list.
  ///
  /// @tparam BinaryPredicate   Function with the signature
  ///                           `bool(const Item&, const Item&)`
  template <typename BinaryPredicate>
  size_t unique(BinaryPredicate pred) {
    if (empty()) {
      return 0;
    }
    size_t removed = 0;
    Item* prev = begin();
    while (true) {
      Item* item = prev->next_;
      if (item == end()) {
        break;
      }
      if (pred(*prev, *item)) {
        erase_after(prev);
        ++removed;
      } else {
        prev = item;
      }
    }
    return removed;
  }

  /// Rearranges the items in the list such that the given comparison function
  /// `comp` evaluates to true for each pair of successive items.
  ///
  /// @tparam BinaryPredicate   Function with the signature
  ///                           `bool(const Item&, const Item&)`
  template <typename Compare>
  void sort(Compare comp) {
    Item* mid = begin();
    bool even = true;
    for (Item* item = begin(); item != end(); item = item->next_) {
      even = !even;
      if (even) {
        mid = mid->next_;
      }
    }

    // Partition the list.
    GenericIntrusiveList<Item> tmp;
    tmp.splice_after(tmp.before_begin(), *this, before_begin(), mid);

    // Check if trivially sorted.
    if (tmp.empty()) {
      return;
    }

    // Sort the sublists.
    tmp.sort(comp);
    sort(comp);

    // Merge the sublists.
    merge(tmp, comp);
  }

 private:
  template <typename Iterator>
  static Item* FromIterator(Iterator& it) {
    if constexpr (std::is_pointer<std::remove_reference_t<decltype(*it)>>()) {
      return *it;
    } else {
      return &(*it);
    }
  }

  // Use an Item for the head pointer. This gives simpler logic for inserting
  // elements compared to using an Item*. It also makes it possible to use
  // &head_ for end(), rather than nullptr. This makes end() unique for each
  // List and ensures that items already in a list cannot be added to another.
  Item head_;
};

}  // namespace pw::containers::internal
