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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <type_traits>
#include <utility>

#include "lib/stdcompat/utility.h"
#include "pw_allocator/allocator.h"
#include "pw_allocator/unique_ptr.h"
#include "pw_assert/assert.h"
#include "pw_containers/internal/generic_deque.h"
#include "pw_containers/storage.h"
#include "pw_polyfill/language_feature_macros.h"
#include "pw_span/span.h"

namespace pw {

/// @submodule{pw_containers,queues}

/// Double-ended queue, similar to `std::deque`, but uses a fixed storage
/// buffer. The buffer is managed externally and may be statically or
/// dynamically allocated.
///
/// `Deque` is not movable. This avoids confusion about which buffer the deque
/// is using, since the buffer is unowned. `FixedDeque`, which derives from
/// `Deque`, optionally owns its buffer and supports moving.
///
/// To instantiate a `Deque` with an integrated buffer, see `FixedDeque`.
/// `FixedDeque` supports either internal static or external static or dynamic
/// allocation of its buffer.
///
/// All `Deque` constructors and operators are infallible. Other functions
/// assert if operations cannot succeed.
///
/// @tparam T What is stored in the deque.
/// @tparam SizeType How to store sizes. A smaller size type limits the maximum
///     number of items, but requires slightly less memory.
template <typename T, typename SizeType = uint16_t>
class Deque : public containers::internal::
                  GenericDeque<Deque<T, SizeType>, T, SizeType> {
 private:
  using Base =
      containers::internal::GenericDeque<Deque<T, SizeType>, T, SizeType>;

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

  /// Constructs an empty `Deque` using the provided buffer for storage.
  ///
  /// @param unaligned_buffer Storage for the deque. If the buffer is not
  ///     aligned as as `value_type`, an aligned subset of the buffer will be
  ///     used. If the buffer is too small to hold at least one `value_type`,
  ///     the queue's capacity will be 0.
  explicit constexpr Deque(span<std::byte> unaligned_buffer) noexcept
      : Deque(Aligned::Align(unaligned_buffer)) {}

  /// Constructs an empty `Deque` using a `pw::containers::Storage` buffer.
  /// This constructor avoids run time alignment checks.
  template <size_t kAlignment, size_t kSizeBytes>
  explicit constexpr Deque(
      containers::Storage<kAlignment, kSizeBytes>& buffer) noexcept
      : Deque(Aligned(buffer.data(), buffer.size())) {
    static_assert(kAlignment >= alignof(value_type));
  }

  /// Copying is not supported since it can fail.
  Deque(const Deque&) = delete;
  Deque& operator=(const Deque&) = delete;

  /// Move is not supported to avoid confusion about deque/buffer pairings.
  Deque(Deque&&) = delete;
  Deque& operator=(Deque&&) = delete;

  PW_CONSTEXPR_CPP20 ~Deque() { Base::DestroyAll(); }

  constexpr size_type max_size() const noexcept { return Base::capacity(); }

 private:
  friend Base;

  template <typename, typename>
  friend class Queue;

  template <typename, size_t, typename>
  friend class FixedDeque;

  static constexpr bool kFixedCapacity = true;  // uses a fixed buffer

  // Span that is known to be aligned as T.
  class Aligned {
   public:
    // Construct from an UNALIGNED span. Adjusts alignment if necessary.
    static constexpr Aligned Align(span<std::byte> unaligned_buffer) {
      Aligned buffer(unaligned_buffer.data(), unaligned_buffer.size());

      if constexpr (alignof(value_type) > alignof(std::byte)) {
        void* data_void = buffer.data_;
        buffer.data_ = static_cast<std::byte*>(std::align(
            alignof(value_type), sizeof(value_type), data_void, buffer.size_));
        if (buffer.data_ == nullptr) {
          buffer.size_ = 0;  // cannot fit any items
        }
      }

      return buffer;
    }

    // Asserts that a buffer is aligned at least as T.
    static constexpr Aligned Assert(std::byte* data, size_t size) {
      void* buffer_start = data;
      PW_ASSERT(reinterpret_cast<uintptr_t>(buffer_start) % alignof(T) == 0);
      return Aligned(data, size);
    }

    constexpr Aligned(std::byte* aligned_data, size_t size_bytes)
        : data_(aligned_data), size_(size_bytes) {}

    constexpr std::byte* data() const { return data_; }
    constexpr size_t capacity() const { return size_ / sizeof(value_type); }

   private:
    std::byte* data_;
    size_t size_;
  };

  explicit constexpr Deque(Aligned buffer) noexcept
      : Base(static_cast<size_type>(buffer.capacity())),
        buffer_(buffer.data()) {}

  constexpr void MoveBufferFrom(Deque& other) noexcept {
    Base::DestroyAll();
    buffer_ = cpp20::exchange(other.buffer_, nullptr);
    Base::MoveAssignIndices(other);
  }

  void MoveItemsFrom(Deque& other) {
    this->assign(std::make_move_iterator(other.begin()),
                 std::make_move_iterator(other.end()));
    other.clear();
  }

  void SwapBufferWith(Deque& other) noexcept {
    Base::SwapIndices(other);
    std::swap(buffer_, other.buffer_);
  }

  void SwapValuesWith(Deque& other);

  pointer data() { return std::launder(reinterpret_cast<pointer>(buffer_)); }
  const_pointer data() const {
    return std::launder(reinterpret_cast<const_pointer>(buffer_));
  }

  // Pointer to the buffer used for the deque. This may differ from the pointer
  // passed into the constructor if it had to be shifted for alignment.
  std::byte* buffer_;
};

/// `FixedDeque` is a `Deque` that owns its statically allocated buffer.
///
/// This `FixedDeque` statically allocates its storage buffer internally.
/// Similar to `InlineDeque`, the capacity is specified as a template parameter.
/// Unlike `InlineDeque`, `FixedDeque` is usable as a generic `Deque`.
///
/// If the capacity is omitted (or set to `containers::kExternalStorage`), the
/// @ref FixedDeque<T, containers::kExternalStorage, S>
/// "external storage specialization" is used. Whether statically or dynamically
/// allocated, the capacity never changes.
///
/// @tparam T What is stored in the deque.
/// @tparam S Size type. A smaller size type limits the maximum number of items,
///     but requires slightly less memory.
template <typename T,
          size_t kInlineCapacity = containers::kExternalStorage,
          typename S = typename Deque<T>::size_type>
class FixedDeque final : private containers::StorageBaseFor<T, kInlineCapacity>,
                         public Deque<T, S> {
 public:
  /// Constructs an empty `FixedDeque` with internal, statically allocated
  /// storage.
  constexpr FixedDeque()
      : containers::StorageBaseFor<T, kInlineCapacity>{},
        Deque<T, S>(this->storage()) {}

  FixedDeque(const FixedDeque&) = delete;
  FixedDeque& operator=(const FixedDeque&) = delete;

  constexpr FixedDeque(FixedDeque&& other) noexcept : FixedDeque() {
    this->MoveItemsFrom(other);
  }

  constexpr FixedDeque& operator=(FixedDeque&& other) noexcept {
    this->clear();
    this->MoveItemsFrom(other);
    return *this;
  }

  /// `FixedDeque` supports moves that are guaranteed to succeed (moving into
  /// equal or greater capacity or between deques with external storage).
  template <size_t kOtherCapacity,
            typename = std::enable_if_t<kOtherCapacity <= kInlineCapacity>>
  FixedDeque(FixedDeque<T, kOtherCapacity>&& other) noexcept : FixedDeque() {
    this->MoveItemsFrom(other);
  }

  template <size_t kOtherCapacity,
            typename = std::enable_if_t<kOtherCapacity <= kInlineCapacity>>
  FixedDeque& operator=(FixedDeque<T, kOtherCapacity>&& other) noexcept {
    this->clear();
    this->MoveItemsFrom(other);
    return *this;
  }

  /// Swaps the contents of this `FixedDeque` with another, in O(n) time.
  /// Crashes if one deque's size is greater than the other's capacity.
  template <size_t kOtherCapacity>
  void swap(FixedDeque<T, kOtherCapacity, S>& other) {
    this->SwapValuesWith(other);
  }

  /// Returns `nullptr`; a `FixedDeque` with static storage never allocates.
  constexpr Deallocator* deallocator() const { return nullptr; }

 private:
  static_assert(
      kInlineCapacity <= std::numeric_limits<S>::max(),
      "The capacity is too large for the size_type; use a larger size_type");
};

/// @ref FixedDeque<T, containers::kExternalStorage, S> "FixedDeque<T>"
/// is a `Deque` that uses external storage. The storage may be unowned or
/// owned and dynamically allocated. The buffer is allocated during or before
/// construction, and no further allocations occur: the buffer never grows or
/// shrinks.
///
/// To statically allocate the buffer, specify the capacity in the `FixedDeque`
/// declaration or allocate it manually and pass it to the constructor.
template <typename T, typename S>
class FixedDeque<T, containers::kExternalStorage, S> final
    : public Deque<T, S> {
 public:
  /// Allocates a buffer large enough to hold `capacity` items and uses it for a
  /// `FixedDeque`'s buffer. Crashes if unable to allocate space for `capacity`
  /// items.
  ///
  /// The `FixedDeque` owns the buffer and frees it upon destruction.
  static FixedDeque Allocate(Allocator& allocator, const S capacity) {
    FixedDeque deque = TryAllocate(allocator, capacity);
    PW_ASSERT(deque.capacity() == capacity);
    return deque;
  }

  /// Attempts to allocate a `T`-aligned buffer large enough to hold `capacity`
  /// items and use it for a `FixedDeque`'s buffer. If unable to allocate space
  /// for `capacity` items, returns a `FixedDeque` with a capacity of 0.
  ///
  /// If allocation succeeds, the `FixedDeque` owns the buffer and frees it upon
  /// destruction.
  static FixedDeque TryAllocate(Allocator& allocator, S capacity) {
    const allocator::Layout layout = allocator::Layout::Of<T[]>(capacity);
    std::byte* array = static_cast<std::byte*>(allocator.Allocate(layout));
    if (array == nullptr) {
      return FixedDeque(Aligned(array, 0u), nullptr);
    }
    return FixedDeque(Aligned(array, layout.size()), &allocator);
  }

  /// Constructs an empty `FixedDeque` that uses the provided `pw::UniquePtr`
  /// for its storage. Takes ownership of the `pw::UniquePtr`.
  ///
  /// @pre `storage` MUST be aligned at least as `value_type`.
  static FixedDeque WithStorage(UniquePtr<std::byte[]>&& storage) {
    const size_t size = storage.size();
    Deallocator* deallocator = storage.deallocator();
    return FixedDeque(Aligned::Assert(storage.Release(), size), deallocator);
  }

  /// Constructs an `FixedDeque` from a buffer. The buffer is unowned and is not
  /// freed when the `FixedDeque` is destroyed.
  ///
  /// @param unaligned_buffer Storage for the deque. If the buffer is not
  ///     aligned as as `value_type`, an aligned subset of the buffer will be
  ///     used. If the buffer is too small to hold at least one `value_type`,
  ///     the queue's capacity will be 0.
  explicit constexpr FixedDeque(span<std::byte> unaligned_buffer) noexcept
      : Deque<T, S>(unaligned_buffer), deallocator_(nullptr) {}

  /// Constructs an empty `FixedDeque` using a `pw::containers::Storage` buffer.
  /// This constructor avoids run time alignment checks.
  ///
  /// The buffer is unowned and is not freed when the `FixedDeque` is destroyed.
  template <size_t kAlignment, size_t kSizeBytes>
  explicit constexpr FixedDeque(
      containers::Storage<kAlignment, kSizeBytes>& buffer) noexcept
      : Deque<T, S>(buffer), deallocator_(nullptr) {}

  /// Copying is not supported since it can fail.
  FixedDeque(const FixedDeque&) = delete;
  FixedDeque& operator=(const FixedDeque&) = delete;

  /// Move construction/assignment from other `FixedDeque`s with external
  /// buffers is supported and infallible. To move items from a statically
  /// allocated `FixedDeque`, use `assign()` with `std::make_move_iterator`, or
  /// move items individually.
  constexpr FixedDeque(FixedDeque&& other) noexcept
      : Deque<T, S>({}),
        deallocator_(cpp20::exchange(other.deallocator_, nullptr)) {
    this->MoveBufferFrom(other);
  }

  constexpr FixedDeque& operator=(FixedDeque&& other) noexcept {
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

  ~FixedDeque() {
    // Clear the deque so that the base `Deque` destructor will not need to
    // access its buffer, which will be freed.
    this->clear();
    if (deallocator_ != nullptr) {
      deallocator_->Deallocate(this->data());
    }
  }

  /// Swaps this `FixedDeque` with another externally allocated `FixedDeque` in
  /// O(1) time. No allocations occur, and individual items are not moved.
  void swap(FixedDeque& other) noexcept {
    this->SwapBufferWith(other);
    std::swap(deallocator_, other.deallocator_);
  }

  /// Swaps the contents of this `FixedDeque` with an internal-storage
  /// `FixedDeque` in O(n) time. No allocations occur, but items are moved
  /// individually. Crashes if the size of one deque exceeds the capacity of the
  /// other.
  template <size_t kInlineCapacity>
  void swap(FixedDeque<T, kInlineCapacity, S>& other) {
    other.swap(*this);  // Use the swap impl for statically allocated deques.
  }

  /// Returns the deallocator that will be used to free this `FixedDeque`'s
  /// storage. Returns `nullptr` if the storage is unowned.
  constexpr Deallocator* deallocator() const { return deallocator_; }

 private:
  using typename Deque<T, S>::Aligned;

  constexpr FixedDeque(Aligned buffer, Deallocator* deallocator)
      : Deque<T, S>(buffer), deallocator_(deallocator) {}

  Deallocator* deallocator_;
};

template <typename T, typename SizeType>
void Deque<T, SizeType>::SwapValuesWith(Deque& other) {
  Deque* smaller;
  Deque* larger;

  if (this->size() < other.size()) {
    smaller = this;
    larger = &other;
  } else {
    smaller = &other;
    larger = this;
  }

  const SizeType smaller_size = smaller->size();
  for (auto it =
           std::swap_ranges(smaller->begin(), smaller->end(), larger->begin());
       it != larger->end();
       ++it) {
    smaller->push_back(std::move(*it));
  }

  while (larger->size() > smaller_size) {
    larger->pop_back();
  }
}

}  // namespace pw
