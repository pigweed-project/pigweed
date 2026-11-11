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

#include <algorithm>
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
/// Producer operations (`push()`, `try_push()`, `emplace()`, `try_emplace()`,
/// `push_some()`, `move_push_some()`, `try_push_all()`, `try_move_push_all()`,
/// `BeginWrite()`) may be called concurrently with consumer operations
/// (`front()`, `try_peek()`, `pop()`, `try_pop()`, `pop_some()`, `pop_n()`,
/// `BeginRead()`), provided there is at most one active producer context and at
/// most one active consumer context at any time. Size observers (`empty()`,
/// `full()`, `size()`, `remaining_capacity()`) may be called concurrently from
/// either the producer or the consumer context (not from a third independent
/// thread). `clear()` and destruction require exclusive access with no
/// concurrent producer or consumer operations.
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

  /// Scoped RAII producer transaction for staging and committing multiple
  /// elements with a single atomic release store.
  ///
  /// `WriteTransaction` loads `tail_` (relaxed) and `head_` (acquire) once upon
  /// construction, caching the available capacity for the lifetime of the
  /// transaction. Elements constructed via `emplace()`, `try_emplace()`,
  /// `push()`, or `try_push()`—or written directly into `contiguous_space()` /
  /// `write_space()` and marked with `advance()`—are not visible to the
  /// consumer until `Commit()` is called or the `WriteTransaction` is
  /// destroyed.
  ///
  /// Calling `Abort()` destroys any staged, uncommitted elements in reverse
  /// construction order and leaves the queue's `tail_` unchanged.
  ///
  /// @pre At most one `WriteTransaction` may be active on a queue at any time,
  /// and no direct producer methods (`push()`, `try_push()`, `emplace()`,
  /// `try_emplace()`, `push_some()`, `move_push_some()`, `try_push_all()`,
  /// `try_move_push_all()`) may be called on the queue while a
  /// `WriteTransaction` is active.
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{maybe}
  /// @nmisafe{maybe}
  class WriteTransaction {
   public:
    /// Constructs an inactive, empty `WriteTransaction`.
    constexpr WriteTransaction() noexcept = default;

    WriteTransaction(const WriteTransaction&) = delete;
    WriteTransaction& operator=(const WriteTransaction&) = delete;

    /// Moves an active `WriteTransaction`, transferring ownership of any
    /// uncommitted staged elements.
    constexpr WriteTransaction(WriteTransaction&& other) noexcept
        : queue_(std::exchange(other.queue_, nullptr)),
          initial_tail_(std::exchange(other.initial_tail_, 0)),
          current_tail_(std::exchange(other.current_tail_, 0)),
          staged_(std::exchange(other.staged_, 0)),
          max_count_(std::exchange(other.max_count_, 0)) {}

    /// Move-assigns a `WriteTransaction`, committing any elements staged in
    /// `*this` before transferring ownership from `other`.
    WriteTransaction& operator=(WriteTransaction&& other) noexcept {
      if (this == &other) {
        return *this;
      }
      Commit();
      queue_ = std::exchange(other.queue_, nullptr);
      initial_tail_ = std::exchange(other.initial_tail_, 0);
      current_tail_ = std::exchange(other.current_tail_, 0);
      staged_ = std::exchange(other.staged_, 0);
      max_count_ = std::exchange(other.max_count_, 0);
      return *this;
    }

    /// Automatically commits any staged elements when the transaction goes out
    /// of scope.
    ~WriteTransaction() { Commit(); }

    /// Returns the number of elements staged since the start of the
    /// transaction or the last `Commit()`.
    [[nodiscard]] constexpr size_type staged_count() const noexcept {
      return staged_;
    }

    /// Returns the number of additional elements that can be staged in this
    /// transaction without refreshing `head_`.
    [[nodiscard]] constexpr size_type remaining_capacity() const noexcept {
      return static_cast<size_type>(max_count_ - staged_);
    }

    /// Returns `true` if no more elements can be staged in this transaction.
    [[nodiscard]] constexpr bool full() const noexcept {
      return staged_ == max_count_;
    }

    /// Constructs an element in-place at the next uncommitted producer slot
    /// without updating the queue's atomic `tail_`.
    ///
    /// @pre `!full()`. Crashes via `PW_ASSERT` if no space remains in this
    /// transaction.
    ///
    /// @param[in] args Arguments forwarded to `T`'s constructor.
    template <typename... Args>
    void emplace(Args&&... args) {
      PW_ASSERT(try_emplace(std::forward<Args>(args)...));
    }

    /// Attempts to construct an element in-place at the next uncommitted
    /// producer slot without updating the queue's atomic `tail_`.
    ///
    /// @param[in] args Arguments forwarded to `T`'s constructor.
    /// @returns `true` if the element was staged, or `false` if `full()`.
    template <typename... Args>
    [[nodiscard]] bool try_emplace(Args&&... args) {
      if (staged_ == max_count_) {
        return false;
      }
      new (queue_->SlotBytes(queue_->BufferIndex(current_tail_)))
          value_type(std::forward<Args>(args)...);
      current_tail_ = queue_->AdvanceIndex(current_tail_, 1);
      ++staged_;
      return true;
    }

    /// Stages a copy of `value` at the next uncommitted producer slot.
    ///
    /// @pre `!full()`. Crashes via `PW_ASSERT` if `full()`.
    ///
    /// @param[in] value The value to copy into the queue.
    void push(const value_type& value) { PW_ASSERT(try_push(value)); }

    /// Stages `value` at the next uncommitted producer slot by move.
    ///
    /// @pre `!full()`. Crashes via `PW_ASSERT` if `full()`.
    ///
    /// @param[in] value The value to move into the queue.
    void push(value_type&& value) { PW_ASSERT(try_push(std::move(value))); }

    /// Attempts to stage a copy of `value` at the next uncommitted producer
    /// slot.
    ///
    /// @param[in] value The value to copy into the queue.
    /// @returns `true` if the element was staged, or `false` if `full()`.
    [[nodiscard]] bool try_push(const value_type& value) {
      return try_emplace(value);
    }

    /// Attempts to stage `value` at the next uncommitted producer slot by move.
    ///
    /// @param[in] value The value to move into the queue. Left unmodified if
    ///     `false` is returned.
    /// @returns `true` if the element was staged, or `false` if `full()`.
    [[nodiscard]] bool try_push(value_type&& value) {
      return try_emplace(std::move(value));
    }

    /// Stages as many elements from `items` as fit in `remaining_capacity()` by
    /// copy.
    ///
    /// @param[in] items Span of elements to copy into the transaction.
    /// @returns The number of elements staged.
    size_type push_some(span<const value_type> items) {
      const size_type to_push = static_cast<size_type>(
          std::min<size_t>(remaining_capacity(), items.size()));
      if (to_push == 0) {
        return 0;
      }
      queue_->CopyIntoAt(
          queue_->BufferIndex(current_tail_), to_push, items.data());
      current_tail_ = queue_->AdvanceIndex(current_tail_, to_push);
      staged_ = static_cast<size_type>(staged_ + to_push);
      return to_push;
    }

    /// Stages as many elements from `items` as fit in `remaining_capacity()` by
    /// move.
    ///
    /// @param[in,out] items Span of elements to move into the transaction.
    /// @returns The number of elements staged.
    size_type move_push_some(span<value_type> items) {
      const size_type to_push = static_cast<size_type>(
          std::min<size_t>(remaining_capacity(), items.size()));
      if (to_push == 0) {
        return 0;
      }
      queue_->MoveIntoAt(
          queue_->BufferIndex(current_tail_), to_push, items.data());
      current_tail_ = queue_->AdvanceIndex(current_tail_, to_push);
      staged_ = static_cast<size_type>(staged_ + to_push);
      return to_push;
    }

    /// Returns the first contiguous span of writable slots starting at the
    /// current uncommitted producer position.
    ///
    /// Only available when `T` is trivially default constructible and trivially
    /// copyable (such as `std::byte`, `uint8_t`, or POD structs), so writing
    /// directly into the returned span without placement `new` is well-defined.
    /// After writing `n` elements into the returned span, call `advance(n)` to
    /// mark them as staged.
    ///
    /// @returns A writable span of up to `remaining_capacity()` contiguous
    ///     elements before the end of the underlying ring buffer.
    template <typename U = value_type,
              typename = std::enable_if_t<
                  std::is_same_v<U, value_type> &&
                  std::is_trivially_default_constructible_v<U> &&
                  std::is_trivially_copyable_v<U>>>
    [[nodiscard]] span<value_type> contiguous_space() noexcept {
      return write_space().first;
    }

    /// Returns both contiguous writable segments covering all
    /// `remaining_capacity()` slots in the transaction (handling ring buffer
    /// wrap-around).
    ///
    /// Only available when `T` is trivially default constructible and trivially
    /// copyable. To preserve FIFO order, write elements sequentially into
    /// `first` from index `0` to `first.size() - 1` before writing into
    /// `second` from index `0` to `second.size() - 1` (`second` is empty if the
    /// available space does not wrap around the end of the ring buffer). After
    /// writing `n` total elements across `first` and `second`, call
    /// `advance(n)` to mark them as staged.
    ///
    /// @returns A pair of writable spans `{first, second}` in FIFO write order
    ///     whose combined size equals `remaining_capacity()`.
    template <typename U = value_type,
              typename = std::enable_if_t<
                  std::is_same_v<U, value_type> &&
                  std::is_trivially_default_constructible_v<U> &&
                  std::is_trivially_copyable_v<U>>>
    [[nodiscard]] std::pair<span<value_type>, span<value_type>>
    write_space() noexcept {
      const size_type avail = remaining_capacity();
      if (avail == 0) {
        return {};
      }
      const size_type offset = queue_->BufferIndex(current_tail_);
      const size_type first_count =
          std::min(avail, static_cast<size_type>(queue_->capacity_ - offset));
      const size_type second_count =
          static_cast<size_type>(avail - first_count);
      span<value_type> first(
          reinterpret_cast<pointer>(queue_->SlotBytes(offset)), first_count);
      span<value_type> second =
          second_count > 0
              ? span<value_type>(
                    reinterpret_cast<pointer>(queue_->SlotBytes(0)),
                    second_count)
              : span<value_type>();
      return {first, second};
    }

    /// Marks `count` elements written into `contiguous_space()` or
    /// `write_space()` as staged in this transaction.
    ///
    /// Only available when `T` is trivially default constructible and trivially
    /// copyable.
    ///
    /// @pre `count <= remaining_capacity()`. Crashes via `PW_ASSERT` if `count`
    /// exceeds `remaining_capacity()`.
    ///
    /// @param[in] count Number of elements written into the writable span(s).
    template <typename U = value_type,
              typename = std::enable_if_t<
                  std::is_same_v<U, value_type> &&
                  std::is_trivially_default_constructible_v<U> &&
                  std::is_trivially_copyable_v<U>>>
    void advance(size_type count) noexcept {
      PW_ASSERT(count <= remaining_capacity());
      if (count == 0) {
        return;
      }
      current_tail_ = queue_->AdvanceIndex(current_tail_, count);
      staged_ = static_cast<size_type>(staged_ + count);
    }

    /// Publishes all staged elements to the consumer with a single
    /// `std::memory_order_release` store to `tail_`.
    ///
    /// After `Commit()`, `staged_count()` is reset to 0 and additional elements
    /// may still be staged up to the remaining `remaining_capacity()`.
    ///
    /// @returns The number of elements committed by this call.
    size_type Commit() noexcept {
      if (staged_ == 0 || queue_ == nullptr) {
        return 0;
      }
      const size_type committed = staged_;
      queue_->tail_.store(current_tail_, std::memory_order_release);
      initial_tail_ = current_tail_;
      max_count_ = static_cast<size_type>(max_count_ - committed);
      staged_ = 0;
      return committed;
    }

    /// Destroys all uncommitted staged elements and rolls back the transaction
    /// without modifying the queue's atomic `tail_`.
    void Abort() noexcept {
      if (staged_ == 0 || queue_ == nullptr) {
        return;
      }
      if constexpr (!std::is_trivially_destructible_v<value_type>) {
        size_type pos = current_tail_;
        for (size_type i = 0; i < staged_; ++i) {
          pos = queue_->RetreatIndex(pos, 1);
          std::destroy_at(queue_->SlotPtr(queue_->BufferIndex(pos)));
        }
      }
      current_tail_ = initial_tail_;
      staged_ = 0;
    }

   private:
    /// @cond
    friend class LockFreeSpscQueue;
    /// @endcond

    constexpr WriteTransaction(LockFreeSpscQueue& queue,
                               size_type tail,
                               size_type avail) noexcept
        : queue_(&queue),
          initial_tail_(tail),
          current_tail_(tail),
          staged_(0),
          max_count_(avail) {}

    LockFreeSpscQueue* queue_ = nullptr;
    size_type initial_tail_ = 0;
    size_type current_tail_ = 0;
    size_type staged_ = 0;
    size_type max_count_ = 0;
  };

  /// Scoped RAII consumer transaction for inspecting and popping multiple
  /// elements with a single atomic release store.
  ///
  /// `ReadTransaction` loads `head_` (relaxed) and `tail_` (acquire) once upon
  /// construction, caching the number of available elements. Elements popped or
  /// consumed through the transaction are destroyed immediately, and `head_` is
  /// updated with a single `std::memory_order_release` store when `Commit()` is
  /// called or when the `ReadTransaction` is destroyed.
  ///
  /// @pre At most one `ReadTransaction` may be active on a queue at any time,
  /// and no direct consumer methods (`front()`, `try_peek()`, `pop()`,
  /// `try_pop()`, `pop_some()`, `pop_n()`) may be called on the queue while a
  /// `ReadTransaction` is active.
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{maybe}
  /// @nmisafe{maybe}
  class ReadTransaction {
   public:
    /// Constructs an inactive, empty `ReadTransaction`.
    constexpr ReadTransaction() noexcept = default;

    ReadTransaction(const ReadTransaction&) = delete;
    ReadTransaction& operator=(const ReadTransaction&) = delete;

    /// Moves an active `ReadTransaction`, transferring ownership of any
    /// uncommitted consumption progress.
    constexpr ReadTransaction(ReadTransaction&& other) noexcept
        : queue_(std::exchange(other.queue_, nullptr)),
          current_head_(std::exchange(other.current_head_, 0)),
          consumed_(std::exchange(other.consumed_, 0)),
          available_(std::exchange(other.available_, 0)) {}

    /// Move-assigns a `ReadTransaction`, committing any elements consumed in
    /// `*this` before transferring ownership from `other`.
    ReadTransaction& operator=(ReadTransaction&& other) noexcept {
      if (this == &other) {
        return *this;
      }
      Commit();
      queue_ = std::exchange(other.queue_, nullptr);
      current_head_ = std::exchange(other.current_head_, 0);
      consumed_ = std::exchange(other.consumed_, 0);
      available_ = std::exchange(other.available_, 0);
      return *this;
    }

    /// Automatically commits all consumed elements when the transaction goes
    /// out of scope.
    ~ReadTransaction() { Commit(); }

    /// Returns the number of elements consumed since the start of the
    /// transaction or the last `Commit()`.
    [[nodiscard]] constexpr size_type consumed_count() const noexcept {
      return consumed_;
    }

    /// Returns the number of unconsumed elements remaining in this transaction.
    [[nodiscard]] constexpr size_type remaining() const noexcept {
      return static_cast<size_type>(available_ - consumed_);
    }

    /// Returns `true` if no unconsumed elements remain in this transaction.
    [[nodiscard]] constexpr bool empty() const noexcept {
      return consumed_ == available_;
    }

    /// Returns a mutable reference to the next unconsumed element in the
    /// transaction.
    ///
    /// @pre `!empty()`. Crashes via `PW_ASSERT` if `empty()`.
    [[nodiscard]] reference front() {
      PW_ASSERT(consumed_ < available_);
      return *queue_->SlotPtr(queue_->BufferIndex(current_head_));
    }

    /// Returns a const reference to the next unconsumed element in the
    /// transaction.
    ///
    /// @pre `!empty()`. Crashes via `PW_ASSERT` if `empty()`.
    [[nodiscard]] const_reference front() const {
      PW_ASSERT(consumed_ < available_);
      return *queue_->SlotPtr(queue_->BufferIndex(current_head_));
    }

    /// Returns a pointer to the next unconsumed element in the transaction, or
    /// `nullptr` if `empty()`.
    [[nodiscard]] pointer try_peek() noexcept {
      if (consumed_ == available_) {
        return nullptr;
      }
      return queue_->SlotPtr(queue_->BufferIndex(current_head_));
    }

    /// Returns a const pointer to the next unconsumed element in the
    /// transaction, or `nullptr` if `empty()`.
    [[nodiscard]] const_pointer try_peek() const noexcept {
      if (consumed_ == available_) {
        return nullptr;
      }
      return queue_->SlotPtr(queue_->BufferIndex(current_head_));
    }

    /// Removes and destroys the next unconsumed element in the transaction
    /// without updating the queue's atomic `head_`.
    ///
    /// @pre `!empty()`. Crashes via `PW_ASSERT` if `empty()`.
    void pop() {
      PW_ASSERT(consumed_ < available_);
      std::destroy_at(queue_->SlotPtr(queue_->BufferIndex(current_head_)));
      current_head_ = queue_->AdvanceIndex(current_head_, 1);
      ++consumed_;
    }

    /// Removes and returns the next unconsumed element in the transaction, or
    /// `std::nullopt` if `empty()`.
    [[nodiscard]] std::optional<value_type> try_pop() {
      if (consumed_ == available_) {
        return std::nullopt;
      }
      pointer item = queue_->SlotPtr(queue_->BufferIndex(current_head_));
      std::optional<value_type> result;
      if constexpr (std::is_move_constructible_v<value_type>) {
        result.emplace(std::move(*item));
      } else {
        result.emplace(*item);
      }
      std::destroy_at(item);
      current_head_ = queue_->AdvanceIndex(current_head_, 1);
      ++consumed_;
      return result;
    }

    /// Removes the next unconsumed element in the transaction and assigns it to
    /// `out` if available.
    ///
    /// @param[out] out Destination object to receive the popped element. Left
    ///     unmodified if `false` is returned.
    /// @returns `true` if an element was popped into `out`, or `false` if
    ///     `empty()`.
    [[nodiscard]] bool try_pop(value_type& out) {
      if (consumed_ == available_) {
        return false;
      }
      pointer item = queue_->SlotPtr(queue_->BufferIndex(current_head_));
      if constexpr (std::is_move_assignable_v<value_type>) {
        out = std::move(*item);
      } else {
        out = *item;
      }
      std::destroy_at(item);
      current_head_ = queue_->AdvanceIndex(current_head_, 1);
      ++consumed_;
      return true;
    }

    /// Moves (or copies if not move-assignable) up to `dest.size()` unconsumed
    /// elements into `dest` and destroys their slots in the queue.
    ///
    /// @param[out] dest Destination span to receive the popped elements.
    /// @returns The number of elements popped into `dest`.
    size_type pop_some(span<value_type> dest) {
      const size_type to_pop =
          static_cast<size_type>(std::min<size_t>(remaining(), dest.size()));
      if (to_pop == 0) {
        return 0;
      }
      queue_->PopIntoAt(
          queue_->BufferIndex(current_head_), to_pop, dest.data());
      current_head_ = queue_->AdvanceIndex(current_head_, to_pop);
      consumed_ = static_cast<size_type>(consumed_ + to_pop);
      return to_pop;
    }

    /// Returns the first contiguous span of unconsumed elements starting at the
    /// current consumer position.
    ///
    /// After inspecting or reading `n` elements from the returned span, call
    /// `consume(n)` to destroy them and advance the transaction's read cursor.
    ///
    /// @warning Calling `consume()` immediately destroys consumed elements. Do
    /// not access consumed elements in a previously returned span after calling
    /// `consume()`.
    ///
    /// @returns A span of up to `remaining()` contiguous elements before the
    ///     end of the underlying ring buffer.
    [[nodiscard]] span<value_type> contiguous_data() noexcept {
      return read_space().first;
    }

    /// Returns the first contiguous const span of unconsumed elements starting
    /// at the current consumer position.
    ///
    /// @warning Calling `consume()` immediately destroys consumed elements. Do
    /// not access consumed elements in a previously returned span after calling
    /// `consume()`.
    [[nodiscard]] span<const value_type> contiguous_data() const noexcept {
      return read_space().first;
    }

    /// Returns both contiguous readable segments covering all `remaining()`
    /// unconsumed elements in the transaction (handling ring buffer
    /// wrap-around).
    ///
    /// To read elements in FIFO order (oldest to newest), read `first`
    /// sequentially from index `0` to `first.size() - 1`, followed by `second`
    /// from index `0` to `second.size() - 1` (`second` is empty if the
    /// unconsumed elements do not wrap around the end of the ring buffer).
    /// After reading `n` total elements across `first` and `second`, call
    /// `consume(n)` to destroy them and advance the transaction's read cursor.
    ///
    /// @warning Calling `consume()` immediately destroys consumed elements. Do
    /// not access consumed elements in a previously returned span after calling
    /// `consume()`.
    ///
    /// @returns A pair of spans `{first, second}` in FIFO read order whose
    ///     combined size equals `remaining()`.
    [[nodiscard]] std::pair<span<value_type>, span<value_type>>
    read_space() noexcept {
      const size_type avail = remaining();
      if (avail == 0) {
        return {};
      }
      const size_type offset = queue_->BufferIndex(current_head_);
      const size_type first_count =
          std::min(avail, static_cast<size_type>(queue_->capacity_ - offset));
      const size_type second_count =
          static_cast<size_type>(avail - first_count);
      span<value_type> first(queue_->SlotPtr(offset), first_count);
      span<value_type> second =
          second_count > 0 ? span<value_type>(queue_->SlotPtr(0), second_count)
                           : span<value_type>();
      return {first, second};
    }

    /// Returns both contiguous const readable segments covering all
    /// `remaining()` unconsumed elements in the transaction in FIFO order
    /// (`first` followed by `second`).
    ///
    /// @warning Calling `consume()` immediately destroys consumed elements. Do
    /// not access consumed elements in a previously returned span after calling
    /// `consume()`.
    [[nodiscard]] std::pair<span<const value_type>, span<const value_type>>
    read_space() const noexcept {
      const size_type avail = remaining();
      if (avail == 0) {
        return {};
      }
      const size_type offset = queue_->BufferIndex(current_head_);
      const size_type first_count =
          std::min(avail, static_cast<size_type>(queue_->capacity_ - offset));
      const size_type second_count =
          static_cast<size_type>(avail - first_count);
      span<const value_type> first(queue_->SlotPtr(offset), first_count);
      span<const value_type> second =
          second_count > 0
              ? span<const value_type>(queue_->SlotPtr(0), second_count)
              : span<const value_type>();
      return {first, second};
    }

    /// Destroys `count` unconsumed elements from the front of the transaction
    /// and advances the transaction's read cursor without updating the queue's
    /// atomic `head_`.
    ///
    /// @warning Calling `consume()` immediately destroys consumed elements. Do
    /// not access consumed elements in a previously returned span from
    /// `contiguous_data()` or `read_space()` after calling `consume()`.
    ///
    /// @pre `count <= remaining()`. Crashes via `PW_ASSERT` if `count` exceeds
    /// `remaining()`.
    ///
    /// @param[in] count Number of elements to consume and destroy.
    void consume(size_type count) noexcept {
      PW_ASSERT(count <= remaining());
      if (count == 0) {
        return;
      }
      queue_->DestroyAt(queue_->BufferIndex(current_head_), count);
      current_head_ = queue_->AdvanceIndex(current_head_, count);
      consumed_ = static_cast<size_type>(consumed_ + count);
    }

    /// Publishes all consumed elements to the producer with a single
    /// `std::memory_order_release` store to `head_`.
    ///
    /// After `Commit()`, `consumed_count()` is reset to 0 and any remaining
    /// unconsumed elements in `remaining()` may still be read and consumed.
    ///
    /// @returns The number of elements committed by this call.
    size_type Commit() noexcept {
      if (consumed_ == 0 || queue_ == nullptr) {
        return 0;
      }
      const size_type committed = consumed_;
      queue_->head_.store(current_head_, std::memory_order_release);
      available_ = static_cast<size_type>(available_ - committed);
      consumed_ = 0;
      return committed;
    }

   private:
    /// @cond
    friend class LockFreeSpscQueue;
    /// @endcond

    constexpr ReadTransaction(LockFreeSpscQueue& queue,
                              size_type head,
                              size_type avail) noexcept
        : queue_(&queue),
          current_head_(head),
          consumed_(0),
          available_(avail) {}

    LockFreeSpscQueue* queue_ = nullptr;
    size_type current_head_ = 0;
    size_type consumed_ = 0;
    size_type available_ = 0;
  };

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
      const size_type head = head_.load(std::memory_order_relaxed);
      const size_type tail = tail_.load(std::memory_order_relaxed);
      const size_type count = SizeFromIndices(head, tail);
      if (count > 0) {
        DestroyAt(BufferIndex(head), count);
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

  // Batch / Transaction Operations

  /// Begins a scoped producer transaction (`WriteTransaction`) that caches the
  /// current queue capacity with a single `std::memory_order_acquire` load of
  /// `head_` and commits all staged elements with a single
  /// `std::memory_order_release` store to `tail_` on `Commit()` or scope exit.
  ///
  /// @note Safe to call concurrently with consumer operations from the single
  /// producer context.
  ///
  /// @returns A `WriteTransaction` bound to this queue.
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{yes}
  /// @nmisafe{maybe}
  [[nodiscard]] WriteTransaction BeginWrite() noexcept {
    const size_type tail = tail_.load(std::memory_order_relaxed);
    const size_type head = head_.load(std::memory_order_acquire);
    const size_type avail =
        static_cast<size_type>(capacity_ - SizeFromIndices(head, tail));
    return WriteTransaction(*this, tail, avail);
  }

  /// Begins a scoped consumer transaction (`ReadTransaction`) that caches the
  /// number of available elements with a single `std::memory_order_acquire`
  /// load of `tail_` and commits all consumed elements with a single
  /// `std::memory_order_release` store to `head_` on `Commit()` or scope exit.
  ///
  /// @note Safe to call concurrently with producer operations from the single
  /// consumer context.
  ///
  /// @returns A `ReadTransaction` bound to this queue.
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{yes}
  /// @nmisafe{maybe}
  [[nodiscard]] ReadTransaction BeginRead() noexcept {
    const size_type head = head_.load(std::memory_order_relaxed);
    const size_type tail = tail_.load(std::memory_order_acquire);
    const size_type avail = SizeFromIndices(head, tail);
    return ReadTransaction(*this, head, avail);
  }

  /// Pushes as many elements from `items` as fit in the queue by copy, updating
  /// `tail_` with a single `std::memory_order_release` store.
  ///
  /// @note Safe to call concurrently with consumer operations from the single
  /// producer context.
  ///
  /// @param[in] items Span of elements to copy into the queue.
  /// @returns The number of elements pushed (`0 <= count <= items.size()`).
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{maybe}
  /// @nmisafe{maybe}
  size_type push_some(span<const value_type> items) {
    WriteTransaction tx = BeginWrite();
    return tx.push_some(items);
  }

  /// Pushes as many elements from `items` as fit in the queue by move, updating
  /// `tail_` with a single `std::memory_order_release` store.
  ///
  /// Only the first `count` elements of `items` (where `count` is the returned
  /// value) are moved from; any remaining elements are left unmodified.
  ///
  /// @note Safe to call concurrently with consumer operations from the single
  /// producer context.
  ///
  /// @param[in,out] items Span of elements to move into the queue.
  /// @returns The number of elements pushed (`0 <= count <= items.size()`).
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{maybe}
  /// @nmisafe{maybe}
  size_type move_push_some(span<value_type> items) {
    WriteTransaction tx = BeginWrite();
    return tx.move_push_some(items);
  }

  /// Attempts to push all elements of `items` into the queue by copy in a
  /// single all-or-nothing batch.
  ///
  /// If fewer than `items.size()` slots are available, no elements are pushed
  /// and `false` is returned.
  ///
  /// @note Safe to call concurrently with consumer operations from the single
  /// producer context.
  ///
  /// @param[in] items Span of elements to copy into the queue.
  /// @returns `true` if all elements in `items` were pushed, or `false` if
  ///     `remaining_capacity() < items.size()`.
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{maybe}
  /// @nmisafe{maybe}
  [[nodiscard]] bool try_push_all(span<const value_type> items) {
    WriteTransaction tx = BeginWrite();
    if (items.size() > tx.remaining_capacity()) {
      return false;
    }
    tx.push_some(items);
    return true;
  }

  /// Attempts to push all elements of `items` into the queue by move in a
  /// single all-or-nothing batch.
  ///
  /// If fewer than `items.size()` slots are available, no elements are moved
  /// from `items` and `false` is returned.
  ///
  /// @note Safe to call concurrently with consumer operations from the single
  /// producer context.
  ///
  /// @param[in,out] items Span of elements to move into the queue.
  /// @returns `true` if all elements in `items` were pushed, or `false` if
  ///     `remaining_capacity() < items.size()`.
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{maybe}
  /// @nmisafe{maybe}
  [[nodiscard]] bool try_move_push_all(span<value_type> items) {
    WriteTransaction tx = BeginWrite();
    if (items.size() > tx.remaining_capacity()) {
      return false;
    }
    tx.move_push_some(items);
    return true;
  }

  /// Pops up to `dest.size()` elements from the front of the queue and assigns
  /// them into `dest` (by move if move-assignable, otherwise by copy), updating
  /// `head_` with a single `std::memory_order_release` store.
  ///
  /// @note Safe to call concurrently with producer operations from the single
  /// consumer context.
  ///
  /// @param[out] dest Destination span to receive the popped elements.
  /// @returns The number of elements popped into `dest`
  ///     (`0 <= count <= dest.size()`).
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{maybe}
  /// @nmisafe{maybe}
  size_type pop_some(span<value_type> dest) {
    ReadTransaction rx = BeginRead();
    return rx.pop_some(dest);
  }

  /// Removes and destroys up to `count` elements from the front of the queue,
  /// updating `head_` with a single `std::memory_order_release` store.
  ///
  /// @note Safe to call concurrently with producer operations from the single
  /// consumer context.
  ///
  /// @param[in] count Maximum number of elements to remove.
  /// @returns The number of elements actually removed (`0 <= removed <=
  /// count`).
  ///
  /// @threadsafe{maybe}
  /// @isrsafe{maybe}
  /// @nmisafe{maybe}
  size_type pop_n(size_type count) noexcept {
    ReadTransaction rx = BeginRead();
    const size_type to_pop = std::min(count, rx.remaining());
    rx.consume(to_pop);
    return to_pop;
  }

 private:
  /// @cond
  template <typename, size_t, typename>
  friend class FixedLockFreeSpscQueue;
  /// @endcond

  using Aligned = containers::internal::Aligned<value_type>;

  explicit constexpr LockFreeSpscQueue(Aligned buffer) noexcept
      : buffer_(buffer.data()),
        capacity_(static_cast<size_type>(buffer.capacity())),
        head_(0),
        tail_(0) {
    PW_ASSERT(buffer.capacity() <= kMaxCapacity);
  }

  void DestroyAt(size_type buffer_index, size_type count) noexcept {
    if constexpr (!std::is_trivially_destructible_v<value_type>) {
      const size_type first =
          std::min(count, static_cast<size_type>(capacity_ - buffer_index));
      const size_type second = static_cast<size_type>(count - first);
      std::destroy_n(SlotPtr(buffer_index), first);
      if (second > 0) {
        std::destroy_n(SlotPtr(0), second);
      }
    }
  }

  void CopyIntoAt(size_type buffer_index, size_type count, const_pointer src) {
    const size_type first =
        std::min(count, static_cast<size_type>(capacity_ - buffer_index));
    const size_type second = static_cast<size_type>(count - first);
    std::uninitialized_copy_n(
        src, first, reinterpret_cast<pointer>(SlotBytes(buffer_index)));
    if (second > 0) {
      std::uninitialized_copy_n(
          src + first, second, reinterpret_cast<pointer>(SlotBytes(0)));
    }
  }

  void MoveIntoAt(size_type buffer_index, size_type count, pointer src) {
    const size_type first =
        std::min(count, static_cast<size_type>(capacity_ - buffer_index));
    const size_type second = static_cast<size_type>(count - first);
    std::uninitialized_move_n(
        src, first, reinterpret_cast<pointer>(SlotBytes(buffer_index)));
    if (second > 0) {
      std::uninitialized_move_n(
          src + first, second, reinterpret_cast<pointer>(SlotBytes(0)));
    }
  }

  void PopIntoAt(size_type buffer_index, size_type count, pointer dest) {
    const size_type first =
        std::min(count, static_cast<size_type>(capacity_ - buffer_index));
    const size_type second = static_cast<size_type>(count - first);
    pointer first_ptr = SlotPtr(buffer_index);
    if constexpr (std::is_move_assignable_v<value_type>) {
      std::move(first_ptr, first_ptr + first, dest);
    } else {
      std::copy_n(first_ptr, first, dest);
    }
    std::destroy_n(first_ptr, first);
    if (second > 0) {
      pointer second_ptr = SlotPtr(0);
      if constexpr (std::is_move_assignable_v<value_type>) {
        std::move(second_ptr, second_ptr + second, dest + first);
      } else {
        std::copy_n(second_ptr, second, dest + first);
      }
      std::destroy_n(second_ptr, second);
    }
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

  constexpr size_type RetreatIndex(size_type virtual_index,
                                   size_type count) const noexcept {
    if (virtual_index >= count) {
      return static_cast<size_type>(virtual_index - count);
    }
    const size_type wrap = static_cast<size_type>(size_type{2} * capacity_);
    return static_cast<size_type>(wrap - count + virtual_index);
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

/// @endsubmodule

}  // namespace pw
