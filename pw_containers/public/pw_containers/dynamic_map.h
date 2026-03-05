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
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_assert/assert.h"
#include "pw_containers/internal/traits.h"
#include "pw_containers/intrusive_map.h"

namespace pw {

/// @submodule{pw_containers,maps}

/// Dynamic ordered map, similar to `std::map`, but optimized for embedded.
///
/// Key features of `pw::DynamicMap`.
///
/// - Uses a `pw::Allocator` for memory operations for each node.
/// - Provides a `std::map`-like API, but adds `try_*` versions of
///   operations that return `std::nullopt` on allocation failure.
///   - `insert()` & `try_insert()`.
///   - `emplace()` & `try_emplace()`.
/// - Never allocates in the constructor. `constexpr` constructible.
/// - Leverages `pw::IntrusiveMap` internally to manage the balanced tree
///   structure without additional per-node overhead beyond the user data and
///   tree pointers.
template <typename Key, typename Value>
class DynamicMap {
 private:
  /// MapItem is the internal node structure.
  /// It inherits from the IntrusiveMap Item to allow it to be linked into the
  /// tree and stores the actual Key-Value pair as data.
  class MapItem : public IntrusiveMap<Key, MapItem>::Item {
   public:
    using IntrusiveMap<Key, MapItem>::Item::Item;

    /// Constructs the Key-Value pair in-place within the node using piecewise
    /// construction to avoid unnecessary copies or moves.
    template <typename K, typename... Args>
    explicit MapItem(K&& key, Args&&... args)
        : Map::Item(),
          key_value(std::piecewise_construct,
                    std::forward_as_tuple(std::forward<K>(key)),
                    std::forward_as_tuple(std::forward<Args>(args)...)) {}

    const Key& key() const { return key_value.first; }
    Value& value() { return key_value.second; }
    const Value& value() const { return key_value.second; }

   private:
    std::pair<const Key, Value> key_value;
    friend class DynamicMap;
  };
  using Map = IntrusiveMap<Key, MapItem>;

  /// Custom iterator that wraps the IntrusiveMap's iterator.
  /// It dereferences to a std::pair<const Key, Value>, matching std::map
  /// behavior.
  template <bool kIsConst>
  class Iterator {
   public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = std::pair<const Key, Value>;
    using difference_type = std::ptrdiff_t;
    using reference =
        std::conditional_t<kIsConst, const value_type&, value_type&>;
    using pointer =
        std::conditional_t<kIsConst, const value_type*, value_type*>;

    constexpr Iterator() = default;

    constexpr reference operator*() const { return it_->key_value; }
    constexpr pointer operator->() const { return &it_->key_value; }

    constexpr Iterator operator++() {
      ++it_;
      return *this;
    }
    constexpr Iterator operator++(int) {
      Iterator result = *this;
      ++it_;
      return result;
    }
    constexpr Iterator operator--() {
      --it_;
      return *this;
    }
    constexpr Iterator operator--(int) {
      Iterator result = *this;
      --it_;
      return result;
    }

    constexpr friend bool operator==(Iterator lhs, Iterator rhs) {
      return lhs.it_ == rhs.it_;
    }
    constexpr friend bool operator!=(Iterator lhs, Iterator rhs) {
      return lhs.it_ != rhs.it_;
    }

   private:
    using BaseIterator = std::conditional_t<kIsConst,
                                            typename Map::const_iterator,
                                            typename Map::iterator>;

    constexpr explicit Iterator(BaseIterator it) : it_(it) {}

    template <bool>
    friend class Iterator;
    friend class DynamicMap;

    BaseIterator it_;
  };

 public:
  using key_type = Key;
  using mapped_type = Value;
  using value_type = std::pair<const Key, Value>;
  using size_type = typename Map::size_type;
  using difference_type = typename Map::difference_type;
  using key_compare = typename Map::key_compare;
  using allocator_type = Allocator;
  using reference = value_type&;
  using const_reference = const value_type&;
  using pointer = value_type*;
  using const_pointer = const value_type*;
  using iterator = Iterator<false>;
  using const_iterator = Iterator<true>;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;
  using insert_return_type = std::pair<iterator, bool>;

  /// Constructs an empty `DynamicMap`. No memory is allocated.
  ///
  /// Since allocations can fail, initialization in the constructor is not
  /// supported.
  constexpr explicit DynamicMap(Allocator& allocator) noexcept
      : allocator_(&allocator), map_() {}

  /// Copy construction/assignment is not supported because they require
  /// allocations that could fail. Use `merge()` or manual insertion instead.
  DynamicMap(const DynamicMap&) = delete;
  DynamicMap& operator=(const DynamicMap&) = delete;

  /// Move construction transfers ownership of all allocated nodes from `other`.
  /// No new allocations are performed.
  DynamicMap(DynamicMap&& other) noexcept {
    allocator_ = other.allocator_;
    map_.swap(other.map_);
  }

  /// Move assignment clears the current map and takes ownership of nodes from
  /// `other`.
  DynamicMap& operator=(DynamicMap&& other) noexcept {
    if (&other == this) {
      return *this;
    }
    clear();
    allocator_ = other.allocator_;
    map_.swap(other.map_);
    return *this;
  }

  ~DynamicMap() { clear(); }

  /// Returns the allocator used by this map.
  constexpr allocator_type& get_allocator() const { return *allocator_; }

  // Iterators

  iterator begin() noexcept { return iterator(map_.begin()); }
  const_iterator begin() const noexcept { return const_iterator(map_.begin()); }
  const_iterator cbegin() const noexcept { return begin(); }
  iterator end() noexcept { return iterator(map_.end()); }
  const_iterator end() const noexcept { return const_iterator(map_.end()); }
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

  // Element access

  /// Returns a reference to the mapped value of the element with key equivalent
  /// to `key`. If no such element exists, an assertion is triggered.
  mapped_type& at(const key_type& key) { return map_.at(key).value(); }

  const mapped_type& at(const key_type& key) const {
    return map_.at(key).value();
  }

  /// Returns a reference to the value associated with `key`. If `key` does not
  /// exist, it is inserted via a default-constructed value.
  ///
  /// @pre The allocation of a new node must succeed. Crashes on failure.
  template <typename U = mapped_type,
            typename = std::enable_if_t<std::is_default_constructible_v<U>>>
  mapped_type& operator[](const key_type& key) {
    return emplace(key).first->second;
  }

  // Capacity

  [[nodiscard]] bool empty() const { return map_.empty(); }
  size_type size() const { return map_.size(); }
  constexpr size_type max_size() const noexcept { return map_.max_size(); }

  /// Removes all elements from the map and deallocates each node.
  void clear() {
    while (!empty()) {
      erase(begin());
    }
  }

  // Modifiers

  /// Attempts to insert a value into the map.
  /// @returns An `insert_return_type` on success, or `std::nullopt` if
  ///          allocation fails.
  [[nodiscard]] std::optional<insert_return_type> try_insert(
      const value_type& value) {
    return try_emplace(value.first, value.second);
  }

  // Moving into a fallible insertion is deleted to prevent "ghost moves."
  // If allocation fails, the object would be moved-from but not stored.
  // Use try_emplace instead to ensure moves only occur on success.
  std::optional<insert_return_type> try_insert(value_type&&) = delete;

  /// Inserts a value into the map. Crashes on allocation failure.
  insert_return_type insert(const value_type& value) {
    return emplace(value.first, value.second);
  }

  insert_return_type insert(value_type&& value) {
    return emplace(std::move(value.first), std::move(value.second));
  }

  /// Inserts a range of elements. Crashes on allocation failure.
  template <typename InputIt,
            typename = containers::internal::EnableIfInputIterator<InputIt>>
  void insert(InputIt first, InputIt last) {
    for (auto it = first; it != last; ++it) {
      emplace(it->first, it->second);
    }
  }

  void insert(std::initializer_list<value_type> ilist) {
    insert(ilist.begin(), ilist.end());
  }

  // TODO: b/483691699 - Support hint-optimized insertion

  /// Attempts to construct an element in-place.
  /// @returns A pair containing the iterator and success bool, or
  ///          `std::nullopt` on allocation failure.
  template <typename K, typename... Args>
  [[nodiscard]] std::optional<std::pair<iterator, bool>> try_emplace(
      K&& key, Args&&... args) {
    return TryEmplaceImpl(std::forward<K>(key), std::forward<Args>(args)...);
  }

  /// Constructs an element in-place. Crashes on allocation failure.
  template <typename K, typename... Args>
  std::pair<iterator, bool> emplace(K&& key, Args&&... args) {
    auto result =
        try_emplace(std::forward<K>(key), std::forward<Args>(args)...);
    PW_ASSERT(result.has_value());
    return result.value();
  }

  /// Removes the element at `pos` and deallocates the node.
  iterator erase(iterator pos) {
    MapItem* item = &(*pos.it_);
    auto next_it = map_.erase(pos.it_);
    allocator_->Delete(item);
    return iterator(next_it);
  }

  iterator erase(const_iterator pos) {
    auto it = find(pos->first);
    return erase(it);
  }

  /// Removes elements in the range `[first, last)`.
  iterator erase(iterator first, iterator last) {
    while (first != last) {
      first = erase(first);
    }
    return last;
  }

  /// Removes the element with the matching key. Returns number of elements
  /// removed (0 or 1).
  template <typename K>
  size_type erase(K&& key) {
    auto it = find(key);
    if (it == end()) {
      return 0;
    }
    erase(it);
    return 1;
  }

  /// Swaps the contents and allocators of two maps. No allocations occur.
  void swap(DynamicMap& other) {
    std::swap(allocator_, other.allocator_);
    map_.swap(other.map_);
  }

  /// Attempts to move elements from `other` into this map. Elements are only
  /// removed from `other` if they are successfully inserted.
  void merge(DynamicMap& other) {
    for (auto it = other.begin(); it != other.end();) {
      auto result = try_emplace(std::move(it->first), std::move(it->second));
      if (result.has_value() && result.value().second) {
        it = other.erase(it);
      } else {
        ++it;
      }
    }
  }

  void merge(DynamicMap&& other) { merge(other); }

  // Lookup

  size_type count(const key_type& key) { return contains(key) ? 1 : 0; }

  iterator find(const key_type& key) { return iterator(map_.find(key)); }

  const_iterator find(const key_type& key) const {
    return const_iterator(map_.find(key));
  }

  [[nodiscard]] bool contains(const key_type& key) const {
    return find(key) != end();
  }

  std::pair<iterator, iterator> equal_range(const key_type& key) {
    auto result = map_.equal_range(key);
    return std::make_pair(iterator(result.first), iterator(result.second));
  }

  std::pair<const_iterator, const_iterator> equal_range(
      const key_type& key) const {
    auto result = map_.equal_range(key);
    return std::make_pair(const_iterator(result.first),
                          const_iterator(result.second));
  }

  iterator lower_bound(const key_type& key) {
    return iterator(map_.lower_bound(key));
  }

  const_iterator lower_bound(const key_type& key) const {
    return const_iterator(map_.lower_bound(key));
  }

  iterator upper_bound(const key_type& key) {
    return iterator(map_.upper_bound(key));
  }

  const_iterator upper_bound(const key_type& key) const {
    return const_iterator(map_.upper_bound(key));
  }

 private:
  /// Internal implementation for emplace operations. Handles allocation
  /// and tree insertion.
  template <typename K, typename... Args>
  [[nodiscard]] std::optional<std::pair<iterator, bool>> TryEmplaceImpl(
      K&& key, Args&&... args) {
    // Perform lookup before allocation to avoid unnecessary heap usage.
    if (auto it = map_.find(key); it != map_.end()) {
      return std::make_pair(iterator(it), false);
    }
    MapItem* item = allocator_->template New<MapItem>(
        std::forward<K>(key), std::forward<Args>(args)...);
    if (item == nullptr) {
      return std::nullopt;
    }
    auto [item_it, inserted] = map_.insert(*item);
    PW_ASSERT(inserted);
    return std::make_pair(iterator(item_it), true);
  }

  Allocator* allocator_;
  Map map_;
};

}  // namespace pw
