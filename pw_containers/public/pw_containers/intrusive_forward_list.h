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

#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <type_traits>

#include "pw_containers/config.h"
#include "pw_containers/internal/intrusive_item.h"
#include "pw_containers/internal/intrusive_list.h"
#include "pw_containers/internal/intrusive_list_item.h"
#include "pw_containers/internal/intrusive_list_iterator.h"

/// @module{pw_containers}
/// @defgroup pw_containers_lists Lists
/// @{

namespace pw {
namespace containers {
namespace internal {

// Forward declaration for friending.
//
// The LegacyIntrusiveList has methods that run in O(n) time and have no
// analogue in `std::forward_list`. If any of these behaviors are needed,
// callers should consider whether a doubly-linked list is more appropriate,
// or whether they should provide the functionality themselves, e.g. by
// tracking size explicitly.
template <typename>
class LegacyIntrusiveList;

}  // namespace internal

/// Inserts an element at the end of the forward list. Runs in O(n) time.
template <typename T>
void PushBackSlow(IntrusiveForwardList<T>& forward_list, T& item) {
  forward_list.list().push_back(item);
}

}  // namespace containers

/// A singly-list intrusive list.
///
/// IntrusiveForwardList<T> is a handle to access and manipulate the list, and
/// IntrusiveForwardList<T>::Item is the type from which the base class items
/// must derive.
///
/// As a singly-linked list, the overhead required is only `sizeof(T*)`.
/// However, operations such as removal may require O(n) time to walk the length
/// of the list.
///
/// This class is modeled on `std::forward_list`, with the following
/// differences:
///
/// - Since items are not allocated by this class, the following methods have
///   no analogue:
///   - std::forward_list<T>::get_allocator
///   - std::forward_list<T>::emplace_after
///   - std::forward_list<T>::emplace_front
///   - std::forward_list<T>::resize
///
/// - Methods corresponding to the following take initializer lists of pointer
///   to items rather than the itenms themselves:
///   - std::forward_list<T>::(constructor)
///   - std::forward_list<T>::assign
///   - std::forward_list<T>::insert_after
///
/// - There are no overloads corresponding to the following methods that take
///   r-value references.:
///   - std::forward_list<T>::insert_after
///   - std::forward_list<T>::push_front
///   - std::forward_list<T>::splice_after
///
/// - Since modifying the list modifies the items themselves, methods
///   corresponding to those below only take `iterator`s and not
///   `const_iterator`s:
///   - std::forward_list<T>::insert_after
///   - std::forward_list<T>::erase_after
///   - std::forward_list<T>::splice_after
///
/// - C++23 methods are not (yet) supported.
///
/// @tparam   T           Type of intrusive items stored in the list.
template <typename T>
class IntrusiveForwardList {
 private:
  using ItemBase = ::pw::containers::internal::IntrusiveForwardListItem;

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
    /// types. A derived type provides a move constructor and move assignment
    /// operator to facilitate using the type with e.g. `Vector::emplace_back`,
    /// but this will not modify the list that the item belongs to, if any.
    constexpr explicit Item() = default;

   private:
    // IntrusiveItem is used to ensure T inherits from this container's Item
    // type. See also `CheckItemType`.
    template <typename, typename, bool>
    friend struct containers::internal::IntrusiveItem;
    using ItemType = T;
  };

  using iterator =
      typename ::pw::containers::internal::ForwardIterator<T, ItemBase>;
  using const_iterator =
      typename ::pw::containers::internal::ForwardIterator<std::add_const_t<T>,
                                                           const ItemBase>;

  constexpr IntrusiveForwardList() = default;

  // Intrusive lists cannot be copied, since each Item can only be in one list.
  IntrusiveForwardList(const IntrusiveForwardList&) = delete;
  IntrusiveForwardList& operator=(const IntrusiveForwardList&) = delete;

  /// Moves the other list's contents into this list.
  ///
  /// This is O(n).
  IntrusiveForwardList(IntrusiveForwardList&&) = default;

  /// Clears this list and moves the other list's contents into it.
  ///
  /// This is O(n).
  IntrusiveForwardList& operator=(IntrusiveForwardList&&) = default;

  /// Constructs a list from an iterator over items. The iterator may
  /// dereference as either Item& (e.g. from std::array<Item>) or Item* (e.g.
  /// from std::initializer_list<Item*>).
  template <typename Iterator>
  IntrusiveForwardList(Iterator first, Iterator last) {
    list().assign(first, last);
  }

  /// Constructs a list from a std::initializer_list of pointers to items.
  IntrusiveForwardList(std::initializer_list<Item*> items)
      : IntrusiveForwardList(items.begin(), items.end()) {}

  template <typename Iterator>
  void assign(Iterator first, Iterator last) {
    list().assign(first, last);
  }

  void assign(std::initializer_list<T*> items) {
    list().assign(items.begin(), items.end());
  }

  // Element access

  /// Reference to the first element in the list. Undefined behavior if empty().
  reference front() { return *static_cast<T*>(list().begin()); }
  const_reference front() const {
    return *static_cast<const T*>(list().begin());
  }

  // Iterators

  iterator before_begin() noexcept { return iterator(list().before_begin()); }
  const_iterator before_begin() const noexcept {
    return const_iterator(list().before_begin());
  }
  const_iterator cbefore_begin() const noexcept { return before_begin(); }

  iterator begin() noexcept { return iterator(list().begin()); }
  const_iterator begin() const noexcept {
    return const_iterator(list().begin());
  }
  const_iterator cbegin() const noexcept { return begin(); }

  iterator end() noexcept { return iterator(list().end()); }
  const_iterator end() const noexcept { return const_iterator(list().end()); }
  const_iterator cend() const noexcept { return end(); }

  // Capacity

  /// @copydoc containers::internal::GenericIntrusiveList<ItemBase>::empty
  [[nodiscard]] bool empty() const noexcept { return list().empty(); }

  /// @copydoc containers::internal::GenericIntrusiveList<ItemBase>::max_size
  constexpr size_type max_size() const noexcept {
    return std::numeric_limits<difference_type>::max();
  }

  // Modifiers

  /// @copydoc containers::internal::GenericIntrusiveList<ItemBase>::clear
  void clear() { list().clear(); }

  /// Inserts the given `item` after the given position, `pos`.
  iterator insert_after(iterator pos, T& item) {
    return iterator(list().insert_after(pos.item_, item));
  }

  /// Inserts the range of items from `first` (inclusive) to `last` (exclusive)
  /// after the given position, `pos`.
  template <typename Iterator>
  iterator insert_after(iterator pos, Iterator first, Iterator last) {
    return iterator(list().insert_after(pos.item_, first, last));
  }

  /// Inserts the range of items from `first` (inclusive) to `last` (exclusive)
  /// after the given position, `pos`.
  iterator insert_after(iterator pos, std::initializer_list<T*> items) {
    return insert_after(pos, items.begin(), items.end());
  }

  /// Removes the item following pos from the list. The item is not destructed.
  iterator erase_after(iterator pos) {
    return iterator(list().erase_after(pos.item_));
  }

  /// Removes the range of items from `first` (inclusive) to `last` (exclusive).
  iterator erase_after(iterator first, iterator last) {
    return iterator(list().erase_after(first.item_, last.item_));
  }

  /// @copydoc containers::internal::GenericIntrusiveList<ItemBase>::push_front
  void push_front(T& item) { list().push_front(item); }

  /// @copydoc containers::internal::GenericIntrusiveList<ItemBase>::pop_front
  void pop_front() { list().pop_front(); }

  /// Exchanges this list's items with the `other` list's items.
  ///
  /// This is O(n), where "n" is the number of items in the range.
  void swap(IntrusiveForwardList<T>& other) noexcept {
    list().swap(other.list());
  }

  // Operations

  /// @copydoc containers::internal::GenericIntrusiveList<ItemBase>::merge
  ///
  /// This overload uses `T::operator<`.
  void merge(IntrusiveForwardList<T>& other) {
    list().merge(other.list(),
                 [](const ItemBase& a, const ItemBase& b) -> bool {
                   return static_cast<const T&>(a) < static_cast<const T&>(b);
                 });
  }

  /// @copydoc containers::internal::GenericIntrusiveList<ItemBase>::merge
  template <typename Compare>
  void merge(IntrusiveForwardList<T>& other, Compare comp) {
    list().merge(other.list(), [comp](const ItemBase& a, const ItemBase& b) {
      return comp(static_cast<const T&>(a), static_cast<const T&>(b));
    });
  }

  /// Inserts the items of `other` to after `pos` in this list. Upon returning,
  /// `other` will be empty.
  void splice_after(iterator pos, IntrusiveForwardList<T>& other) {
    splice_after(pos, other, other.before_begin(), other.end());
  }

  /// Moves the item pointed to by the iterator *following* `it` from `other`
  /// to after `pos` in this list.
  void splice_after(iterator pos, IntrusiveForwardList<T>& other, iterator it) {
    splice_after(pos, other, it, std::next(std::next(it)));
  }

  /// Moves the items exclusively between `first` and `last` from `other` to
  /// after `pos` in this list.
  void splice_after(iterator pos,
                    IntrusiveForwardList<T>& other,
                    iterator first,
                    iterator last) {
    list().splice_after(pos.item_, other.list(), first.item_, last.item_);
  }

  /// @copydoc containers::internal::GenericIntrusiveList<ItemBase>::remove
  bool remove(const T& item) { return list().remove(item); }

  /// @copydoc containers::internal::GenericIntrusiveList<ItemBase>::remove_if
  template <typename UnaryPredicate>
  size_type remove_if(UnaryPredicate pred) {
    return list().remove_if([pred](const ItemBase& item) -> bool {
      return pred(static_cast<const T&>(item));
    });
  }

  /// @copydoc containers::internal::GenericIntrusiveList<ItemBase>::reverse
  void reverse() { list().reverse(); }

  /// @copydoc containers::internal::GenericIntrusiveList<ItemBase>::unique
  ///
  /// This overload uses `T::operator==`.
  size_type unique() {
    return list().unique([](const ItemBase& a, const ItemBase& b) -> bool {
      return static_cast<const T&>(a) == static_cast<const T&>(b);
    });
  }

  /// @copydoc containers::internal::GenericIntrusiveList<ItemBase>::unique
  template <typename BinaryPredicate>
  size_type unique(BinaryPredicate pred) {
    return list().unique([pred](const ItemBase& a, const ItemBase& b) {
      return pred(static_cast<const T&>(a), static_cast<const T&>(b));
    });
  }

  /// @copydoc containers::internal::GenericIntrusiveList<ItemBase>::sort
  ///
  /// This overload uses `T::operator<`.
  void sort() {
    list().sort([](const ItemBase& a, const ItemBase& b) -> bool {
      return static_cast<const T&>(a) < static_cast<const T&>(b);
    });
  }

  /// @copydoc containers::internal::GenericIntrusiveList<ItemBase>::sort
  template <typename Compare>
  void sort(Compare comp) {
    list().sort([comp](const ItemBase& a, const ItemBase& b) {
      return comp(static_cast<const T&>(a), static_cast<const T&>(b));
    });
  }

 private:
  template <typename>
  friend class containers::internal::LegacyIntrusiveList;

  friend void containers::PushBackSlow<T>(IntrusiveForwardList<T>&, T&);

  // Check that T is an Item in a function, since the class T will not be fully
  // defined when the IntrusiveList<T> class is instantiated.
  static constexpr void CheckItemType() {
    using IntrusiveItemType =
        typename containers::internal::IntrusiveItem<ItemBase, T>::Type;
    static_assert(
        std::is_base_of<IntrusiveItemType, T>(),
        "IntrusiveForwardList items must be derived from "
        "IntrusiveForwardList<T>::Item, where T is the item or one of its "
        "bases.");
  }

  // The generic list is accessed through this function to ensure
  // CheckItemType() is instantiated. CheckItemType() is not instantiated in the
  // constructor to allow constructing lists of forward declared items.
  constexpr auto& list() {
    CheckItemType();
    return generic_list_;
  }

  constexpr const auto& list() const {
    CheckItemType();
    return generic_list_;
  }

  containers::internal::GenericIntrusiveList<ItemBase> generic_list_;
};

/// `IntrusiveForwardListItem` is a thin wrapper around a templated item which
/// is itself a intrusive forward list item, permitting registration where the
/// client holds the storage for the item.
template <typename T>
class IntrusiveForwardListItem final
    : public IntrusiveForwardList<IntrusiveForwardListItem<T>>::Item {
 public:
  using element_type = T;
  using value_type = std::remove_cv_t<element_type>;
  using reference = value_type&;
  using const_reference = const value_type&;

  template <typename... Args>
  IntrusiveForwardListItem(Args&&... args)
      : item_(std::forward<Args>(args)...) {}

  reference item() { return item_; }
  const_reference item() const { return item_; }

 private:
  element_type item_;
};

}  // namespace pw

/// @}
/// @endmodule
