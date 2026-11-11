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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_allocator/unique_ptr.h"
#include "pw_assert/assert.h"
#include "pw_containers/storage.h"
#include "pw_span/span.h"

namespace pw {

template <typename T, size_t kInlineCapacity, typename SizeType>
class FixedLockFreeSpscQueue;

/// @submodule{pw_containers,queues}

/// A wait-free single-producer, single-consumer (SPSC) FIFO queue backed by a
/// caller-provided, fixed-size storage buffer.
///
/// `LockFreeSpscQueue` provides a `pw::Queue`-like interface and synchronizes
/// between a single producer context and a single consumer context using C++
/// atomics with acquire/release memory ordering rather than locks. This makes
/// it suitable for passing typed elements between two threads or between a
/// thread and an Interrupt Service Routine (ISR).
///
/// Like `pw::Queue` and `pw::Deque`, `LockFreeSpscQueue` does not own its
/// underlying storage buffer and is neither copyable nor movable. To
/// instantiate a `LockFreeSpscQueue` with an inline statically allocated buffer
/// or an owned dynamically allocated buffer, use `pw::FixedLockFreeSpscQueue`.
///
/// Producer operations (`push()`, `try_push()`, `emplace()`, `try_emplace()`)
/// may be called concurrently with consumer operations (`front()`,
/// `try_peek()`, `pop()`, `try_pop()`), provided there is at most one active
/// producer context and at most one active consumer context at any time. Size
/// observers (`empty()`, `full()`, `size()`, `remaining_capacity()`) may be
/// called concurrently from either the producer or the consumer context (not
/// from a third independent thread). `clear()` and destruction require
/// exclusive access with no concurrent producer or consumer operations.
///
/// @note On Armv6-M targets (Cortex-M0 and Cortex-M0+), aligned 8-, 16-, and
/// 32-bit loads and stores are single-copy atomic in hardware. Under GCC
/// (`arm-none-eabi-g++`), `std::atomic` loads and stores are lowered directly
/// to inline hardware load/store instructions and `dmb` barriers. Under Clang
/// (`--target=armv6m-none-eabi`), `std::atomic` loads and stores are lowered to
/// `__atomic_load_*` and `__atomic_store_*` library calls (`pw_atomic` masks
/// interrupts via `PRIMASK`).
///
/// @tparam T The type of elements stored in the queue.
/// @tparam SizeType Unsigned integer type used to store sizes and indices.
///     Defaults to `size_t` rather than a smaller integer type to ensure that
///     atomic operations on indices are lock-free across all supported target
///     architectures.
///
/// @threadsafe{maybe}
/// @isrsafe{maybe}
/// @nmisafe{maybe}
template <typename T, typename SizeType = size_t>
class LockFreeSpscQueue {
 public:
  /// The type of elements stored in the queue.
  using value_type = T;

  /// Unsigned integer type used for indexing and sizes.
  using size_type = SizeType;

  /// Signed integer type for pointer differences.
  using difference_type = ptrdiff_t;

  /// Mutable reference to an element.
  using reference = value_type&;

  /// Const reference to an element.
  using const_reference = const value_type&;

  /// Pointer to an element.
  using pointer = value_type*;

  /// Const pointer to an element.
  using const_pointer = const value_type*;

  static_assert(!std::is_const_v<T>, "const elements are not supported");
  static_assert(std::is_unsigned_v<SizeType>, "SizeType must be unsigned.");
#if defined(__ARM_ARCH_6M__) && __ARM_ARCH_6M__
  static_assert(sizeof(size_type) <= sizeof(uint32_t),
                "SizeType must be at most 32 bits for atomic loads and stores "
                "on Armv6-M.");
#else
  static_assert(std::atomic<size_type>::is_always_lock_free,
                "SizeType must be always lock-free on the target platform.");
#endif  // defined(__ARM_ARCH_6M__) && __ARM_ARCH_6M__

  /// Maximum number of elements supported by `SizeType`. Virtual head and tail
  /// indices range over `[0, 2 * capacity)` to distinguish empty and full
  /// states without wasting a storage slot.
  static constexpr size_type kMaxCapacity =
      std::numeric_limits<size_type>::max() / 2;

  /// Constructs an empty `LockFreeSpscQueue` using the provided buffer for
  /// storage.
  ///
  /// @pre The aligned capacity (`unaligned_buffer.size() / sizeof(value_type)`)
  /// must not exceed `kMaxCapacity`.
  ///
  /// @param[in] unaligned_buffer Storage for the queue. If the buffer is not
  ///     aligned as `value_type`, an aligned subset of the buffer will be used.
  ///     If the buffer is too small to hold at least one `value_type`, the
  ///     queue's capacity will be 0.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{yes}
  /// @nmisafe{maybe}
  explicit constexpr LockFreeSpscQueue(
      span<std::byte> unaligned_buffer) noexcept
      : LockFreeSpscQueue(Aligned::Align(unaligned_buffer)) {}

  /// Constructs an empty `LockFreeSpscQueue` using a
  /// `pw::containers::Storage` buffer. This constructor avoids runtime
  /// alignment checks.
  ///
  /// @param[in,out] storage Pre-aligned storage buffer for the queue.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{yes}
  /// @nmisafe{yes}
  template <size_t kAlignment, size_t kSizeBytes>
  explicit constexpr LockFreeSpscQueue(
      containers::Storage<kAlignment, kSizeBytes>& storage) noexcept
      : LockFreeSpscQueue(Aligned(storage.data(), storage.size())) {
    static_assert(kAlignment >= alignof(value_type));
    static_assert(
        kSizeBytes / sizeof(value_type) <= kMaxCapacity,
        "The capacity is too large for the size_type; use a larger size_type");
  }

  // Disable copy and move since the queue does not own the buffer.
  LockFreeSpscQueue(const LockFreeSpscQueue&) = delete;
  LockFreeSpscQueue& operator=(const LockFreeSpscQueue&) = delete;

  LockFreeSpscQueue(LockFreeSpscQueue&&) = delete;
  LockFreeSpscQueue& operator=(LockFreeSpscQueue&&) = delete;

  /// Destroys the queue and any remaining elements.
  ///
  /// @pre No producer or consumer operations may be running concurrently during
  /// destruction.
  ///
  /// @note ISR-safe and NMI-safe if `T::~T()` is ISR/NMI-safe.
  ///
  /// @threadsafe{no}
  /// @isrsafe{maybe}
  /// @nmisafe{maybe}
  ~LockFreeSpscQueue() { clear(); }

  // Access (consumer side)

  /// Returns a mutable reference to the front element in the queue.
  ///
  /// @pre The queue must not be empty (`!empty()`). Crashes via `PW_ASSERT` if
  /// empty.
  ///
  /// @note Safe to call concurrently with producer operations from the single
  /// consumer context. NMI-safe if `!empty()`.
  ///
  /// @returns A reference to the oldest element in the queue.
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{yes}
  /// @nmisafe{maybe}
  [[nodiscard]] reference front() {
    pointer item = try_peek();
    PW_ASSERT(item != nullptr);
    return *item;
  }

  /// Returns a const reference to the front element in the queue.
  ///
  /// @pre The queue must not be empty (`!empty()`). Crashes via `PW_ASSERT` if
  /// empty.
  ///
  /// @note Safe to call concurrently with producer operations from the single
  /// consumer context. NMI-safe if `!empty()`.
  ///
  /// @returns A const reference to the oldest element in the queue.
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{yes}
  /// @nmisafe{maybe}
  [[nodiscard]] const_reference front() const {
    const_pointer item = try_peek();
    PW_ASSERT(item != nullptr);
    return *item;
  }

  /// Returns a pointer to the front element in the queue if available, without
  /// removing it.
  ///
  /// Unlike calling `!empty()` followed by `front()`, `try_peek()` performs a
  /// single acquire load of `tail_`. The returned pointer remains valid until
  /// the element is removed via `pop()` or `try_pop()`, or the queue is cleared
  /// or destroyed.
  ///
  /// @note Safe to call concurrently with producer operations from the single
  /// consumer context.
  ///
  /// @returns A pointer to the oldest element in the queue, or `nullptr` if the
  /// queue is empty.
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{yes}
  /// @nmisafe{maybe}
  [[nodiscard]] pointer try_peek() noexcept {
    const size_type head = head_.load(std::memory_order_relaxed);
    const size_type tail = tail_.load(std::memory_order_acquire);
    if (head == tail) {
      return nullptr;
    }
    return SlotPtr(BufferIndex(head));
  }

  /// Returns a const pointer to the front element in the queue if available,
  /// without removing it.
  ///
  /// @note Safe to call concurrently with producer operations from the single
  /// consumer context.
  ///
  /// @returns A const pointer to the oldest element in the queue, or `nullptr`
  /// if the queue is empty.
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{yes}
  /// @nmisafe{maybe}
  [[nodiscard]] const_pointer try_peek() const noexcept {
    const size_type head = head_.load(std::memory_order_relaxed);
    const size_type tail = tail_.load(std::memory_order_acquire);
    if (head == tail) {
      return nullptr;
    }
    return SlotPtr(BufferIndex(head));
  }

  // Size

  /// Checks whether the queue has no elements.
  ///
  /// Because `head_` is only advanced by the consumer and `tail_` is only
  /// advanced by the producer, if `empty()` returns `false` to the consumer, at
  /// least one element is guaranteed to remain available to consume.
  ///
  /// @note Safe to call concurrently from either the producer or the consumer
  /// context.
  ///
  /// @returns `true` if the queue is empty, or `false` otherwise.
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{yes}
  /// @nmisafe{maybe}
  [[nodiscard]] bool empty() const noexcept {
    const size_type head = head_.load(std::memory_order_acquire);
    const size_type tail = tail_.load(std::memory_order_acquire);
    return head == tail;
  }

  /// Checks whether the queue is at full capacity.
  ///
  /// Because `tail_` is only advanced by the producer and `head_` is only
  /// advanced by the consumer, if `full()` returns `false` to the producer, at
  /// least one slot is guaranteed to remain available for the producer.
  ///
  /// @note Safe to call concurrently from either the producer or the consumer
  /// context.
  ///
  /// @returns `true` if the queue is at capacity, or `false` otherwise.
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{yes}
  /// @nmisafe{maybe}
  [[nodiscard]] bool full() const noexcept { return size() == capacity_; }

  /// Returns the number of elements currently in the queue.
  ///
  /// In an SPSC queue, at least one of `head_` (consumer) or `tail_` (producer)
  /// is invariant when called from either endpoint, so loading both with
  /// acquire ordering yields a consistent snapshot without a retry loop. For
  /// the consumer, the returned count is a conservative lower bound on
  /// available items; for the producer, it is a conservative upper bound on
  /// queued items.
  ///
  /// @note Safe to call concurrently from either the producer or the consumer
  /// context.
  ///
  /// @returns The number of elements in the queue.
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{yes}
  /// @nmisafe{maybe}
  [[nodiscard]] size_type size() const noexcept {
    const size_type head = head_.load(std::memory_order_acquire);
    const size_type tail = tail_.load(std::memory_order_acquire);
    return SizeFromIndices(head, tail);
  }

  /// Returns the maximum number of elements the queue can hold.
  ///
  /// @returns The capacity of the queue in elements.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{yes}
  /// @nmisafe{yes}
  [[nodiscard]] constexpr size_type capacity() const noexcept {
    return capacity_;
  }

  /// Returns the maximum number of elements the queue can hold.
  ///
  /// @returns The maximum capacity of the queue in elements.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{yes}
  /// @nmisafe{yes}
  [[nodiscard]] constexpr size_type max_size() const noexcept {
    return capacity_;
  }

  /// Returns the number of additional elements that can be pushed without
  /// exceeding capacity.
  ///
  /// @note Safe to call concurrently from either the producer or the consumer
  /// context.
  ///
  /// @returns The number of unoccupied slots in the queue.
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{yes}
  /// @nmisafe{maybe}
  [[nodiscard]] size_type remaining_capacity() const noexcept {
    return static_cast<size_type>(capacity_ - size());
  }

  // Modify

  /// Destroys all elements in the queue and resets its indices.
  ///
  /// @pre Requires exclusive access; no producer or consumer operations may run
  /// concurrently with `clear()`.
  ///
  /// @note ISR-safe and NMI-safe if `T::~T()` is ISR/NMI-safe.
  ///
  /// @threadsafe{no}
  /// @isrsafe{maybe}
  /// @nmisafe{maybe}
  void clear() noexcept {
    if constexpr (!std::is_trivially_destructible_v<value_type>) {
      size_type head = head_.load(std::memory_order_relaxed);
      const size_type tail = tail_.load(std::memory_order_relaxed);
      while (head != tail) {
        std::destroy_at(SlotPtr(BufferIndex(head)));
        head = AdvanceIndex(head, 1);
      }
    }
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
  }

  /// Pushes a copy of `value` to the back of the queue.
  ///
  /// @pre The queue must not be full (`!full()`). Crashes via `PW_ASSERT` if
  /// the queue is full.
  ///
  /// @note Safe to call concurrently with consumer operations from the single
  /// producer context. ISR-safe and NMI-safe if the queue is not full and
  /// `T`'s copy constructor is ISR/NMI-safe.
  ///
  /// @param[in] value The value to copy into the queue.
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{maybe}
  /// @nmisafe{maybe}
  void push(const value_type& value) { PW_ASSERT(try_push(value)); }

  /// Pushes `value` to the back of the queue by move.
  ///
  /// @pre The queue must not be full (`!full()`). Crashes via `PW_ASSERT` if
  /// the queue is full.
  ///
  /// @note Safe to call concurrently with consumer operations from the single
  /// producer context. ISR-safe and NMI-safe if the queue is not full and
  /// `T`'s move constructor is ISR/NMI-safe.
  ///
  /// @param[in] value The value to move into the queue.
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{maybe}
  /// @nmisafe{maybe}
  void push(value_type&& value) { PW_ASSERT(try_push(std::move(value))); }

  /// Attempts to push a copy of `value` to the back of the queue.
  ///
  /// @note Safe to call concurrently with consumer operations from the single
  /// producer context. ISR-safe and NMI-safe if `T`'s copy constructor is
  /// ISR/NMI-safe.
  ///
  /// @param[in] value The value to copy into the queue.
  /// @returns `true` if the element was pushed, or `false` if the queue is
  /// full.
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{maybe}
  /// @nmisafe{maybe}
  [[nodiscard]] bool try_push(const value_type& value) {
    return try_emplace(value);
  }

  /// Attempts to push `value` to the back of the queue by move.
  ///
  /// @note Safe to call concurrently with consumer operations from the single
  /// producer context. ISR-safe and NMI-safe if `T`'s move constructor is
  /// ISR/NMI-safe.
  ///
  /// @param[in] value The value to move into the queue. If `false` is returned
  /// because the queue is full, `value` is not moved from.
  /// @returns `true` if the element was pushed, or `false` if the queue is
  /// full.
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{maybe}
  /// @nmisafe{maybe}
  [[nodiscard]] bool try_push(value_type&& value) {
    return try_emplace(std::move(value));
  }

  /// Constructs an element in-place at the back of the queue.
  ///
  /// @pre The queue must not be full (`!full()`). Crashes via `PW_ASSERT` if
  /// the queue is full.
  ///
  /// @note Safe to call concurrently with consumer operations from the single
  /// producer context. ISR-safe and NMI-safe if the queue is not full and
  /// `T`'s constructor is ISR/NMI-safe.
  ///
  /// @tparam Args Constructor argument types for `T`.
  /// @param[in] args Arguments forwarded to `T`'s constructor.
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{maybe}
  /// @nmisafe{maybe}
  template <typename... Args>
  void emplace(Args&&... args) {
    PW_ASSERT(try_emplace(std::forward<Args>(args)...));
  }

  /// Attempts to construct an element in-place at the back of the queue.
  ///
  /// @note Safe to call concurrently with consumer operations from the single
  /// producer context. ISR-safe and NMI-safe if `T`'s constructor is
  /// ISR/NMI-safe.
  ///
  /// @tparam Args Constructor argument types for `T`.
  /// @param[in] args Arguments forwarded to `T`'s constructor.
  /// @returns `true` if the element was constructed and pushed, or `false` if
  /// the queue is full.
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{maybe}
  /// @nmisafe{maybe}
  template <typename... Args>
  [[nodiscard]] bool try_emplace(Args&&... args) {
    const size_type tail = tail_.load(std::memory_order_relaxed);
    const size_type head = head_.load(std::memory_order_acquire);
    if (SizeFromIndices(head, tail) == capacity_) {
      return false;
    }
    new (SlotBytes(BufferIndex(tail))) value_type(std::forward<Args>(args)...);
    tail_.store(AdvanceIndex(tail, 1), std::memory_order_release);
    return true;
  }

  /// Removes and destroys the front element from the queue.
  ///
  /// @pre The queue must not be empty (`!empty()`). Crashes via `PW_ASSERT` if
  /// empty.
  ///
  /// @note Safe to call concurrently with producer operations from the single
  /// consumer context. ISR-safe and NMI-safe if `!empty()` and `T::~T()` is
  /// ISR/NMI-safe.
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{maybe}
  /// @nmisafe{maybe}
  void pop() {
    const size_type head = head_.load(std::memory_order_relaxed);
    const size_type tail = tail_.load(std::memory_order_acquire);
    PW_ASSERT(head != tail);
    std::destroy_at(SlotPtr(BufferIndex(head)));
    head_.store(AdvanceIndex(head, 1), std::memory_order_release);
  }

  /// Removes and returns the front element from the queue if available.
  ///
  /// @note Safe to call concurrently with producer operations from the single
  /// consumer context. ISR-safe and NMI-safe if `T`'s move/copy constructor
  /// and destructor are ISR/NMI-safe.
  ///
  /// @returns The front element moved out of the queue, or `std::nullopt` if
  /// the queue is empty.
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{maybe}
  /// @nmisafe{maybe}
  [[nodiscard]] std::optional<value_type> try_pop() {
    const size_type head = head_.load(std::memory_order_relaxed);
    const size_type tail = tail_.load(std::memory_order_acquire);
    if (head == tail) {
      return std::nullopt;
    }
    pointer item = SlotPtr(BufferIndex(head));
    std::optional<value_type> result;
    if constexpr (std::is_move_constructible_v<value_type>) {
      result.emplace(std::move(*item));
    } else {
      result.emplace(*item);
    }
    std::destroy_at(item);
    head_.store(AdvanceIndex(head, 1), std::memory_order_release);
    return result;
  }

  /// Removes the front element from the queue and assigns it to `out` if
  /// available.
  ///
  /// @note Safe to call concurrently with producer operations from the single
  /// consumer context. ISR-safe and NMI-safe if `T`'s move/copy assignment
  /// operator and destructor are ISR/NMI-safe.
  ///
  /// @param[out] out Destination object to receive the popped element. Left
  /// unmodified if `false` is returned.
  /// @returns `true` if an element was popped into `out`, or `false` if the
  /// queue is empty.
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{maybe}
  /// @nmisafe{maybe}
  [[nodiscard]] bool try_pop(value_type& out) {
    const size_type head = head_.load(std::memory_order_relaxed);
    const size_type tail = tail_.load(std::memory_order_acquire);
    if (head == tail) {
      return false;
    }
    pointer item = SlotPtr(BufferIndex(head));
    if constexpr (std::is_move_assignable_v<value_type>) {
      out = std::move(*item);
    } else {
      out = *item;
    }
    std::destroy_at(item);
    head_.store(AdvanceIndex(head, 1), std::memory_order_release);
    return true;
  }

 private:
  template <typename, size_t, typename>
  friend class FixedLockFreeSpscQueue;

  using Aligned = containers::internal::Aligned<value_type>;

  explicit constexpr LockFreeSpscQueue(Aligned buffer) noexcept
      : buffer_(buffer.data()),
        capacity_(static_cast<size_type>(buffer.capacity())),
        head_(0),
        tail_(0) {
    PW_ASSERT(buffer.capacity() <= kMaxCapacity);
  }

  void MoveItemsFrom(LockFreeSpscQueue& other) {
    size_type other_head = other.head_.load(std::memory_order_relaxed);
    const size_type other_tail = other.tail_.load(std::memory_order_relaxed);
    PW_ASSERT(other.SizeFromIndices(other_head, other_tail) <= capacity_);
    size_type new_tail = 0;
    while (other_head != other_tail) {
      pointer item = other.SlotPtr(other.BufferIndex(other_head));
      if constexpr (std::is_move_constructible_v<value_type>) {
        new (SlotBytes(new_tail)) value_type(std::move(*item));
      } else {
        new (SlotBytes(new_tail)) value_type(*item);
      }
      std::destroy_at(item);
      other_head = other.AdvanceIndex(other_head, 1);
      ++new_tail;
    }
    other.head_.store(0, std::memory_order_relaxed);
    other.tail_.store(0, std::memory_order_relaxed);
    head_.store(0, std::memory_order_relaxed);
    tail_.store(new_tail, std::memory_order_release);
  }

  void MoveBufferFrom(LockFreeSpscQueue& other) noexcept {
    clear();
    buffer_ = std::exchange(other.buffer_, nullptr);
    capacity_ = std::exchange(other.capacity_, 0);
    head_.store(other.head_.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
    tail_.store(other.tail_.load(std::memory_order_relaxed),
                std::memory_order_release);
    other.head_.store(0, std::memory_order_relaxed);
    other.tail_.store(0, std::memory_order_relaxed);
  }

  std::byte* raw_buffer() noexcept { return buffer_; }

  // `SlotBytes` returns the raw storage address for placement `new` before an
  // element's lifetime begins, whereas `SlotPtr` uses `std::launder` (which
  // requires a live `value_type` object to already exist at that address) to
  // access or destroy an existing element.
  std::byte* SlotBytes(size_type buffer_index) noexcept {
    return buffer_ + buffer_index * sizeof(value_type);
  }

  const std::byte* SlotBytes(size_type buffer_index) const noexcept {
    return buffer_ + buffer_index * sizeof(value_type);
  }

  pointer SlotPtr(size_type buffer_index) noexcept {
    return std::launder(reinterpret_cast<pointer>(SlotBytes(buffer_index)));
  }

  const_pointer SlotPtr(size_type buffer_index) const noexcept {
    return std::launder(
        reinterpret_cast<const_pointer>(SlotBytes(buffer_index)));
  }

  constexpr size_type BufferIndex(size_type virtual_index) const noexcept {
    return virtual_index < capacity_
               ? virtual_index
               : static_cast<size_type>(virtual_index - capacity_);
  }

  constexpr size_type AdvanceIndex(size_type virtual_index,
                                   size_type count) const noexcept {
    const size_type wrap = static_cast<size_type>(size_type{2} * capacity_);
    const size_type to_wrap = static_cast<size_type>(wrap - virtual_index);
    return count < to_wrap ? static_cast<size_type>(virtual_index + count)
                           : static_cast<size_type>(count - to_wrap);
  }

  constexpr size_type SizeFromIndices(size_type head,
                                      size_type tail) const noexcept {
    if (tail >= head) {
      return static_cast<size_type>(tail - head);
    }
    const size_type wrap = static_cast<size_type>(size_type{2} * capacity_);
    return static_cast<size_type>(wrap - head + tail);
  }

  std::byte* buffer_;
  size_type capacity_;
  std::atomic<size_type> head_;
  std::atomic<size_type> tail_;
};

/// Single-producer, single-consumer (SPSC) lock-free queue with integrated or
/// owned storage, equivalent to `pw::FixedQueue`.
///
/// When `kInlineCapacity` is specified, `FixedLockFreeSpscQueue` statically
/// allocates its storage buffer inline. When `kInlineCapacity` is omitted (or
/// set to `containers::kExternalStorage`), the external-storage specialization
/// is used, which supports dynamic allocation via `Allocate()` /
/// `TryAllocate()`, ownership transfer via `WithStorage()`, or unowned external
/// buffers. Both specializations inherit from `LockFreeSpscQueue<T, SizeType>`
/// and can be used through a capacity-erased `LockFreeSpscQueue<T, SizeType>&`
/// reference.
///
/// @tparam T The type of elements stored in the queue.
/// @tparam kInlineCapacity The maximum number of elements the queue can hold.
///     If set to `containers::kExternalStorage`, the queue storage will be
///     outside the object.
/// @tparam SizeType Unsigned integer type used to store sizes and indices.
///
/// @threadsafe{maybe}
/// @isrsafe{maybe}
/// @nmisafe{maybe}
template <typename T,
          size_t kInlineCapacity = containers::kExternalStorage,
          typename SizeType = typename LockFreeSpscQueue<T>::size_type>
class FixedLockFreeSpscQueue final
    : private containers::StorageBaseFor<T, kInlineCapacity>,
      public LockFreeSpscQueue<T, SizeType> {
 public:
  static_assert(kInlineCapacity > 0, "Capacity must be greater than zero.");

  /// Constructs an empty `FixedLockFreeSpscQueue` with internal, statically
  /// allocated storage.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{yes}
  /// @nmisafe{yes}
  constexpr FixedLockFreeSpscQueue() noexcept
      : containers::StorageBaseFor<T, kInlineCapacity>{},
        LockFreeSpscQueue<T, SizeType>(this->storage()) {}

  FixedLockFreeSpscQueue(const FixedLockFreeSpscQueue&) = delete;
  FixedLockFreeSpscQueue& operator=(const FixedLockFreeSpscQueue&) = delete;

  /// Move-constructs a `FixedLockFreeSpscQueue` from another queue of equal
  /// capacity.
  ///
  /// @pre Neither `*this` nor `other` may be accessed concurrently during the
  /// move.
  ///
  /// @param[in] other The queue to move elements from.
  ///
  /// @threadsafe{no}
  /// @isrsafe{maybe}
  /// @nmisafe{maybe}
  FixedLockFreeSpscQueue(FixedLockFreeSpscQueue&& other) noexcept
      : FixedLockFreeSpscQueue() {
    this->MoveItemsFrom(other);
  }

  /// Move-assigns a `FixedLockFreeSpscQueue` from another queue of equal
  /// capacity.
  ///
  /// @pre Neither `*this` nor `other` may be accessed concurrently during the
  /// move.
  ///
  /// @param[in] other The queue to move elements from.
  /// @returns A reference to `*this`.
  ///
  /// @threadsafe{no}
  /// @isrsafe{maybe}
  /// @nmisafe{maybe}
  FixedLockFreeSpscQueue& operator=(FixedLockFreeSpscQueue&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    this->clear();
    this->MoveItemsFrom(other);
    return *this;
  }

  /// Move-constructs from a `FixedLockFreeSpscQueue` of smaller inline
  /// capacity.
  ///
  /// @pre Neither `*this` nor `other` may be accessed concurrently during the
  /// move.
  ///
  /// @param[in] other The queue to move elements from.
  ///
  /// @threadsafe{no}
  /// @isrsafe{maybe}
  /// @nmisafe{maybe}
  template <size_t kOtherCapacity,
            typename = std::enable_if_t<(kOtherCapacity < kInlineCapacity)>>
  FixedLockFreeSpscQueue(
      FixedLockFreeSpscQueue<T, kOtherCapacity, SizeType>&& other) noexcept
      : FixedLockFreeSpscQueue() {
    this->MoveItemsFrom(other);
  }

  /// Move-assigns from a `FixedLockFreeSpscQueue` of smaller inline capacity.
  ///
  /// @pre Neither `*this` nor `other` may be accessed concurrently during the
  /// move.
  ///
  /// @param[in] other The queue to move elements from.
  /// @returns A reference to `*this`.
  ///
  /// @threadsafe{no}
  /// @isrsafe{maybe}
  /// @nmisafe{maybe}
  template <size_t kOtherCapacity,
            typename = std::enable_if_t<(kOtherCapacity < kInlineCapacity)>>
  FixedLockFreeSpscQueue& operator=(
      FixedLockFreeSpscQueue<T, kOtherCapacity, SizeType>&& other) noexcept {
    this->clear();
    this->MoveItemsFrom(other);
    return *this;
  }

  /// Returns `nullptr`; a `FixedLockFreeSpscQueue` with static storage never
  /// allocates.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{yes}
  /// @nmisafe{yes}
  constexpr Deallocator* deallocator() const noexcept { return nullptr; }
};

/// Specialization of `FixedLockFreeSpscQueue` using an external static or
/// dynamically allocated fixed storage buffer. Equivalent to
/// `pw::FixedQueue<T, containers::kExternalStorage, S>`.
template <typename T, typename S>
class FixedLockFreeSpscQueue<T, containers::kExternalStorage, S> final
    : public LockFreeSpscQueue<T, S> {
 public:
  /// Allocates a buffer large enough to hold `capacity` items and uses it for a
  /// `FixedLockFreeSpscQueue`'s buffer. Crashes if unable to allocate space for
  /// `capacity` items or if `capacity` exceeds `kMaxCapacity`.
  ///
  /// The `FixedLockFreeSpscQueue` owns the buffer and frees it upon
  /// destruction.
  ///
  /// @param[in,out] allocator The allocator used to allocate the buffer.
  /// @param[in] capacity The maximum number of elements the queue can hold.
  /// @returns The allocated `FixedLockFreeSpscQueue`.
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{maybe}
  /// @nmisafe{maybe}
  static FixedLockFreeSpscQueue Allocate(Allocator& allocator,
                                         const S capacity) {
    FixedLockFreeSpscQueue queue = TryAllocate(allocator, capacity);
    PW_ASSERT(queue.capacity() == capacity);
    return queue;
  }

  /// Attempts to allocate a `T`-aligned buffer large enough to hold `capacity`
  /// items and use it for a `FixedLockFreeSpscQueue`'s buffer. If unable to
  /// allocate space for `capacity` items or if `capacity` exceeds
  /// `kMaxCapacity`, returns a `FixedLockFreeSpscQueue` with a capacity of 0.
  ///
  /// If allocation succeeds, the `FixedLockFreeSpscQueue` owns the buffer and
  /// frees it upon destruction.
  ///
  /// @param[in,out] allocator The allocator used to allocate the buffer.
  /// @param[in] capacity The maximum number of elements the queue can hold.
  /// @returns The allocated `FixedLockFreeSpscQueue`, or an empty queue with
  ///     capacity 0 if allocation fails.
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{maybe}
  /// @nmisafe{maybe}
  static FixedLockFreeSpscQueue TryAllocate(Allocator& allocator, S capacity) {
    if (capacity == 0 || capacity > LockFreeSpscQueue<T, S>::kMaxCapacity) {
      return FixedLockFreeSpscQueue(Aligned(nullptr, 0u), nullptr);
    }
    const allocator::Layout layout = allocator::Layout::Of<T[]>(capacity);
    std::byte* array = static_cast<std::byte*>(allocator.Allocate(layout));
    if (array == nullptr) {
      return FixedLockFreeSpscQueue(Aligned(array, 0u), nullptr);
    }
    return FixedLockFreeSpscQueue(Aligned(array, layout.size()), &allocator);
  }

  /// Constructs an empty `FixedLockFreeSpscQueue` that uses the provided
  /// `pw::UniquePtr` for its storage. Takes ownership of the `pw::UniquePtr`.
  ///
  /// @pre `storage` MUST be aligned at least as `value_type` and its element
  /// capacity must not exceed `kMaxCapacity`.
  ///
  /// @param[in] storage Owned byte buffer to use for element storage.
  /// @returns A `FixedLockFreeSpscQueue` owning `storage`.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{yes}
  /// @nmisafe{maybe}
  static FixedLockFreeSpscQueue WithStorage(UniquePtr<std::byte[]>&& storage) {
    const size_t size = storage.size();
    Deallocator* deallocator = storage.deallocator();
    return FixedLockFreeSpscQueue(Aligned::Assert(storage.Release(), size),
                                  deallocator);
  }

  /// Constructs a `FixedLockFreeSpscQueue` from an unowned buffer. The buffer
  /// is not freed when the `FixedLockFreeSpscQueue` is destroyed.
  ///
  /// @param[in] unaligned_buffer Storage for the queue. If the buffer is not
  ///     aligned as `value_type`, an aligned subset of the buffer will be used.
  ///     If the buffer is too small to hold at least one `value_type`, the
  ///     queue's capacity will be 0.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{yes}
  /// @nmisafe{maybe}
  explicit constexpr FixedLockFreeSpscQueue(
      span<std::byte> unaligned_buffer) noexcept
      : LockFreeSpscQueue<T, S>(unaligned_buffer), deallocator_(nullptr) {}

  /// Constructs an empty `FixedLockFreeSpscQueue` using a
  /// `pw::containers::Storage` buffer. This constructor avoids runtime
  /// alignment checks.
  ///
  /// The buffer is unowned and is not freed when the `FixedLockFreeSpscQueue`
  /// is destroyed.
  ///
  /// @param[in,out] buffer Pre-aligned storage buffer for the queue.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{yes}
  /// @nmisafe{yes}
  template <size_t kAlignment, size_t kSizeBytes>
  explicit constexpr FixedLockFreeSpscQueue(
      containers::Storage<kAlignment, kSizeBytes>& buffer) noexcept
      : LockFreeSpscQueue<T, S>(buffer), deallocator_(nullptr) {}

  FixedLockFreeSpscQueue(const FixedLockFreeSpscQueue&) = delete;
  FixedLockFreeSpscQueue& operator=(const FixedLockFreeSpscQueue&) = delete;

  /// Move-constructs a `FixedLockFreeSpscQueue` with external storage in O(1)
  /// time by transferring ownership of the underlying buffer.
  ///
  /// @pre `other` must not be accessed concurrently during the move.
  ///
  /// @param[in] other The queue to move from.
  ///
  /// @threadsafe{no}
  /// @isrsafe{yes}
  /// @nmisafe{maybe}
  FixedLockFreeSpscQueue(FixedLockFreeSpscQueue&& other) noexcept
      : LockFreeSpscQueue<T, S>(span<std::byte>()),
        deallocator_(std::exchange(other.deallocator_, nullptr)) {
    this->MoveBufferFrom(other);
  }

  /// Move-assigns a `FixedLockFreeSpscQueue` with external storage in O(1)
  /// time, destroying any elements and freeing any owned buffer in `*this`
  /// before transferring ownership from `other`.
  ///
  /// @pre Neither `*this` nor `other` may be accessed concurrently during the
  /// move.
  ///
  /// @param[in] other The queue to move from.
  /// @returns A reference to `*this`.
  ///
  /// @threadsafe{no}
  /// @isrsafe{maybe}
  /// @nmisafe{maybe}
  FixedLockFreeSpscQueue& operator=(FixedLockFreeSpscQueue&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    std::byte* const old_buffer = this->raw_buffer();
    this->MoveBufferFrom(other);

    if (deallocator_ != nullptr) {
      deallocator_->Deallocate(old_buffer);
    }
    deallocator_ = std::exchange(other.deallocator_, nullptr);
    return *this;
  }

  /// Destroys all remaining elements in the queue and frees the storage buffer
  /// if owned.
  ///
  /// @pre No producer or consumer operations may be running concurrently during
  /// destruction.
  ///
  /// @threadsafe{no}
  /// @isrsafe{maybe}
  /// @nmisafe{maybe}
  ~FixedLockFreeSpscQueue() {
    this->clear();
    if (deallocator_ != nullptr) {
      deallocator_->Deallocate(this->raw_buffer());
    }
  }

  /// Returns the deallocator that will be used to free this
  /// `FixedLockFreeSpscQueue`'s storage, or `nullptr` if the storage is
  /// unowned.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{yes}
  /// @nmisafe{yes}
  constexpr Deallocator* deallocator() const noexcept { return deallocator_; }

 private:
  using Aligned = typename LockFreeSpscQueue<T, S>::Aligned;

  constexpr FixedLockFreeSpscQueue(Aligned buffer, Deallocator* deallocator)
      : LockFreeSpscQueue<T, S>(buffer), deallocator_(deallocator) {}

  Deallocator* deallocator_;
};

/// @}

}  // namespace pw
