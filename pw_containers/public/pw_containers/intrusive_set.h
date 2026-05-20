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

#include <type_traits>

#include "pw_containers/internal/aa_tree.h"
#include "pw_containers/internal/aa_tree_item.h"

namespace pw {

/// @module{pw_containers}

/// @addtogroup pw_containers_sets
/// @{

/// A `std::set<Key, Compare>`-like class that uses intrusive items as keys.
///
/// Since the set structure is stored in the items themselves, each item must
/// outlive any set it is a part of and must be part of at most one set.
///
/// This set requires unique keys. Attempting to add an item with same key as an
/// item already in the set will fail.
///
/// - Since items are not allocated by this class, the following methods have
///   no analogue:
///   - std::set<T>::operator=
///   - std::set<T>::operator[]
///   - std::set<T>::get_allocator
///   - std::set<T>::insert_or_assign
///   - std::set<T>::emplace
///   - std::set<T>::emplace_hint
///   - std::set<T>::try_emplace
///
/// - Methods corresponding to the following take initializer lists of pointer
///   to items rather than the items themselves:
///   - std::set<T>::(constructor)
///   - std::set<T>::insert
///
/// - There are no overloads corresponding to the following methods that take
///   r-value references.:
///   - std::set<T>::insert
///   - std::set<T>::merge
///
/// - Since modifying the set modifies the items themselves, methods
///   corresponding to those below only take `iterator`s and not
///   `const_iterator`s:
///   - std::set<T>::insert
///   - std::set<T>::erase
///
/// - C++23 methods are not (yet) supported.
///
/// @tparam   T           Type of data stored in the set.
template <typename T>
class IntrusiveSet {
 private:
  using GenericIterator = containers::internal::GenericAATree::iterator;
  using Tree = containers::internal::AATree<const T&, T>;
  using Compare = typename Tree::Compare;
  using GetKey = typename Tree::GetKey;

 public:
  /// IntrusiveSet items must derive from `Item`.
  using Item = typename Tree::Item;

  using key_type = T;
  using value_type = T;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using key_compare = Compare;
  using value_compare = Compare;
  using reference = value_type&;
  using const_reference = const value_type&;
  using pointer = value_type*;
  using const_pointer = const value_type*;

 public:
  class iterator : public containers::internal::AATreeIterator<T> {
   public:
    constexpr iterator() = default;

   private:
    friend IntrusiveSet;
    constexpr explicit iterator(GenericIterator iter)
        : containers::internal::AATreeIterator<T>(iter) {}
  };

  class const_iterator
      : public containers::internal::AATreeIterator<std::add_const_t<T>> {
   public:
    constexpr const_iterator() = default;

   private:
    friend IntrusiveSet;
    constexpr explicit const_iterator(GenericIterator iter)
        : containers::internal::AATreeIterator<std::add_const_t<T>>(iter) {}
  };

  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  /// Constructs an empty set of items.
  constexpr explicit IntrusiveSet() : IntrusiveSet(std::less<T>()) {}

  /// Constructs an empty set of items.
  ///
  /// SFINAE is used to disambiguate between this constructor and the one that
  /// takes an initializer list.
  ///
  /// @param    compare   Function with the signature `bool(T, T)` that is
  ///                     used to order items.
  template <typename Comparator>
  constexpr explicit IntrusiveSet(Comparator compare)
      : tree_(true, std::move(compare), [](const T& t) -> const T& {
          return t;
        }) {
    CheckItemType();
  }

  /// Constructs an IntrusiveSet from an iterator over Items.
  ///
  /// The iterator may dereference as either Item& (e.g. from std::array<Item>)
  /// or Item* (e.g. from std::initializer_list<Item*>).
  template <typename Iterator, typename... Functors>
  IntrusiveSet(Iterator first, Iterator last, Functors&&... functors)
      : IntrusiveSet(std::forward<Functors>(functors)...) {
    tree_.insert(first, last);
  }

  /// Constructs an IntrusiveSet from a std::initializer_list of pointers
  /// to items.
  template <typename... Functors>
  IntrusiveSet(std::initializer_list<T*> items, Functors&&... functors)
      : IntrusiveSet(
            items.begin(), items.end(), std::forward<Functors>(functors)...) {}

  // Iterators

  iterator begin() noexcept { return iterator(tree_.begin()); }
  const_iterator begin() const noexcept {
    return const_iterator(tree_.begin());
  }
  const_iterator cbegin() const noexcept { return begin(); }

  iterator end() noexcept { return iterator(tree_.end()); }
  const_iterator end() const noexcept { return const_iterator(tree_.end()); }
  const_iterator cend() const noexcept { return end(); }

  reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
  const_reverse_iterator rbegin() const noexcept {
    return const_reverse_iterator(end());
  }
  const_reverse_iterator crbegin() const noexcept { return rbegin(); }

  reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
  const_reverse_iterator rend() const noexcept {
    return const_reverse_iterator(begin());
  }
  const_reverse_iterator crend() const noexcept { return rend(); }

  // Capacity

  /// Returns whether the set has zero items or not.
  [[nodiscard]] bool empty() const noexcept { return tree_.empty(); }

  /// Returns the number of items in the set.
  size_t size() const { return tree_.size(); }

  /// Returns how many items can be added.
  ///
  /// As an intrusive container, this is effectively unbounded.
  constexpr size_t max_size() const noexcept { return tree_.max_size(); }

  // Modifiers

  /// Removes all items from the set and leaves it empty.
  ///
  /// The items themselves are not destructed.
  void clear() { tree_.clear(); }

  /// Attempts to add the given item to the set.
  ///
  /// The item will be added if the set does not already contain an equivalent
  /// item.
  ///
  /// @returns  A pointer to the inserted item and `true`, or a pointer to the
  ///           equivalent item and `false`.
  std::pair<iterator, bool> insert(T& item) {
    auto result = tree_.insert(item);
    return std::make_pair(iterator(result.first), result.second);
  }

  iterator insert(iterator, T& item) {
    // Disregard the hint.
    return insert(item).first;
  }

  template <class Iterator>
  void insert(Iterator first, Iterator last) {
    tree_.insert(first, last);
  }

  void insert(std::initializer_list<T*> ilist) {
    tree_.insert(ilist.begin(), ilist.end());
  }

  /// Removes an item from the set and returns an iterator to the item after
  /// the removed item..
  ///
  /// The items themselves are not destructed.
  iterator erase(iterator pos) { return iterator(tree_.erase_one(*pos)); }

  iterator erase(iterator first, iterator last) {
    return iterator(tree_.erase_range(first, last));
  }

  size_t erase(const T& item) { return tree_.erase_all(item); }

  /// Exchanges this set's items with the `other` set's items.
  void swap(IntrusiveSet<T>& other) { tree_.swap(other.tree_); }

  /// Splices items from the `other` set into this one.
  ///
  /// The receiving set's `Compare` function is used when inserting items.
  template <typename MapType>
  void merge(MapType& other) {
    tree_.merge(other.tree_);
  }

  /// Returns the number of equivalent items in the set.
  ///
  /// Since the set requires unique keys, this is always 0 or 1.
  size_t count(const T& item) const { return tree_.count(item); }

  /// Returns a pointer to an item with the given key, or null if the set does
  /// not contain such an item.
  iterator find(const T& item) { return iterator(tree_.find(item)); }

  const_iterator find(const T& item) const {
    return const_iterator(tree_.find(item));
  }

  /// Returns a pair of iterators where the first points to the smallest item
  /// that is not less than the given item, and the second points to the
  /// smallest item that is strictly greater than the given item.
  std::pair<iterator, iterator> equal_range(const T& item) {
    auto result = tree_.equal_range(item);
    return std::make_pair(iterator(result.first), iterator(result.second));
  }
  std::pair<const_iterator, const_iterator> equal_range(const T& item) const {
    auto result = tree_.equal_range(item);
    return std::make_pair(const_iterator(result.first),
                          const_iterator(result.second));
  }

  /// Returns an iterator to the smallest item in the set that is greater than
  /// or equal to the given item, or `end()` if the set is empty.
  iterator lower_bound(const T& item) {
    return iterator(tree_.lower_bound(item));
  }
  const_iterator lower_bound(const T& item) const {
    return const_iterator(tree_.lower_bound(item));
  }

  /// Returns an iterator to the smallest item in the set that is strictly
  /// greater than the given item, or `end()` if the set is empty.
  iterator upper_bound(const T& item) {
    return iterator(tree_.upper_bound(item));
  }
  const_iterator upper_bound(const T& item) const {
    return const_iterator(tree_.upper_bound(item));
  }

 private:
  // Check that T is an Item in a function, since the class T will not be fully
  // defined when the IntrusiveList<T> class is instantiated.
  static constexpr void CheckItemType() {
    using ItemBase = containers::internal::AATreeItem;
    using IntrusiveItemType =
        typename containers::internal::IntrusiveItem<ItemBase, T>::Type;
    static_assert(
        std::is_base_of<IntrusiveItemType, T>(),
        "IntrusiveSet items must be derived from IntrusiveSet<T>::Item, where "
        "T is the item or one of its bases.");
  }

  // Allow sets to access the tree for `merge`.
  template <typename>
  friend class IntrusiveMultiSet;

  // The AA tree that stores the set.
  //
  // This field is mutable so that it doesn't need const overloads.
  mutable Tree tree_;
};

}  // namespace pw
