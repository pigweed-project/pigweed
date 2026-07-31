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

#include <functional>
#include <list>
#include <optional>
#include <unordered_map>
#include <utility>

#include "pw_assert/check.h"

namespace bt {

// A least-recently-used (LRU) cache that maintains a fixed maximum number of
// items, evicting the least recently accessed entry when capacity is reached.
// Uses an unordered_map for O(1) key lookups and a list for O(1) LRU ordering
// and eviction.
template <typename Key,
          typename Value,
          class Hasher = std::hash<Key>,
          class KeyEqual = std::equal_to<Key>>
class LruCache {
 public:
  explicit LruCache(size_t max_size) : max_size_(max_size) {
    PW_CHECK(max_size > 0, "LruCache max_size must be greater than 0");
  }

  // Looks up the item by |key|. If found, marks the item as most recently used
  // and returns a reference to its value. Returns std::nullopt if not found.
  std::optional<std::reference_wrapper<Value>> get(const Key& key) {
    auto map_it = map_.find(key);
    if (map_it == map_.end()) {
      return std::nullopt;
    }

    // Move the accessed node to the front of the list (most recently used).
    list_.splice(list_.begin(), list_, map_it->second);
    return std::ref(map_it->second->value);
  }

  // Const version of get() that does not alter LRU ordering.
  std::optional<std::reference_wrapper<const Value>> peek(
      const Key& key) const {
    auto map_it = map_.find(key);
    if (map_it == map_.end()) {
      return std::nullopt;
    }

    return std::cref(map_it->second->value);
  }

  // Inserts or updates the item with |key|. If an entry already exists for
  // |key|, its value is updated and it becomes the most recently used item.
  // Otherwise, a new entry is inserted at the front of the LRU queue. If the
  // cache is at maximum capacity, the least recently used entry is evicted.
  void put(Key key, Value value) {
    auto map_it = map_.find(key);
    if (map_it != map_.end()) {
      map_it->second->value = std::move(value);
      list_.splice(list_.begin(), list_, map_it->second);
      return;
    }

    if (list_.size() >= max_size_) {
      map_.erase(list_.back().key);
      list_.pop_back();
    }

    list_.push_front(Node{key, std::move(value)});
    map_.emplace(list_.front().key, list_.begin());
  }

  // Removes the item associated with |key| from the cache if it exists.
  // Returns true if an item was removed, false otherwise.
  bool remove(const Key& key) {
    auto map_it = map_.find(key);
    if (map_it == map_.end()) {
      return false;
    }

    list_.erase(map_it->second);
    map_.erase(map_it);
    return true;
  }

  // Removes all elements from the cache.
  void clear() {
    map_.clear();
    list_.clear();
  }

  // Returns true if an item with |key| exists in the cache. Does not alter LRU
  // ordering.
  bool contains(const Key& key) const { return map_.find(key) != map_.end(); }

  // Returns the current number of elements in the cache.
  size_t size() const { return list_.size(); }

  // Returns true if the cache contains no elements.
  bool empty() const { return list_.empty(); }

  // Returns the maximum capacity of the cache.
  size_t max_size() const { return max_size_; }

 private:
  struct Node {
    Key key;
    Value value;
  };

  using ListIterator = typename std::list<Node>::iterator;

  size_t max_size_;
  std::list<Node> list_;
  std::unordered_map<Key, ListIterator, Hasher, KeyEqual> map_;
};

}  // namespace bt
