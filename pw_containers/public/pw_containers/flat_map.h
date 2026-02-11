// Copyright 2020 The Pigweed Authors
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
#include <array>
#include <cstddef>
#include <iterator>
#include <type_traits>
#include <utility>

#include "pw_assert/assert.h"

namespace pw::containers {

// Define and use a custom Pair object. This is because std::pair does not
// support constexpr assignment until C++20. The assignment is needed since
// the array of pairs will be sorted in the constructor (if not already).
template <typename First, typename Second>
struct Pair {
  First first;
  Second second;

  bool operator==(const Pair& p) const {
    return first == p.first && second == p.second;
  }

  bool operator!=(const Pair& p) const { return !(*this == p); }
};

template <typename T1, typename T2>
Pair(T1, T2) -> Pair<T1, T2>;

/// @module{pw_containers}

/// @addtogroup pw_containers_maps
/// @{

/// A simple, fixed-size associative array with lookup by key or value.
///
/// The keys do not need to be sorted as the constructor will sort the items
/// if need be.
template <typename Key, typename Value, size_t kArraySize>
class FlatMap {
 public:
  using key_type = Key;
  using mapped_type = Value;
  using value_type = Pair<key_type, mapped_type>;
  using pointer = value_type*;
  using reference = value_type&;
  using size_type = size_t;
  using difference_type = ptrdiff_t;
  using container_type = typename std::array<value_type, kArraySize>;
  using iterator = typename container_type::iterator;
  using const_iterator = typename container_type::const_iterator;

  /// A bidirectional iterator for the mapped values.
  ///
  /// Also an output iterator.
  class mapped_iterator {
   public:
    using value_type = FlatMap::mapped_type;
    using difference_type = std::ptrdiff_t;
    using pointer = value_type*;
    using reference = value_type&;
    using iterator_category = std::bidirectional_iterator_tag;

    constexpr mapped_iterator() : current_{} {}

    constexpr mapped_iterator(const mapped_iterator& other) = default;
    constexpr mapped_iterator& operator=(const mapped_iterator& other) =
        default;

    constexpr reference operator*() const {
      reference value = current_->second;
      return value;
    }

    constexpr pointer operator->() const { return &operator*(); }

    constexpr mapped_iterator& operator++() {
      ++current_;
      return *this;
    }

    constexpr mapped_iterator operator++(int) {
      mapped_iterator original = *this;
      operator++();
      return original;
    }

    constexpr mapped_iterator& operator--() {
      --current_;
      return *this;
    }

    constexpr mapped_iterator operator--(int) {
      mapped_iterator original = *this;
      operator--();
      return original;
    }

    constexpr bool operator==(const mapped_iterator& other) const {
      return current_ == other.current_;
    }

    constexpr bool operator!=(const mapped_iterator& other) const {
      return !(*this == other);
    }

   private:
    friend class FlatMap;

    constexpr mapped_iterator(FlatMap::iterator current) : current_(current) {}

    FlatMap::iterator current_;
  };

  constexpr FlatMap(const std::array<value_type, kArraySize>& items)
      : items_(items) {
    ConstexprSort(items_.begin(), kArraySize);
  }

  constexpr FlatMap(std::array<value_type, kArraySize>&& items)
      : items_(std::move(items)) {
    ConstexprSort(items_.begin(), kArraySize);
  }

  // Omits explicit here to support assignment-like syntax, which is common to
  // initialize a container.
  template <typename... Items,
            typename = std::enable_if_t<
                std::conjunction_v<std::is_same<Items, value_type>...>>>
  constexpr FlatMap(const Items&... items) : FlatMap(std::array{items...}) {}

  FlatMap(FlatMap&) = delete;
  FlatMap& operator=(FlatMap&) = delete;

  // Capacity.
  constexpr size_type size() const { return kArraySize; }
  constexpr size_type empty() const { return size() == 0; }
  constexpr size_type max_size() const { return kArraySize; }

  // Lookup.
  /// Accesses a mutable mapped value.
  ///
  /// @pre The key must exist.
  ///
  /// @param[in] key The key to the mapped value.
  ///
  /// @returns A reference to the mapped value.
  constexpr mapped_type& at(const key_type& key) {
    PW_ASSERT(end() != begin());
    iterator it = std::lower_bound(
        items_.begin(), items_.end(), key, IsItemKeyLessThanGivenKey);
    const key_type& found_key = it->first;
    PW_ASSERT(it != items_.end() && found_key == key);
    mapped_type& found_value = it->second;
    return found_value;
  }

  /// Accesses a mapped value.
  ///
  /// @pre The key must exist.
  ///
  /// @param[in] key The key to the mapped value.
  ///
  /// @returns A const reference to the mapped value.
  constexpr const mapped_type& at(const key_type& key) const {
    PW_ASSERT(end() != begin());
    const_iterator it = std::lower_bound(
        items_.cbegin(), items_.cend(), key, IsItemKeyLessThanGivenKey);
    const key_type& found_key = it->first;
    PW_ASSERT(it != items_.cend() && found_key == key);
    const mapped_type& found_value = it->second;
    return found_value;
  }

  constexpr bool contains(const key_type& key) const {
    return find(key) != end();
  }

  constexpr const_iterator find(const key_type& key) const {
    if (end() == begin()) {
      return end();
    }

    const_iterator it = lower_bound(key);
    if (it == end() || it->first != key) {
      return end();
    }
    return it;
  }

  constexpr const_iterator lower_bound(const key_type& key) const {
    return std::lower_bound(begin(), end(), key, IsItemKeyLessThanGivenKey);
  }

  constexpr const_iterator upper_bound(const key_type& key) const {
    return std::upper_bound(
        begin(), end(), key, [](key_type lkey, const value_type& item) {
          return item.first > lkey;
        });
  }

  constexpr std::pair<const_iterator, const_iterator> equal_range(
      const key_type& key) const {
    if (end() == begin()) {
      return std::make_pair(end(), end());
    }

    return std::make_pair(lower_bound(key), upper_bound(key));
  }

  // Iterators.
  constexpr const_iterator begin() const { return cbegin(); }
  constexpr const_iterator cbegin() const { return items_.cbegin(); }
  constexpr const_iterator end() const { return cend(); }
  constexpr const_iterator cend() const { return items_.cend(); }

  /// Gets an iterator to the first mapped value.
  ///
  /// Mapped iterators iterate through the mapped values, and allow mutation of
  /// those values (for a non-const FlatMap object).
  ///
  /// @returns An iterator to the first mapped value.
  constexpr mapped_iterator mapped_begin() {
    return mapped_iterator(items_.begin());
  }

  /// Gets an iterator to one past the last mapped value.
  ///
  /// Mapped iterators iterate through the mapped values, and allow mutation of
  /// those values (for a non-const FlatMap object).
  ///
  /// @returns An iterator to one past the last mapped value.
  constexpr mapped_iterator mapped_end() {
    return mapped_iterator(items_.end());
  }

 private:
  // Simple stable insertion sort function for constexpr support.
  // std::stable_sort is not constexpr. Should not be a problem with performance
  // in regards to the sizes that are typically dealt with.
  static constexpr void ConstexprSort(iterator data, size_type size) {
    if (size < 2) {
      return;
    }

    for (iterator it = data + 1,
                  end = data + static_cast<difference_type>(size);
         it < end;
         ++it) {
      if (it->first < it[-1].first) {
        // Rotate the value into place.
        value_type temp = std::move(*it);
        iterator it2 = it - 1;
        while (true) {
          *(it2 + 1) = std::move(*it2);
          if (it2 == data || !(temp.first < it2[-1].first)) {
            break;
          }
          --it2;
        }
        *it2 = std::move(temp);
      }
    }
  }

  static constexpr bool IsItemKeyLessThanGivenKey(const value_type& item,
                                                  key_type key) {
    const key_type& item_key = item.first;
    return item_key < key;
  }

  std::array<value_type, kArraySize> items_;
};

template <typename K, typename V, typename... Items>
FlatMap(const Pair<K, V>& item1, const Items&... items)
    -> FlatMap<K, V, 1 + sizeof...(items)>;

}  // namespace pw::containers
