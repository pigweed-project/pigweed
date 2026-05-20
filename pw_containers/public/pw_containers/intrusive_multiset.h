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

#include "pw_containers/internal/aa_tree.h"
#include "pw_containers/internal/aa_tree_item.h"

namespace pw {

/// @module{pw_containers}

/// @defgroup pw_containers_sets Sets
/// @{

/// A `std::multiset<Key, Compare>`-like class that uses intrusive items.
///
/// Since the set structure is stored in the items themselves, each item must
/// outlive any set it is a part of and must be part of at most one set.
///
/// This set does not require unique keys. Multiple equivalent items may be
/// added.
///
/// - Since items are not allocated by this class, the following methods have
///   no analogue:
///   - std::multiset<T>::operator=
///   - std::multiset<T>::get_allocator
///   - std::multiset<T>::emplace
///   - std::multiset<T>::emplace_hint
///
/// - Methods corresponding to the following take initializer lists of pointer
///   to items rather than the items themselves:
///   - std::multiset<T>::(constructor)
///   - std::multiset<T>::insert
///
/// - There are no overloads corresponding to the following methods that take
///   r-value references.:
///   - std::multiset<T>::insert
///   - std::multiset<T>::merge
///
/// - Since modifying the set modifies the items themselves, methods
///   corresponding to those below only take `iterator`s and not
///   `const_iterator`s:
///   - std::multiset<T>::insert
///   - std::multiset<T>::erase
///
/// - C++23 methods are not (yet) supported.
///
/// @tparam   T           Type of items stored in the set.
template <typename T>
class IntrusiveMultiSet {
 private:
  using GenericIterator = containers::internal::GenericAATree::iterator;
  using Tree = containers::internal::AATree<const T&, T>;
  using Compare = typename Tree::Compare;
  using GetKey = typename Tree::GetKey;

 public:
  /// IntrusiveMultiSet items must derive from `Item`.
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
    using Base = containers::internal::AATreeIterator<T>;
    friend IntrusiveMultiSet;
    constexpr explicit iterator(GenericIterator iter) : Base(iter) {}
  };

  class const_iterator
      : public containers::internal::AATreeIterator<std::add_const_t<T>> {
   public:
    constexpr const_iterator() = default;

   private:
    using Base = containers::internal::AATreeIterator<std::add_const_t<T>>;
    friend IntrusiveMultiSet;
    constexpr explicit const_iterator(GenericIterator iter) : Base(iter) {}
  };

  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  /// Constructs an empty set of items.
  constexpr explicit IntrusiveMultiSet() : IntrusiveMultiSet(std::less<>()) {}

  /// Constructs an empty set of items.
  ///
  /// SFINAE is used to disambiguate between this constructor and the one that
  /// takes an initializer list.
  ///
  /// @param    compare   Function with the signature `bool(T, T)` that is
  ///                     used to order items.
  template <typename Comparator>
  constexpr explicit IntrusiveMultiSet(Comparator compare)
      : tree_(false, std::move(compare), [](const T& t) -> const T& {
          return t;
        }) {
    CheckItemType();
  }

  /// Constructs an IntrusiveMultiSet from an iterator over Items.
  ///
  /// The iterator may dereference as either Item& (e.g. from std::array<Item>)
  /// or Item* (e.g. from std::initializer_list<Item*>).
  template <typename Iterator, typename... Functors>
  IntrusiveMultiSet(Iterator first, Iterator last, Functors&&... functors)
      : IntrusiveMultiSet(std::forward<Functors>(functors)...) {
    tree_.insert(first, last);
  }

  /// Constructs an IntrusiveMultiSet from a std::initializer_list of pointers
  /// to items.
  template <typename... Functors>
  IntrusiveMultiSet(std::initializer_list<T*> items, Functors&&... functors)
      : IntrusiveMultiSet(
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

  /// Returns whether the multiset has zero items or not.
  [[nodiscard]] bool empty() const noexcept { return tree_.empty(); }

  /// Returns the number of items in the multiset.
  size_t size() const { return tree_.size(); }

  /// Returns how many items can be added.
  ///
  /// As an intrusive container, this is effectively unbounded.
  constexpr size_t max_size() const noexcept { return tree_.max_size(); }

  // Modifiers

  /// Removes all items from the multiset and leaves it empty.
  ///
  /// The items themselves are not destructed.
  void clear() { tree_.clear(); }

  /// Adds the given item to the multiset.
  iterator insert(T& item) { return iterator(tree_.insert(item).first); }

  iterator insert(iterator, T& item) {
    // Disregard the hint.
    return iterator(insert(item));
  }

  template <class Iterator>
  void insert(Iterator first, Iterator last) {
    tree_.insert(first, last);
  }

  void insert(std::initializer_list<T*> ilist) {
    tree_.insert(ilist.begin(), ilist.end());
  }

  /// Removes an item from the multiset and returns an iterator to the item
  /// after the removed item.
  ///
  /// The items themselves are not destructed.
  iterator erase(iterator pos) { return iterator(tree_.erase_one(*pos)); }

  iterator erase(iterator first, iterator last) {
    return iterator(tree_.erase_range(first, last));
  }

  size_t erase(const T& item) { return tree_.erase_all(item); }

  /// Exchanges this multiset's items with the `other` multiset's items.
  void swap(IntrusiveMultiSet<T>& other) { tree_.swap(other.tree_); }

  /// Splices items from the `other` set into this one.
  ///
  /// The receiving set's `Compare` function is used when inserting items.
  template <typename MapType>
  void merge(MapType& other) {
    tree_.merge(other.tree_);
  }

  /// Returns the number of items in the multimap with the given key.
  size_t count(const T& item) const { return tree_.count(item); }

  /// Returns a pointer to an item with the given key, or null if the multimap
  /// does not contain such an item.
  iterator find(const T& item) { return iterator(tree_.find(item)); }

  const_iterator find(const T& item) const {
    return const_iterator(tree_.find(item));
  }

  /// Returns a pair of iterators where the first points to the item with the
  /// smallest key that is not less than the given key, and the second points to
  /// the item with the smallest key that is greater than the given key.
  std::pair<iterator, iterator> equal_range(const T& item) {
    auto result = tree_.equal_range(item);
    return std::make_pair(iterator(result.first), iterator(result.second));
  }
  std::pair<const_iterator, const_iterator> equal_range(const T& item) const {
    auto result = tree_.equal_range(item);
    return std::make_pair(const_iterator(result.first),
                          const_iterator(result.second));
  }

  /// Returns an iterator to the item in the multimap with the smallest key that
  /// is greater than or equal to the given key, or `end()` if the multimap is
  /// empty.
  iterator lower_bound(const T& item) {
    return iterator(tree_.lower_bound(item));
  }
  const_iterator lower_bound(const T& item) const {
    return const_iterator(tree_.lower_bound(item));
  }

  /// Returns an iterator to the item in the multimap with the smallest key that
  /// is strictly greater than the given key, or `end()` if the multimap is
  /// empty.
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
        "IntrusiveMultiSet items must be derived from "
        "IntrusiveMultiSet<T>::Item, where T is the item or one of its "
        "bases.");
  }

  // Allow sets to access the tree for `merge`.
  template <typename>
  friend class IntrusiveSet;

  // The AA tree that stores the set.
  //
  // This field is mutable so that it doesn't need const overloads.
  mutable Tree tree_;
};

}  // namespace pw
