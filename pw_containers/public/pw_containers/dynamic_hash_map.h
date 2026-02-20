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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_assert/assert.h"
#include "pw_containers/dynamic_deque.h"
#include "pw_containers/dynamic_ptr_vector.h"
#include "pw_containers/functional.h"
#include "pw_preprocessor/compiler.h"

namespace pw {

/// @submodule{pw_containers,maps}

/// Unordered associative container, similar to `std::unordered_map`, but
/// optimized for embedded environments.
///
/// Key features of `pw::DynamicHashMap`:
///
/// - Uses a `pw::Allocator` for all memory operations.
/// - Provides the `std::unordered_map` API, but adds `try_*` versions of
///   operations that crash on allocation failure.
///   - `insert()` & `try_insert()`
///   - `emplace()` & `try_emplace()`
///   - `rehash()` & `try_rehash()`
///   - `reserve()` & `try_reserve()`
/// - Hybrid storage model:
///   - **Nodes:** Stored in a dense `DynamicPtrVector` to enable efficient,
///     linear iteration.
///   - **Buckets:** Stored in a `DynamicDeque` for O(1) average lookup.
/// - **Unstable Iteration:** Uses "swap-and-pop" erasure for efficiency.
///   Erasing an element moves the last element of the map into the erased
///   position, changing the iteration order.
/// - Never allocates in the constructor.
template <typename Key,
          typename Value,
          typename Hash = pw::Hash,
          typename Equal = pw::EqualTo,
          typename SizeType = uint16_t>
class DynamicHashMap {
 private:
  /// Internal node linking vector storage (`index`) and bucket lookups
  /// (`next`).
  struct Node {
    /// The stored key-value pair.
    std::pair<const Key, Value> key_value;

    /// Pointer to the next node in the same hash bucket (collision chain).
    Node* next = nullptr;

    /// Index of this node in the `nodes_` vector, enabling O(1) erasure.
    SizeType index = 0;

    template <typename K, typename... Args>
    explicit Node(K&& key, Args&&... args)
        : key_value(std::piecewise_construct,
                    std::forward_as_tuple(std::forward<K>(key)),
                    std::forward_as_tuple(std::forward<Args>(args)...)) {}
  };

  using NodeType = Node;
  using NodeVectorType = pw::DynamicPtrVector<NodeType, SizeType>;
  using KeyValueType = decltype(std::declval<NodeType>().key_value);

  // Intermediate type for math to prevent overflow while staying efficient.
  // Uses 32-bit for 16-bit SizeType, and 64-bit for 32-bit SizeType.
  using CalcType = std::
      conditional_t<sizeof(SizeType) < sizeof(uint32_t), uint32_t, uint64_t>;

  /// Custom iterator that wraps the DynamicPtrVector's iterator.
  /// It dereferences to a std::pair<const Key, Value>, matching
  /// std::unordered_map behavior.
  template <bool kIsConst>
  class Iterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = KeyValueType;
    using difference_type = std::ptrdiff_t;
    using reference =
        std::conditional_t<kIsConst, const value_type&, value_type&>;
    using pointer =
        std::conditional_t<kIsConst, const value_type*, value_type*>;

    constexpr Iterator() = default;

    template <bool kOtherConst,
              typename = std::enable_if_t<kIsConst && !kOtherConst>>
    constexpr Iterator(const Iterator<kOtherConst>& other) : it_(other.it_) {}

    reference operator*() const { return it_->key_value; }
    pointer operator->() const { return &(it_->key_value); }

    Iterator operator++() {
      ++it_;
      return *this;
    }
    Iterator operator++(int) {
      Iterator tmp = *this;
      ++it_;
      return tmp;
    }
    friend bool operator==(Iterator lhs, Iterator rhs) {
      return lhs.it_ == rhs.it_;
    }
    friend bool operator!=(Iterator lhs, Iterator rhs) {
      return lhs.it_ != rhs.it_;
    }

   private:
    using BaseIterator =
        std::conditional_t<kIsConst,
                           typename NodeVectorType::const_iterator,
                           typename NodeVectorType::iterator>;

    constexpr explicit Iterator(BaseIterator it) : it_(it) {}

    friend class DynamicHashMap;

    BaseIterator it_;
  };

 public:
  using key_type = Key;
  using mapped_type = Value;
  using value_type = KeyValueType;
  using size_type = SizeType;
  using difference_type = std::ptrdiff_t;
  using hasher = Hash;
  using key_equal = Equal;
  using allocator_type = Allocator;
  using reference = value_type&;
  using const_reference = const value_type&;
  using pointer = value_type*;
  using const_pointer = const value_type*;
  using iterator = Iterator<false>;
  using const_iterator = Iterator<true>;
  using node_type = NodeType;
  using insert_return_type = std::pair<iterator, bool>;

  /// Constructs an empty `DynamicHashMap`. No memory is allocated.
  ///
  /// Since allocations can fail, initialization in the constructor is not
  /// supported.
  constexpr explicit DynamicHashMap(Allocator& allocator,
                                    const Hash& hash = Hash(),
                                    const Equal& equal = Equal()) noexcept
      : buckets_(allocator), nodes_(allocator), hash_(hash), equal_(equal) {}

  ~DynamicHashMap() { clear(); }

  /// Copy construction/assignment is not supported because they require
  /// allocations that could fail.
  DynamicHashMap(const DynamicHashMap&) = delete;
  DynamicHashMap& operator=(const DynamicHashMap&) = delete;

  /// Move construction is supported since it cannot fail. Copy construction
  /// is not supported.
  DynamicHashMap(DynamicHashMap&& other) noexcept
      : buckets_(std::move(other.buckets_)),
        nodes_(std::move(other.nodes_)),
        hash_(std::move(other.hash_)),
        equal_(std::move(other.equal_)),
        max_load_factor_percent_(other.max_load_factor_percent_),
        rehash_threshold_(other.rehash_threshold_) {}

  /// Move assignment clears the current map and takes ownership of nodes from
  /// `other`.
  DynamicHashMap& operator=(DynamicHashMap&& other) noexcept {
    if (&other == this) {
      return *this;
    }
    clear();
    buckets_ = std::move(other.buckets_);
    nodes_ = std::move(other.nodes_);
    hash_ = std::move(other.hash_);
    equal_ = std::move(other.equal_);
    max_load_factor_percent_ = other.max_load_factor_percent_;
    rehash_threshold_ = other.rehash_threshold_;
    return *this;
  }

  /// Returns the allocator used by this map.
  allocator_type& get_allocator() const { return nodes_.get_allocator(); }

  // Iterators

  iterator begin() { return iterator(nodes_.begin()); }
  const_iterator begin() const { return const_iterator(nodes_.begin()); }
  const_iterator cbegin() const { return begin(); }
  iterator end() { return iterator(nodes_.end()); }
  const_iterator end() const { return const_iterator(nodes_.end()); }
  const_iterator cend() const { return end(); }

  // Capacity

  [[nodiscard]] bool empty() const { return nodes_.empty(); }
  size_type size() const { return nodes_.size(); }
  size_type max_size() const { return nodes_.max_size(); }

  // Modifiers

  /// Removes all elements from the map.
  ///
  /// This destroys all nodes and deallocates them via the allocator.
  void clear() {
    nodes_.clear();
    buckets_.clear();
  }

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

  /// Attempts to construct an element in-place.
  /// @returns A pair containing the iterator and success bool, or
  /// `std::nullopt` on allocation failure.
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

  /// Removes the element at `pos`.
  ///
  /// @note Because the underlying storage is unstable ("swap-and-pop"), erasing
  /// an element invalidates iterators to the element that was swapped into
  /// the erased position (the last element).
  ///
  /// @returns Iterator following the removed element.
  iterator erase(iterator pos) {
    PW_ASSERT(pos != end());
    NodeType* node = &(*pos.it_);
    UnlinkFromBucket(node);
    return EraseFromVector(node->index);
  }

  iterator erase(const_iterator pos) {
    auto it = iterator(nodes_.begin() + pos.it_->index);
    return erase(it);
  }

  /// Removes elements in the range `[first, last)`.
  ///
  /// @note This operation iterates backwards and erases to handle the
  /// invalidation caused by "swap-and-pop" erasure.
  iterator erase(const_iterator first, const_iterator last) {
    if (first == last) {
      return (first == end()) ? end()
                              : iterator(nodes_.begin() + first.it_->index);
    }

    size_type index_first = first.it_->index;
    size_type index_last = (last == end()) ? nodes_.size() : last.it_->index;
    for (size_type i = index_last - 1; i > index_first; --i) {
      auto it = iterator(nodes_.begin() + i);
      erase(it);
    }

    return erase(first);
  }

  /// Removes the element with the matching key. Returns number of elements
  /// removed (0 or 1).
  template <typename K>
  size_type erase(K&& key) {
    NodeType* node = UnlinkFromBucket(std::forward<K>(key));
    if (node == nullptr) {
      return 0;
    }
    EraseFromVector(node->index);
    return 1;
  }

  /// Swaps the contents of two maps. No allocations occur.
  void swap(DynamicHashMap& other) noexcept {
    buckets_.swap(other.buckets_);
    nodes_.swap(other.nodes_);
    std::swap(hash_, other.hash_);
    std::swap(equal_, other.equal_);
    std::swap(max_load_factor_percent_, other.max_load_factor_percent_);
    std::swap(rehash_threshold_, other.rehash_threshold_);
  }

  /// Merges another map into this one.
  ///
  /// Attempts to extract each element in `other` and insert it into `this`.
  /// The element is removed from `other` only if the insertion is successful.
  ///
  /// @note
  /// 1. Requires node reallocation.
  /// 2. May trigger multiple rehashes.
  /// 3. Could be optimized by switching to DynamicPtrVector's take() and
  /// insert().
  void merge(DynamicHashMap& other) {
    if (&other == this) {
      return;
    }

    auto it = other.begin();
    while (it != other.end()) {
      auto result = try_emplace(it->first, std::move(it->second));
      if (result.has_value() && result.value().second) {
        it = other.erase(it);
      } else {
        ++it;
      }
    }
  }

  void merge(DynamicHashMap&& other) { merge(other); }

  // Lookup

  /// Returns a reference to the mapped value of the element with key equivalent
  /// to `key`.
  ///
  /// @pre The key must exist in the map. Crashes if not found.
  mapped_type& at(const key_type& key) {
    auto it = find(key);
    PW_ASSERT(it != end());
    return it->second;
  }

  const mapped_type& at(const key_type& key) const {
    auto it = find(key);
    PW_ASSERT(it != end());
    return it->second;
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

  /// Returns the number of elements with key `key` (0 or 1).
  size_type count(const key_type& key) const {
    return find(key) != end() ? 1 : 0;
  }

  /// Finds an element with key equivalent to `key`.
  /// @returns Iterator to an element with key equivalent to `key`, or `end()`
  /// if no such element is found.
  iterator find(const key_type& key) {
    NodeType* node = FindImpl(key);
    if (node != nullptr) {
      return iterator(nodes_.begin() + node->index);
    }
    return end();
  }

  const_iterator find(const key_type& key) const {
    NodeType* node = FindImpl(key);
    if (node != nullptr) {
      return const_iterator(nodes_.begin() + node->index);
    }
    return cend();
  }

  /// Checks if there is an element with key equivalent to `key` in the
  /// container.
  bool contains(const key_type& key) const { return find(key) != end(); }

  /// Returns a range containing all elements with the given key in the
  /// container. Since this is a unique map, the range will contain at most one
  /// element.
  std::pair<iterator, iterator> equal_range(const key_type& key) {
    auto it = find(key);
    if (it == end()) {
      return std::make_pair(it, it);
    }
    return std::make_pair(it, std::next(it));
  }

  std::pair<const_iterator, const_iterator> equal_range(
      const key_type& key) const {
    auto it = find(key);
    if (it == end()) {
      return std::make_pair(it, it);
    }
    return std::make_pair(it, std::next(it));
  }

  // Hash policy

  /// Returns the current load factor (ratio of elements to buckets) as a
  /// percentage.
  size_type load_factor_percent() const {
    if (buckets_.empty()) {
      return 0;
    }
    return (nodes_.size() * 100) / buckets_.size();
  }

  /// Returns the maximum load factor (in percent) that the map attempts to
  /// maintain.
  size_type max_load_factor_percent() const { return max_load_factor_percent_; }

  /// Sets the maximum load factor (in percent).
  ///
  /// This operation does not trigger a rehash, but will affect the threshold
  /// for future automatic rehashes.
  void max_load_factor_percent(size_type percent) {
    PW_ASSERT(percent > 0 && percent <= kMaxLoadFactorPercentLimit);
    max_load_factor_percent_ = percent;
    UpdateRehashThreshold();
  }

  /// Attempts to set the number of buckets to `count` and rehash the
  /// container.
  ///
  /// @returns `true` if successful, `false` if allocation fails.
  [[nodiscard]] bool try_rehash(size_type count) {
    if (count < buckets_.size()) {
      return true;
    }

    pw::DynamicDeque<NodeType*> new_buckets(get_allocator());
    if (!new_buckets.try_assign(count, nullptr)) {
      return false;
    }

    for (auto& node : nodes_) {
      size_type bucket_index = hash_(node.key_value.first) % new_buckets.size();
      node.next = new_buckets[bucket_index];
      new_buckets[bucket_index] = &node;
    }

    buckets_ = std::move(new_buckets);

    UpdateRehashThreshold();
    return true;
  }

  /// Sets the number of buckets to `count` and rehashes the container.
  /// Crashes on allocation failure.
  void rehash(size_type count) { PW_ASSERT(try_rehash(count)); }

  /// Attempts to prepare the map to hold at least `count` elements without
  /// requiring further rehashing or node vector reallocation.
  ///
  /// This may rehash the map to increase bucket count and reserve space in
  /// the underlying node vector.
  ///
  /// @note This does NOT guarantee that future operations that add items will
  /// succeed, since they require separate allocations for the objects
  /// (nodes) themselves.
  ///
  /// @returns `true` if successful; `false` if allocation failed.
  [[nodiscard]] bool try_reserve(size_type count) {
    const CalcType required_buckets_calc =
        (static_cast<CalcType>(count) * 100 + max_load_factor_percent() - 1) /
        max_load_factor_percent();
    if (required_buckets_calc > std::numeric_limits<size_type>::max()) {
      return false;
    }

    size_type required_buckets = static_cast<size_type>(required_buckets_calc);
    if (!try_rehash(required_buckets)) {
      return false;
    }
    if (!nodes_.try_reserve_ptr(count)) {
      return false;
    }

    return true;
  }

  /// Reserves enough space for `count` elements. Crashes on allocation
  /// failure.
  void reserve(size_type count) { PW_ASSERT(try_reserve(count)); }

 private:
  size_type GetBucketIndex(const key_type& key) const {
    PW_ASSERT(!buckets_.empty());
    return hash_(key) % buckets_.size();
  }

  size_type CalculateNewBucketCount(size_type current_count) const {
    size_type max_bucket_count = buckets_.max_size();
    if (current_count > max_bucket_count / 2) {
      return max_bucket_count;
    }
    return current_count * 2;
  }

  void UpdateRehashThreshold() {
    if (buckets_.empty()) {
      rehash_threshold_ = 0;
    } else {
      rehash_threshold_ = static_cast<size_type>(
          (static_cast<CalcType>(buckets_.size()) * max_load_factor_percent()) /
          100);
    }
  }

  NodeType* FindImpl(const key_type& key) const {
    if (empty()) {
      return nullptr;
    }
    size_type bucket_index = GetBucketIndex(key);
    NodeType* node = buckets_[bucket_index];
    while (node != nullptr) {
      if (equal_(node->key_value.first, key)) {
        return node;
      }
      node = node->next;
    }
    return nullptr;
  }

  template <typename K, typename... Args>
  [[nodiscard]] std::optional<std::pair<iterator, bool>> TryEmplaceImpl(
      K&& key, Args&&... args) {
    if (auto it = find(key); it != end()) {
      return std::make_pair(iterator(it), false);
    }

    if (size() >= max_size()) {
      return std::nullopt;
    }

    if (buckets_.empty()) {
      if (!buckets_.try_assign(kDefaultInitialBuckets, nullptr)) {
        return std::nullopt;
      }
      UpdateRehashThreshold();
    }

    if (!nodes_.try_emplace_back(std::forward<K>(key),
                                 std::forward<Args>(args)...)) {
      return std::nullopt;
    }

    NodeType& new_node = nodes_.back();
    size_type bucket_index = GetBucketIndex(new_node.key_value.first);
    new_node.next = buckets_[bucket_index];
    new_node.index = nodes_.size() - 1;
    buckets_[bucket_index] = &new_node;

    if (size() > rehash_threshold_) {
      size_type new_bucket_count = CalculateNewBucketCount(buckets_.size());
      (void)try_rehash(new_bucket_count);
    }

    return std::make_pair(iterator(std::prev(nodes_.end())), true);
  }

  iterator EraseFromVector(size_type index_to_remove) {
    size_type last_index = nodes_.size() - 1;

    if (index_to_remove != last_index) {
      nodes_.swap(index_to_remove, last_index);
      nodes_[index_to_remove].index = index_to_remove;
    }

    nodes_.pop_back();
    return iterator(nodes_.begin() + index_to_remove);
  }

  void UnlinkFromBucket(NodeType* node) {
    size_type bucket_index = GetBucketIndex(node->key_value.first);
    FindAndUnlinkFromBucket(bucket_index,
                            [node](NodeType* curr) { return curr == node; });
  }

  template <typename K>
  NodeType* UnlinkFromBucket(K&& key) {
    size_type bucket_index = GetBucketIndex(key);
    return FindAndUnlinkFromBucket(bucket_index, [this, &key](NodeType* curr) {
      return equal_(curr->key_value.first, key);
    });
  }

  template <typename Predicate>
  NodeType* FindAndUnlinkFromBucket(size_type bucket_index, Predicate pred) {
    NodeType* curr = buckets_[bucket_index];
    NodeType* prev = nullptr;

    while (curr != nullptr) {
      if (pred(curr)) {
        if (prev == nullptr) {
          buckets_[bucket_index] = curr->next;
        } else {
          prev->next = curr->next;
        }
        return curr;
      }
      prev = curr;
      curr = curr->next;
    }

    return nullptr;
  }

  pw::DynamicDeque<NodeType*, size_type> buckets_;
  NodeVectorType nodes_;
  PW_NO_UNIQUE_ADDRESS Hash hash_;
  PW_NO_UNIQUE_ADDRESS Equal equal_;
  size_type max_load_factor_percent_ = 75;
  size_type rehash_threshold_ = 0;
  static constexpr size_type kDefaultInitialBuckets = 11;

  /// The absolute maximum load factor percentage allowed (500%).
  ///
  /// This high limit provides flexibility for memory-constrained embedded
  /// environments. While a standard 75% load factor favors lookup speed,
  /// allowing up to 500% lets developers significantly shrink the bucket
  /// array's memory footprint when CPU cycles are less scarce than RAM.
  static constexpr size_type kMaxLoadFactorPercentLimit = 500;
};

}  // namespace pw
