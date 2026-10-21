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

#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_containers/dynamic_deque.h"
#include "pw_containers/internal/generic_queue.h"  // IWYU pragma: export

namespace pw {

/// @module{pw_containers}

/// @defgroup pw_containers_queues Queues
/// @{

/// A queue implementation backed by `pw::DynamicDeque`.
///
/// This class provides a `std::queue`-like interface but uses a Pigweed
/// allocator for dynamic memory management. It includes fallible `try_*`
/// operations for scenarios where allocation failure may be handled gracefully.
///
/// @warning The container's allocator MUST outlive the container, unless one of
/// the following steps is taken:
/// - Call `reset()` to free all memory, including the underlying buffer, and
///   make no further modifications to the container.
/// - Assign from a container with a different allocator (e.g. `NullAllocator`)
///   before the first allocator goes out of scope.
///
/// @warning Calling `clear()` is NOT sufficient; an empty container may still
/// hold a pointer to an allocated buffer.
///
/// @tparam T The type of elements stored in the queue.
template <typename T, typename SizeType = uint16_t>
class DynamicQueue
    : public containers::internal::GenericQueue<DynamicQueue<T, SizeType>,
                                                DynamicDeque<T, SizeType>> {
 private:
  using Deque = DynamicDeque<T, SizeType>;
  using Base = containers::internal::GenericQueue<DynamicQueue, Deque>;

 public:
  using const_iterator = typename Deque::const_iterator;
  using const_pointer = typename Deque::const_pointer;
  using const_reference = typename Deque::const_reference;
  using difference_type = typename Deque::difference_type;
  using iterator = typename Deque::iterator;
  using pointer = typename Deque::pointer;
  using reference = typename Deque::reference;
  using size_type = typename Deque::size_type;
  using value_type = typename Deque::value_type;

  /// Constructs a `DynamicQueue` using the provided allocator.
  explicit constexpr DynamicQueue(pw::Allocator& allocator)
      : deque_(allocator) {}

  // Disable copy since it can fail.
  DynamicQueue(const DynamicQueue&) = delete;
  DynamicQueue& operator=(const DynamicQueue&) = delete;

  /// Move operations are supported and incur no allocations.
  constexpr DynamicQueue(DynamicQueue&&) = default;
  DynamicQueue& operator=(DynamicQueue&&) = default;

  /// Attempts to add an element to the back of the queue.
  [[nodiscard]] bool try_push(const value_type& value) {
    return deque_.try_push_back(value);
  }

  /// Attempts to add an element to the back of the queue (move version).
  [[nodiscard]] bool try_push(value_type&& value) {
    return deque_.try_push_back(std::move(value));
  }

  /// Attempts to construct an element in place at the back of the queue.
  template <typename... Args>
  [[nodiscard]] bool try_emplace(Args&&... args) {
    return deque_.try_emplace_back(std::forward<Args>(args)...);
  }

  /// Sets the queue capacity to at least `max(capacity, size())` elements.
  void reserve(size_type capacity) { deque_.reserve(capacity); }

  /// Attempts to set the queue capacity to at least `max(capacity, size())`
  /// elements.
  [[nodiscard]] bool try_reserve(size_type capacity) {
    return deque_.try_reserve(capacity);
  }

  /// Sets the queue capacity to `max(capacity, size())` elements.
  void reserve_exact(size_type capacity) { deque_.reserve_exact(capacity); }

  /// Attempts to set the queue capacity to `max(capacity, size())` elements.
  [[nodiscard]] bool try_reserve_exact(size_type capacity) {
    return deque_.try_reserve_exact(capacity);
  }

  /// Reduces memory usage by releasing unused capacity, if possible.
  void shrink_to_fit() { deque_.shrink_to_fit(); }

  /// @copydoc DynamicDeque::reset
  void reset() { deque_.reset(); }

  /// Swaps the contents with another queue.
  void swap(DynamicQueue& other) { deque_.swap(other.deque_); }

 private:
  friend Base;

  // Hide full() since the capacity can grow.
  using Base::full;

  // Hide overwrite methods since capacity can grow.
  using Base::emplace_overwrite;
  using Base::push_overwrite;

  Deque& deque() { return deque_; }
  const Deque& deque() const { return deque_; }

  Deque deque_;
};

}  // namespace pw
