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
#include "pw_allocator/unique_ptr.h"
#include "pw_containers/deque.h"
#include "pw_containers/internal/generic_queue.h"
#include "pw_containers/storage.h"

namespace pw {

/// @submodule{pw_containers,queues}

/// A queue implementation backed by `pw::Deque`.
///
/// This class provides a `std::queue`-like interface and stores items in a
/// caller-provided, fixed-size buffer.
///
/// @tparam T The type of elements stored in the queue.
/// @tparam SizeType How to store sizes. A smaller size type limits the maximum
///     number of items, but requires slightly less memory.
template <typename T, typename SizeType = uint16_t>
class Queue : public containers::internal::GenericQueue<Queue<T, SizeType>,
                                                        Deque<T, SizeType>> {
 public:
  /// Constructs an empty `Queue` using the provided buffer for storage.
  ///
  /// @param unaligned_buffer Storage for the queue. If the buffer is not
  ///     aligned as as `value_type`, an aligned subset of the buffer will be
  ///     used. If the buffer is too small to hold at least one `value_type`,
  ///     the queue's capacity will be 0.
  explicit constexpr Queue(span<std::byte> unaligned_buffer) noexcept
      : Queue(Aligned::Align(unaligned_buffer)) {}

  /// Constructs an empty `Queue` using a `pw::containers::Storage` buffer.
  /// This constructor avoids run time alignment checks.
  template <size_t kAlignment, size_t kSizeBytes>
  explicit constexpr Queue(
      containers::Storage<kAlignment, kSizeBytes>& storage) noexcept
      : deque_(storage) {}

  // Disable copy and move since the queue does not own the buffer.
  Queue(const Queue&) = delete;
  Queue& operator=(const Queue&) = delete;

  Queue(Queue&&) = delete;
  Queue& operator=(Queue&&) = delete;

 private:
  template <typename, typename>
  friend class containers::internal::GenericQueue;

  template <typename, size_t, typename>
  friend class FixedQueue;

  using Aligned = typename Deque<T, SizeType>::Aligned;

  // Constructor for a buffer that is known to be aligned.
  explicit constexpr Queue(Aligned buffer) noexcept : deque_(buffer) {}

  void MoveItemsFrom(Queue& other) { deque_.MoveItemsFrom(other.deque_); }
  void MoveBufferFrom(Queue& other) { deque_.MoveBufferFrom(other.deque_); }
  void SwapValuesWith(Queue& other) { deque_.SwapValuesWith(other.deque_); }
  void SwapBufferWith(Queue& other) { deque_.SwapBufferWith(other.deque_); }

  T* data() { return deque_.data(); }

  Deque<T, SizeType>& deque() { return deque_; }
  const Deque<T, SizeType>& deque() const { return deque_; }

  Deque<T, SizeType> deque_;
};

/// Queue implementation equivalent to `pw::FixedDeque`.
///
/// @tparam T The type of elements stored in the queue.
/// @tparam kCapacity The maximum number of elements the queue can hold. If set
///     to `containers::kExternalStorage`, the queue storage will be outside the
///     object.
/// @tparam SizeType The type used to store the size of the queue.
template <typename T,
          size_t kInlineCapacity = containers::kExternalStorage,
          typename SizeType = typename Queue<T>::size_type>
class FixedQueue final : private containers::StorageBaseFor<T, kInlineCapacity>,
                         public Queue<T, SizeType> {
 public:
  /// Constructs an empty `FixedQueue` with internal, statically allocated
  /// storage.
  constexpr FixedQueue()
      : containers::StorageBaseFor<T, kInlineCapacity>{},
        Queue<T, SizeType>(this->storage()) {}

  FixedQueue(const FixedQueue&) = delete;
  FixedQueue& operator=(const FixedQueue&) = delete;

  constexpr FixedQueue(FixedQueue&& other) noexcept : FixedQueue() {
    this->MoveItemsFrom(other);
  }

  constexpr FixedQueue& operator=(FixedQueue&& other) noexcept {
    this->clear();
    this->MoveItemsFrom(other);
    return *this;
  }

  /// `FixedQueue` supports moves that are guaranteed to succeed (moving into
  /// equal or greater capacity or between queues with external storage).
  template <size_t kOtherCapacity,
            typename = std::enable_if_t<kOtherCapacity <= kInlineCapacity>>
  FixedQueue(FixedQueue<T, kOtherCapacity>&& other) noexcept : FixedQueue() {
    this->MoveItemsFrom(other);
  }

  template <size_t kOtherCapacity,
            typename = std::enable_if_t<kOtherCapacity <= kInlineCapacity>>
  FixedQueue& operator=(FixedQueue<T, kOtherCapacity>&& other) noexcept {
    this->clear();
    this->MoveItemsFrom(other);
    return *this;
  }

  /// Swaps the contents of this `FixedQueue` with another, in O(n) time.
  /// Crashes if one queue's size is greater than the other's capacity.
  template <size_t kOtherCapacity>
  void swap(FixedQueue<T, kOtherCapacity, SizeType>& other) {
    this->SwapValuesWith(other);
  }

  /// Returns `nullptr`; a `FixedQueue` with static storage never allocates.
  constexpr Deallocator* deallocator() const { return nullptr; }
};

/// Specialization of `FixedQueue` using an external static or dynamically
/// allocated, but fixed, storage buffer. Equivalent to
/// @ref FixedDeque.
template <typename T, typename S>
class FixedQueue<T, containers::kExternalStorage, S> final
    : public Queue<T, S> {
 public:
  /// Allocates a buffer large enough to hold `capacity` items and uses it for a
  /// `FixedQueue`'s buffer. Crashes if unable to allocate space for `capacity`
  /// items.
  ///
  /// The `FixedQueue` owns the buffer and frees it upon destruction.
  static FixedQueue Allocate(Allocator& allocator, const S capacity) {
    FixedQueue queue = TryAllocate(allocator, capacity);
    PW_ASSERT(queue.capacity() == capacity);
    return queue;
  }

  /// Attempts to allocate a `T`-aligned buffer large enough to hold `capacity`
  /// items and use it for a `FixedQueue`'s buffer. If unable to allocate space
  /// for `capacity` items, returns a `FixedQueue` with a capacity of 0.
  ///
  /// If allocation succeeds, the `FixedQueue` owns the buffer and frees it upon
  /// destruction.
  static FixedQueue TryAllocate(Allocator& allocator, S capacity) {
    const allocator::Layout layout = allocator::Layout::Of<T[]>(capacity);
    std::byte* array = static_cast<std::byte*>(allocator.Allocate(layout));
    if (array == nullptr) {
      return FixedQueue(Aligned(array, 0u), nullptr);
    }
    return FixedQueue(Aligned(array, layout.size()), &allocator);
  }

  /// Constructs an empty `FixedQueue` that uses the provided `pw::UniquePtr`
  /// for its storage. Takes ownership of the `pw::UniquePtr`.
  ///
  /// @pre `storage` MUST be aligned at least as `value_type`.
  static FixedQueue WithStorage(UniquePtr<std::byte[]>&& storage) {
    const size_t size = storage.size();
    Deallocator* deallocator = storage.deallocator();
    return FixedQueue(Aligned::Assert(storage.Release(), size), deallocator);
  }

  /// Constructs an `FixedQueue` from a buffer. The buffer is unowned and is not
  /// freed when the `FixedQueue` is destroyed.
  ///
  /// @param unaligned_buffer Storage for the queue. If the buffer is not
  ///     aligned as as `value_type`, an aligned subset of the buffer will be
  ///     used. If the buffer is too small to hold at least one `value_type`,
  ///     the queue's capacity will be 0.
  explicit constexpr FixedQueue(span<std::byte> unaligned_buffer) noexcept
      : Queue<T, S>(unaligned_buffer), deallocator_(nullptr) {}

  /// Constructs an empty `FixedQueue` using a `pw::containers::Storage` buffer.
  /// This constructor avoids run time alignment checks.
  ///
  /// The buffer is unowned and is not freed when the `FixedQueue` is destroyed.
  template <size_t kAlignment, size_t kSizeBytes>
  explicit constexpr FixedQueue(
      containers::Storage<kAlignment, kSizeBytes>& buffer) noexcept
      : Queue<T, S>(buffer), deallocator_(nullptr) {}

  /// Copying is not supported since it can fail.
  FixedQueue(const FixedQueue&) = delete;
  FixedQueue& operator=(const FixedQueue&) = delete;

  /// Move construction/assignment from other `FixedQueue`s with external
  /// buffers is supported and infallible. To move items from a statically
  /// allocated `FixedQueue`, use `assign()` with `std::make_move_iterator`, or
  /// move items individually.
  constexpr FixedQueue(FixedQueue&& other) noexcept
      : Queue<T, S>({}),
        deallocator_(cpp20::exchange(other.deallocator_, nullptr)) {
    this->MoveBufferFrom(other);
  }

  constexpr FixedQueue& operator=(FixedQueue&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    T* const data = this->data();
    this->MoveBufferFrom(other);

    if (deallocator_ != nullptr) {
      deallocator_->Deallocate(data);
    }
    deallocator_ = cpp20::exchange(other.deallocator_, nullptr);
    return *this;
  }

  ~FixedQueue() {
    // Clear the queue so that the base `Queue` destructor will not need to
    // access its buffer, which will be freed.
    this->clear();
    if (deallocator_ != nullptr) {
      deallocator_->Deallocate(this->data());
    }
  }

  /// Swaps this `FixedQueue` with another externally allocated `FixedQueue` in
  /// O(1) time. No allocations occur, and individual items are not moved.
  void swap(FixedQueue& other) noexcept {
    this->SwapBufferWith(other);
    std::swap(deallocator_, other.deallocator_);
  }

  /// Swaps the contents of this `FixedQueue` with an internal-storage
  /// `FixedQueue` in O(n) time. No allocations occur, but items are moved
  /// individually. Crashes if the size of one queue exceeds the capacity of the
  /// other.
  template <size_t kInlineCapacity>
  void swap(FixedQueue<T, kInlineCapacity, S>& other) {
    other.swap(*this);  // Use the swap impl for statically allocated queues.
  }

  /// Returns the deallocator that will be used to free this `FixedQueue`'s
  /// storage. Returns `nullptr` if the storage is unowned.
  constexpr Deallocator* deallocator() const { return deallocator_; }

 private:
  using Aligned = typename Queue<T, S>::Aligned;

  constexpr FixedQueue(Aligned buffer, Deallocator* deallocator)
      : Queue<T, S>(buffer), deallocator_(deallocator) {}

  Deallocator* deallocator_;
};

}  // namespace pw
