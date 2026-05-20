// Copyright 2021 The Pigweed Authors
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
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <type_traits>

#include "pw_containers/config.h"
#include "pw_containers/internal/intrusive_item.h"
#include "pw_containers/internal/intrusive_list.h"
#include "pw_containers/internal/intrusive_list_item.h"
#include "pw_containers/internal/intrusive_list_iterator.h"
#include "pw_containers/internal/legacy_intrusive_list.h"
#include "pw_containers/intrusive_forward_list.h"

namespace pw {

/// @module{pw_containers}

namespace containers::future {

/// @addtogroup pw_containers_lists
/// @{

/// A doubly-linked intrusive list.
///
/// IntrusiveList<T> is a handle to access and manipulate the list, and
/// IntrusiveList<T>::Item is the type from which the base class items
/// must derive.
///
/// As a doubly-linked list, operations such as removal are O(1) in time, and
/// the list may be iterated in either direction. However, the overhead needed
/// is `2*sizeof(T*)`.
///
/// This class is modeled on `std::list`, with the following differences:
///
/// - Since items are not allocated by this class, the following methods have
///   no analogue:
///   - std::list<T>::get_allocator
///   - std::list<T>::emplace
///   - std::list<T>::emplace_front
///   - std::list<T>::emplace_back
///   - std::list<T>::resize
///
/// - Methods corresponding to the following take initializer lists of pointer
///   to items rather than the items themselves:
///   - std::list<T>::(constructor)
///   - std::list<T>::assign
///   - std::list<T>::insert
///
/// - There are no overloads corresponding to the following methods that take
///   r-value references.:
///   - std::list<T>::insert
///   - std::list<T>::push_back
///   - std::list<T>::push_front
///   - std::list<T>::splice
///
/// - Since modifying the list modifies the items themselves, methods
///   corresponding to those below only take `iterator`s and not
///   `const_iterator`s:
///   - std::list<T>::insert
///   - std::list<T>::erase
///   - std::list<T>::splice
///
/// - An additional overload of `erase` is provided that takes a direct
///   reference to an item.
///
/// - C++23 methods are not (yet) supported.
///
/// @tparam   T           Type of intrusive items stored in the list.
template <typename T>
class IntrusiveList {
 private:
  using ItemBase = ::pw::containers::internal::IntrusiveListItem;

 public:
  using element_type = T;
  using value_type = std::remove_cv_t<element_type>;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using reference = value_type&;
  using const_reference = const value_type&;
  using pointer = element_type*;
  using const_pointer = const element_type*;

  class Item : public ItemBase {
   protected:
    /// The default constructor is the only constructor provided to derived item
    /// types. A derived type may provide a move constructor and move assignment
    /// operator that uses `ItemBase::replace` to facilitate using the type
    /// with routines that require items to be move-assignable, e.g.
    /// `Vector::emplace_back`,
    constexpr explicit Item() = default;

   private:
    // IntrusiveItem is used to ensure T inherits from this container's Item
    // type. See also `CheckItemType`.
    template <typename, typename, bool>
    friend struct containers::internal::IntrusiveItem;
    using ItemType = T;
  };

  using iterator =
      typename ::pw::containers::internal::BidirectionalIterator<T, ItemBase>;
  using const_iterator = typename ::pw::containers::internal::
      BidirectionalIterator<std::add_const_t<T>, const ItemBase>;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  constexpr IntrusiveList() { CheckItemType(); }

  // Intrusive lists cannot be copied, since each Item can only be in one list.
  IntrusiveList(const IntrusiveList&) = delete;
  IntrusiveList& operator=(const IntrusiveList&) = delete;

  /// Clears this list and moves the other list's contents into it.
  ///
  /// This is O(1).
  IntrusiveList(IntrusiveList&&) = default;

  /// Clears this list and moves the other list's contents into it.
  ///
  /// This is O(1).
  IntrusiveList& operator=(IntrusiveList&&) = default;

  // Constructs an IntrusiveList from an iterator over Items. The iterator may
  // dereference as either Item& (e.g. from std::array<Item>) or Item* (e.g.
  // from std::initializer_list<Item*>).
  template <typename Iterator>
  IntrusiveList(Iterator first, Iterator last) {
    list_.assign(first, last);
  }

  // Constructs an IntrusiveList from a std::initializer_list of pointers to
  // items.
  IntrusiveList(std::initializer_list<Item*> items)
      : IntrusiveList(items.begin(), items.end()) {}

  template <typename Iterator>
  void assign(Iterator first, Iterator last) {
    list_.assign(first, last);
  }

  void assign(std::initializer_list<T*> items) {
    list_.assign(items.begin(), items.end());
  }

  // Element access

  /// Reference to the first element in the list. Undefined behavior if empty().
  T& front() { return *static_cast<T*>(list_.begin()); }

  /// Reference to the last element in the list. Undefined behavior if empty().
  T& back() { return *static_cast<T*>(list_.before_end()); }

  // Iterators

  iterator begin() noexcept {
    return iterator(static_cast<Item*>(list_.begin()));
  }
  const_iterator begin() const noexcept {
    return const_iterator(static_cast<const Item*>(list_.begin()));
  }
  const_iterator cbegin() const noexcept { return begin(); }

  iterator end() noexcept { return iterator(static_cast<Item*>(list_.end())); }
  const_iterator end() const noexcept {
    return const_iterator(static_cast<const Item*>(list_.end()));
  }
  const_iterator cend() const noexcept { return end(); }

  reverse_iterator rbegin() { return reverse_iterator(end()); }
  const_reverse_iterator rbegin() const {
    return const_reverse_iterator(end());
  }
  const_reverse_iterator crbegin() const {
    return const_reverse_iterator(end());
  }

  reverse_iterator rend() { return reverse_iterator(begin()); }
  const_reverse_iterator rend() const {
    return const_reverse_iterator(begin());
  }
  const_reverse_iterator crend() const {
    return const_reverse_iterator(begin());
  }

  // Capacity

  /// @copydoc ::internal::GenericIntrusiveList<ItemBase>::empty
  [[nodiscard]] bool empty() const noexcept { return list_.empty(); }

  /// Returns the number of elements in the intrusive list.
  ///
  /// This operation is O(n) because it iterates through the list to count the
  /// elements.
  size_t size() const {
    return static_cast<size_type>(std::distance(begin(), end()));
  }

  /// @copydoc internal::GenericIntrusiveList<ItemBase>::max_size
  constexpr size_type max_size() const noexcept { return list_.max_size(); }

  // Modifiers

  /// @copydoc ::internal::GenericIntrusiveList<ItemBase>::clear
  void clear() { list_.clear(); }

  /// Inserts the given `item` before the given position, `pos`.
  iterator insert(iterator pos, T& item) {
    return iterator(list_.insert_after((--pos).item_, item));
  }

  /// Inserts the range of items from `first` (inclusive) to `last` (exclusive)
  /// before the given position, `pos`.
  template <typename Iterator>
  iterator insert(iterator pos, Iterator first, Iterator last) {
    return iterator(list_.insert_after((--pos).item_, first, last));
  }

  /// Inserts the range of items from `first` (inclusive) to `last` (exclusive)
  /// before the given position, `pos`.
  iterator insert(iterator pos, std::initializer_list<T*> items) {
    return insert(pos, items.begin(), items.end());
  }

  /// Removes the given `item` from the list. The item is not destructed.
  iterator erase(T& item) { return erase(iterator(&item)); }

  /// Removes the item following `pos` from the list. The item is not
  /// destructed.
  iterator erase(iterator pos) {
    return iterator(list_.erase_after((--pos).item_));
  }

  /// Removes the range of items from `first` (inclusive) to `last` (exclusive).
  iterator erase(iterator first, iterator last) {
    return iterator(list_.erase_after((--first).item_, last.item_));
  }

  /// @copydoc ::internal::GenericIntrusiveList<ItemBase>::push_back
  void push_back(T& item) { list_.push_back(item); }

  /// @copydoc ::internal::GenericIntrusiveList<ItemBase>::pop_back
  void pop_back() { list_.pop_back(); }

  /// @copydoc ::internal::GenericIntrusiveList<ItemBase>::push_front
  void push_front(T& item) { list_.push_front(item); }

  /// @copydoc ::internal::GenericIntrusiveList<ItemBase>::pop_front
  void pop_front() { list_.pop_front(); }

  /// Exchanges this list's items with the `other` list's items.
  ///
  /// This is O(1).
  void swap(IntrusiveList<T>& other) noexcept { list_.swap(other.list_); }

  // Operations

  /// @copydoc ::internal::GenericIntrusiveList<ItemBase>::merge
  ///
  /// This overload uses `T::operator<`.
  void merge(IntrusiveList<T>& other) {
    list_.merge(other.list_, [](const ItemBase& a, const ItemBase& b) {
      return static_cast<const T&>(a) < static_cast<const T&>(b);
    });
  }

  /// @copydoc ::internal::GenericIntrusiveList<ItemBase>::merge
  template <typename Compare>
  void merge(IntrusiveList<T>& other, Compare comp) {
    list_.merge(other.list_, [comp](const ItemBase& a, const ItemBase& b) {
      return comp(static_cast<const T&>(a), static_cast<const T&>(b));
    });
  }

  /// Inserts the items of `other` to before `pos` in this list. Upon returning,
  /// `other` will be empty.
  void splice(iterator pos, IntrusiveList<T>& other) {
    splice(pos, other, other.begin(), other.end());
  }

  /// Moves the item pointed to by `it` from `other` to before `pos` in this
  /// list.
  void splice(iterator pos, IntrusiveList<T>& other, iterator it) {
    splice(pos, other, it, std::next(it));
  }

  /// Moves the items between `first`, inclusively, and `last`, exclusively from
  /// `other` to before `pos` in this list.
  void splice(iterator pos,
              IntrusiveList<T>& other,
              iterator first,
              iterator last) {
    list_.splice_after((--pos).item_, other.list_, (--first).item_, last.item_);
  }

  /// @copydoc ::internal::GenericIntrusiveList<ItemBase>::remove
  bool remove(const T& item) { return list_.remove(item); }

  /// @copydoc ::internal::GenericIntrusiveList<ItemBase>::remove_if
  template <typename UnaryPredicate>
  size_type remove_if(UnaryPredicate pred) {
    return list_.remove_if([pred](const ItemBase& item) -> bool {
      return pred(static_cast<const T&>(item));
    });
  }

  /// @copydoc ::internal::GenericIntrusiveList<ItemBase>::reverse
  void reverse() { std::reverse(begin(), end()); }

  /// @copydoc ::internal::GenericIntrusiveList<ItemBase>::unique
  ///
  /// This overload uses `T::operator==`.
  size_type unique() {
    return list_.unique([](const ItemBase& a, const ItemBase& b) -> bool {
      return static_cast<const T&>(a) == static_cast<const T&>(b);
    });
  }

  /// @copydoc ::internal::GenericIntrusiveList<ItemBase>::unique
  template <typename BinaryPredicate>
  size_type unique(BinaryPredicate pred) {
    return list_.unique([pred](const ItemBase& a, const ItemBase& b) -> bool {
      return pred(static_cast<const T&>(a), static_cast<const T&>(b));
    });
  }

  /// @copydoc ::internal::GenericIntrusiveList<ItemBase>::sort
  ///
  /// This overload uses `T::operator<`.
  void sort() {
    list_.sort([](const ItemBase& a, const ItemBase& b) -> bool {
      return static_cast<const T&>(a) < static_cast<const T&>(b);
    });
  }

  /// @copydoc ::internal::GenericIntrusiveList<ItemBase>::sort
  template <typename Compare>
  void sort(Compare comp) {
    list_.sort([comp](const ItemBase& a, const ItemBase& b) -> bool {
      return comp(static_cast<const T&>(a), static_cast<const T&>(b));
    });
  }

 private:
  // Check that T is an Item in a function, since the class T will not be fully
  // defined when the IntrusiveList<T> class is instantiated.
  static constexpr void CheckItemType() {
    using IntrusiveItemType =
        typename containers::internal::IntrusiveItem<ItemBase, T>::Type;
    static_assert(
        std::is_base_of<IntrusiveItemType, T>(),
        "IntrusiveList items must be derived from IntrusiveList<T>::Item, "
        "where T is the item or one of its bases.");
  }

  ::pw::containers::internal::GenericIntrusiveList<ItemBase> list_;
};

}  // namespace containers::future

// See b/362348318 and comments on `PW_CONTAINERS_USE_LEGACY_INTRUSIVE_LIST` and
// `PW_CONTAINERS_INTRUSIVE_LIST_SUPPRESS_DEPRECATION_WARNING`.
#if PW_CONTAINERS_USE_LEGACY_INTRUSIVE_LIST
#if PW_CONTAINERS_INTRUSIVE_LIST_SUPPRESS_DEPRECATION_WARNING
#define PW_CONTAINERS_INTRUSIVE_LIST_DEPRECATED
#else
#define PW_CONTAINERS_INTRUSIVE_LIST_DEPRECATED \
  [[deprecated("See b/362348318 for background and workarounds.")]]
#endif  // PW_CONTAINERS_INTRUSIVE_LIST_SUPPRESS_DEPRECATION_WARNING

/// @copydoc containers::future::IntrusiveForwardList
template <typename T>
using IntrusiveList PW_CONTAINERS_INTRUSIVE_LIST_DEPRECATED =
    containers::internal::LegacyIntrusiveList<T>;

#else  // PW_CONTAINERS_USE_LEGACY_INTRUSIVE_LIST

/// @copydoc containers::future::IntrusiveList
template <typename T>
using IntrusiveList = containers::future::IntrusiveList<T>;

#endif  // PW_CONTAINERS_USE_LEGACY_INTRUSIVE_LIST

}  // namespace pw
