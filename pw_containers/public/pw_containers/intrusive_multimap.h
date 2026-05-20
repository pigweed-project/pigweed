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

/// @addtogroup pw_containers_maps
/// @{

/// A `std::multimap<Key, T, Compare>`-like class that uses intrusive items.
///
/// Since the map structure is stored in the items themselves, each item must
/// outlive any map it is a part of and must be part of at most one map.
///
/// This map requires unique keys. Attempting to add an item with same key as an
/// item already in the map will fail.
///
/// - Since items are not allocated by this class, the following methods have
///   no analogue:
///   - std::multimap<T>::operator=
///   - std::multimap<T>::get_allocator
///   - std::multimap<T>::emplace
///   - std::multimap<T>::emplace_hint
///
/// - Methods corresponding to the following take initializer lists of pointer
///   to items rather than the items themselves:
///   - std::multimap<T>::(constructor)
///   - std::multimap<T>::insert
///
/// - There are no overloads corresponding to the following methods that take
///   r-value references.:
///   - std::multimap<T>::insert
///   - std::multimap<T>::merge
///
/// - Since modifying the map modifies the items themselves, methods
///   corresponding to those below only take `iterator`s and not
///   `const_iterator`s:
///   - std::multimap<T>::insert
///   - std::multimap<T>::erase
///
/// - An additional overload of `erase` is provided that takes a direct
///   reference to an item.
///
/// - C++23 methods are not (yet) supported.
///
/// @tparam   Key         Type to sort items on
/// @tparam   T           Type of values stored in the map.
template <typename Key, typename T>
class IntrusiveMultiMap {
 private:
  using GenericIterator = containers::internal::GenericAATree::iterator;
  using Tree = containers::internal::AATree<Key, T>;
  using Compare = typename Tree::Compare;
  using GetKey = typename Tree::GetKey;

 public:
  /// IntrusiveMultiMap items must derive from either `Item` or `Pair`.
  /// Use `Pair` to automatically provide storage for a `Key`.
  /// Use `Item` when the derived type has a `key()` accessor method or when the
  /// map provides a custom `GetKey` function object.
  using Item = typename Tree::Item;
  using Pair = typename Tree::Pair;

  using key_type = Key;
  using mapped_type = std::remove_cv_t<T>;
  using value_type = Item;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using key_compare = Compare;
  using reference = value_type&;
  using const_reference = const value_type&;
  using pointer = value_type*;
  using const_pointer = const value_type*;

  class iterator : public containers::internal::AATreeIterator<T> {
   public:
    constexpr iterator() = default;

   private:
    using Base = containers::internal::AATreeIterator<T>;
    friend IntrusiveMultiMap;
    constexpr explicit iterator(GenericIterator iter) : Base(iter) {}
  };

  class const_iterator
      : public containers::internal::AATreeIterator<std::add_const_t<T>> {
   public:
    constexpr const_iterator() = default;

   private:
    using Base = containers::internal::AATreeIterator<std::add_const_t<T>>;
    friend IntrusiveMultiMap;
    constexpr explicit const_iterator(GenericIterator iter) : Base(iter) {}
  };

  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  /// Constructs an empty map of items.
  constexpr explicit IntrusiveMultiMap()
      : IntrusiveMultiMap(std::less<Key>()) {}

  /// Constructs an empty map of items.
  ///
  /// SFINAE is used to disambiguate between this constructor and the one that
  /// takes an initializer list.
  ///
  /// @param  compare   Function with the signature `bool(Key, Key)` that is
  ///                   used to order items.
  template <typename Comparator>
  constexpr explicit IntrusiveMultiMap(Comparator compare)
      : IntrusiveMultiMap(std::move(compare),
                          [](const T& t) { return t.key(); }) {}

  /// Constructs an empty map of items.
  ///
  /// @param  compare   Function with the signature `bool(Key, Key)` that is
  ///                   used to order items.
  /// @param  get_key   Function with signature `Key(const T&)` that returns the
  ///                   value that items are sorted on.
  template <typename Comparator, typename KeyRetriever>
  constexpr IntrusiveMultiMap(Comparator&& compare, KeyRetriever&& get_key)
      : tree_(false,
              std::forward<Comparator>(compare),
              std::forward<KeyRetriever>(get_key)) {
    CheckItemType();
  }

  /// Constructs an IntrusiveMultiMap from an iterator over Items.
  ///
  /// The iterator may dereference as either Item& (e.g. from std::array<Item>)
  /// or Item* (e.g. from std::initializer_list<Item*>).
  template <typename Iterator, typename... Functors>
  IntrusiveMultiMap(Iterator first, Iterator last, Functors&&... functors)
      : IntrusiveMultiMap(std::forward<Functors>(functors)...) {
    tree_.insert(first, last);
  }

  /// Constructs an IntrusiveMultiMap from a std::initializer_list of pointers
  /// to items.
  template <typename... Functors>
  IntrusiveMultiMap(std::initializer_list<T*> items, Functors&&... functors)
      : IntrusiveMultiMap(
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

  /// Returns whether the multimap has zero items or not.
  [[nodiscard]] bool empty() const noexcept { return tree_.empty(); }

  /// Returns the number of items in the multimap.
  size_t size() const { return tree_.size(); }

  /// Returns how many items can be added.
  ///
  /// As an intrusive container, this is effectively unbounded.
  constexpr size_t max_size() const noexcept { return tree_.max_size(); }

  // Modifiers

  /// Removes all items from the multimap and leaves it empty.
  ///
  /// The items themselves are not destructed.
  void clear() { tree_.clear(); }

  /// Adds the given item to the multimap.
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

  /// Removes an item from the multimap and returns an iterator to the item
  /// after the removed item.
  ///
  /// The items themselves are not destructed.
  iterator erase(T& item) { return iterator(tree_.erase_one(item)); }

  iterator erase(iterator pos) { return iterator(tree_.erase_one(*pos)); }

  iterator erase(iterator first, iterator last) {
    return iterator(tree_.erase_range(first, last));
  }

  size_t erase(const Key& key) { return tree_.erase_all(key); }

  /// Exchanges this multimap's items with the `other` multimap's items.
  void swap(IntrusiveMultiMap<Key, T>& other) { tree_.swap(other.tree_); }

  /// Splices items from the `other` map into this one.
  template <typename MapType>
  void merge(MapType& other) {
    tree_.merge(other.tree_);
  }

  /// Returns the number of items in the multimap with the given key.
  size_t count(const Key& key) const { return tree_.count(key); }

  /// Returns a pointer to an item with the given key, or null if the multimap
  /// does not contain such an item.
  iterator find(const Key& key) { return iterator(tree_.find(key)); }

  const_iterator find(const Key& key) const {
    return const_iterator(tree_.find(key));
  }

  /// Returns a pair of iterators where the first points to the item with the
  /// smallest key that is not less than the given key, and the second points to
  /// the item with the smallest key that is greater than the given key.
  std::pair<iterator, iterator> equal_range(const Key& key) {
    auto result = tree_.equal_range(key);
    return std::make_pair(iterator(result.first), iterator(result.second));
  }
  std::pair<const_iterator, const_iterator> equal_range(const Key& key) const {
    auto result = tree_.equal_range(key);
    return std::make_pair(const_iterator(result.first),
                          const_iterator(result.second));
  }

  /// Returns an iterator to the item in the multimap with the smallest key that
  /// is greater than or equal to the given key, or `end()` if the multimap is
  /// empty.
  iterator lower_bound(const Key& key) {
    return iterator(tree_.lower_bound(key));
  }
  const_iterator lower_bound(const Key& key) const {
    return const_iterator(tree_.lower_bound(key));
  }

  /// Returns an iterator to the item in the multimap with the smallest key that
  /// is strictly greater than the given key, or `end()` if the multimap is
  /// empty.
  iterator upper_bound(const Key& key) {
    return iterator(tree_.upper_bound(key));
  }
  const_iterator upper_bound(const Key& key) const {
    return const_iterator(tree_.upper_bound(key));
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
        "IntrusiveMultiMap items must be derived from "
        "IntrusiveMultiMap<Key, T>::Item, where T is the item or one of its "
        "bases.");
  }

  // Allow maps to access the tree for `merge`.
  template <typename, typename>
  friend class IntrusiveMap;

  // The AA tree that stores the map.
  //
  // This field is mutable so that it doesn't need const overloads.
  mutable Tree tree_;
};

}  // namespace pw
