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

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_assert/assert.h"
#include "pw_containers/internal/generic_deque.h"
#include "pw_numeric/saturating_arithmetic.h"

namespace pw {

/// @module{pw_containers}

/// @addtogroup pw_containers_queues
/// @{

/// Double-ended queue, similar to `std::deque`, but optimized for embedded.
///
/// Key features of `pw::DynamicDeque`.
///
/// - Uses a `pw::Allocator` for memory operations.
/// - Provides the `std::deque` API, but adds `try_*` versions of
///   operations that crash on allocation failure.
///   - `assign()` & `try_assign()`.
///   - `push_front()` & `try_push_front()`, `push_back()` & `try_push_back()`
///   - `emplace_front()` & `try_emplace_front()`, `emplace_back()` &
///   `try_emplace_back()`
///   - `resize()` & `try_resize()`.
/// - Offers `reserve()`/`try_reserve()`,
///   `reserve_exact()`/`try_reserve_exact()``, and `shrink_to_fit()` to manage
///   memory usage.
/// - Never allocates in the constructor. `constexpr` constructible.
/// - Compact representation when used with a `size_type` of `uint8_t` or
///   `uint16_t`.
/// - Uses `pw::Allocator::Resize()` when possible to maximize efficiency.
template <typename ValueType, typename SizeType = uint16_t>
class DynamicDeque
    : public containers::internal::
          GenericDeque<DynamicDeque<ValueType, SizeType>, ValueType, SizeType> {
 private:
  using Base = containers::internal::
      GenericDeque<DynamicDeque<ValueType, SizeType>, ValueType, SizeType>;

 public:
  using typename Base::const_iterator;
  using typename Base::const_pointer;
  using typename Base::const_reference;
  using typename Base::difference_type;
  using typename Base::iterator;
  using typename Base::pointer;
  using typename Base::reference;
  using typename Base::size_type;
  using typename Base::value_type;

  using allocator_type = Allocator;

  /// Constructs an empty `DynamicDeque`. No memory is allocated.
  ///
  /// Since allocations can fail, initialization in the constructor is not
  /// supported.
  constexpr DynamicDeque(Allocator& allocator) noexcept
      : Base(0), allocator_(&allocator), buffer_(nullptr) {}

  DynamicDeque(const DynamicDeque&) = delete;
  DynamicDeque& operator=(const DynamicDeque&) = delete;

  /// Move construction/assignment is supported since it cannot fail. Copy
  /// construction/assignment is not supported. Can use
  /// `assign()`/`try_assign()` instead.
  constexpr DynamicDeque(DynamicDeque&& other) noexcept
      : Base(0), allocator_(other.allocator_), buffer_(other.buffer_) {
    other.buffer_ = nullptr;  // clear other's buffer_, but not its allocator_
    Base::MoveAssignIndices(other);
  }

  DynamicDeque& operator=(DynamicDeque&& other) noexcept;

  ~DynamicDeque();

  // Provide try_* versions of functions that return false if allocation fails.
  using Base::try_assign;
  using Base::try_emplace;
  using Base::try_emplace_back;
  using Base::try_emplace_front;
  using Base::try_insert;
  using Base::try_push_back;
  using Base::try_push_front;
  using Base::try_resize;

  // The GenericDeque's input iterator insert implementation emplaces items one
  // at a time, which is inefficient. For DynamicDeque, use a more efficient
  // implementation that inserts all items into a temporary DynamicDeque first.
  template <typename InputIt,
            typename = containers::internal::EnableIfInputIterator<InputIt>>
  iterator insert(const_iterator pos, InputIt first, InputIt last);

  iterator insert(const_iterator pos, const value_type& value) {
    return Base::insert(pos, value);
  }

  iterator insert(const_iterator pos, value_type&& value) {
    return Base::insert(pos, std::move(value));
  }

  iterator insert(const_iterator pos,
                  size_type count,
                  const value_type& value) {
    return Base::insert(pos, count, value);
  }

  iterator insert(const_iterator pos, std::initializer_list<value_type> ilist) {
    return Base::insert(pos, ilist);
  }

  /// Attempts to increase `capacity()` to at least `new_capacity`, allocating
  /// memory if needed. Does nothing if `new_capacity` is less than or equal to
  /// `capacity()`. Iterators are invalidated if allocation occurs.
  ///
  /// `try_reserve` may increase the capacity to be larger than `new_capacity`,
  /// with the same behavior as if the size were increased to `new_capacity`.
  /// To increase the capacity to a precise value, use `try_reserve_exact()`.
  ///
  /// @returns true if allocation succeeded or `capacity()` was already large
  /// enough; false if allocation failed
  [[nodiscard]] bool try_reserve(size_type new_capacity);

  /// Increases `capacity()` to at least `new_capacity`. Crashes on failure.
  void reserve(size_type new_capacity) { PW_ASSERT(try_reserve(new_capacity)); }

  /// Attempts to increase `capacity()` to `new_capacity`, allocating memory if
  /// needed. Does nothing if `new_capacity` is less than or equal to
  /// `capacity()`.
  ///
  /// This differs from `try_reserve()`, which may reserve space for more than
  /// `new_capacity`.
  ///
  /// @returns true if allocation succeeded or `capacity()` was already large
  /// enough; false if allocation failed
  [[nodiscard]] bool try_reserve_exact(size_type new_capacity) {
    return new_capacity <= Base::capacity() || IncreaseCapacity(new_capacity);
  }

  /// Increases `capacity()` to exactly `new_capacity`. Crashes on failure.
  void reserve_exact(size_type new_capacity) {
    PW_ASSERT(try_reserve_exact(new_capacity));
  }

  /// Attempts to reduce `capacity()` to `size()`. Not guaranteed to succeed.
  void shrink_to_fit();

  constexpr size_type max_size() const noexcept {
    return std::numeric_limits<size_type>::max();
  }

  /// Returns the deque's allocator.
  constexpr allocator_type& get_allocator() const { return *allocator_; }

  /// Swaps the contents of two deques. No allocations occur.
  void swap(DynamicDeque& other) noexcept {
    Base::SwapIndices(other);
    std::swap(allocator_, other.allocator_);
    std::swap(buffer_, other.buffer_);
  }

 private:
  friend Base;

  template <typename, typename>
  friend class DynamicVector;  // Allow direct access to data()

  static constexpr bool kFixedCapacity = false;  // uses dynamic allocation

  // Hide full() since the capacity can grow.
  using Base::full;

  pointer data() { return std::launder(reinterpret_cast<pointer>(buffer_)); }
  const_pointer data() const {
    return std::launder(reinterpret_cast<const_pointer>(buffer_));
  }

  [[nodiscard]] bool IncreaseCapacity(size_type new_capacity);

  size_type GetNewCapacity(const size_type new_size) {
    // For the initial allocation, allocate at least 4 words worth of items.
    if (Base::capacity() == 0) {
      return std::max(size_type{4 * sizeof(void*) / sizeof(value_type)},
                      new_size);
    }
    // Double the capacity. May introduce other allocation policies later.
    return std::max(mul_sat(Base::capacity(), size_type{2}), new_size);
  }

  bool ReallocateBuffer(size_type new_capacity);

  Allocator* allocator_;
  std::byte* buffer_;  // raw array for in-place construction and destruction
};

template <typename ValueType, typename SizeType>
DynamicDeque<ValueType, SizeType>& DynamicDeque<ValueType, SizeType>::operator=(
    DynamicDeque&& other) noexcept {
  Base::DestroyAll();
  allocator_->Deallocate(buffer_);

  allocator_ = other.allocator_;  // The other deque keeps its allocator
  buffer_ = std::exchange(other.buffer_, nullptr);

  Base::MoveAssignIndices(other);
  return *this;
}

template <typename ValueType, typename SizeType>
DynamicDeque<ValueType, SizeType>::~DynamicDeque() {
  Base::DestroyAll();
  allocator_->Deallocate(buffer_);
}

template <typename ValueType, typename SizeType>
bool DynamicDeque<ValueType, SizeType>::try_reserve(size_type new_capacity) {
  return new_capacity <= Base::capacity() ||
         IncreaseCapacity(GetNewCapacity(new_capacity)) ||
         IncreaseCapacity(new_capacity);
}

template <typename ValueType, typename SizeType>
bool DynamicDeque<ValueType, SizeType>::IncreaseCapacity(
    size_type new_capacity) {
  // Try resizing the existing array. Only works if inserting at the end.
  if (buffer_ != nullptr && Base::CanExtendBuffer() &&
      allocator_->Resize(buffer_, new_capacity * sizeof(value_type))) {
    Base::HandleExtendedBuffer(new_capacity);
    return true;
  }

  // Allocate a new array and move items to it.
  return ReallocateBuffer(new_capacity);
}

template <typename ValueType, typename SizeType>
void DynamicDeque<ValueType, SizeType>::shrink_to_fit() {
  if (Base::size() == Base::capacity()) {
    return;  // Nothing to do; deque is full or buffer_ is nullptr
  }

  if (Base::empty()) {  // Empty deque, but a buffer_ is allocated; free it
    allocator_->Deallocate(buffer_);
    buffer_ = nullptr;
    Base::HandleShrunkBuffer(0);
    return;
  }

  // Attempt to shrink if buffer if possible, and reallocate it if needed.
  //
  // If there are unused slots at the start, could shift back and Resize()
  // instead of calling ReallocateBuffer(), but may not be worth the complexity.
  if (Base::CanShrinkBuffer() &&
      allocator_->Resize(buffer_, Base::size() * sizeof(value_type))) {
    Base::HandleShrunkBuffer(Base::size());
  } else {
    ReallocateBuffer(Base::size());
  }
}

template <typename ValueType, typename SizeType>
bool DynamicDeque<ValueType, SizeType>::ReallocateBuffer(
    size_type new_capacity) {
  std::byte* new_buffer = static_cast<std::byte*>(
      allocator_->Allocate(allocator::Layout::Of<value_type[]>(new_capacity)));
  if (new_buffer == nullptr) {
    return false;
  }

  pointer dest = std::launder(reinterpret_cast<pointer>(new_buffer));
  auto [data_1, data_2] = Base::contiguous_data();

  if constexpr (std::is_move_constructible_v<value_type>) {
    dest = std::uninitialized_move(data_1.begin(), data_1.end(), dest);
    std::uninitialized_move(data_2.begin(), data_2.end(), dest);
  } else {  // if it can't be moved, try copying
    dest = std::uninitialized_copy(data_1.begin(), data_1.end(), dest);
    std::uninitialized_copy(data_2.begin(), data_2.end(), dest);
  }

  std::destroy(data_1.begin(), data_1.end());
  std::destroy(data_2.begin(), data_2.end());

  allocator_->Deallocate(buffer_);
  buffer_ = new_buffer;

  Base::HandleNewBuffer(new_capacity);
  return true;
}

template <typename ValueType, typename SizeType>
template <typename InputIt, typename>
typename DynamicDeque<ValueType, SizeType>::iterator
DynamicDeque<ValueType, SizeType>::insert(const_iterator pos,
                                          InputIt first,
                                          InputIt last) {
  // Can't safely check std::distance for InputIterator. Use a workaround.
  if constexpr (std::is_same_v<std::input_iterator_tag,
                               typename std::iterator_traits<
                                   InputIt>::iterator_category>) {
    // Read into a temporary deque so the items can be counted. Then, move into
    // this deque in one operation. This way, existing items are shifted once to
    // their final positions, instead of shifting N times for repeated inserts.
    DynamicDeque temp(*allocator_);
    temp.assign(first, last);
    return Base::insert(pos,
                        std::make_move_iterator(temp.data()),
                        std::make_move_iterator(temp.data() + temp.size()));
  } else {  // Use the efficient base implementation for forward iterators.
    return Base::insert(pos, first, last);
  }
}

}  // namespace pw
