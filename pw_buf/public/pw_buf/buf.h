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
#include <limits>
#include <type_traits>
#include <utility>

#include "lib/stdcompat/utility.h"
#include "pw_allocator/allocator.h"
#include "pw_allocator/unique_ptr.h"
#include "pw_assert/assert.h"
#include "pw_bytes/span.h"
#include "pw_containers/ptr_iterator.h"

namespace pw {
namespace multibuf::v2::internal {

class GenericMultiBuf;

}  // namespace multibuf::v2::internal

/// @module{pw_buf}

/// Represents a mutable view of a contiguous block of bytes.
///
/// An `empty()` `Buf` has no bytes (`size() == 0`). A null `Buf` has no bytes,
/// its `base_size()` is 0, and its `base()` and `data()` are `nullptr`. A `Buf`
/// is null when default-constructed, moved-from, reset, or when allocation
/// fails.
///
/// An empty `Buf` is not necessarily null (for example, if it has been sliced
/// or truncated to length 0), in which case `base()` and `data()` return
/// non-null pointers and `base_size()` returns the base allocation size.
///
/// Nullness can be checked via comparison to `nullptr` (`buf == nullptr`,
/// `buf != nullptr`). `operator bool` is not provided to prevent confusion
/// between nullness and emptiness.
class Buf {
 public:
  using value_type = std::byte;
  using size_type = size_t;
  using difference_type = ptrdiff_t;
  using pointer = std::byte*;
  using reference = std::byte&;
  using const_pointer = const std::byte*;
  using const_reference = const std::byte&;
  using iterator = containers::PtrIterator<Buf>;
  using const_iterator = containers::ConstPtrIterator<Buf>;

  /// Constructs a null `Buf`.
  constexpr Buf()
      : data_(nullptr),
        size_(0),
        prefix_(0),
        suffix_(0),
        deallocator_(nullptr) {}

  /// Constructs a null `Buf`.
  constexpr Buf(std::nullptr_t) noexcept : Buf() {}

  /// Integers do not convert to `Buf`. Without this, the literal `0` would
  /// convert through the `nullptr_t` constructor, so passing `0` where a
  /// `Buf` is expected would silently produce a null buffer.
  template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
  Buf(T) = delete;

  /// Move constructor.
  ///
  /// The moved-from `Buf` is left null.
  constexpr Buf(Buf&& other) noexcept
  // MoveFrom initializes all members, so skip initializing them twice to reduce
  // code size. This is not supported in C++17, however.
#if __cpp_constexpr < 201907L
      : Buf()
#endif  // __cpp_constexpr < 201907L
  {
    MoveFrom(std::move(other));
  }

  /// Constructs an owned `Buf` from a `UniquePtr`.
  ///
  /// If `buffer` is empty/null, constructs a null `Buf`.
  explicit Buf(UniquePtr<std::byte[]>&& buffer)
      : Buf(buffer.get(), 0, buffer.size(), buffer.deallocator()) {
    buffer.Release();
  }

  /// Constructs an owned `Buf` using the remaining length starting from
  /// `offset`.
  explicit Buf(UniquePtr<std::byte[]>&& buffer, size_t offset);

  /// Constructs an owned `Buf` from a `UniquePtr`, offset, and size.
  explicit Buf(UniquePtr<std::byte[]>&& buffer, size_t offset, size_t size);

  /// Constructs an owned `Buf` from a raw buffer, size, and deallocator.
  explicit constexpr Buf(std::byte* allocation,
                         size_t size,
                         Deallocator& deallocator)
      : Buf(allocation, 0, size, &deallocator) {}

  /// Constructs an owned `Buf` from a raw buffer, offset, size, and
  /// deallocator.
  explicit constexpr Buf(std::byte* allocation,
                         size_t offset,
                         size_t size,
                         Deallocator& deallocator)
      : Buf(allocation, offset, size, &deallocator) {}

  /// Creates an unowned `Buf` from a `ByteSpan`, offset, and size.
  [[nodiscard]] static Buf Unowned(ByteSpan span, size_t offset, size_t size);

  /// Creates an unowned `Buf` using the remaining length starting from
  /// `offset`.
  [[nodiscard]] static Buf Unowned(ByteSpan span, size_t offset = 0);

  /// Creates an unowned `Buf` from a pointer, offset, and size.
  [[nodiscard]] static Buf Unowned(std::byte* base,
                                   size_t offset,
                                   size_t size) {
    return Buf(base, offset, size, nullptr);
  }

  /// Creates an unowned `Buf` from a pointer and size.
  [[nodiscard]] static Buf Unowned(std::byte* base, size_t size) {
    return Buf(base, 0, size, nullptr);
  }

  /// Allocates a new owned `Buf` of the specified size.
  ///
  /// Asserts if allocation fails.
  [[nodiscard]] static Buf Allocate(Allocator& allocator, size_t size_bytes);

  /// Allocates a new owned `Buf` of the specified size and fills it with
  /// `value`.
  ///
  /// Asserts if allocation fails.
  [[nodiscard]] static Buf AllocateFill(Allocator& allocator,
                                        size_t size_bytes,
                                        std::byte value = std::byte{0});

  /// Allocates a new owned `Buf` the same size as `data` and copies the
  /// contents of `data` into it.
  ///
  /// Asserts if allocation fails.
  [[nodiscard]] static Buf AllocateCopy(Allocator& allocator,
                                        ConstByteSpan data) {
    return AllocateCopy(allocator, data.data(), data.size());
  }

  /// Allocates a new owned `Buf` of the specified size and copies from `data`
  /// into it.
  ///
  /// Asserts if allocation fails.
  [[nodiscard]] static Buf AllocateCopy(Allocator& allocator,
                                        const std::byte* data,
                                        size_t size_bytes);

  /// Allocates a new owned `Buf` of the specified size.
  ///
  /// Returns a null `Buf` if allocation fails.
  [[nodiscard]] static Buf TryAllocate(Allocator& allocator, size_t size_bytes);

  /// Allocates a new owned `Buf` of the specified size and fills it with
  /// `value`.
  ///
  /// Returns a null `Buf` if allocation fails.
  [[nodiscard]] static Buf TryAllocateFill(Allocator& allocator,
                                           size_t size_bytes,
                                           std::byte value = std::byte{0});

  /// Allocates a new owned `Buf` the same size as `data` and copies the
  /// contents of `data` into it.
  ///
  /// Returns a null `Buf` if allocation fails.
  [[nodiscard]] static Buf TryAllocateCopy(Allocator& allocator,
                                           ConstByteSpan data) {
    return TryAllocateCopy(allocator, data.data(), data.size());
  }

  /// Allocates a new owned `Buf` of the specified size and copies from `data`
  /// into it.
  ///
  /// Returns a null `Buf` if allocation fails.
  [[nodiscard]] static Buf TryAllocateCopy(Allocator& allocator,
                                           const std::byte* data,
                                           size_t size_bytes);

  Buf(const Buf&) = delete;

  /// Destructor. Releases the owned memory (if any) back to the allocator.
  ~Buf() { reset(); }

  /// Move assignment operator.
  ///
  /// The moved-from `Buf` is left null.
  Buf& operator=(Buf&& other) noexcept;

  Buf& operator=(const Buf&) = delete;

  /// Frees the owned memory (if any) and sets the buffer to null. Same as
  /// `reset()`.
  Buf& operator=(std::nullptr_t) noexcept {
    reset();
    return *this;
  }

  /// Integers cannot be assigned to a `Buf`, not even the literal `0`.
  template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
  Buf& operator=(T) = delete;

  /// `operator bool` is not provided to prevent confusion between nullness and
  /// emptiness. Use `buf == nullptr` to check for null, or `buf.empty()` for
  /// size 0.
  explicit operator bool() const = delete;

  [[nodiscard]] friend constexpr bool operator==(const Buf& lhs,
                                                 std::nullptr_t) noexcept {
    return lhs.data() == nullptr;
  }
  [[nodiscard]] friend constexpr bool operator==(std::nullptr_t,
                                                 const Buf& rhs) noexcept {
    return rhs.data() == nullptr;
  }
  [[nodiscard]] friend constexpr bool operator!=(const Buf& lhs,
                                                 std::nullptr_t) noexcept {
    return lhs.data() != nullptr;
  }
  [[nodiscard]] friend constexpr bool operator!=(std::nullptr_t,
                                                 const Buf& rhs) noexcept {
    return rhs.data() != nullptr;
  }

  /// Accesses the byte at the specified index as read-only.
  const_reference operator[](size_t index) const {
    PW_DASSERT(index < size_);
    return data_[index];
  }

  /// Accesses the byte at the specified index as mutable.
  reference operator[](size_t index) {
    PW_DASSERT(index < size_);
    return data_[index];
  }

  /// Returns a pointer to the read-only data, or `nullptr` if the `Buf` is
  /// null.
  ///
  /// @note An empty `Buf` that is not null (e.g. sliced or truncated to length
  /// 0) returns a non-null pointer. `data()` is only `nullptr` when the buffer
  /// is null.
  constexpr const_pointer data() const { return data_; }

  /// Returns a pointer to the mutable data, or `nullptr` if the `Buf` is null.
  ///
  /// @note An empty `Buf` that is not null (e.g. sliced or truncated to length
  /// 0) returns a non-null pointer. `data()` is only `nullptr` when the buffer
  /// is null.
  constexpr pointer data() { return data_; }

  /// Returns the size of the buffer.
  constexpr size_t size() const { return size_; }

  /// Returns true if the buffer is empty (has no bytes).
  ///
  /// `empty()` returns true for both null buffers and non-null buffers of size
  /// zero (e.g. after being sliced or truncated to length 0).
  [[nodiscard]] constexpr bool empty() const { return size_ == 0; }

  /// Returns a pointer to the base address of the original, underlying memory
  /// buffer, or `nullptr` if the `Buf` is null.
  [[nodiscard]] constexpr const std::byte* base() const {
    return data_ - prefix_;
  }

  /// Returns the size of the original, underlying memory buffer, or 0 if the
  /// `Buf` is null.
  [[nodiscard]] constexpr size_t base_size() const {
    return prefix_ + size_ + suffix_;
  }

  /// Returns the number of bytes between `base()` and `data()`. These bytes
  /// were excluded by slicing and may be recovered with `ReclaimPrefix()`.
  [[nodiscard]] constexpr size_t prefix_size() const { return prefix_; }

  /// Returns the number of bytes after the end of the data in the underlying
  /// memory buffer. These bytes were excluded by slicing or truncating and may
  /// be recovered with `ReclaimSuffix()`.
  [[nodiscard]] constexpr size_t suffix_size() const { return suffix_; }

  /// Returns true if the `Buf` views only part of its underlying memory buffer,
  /// i.e. it was sliced or truncated, or constructed with an offset.
  [[nodiscard]] constexpr bool sliced() const {
    return prefix_ != 0 || suffix_ != 0;
  }

  /// Returns a pointer to the deallocator if this buffer owns the underlying
  /// memory allocation, or `nullptr` if the buffer is unowned or null.
  [[nodiscard]] constexpr Deallocator* deallocator() const {
    return deallocator_;
  }

  /// Returns a read-only iterator pointing to the beginning of the data.
  const_iterator begin() const { return const_iterator(data_); }

  /// Returns a read-only iterator pointing past the end of the data.
  const_iterator end() const { return const_iterator(data_ + size_); }

  /// Returns a read-only iterator pointing to the beginning of the data.
  const_iterator cbegin() const { return const_iterator(data_); }

  /// Returns a read-only iterator pointing past the end of the data.
  const_iterator cend() const { return const_iterator(data_ + size_); }

  /// Returns a mutable iterator pointing to the beginning of the data.
  iterator begin() { return iterator(data_); }

  /// Returns a mutable iterator pointing past the end of the data.
  iterator end() { return iterator(data_ + size_); }

  /// Frees the owned memory (if any) and sets the buffer to null.
  void reset();

  /// Exchanges the contents of this `Buf` with `other`.
  void swap(Buf& other) noexcept {
    std::swap(data_, other.data_);
    std::swap(size_, other.size_);
    std::swap(prefix_, other.prefix_);
    std::swap(suffix_, other.suffix_);
    std::swap(deallocator_, other.deallocator_);
  }

  /// Exchanges the contents of two `Buf`s.
  friend void swap(Buf& lhs, Buf& rhs) noexcept { lhs.swap(rhs); }

 private:
  using OffsetType =
      std::conditional_t<sizeof(size_t) >= 8, uint32_t, uint16_t>;

  constexpr explicit Buf(std::byte* base,
                         size_t offset,
                         size_t size,
                         Deallocator* deallocator)
      : data_(base + offset),
        size_(size),
        prefix_(static_cast<OffsetType>(offset)),
        suffix_(0),
        // Never store an allocator for a null Buf.
        deallocator_(base != nullptr ? deallocator : nullptr) {
    PW_ASSERT(base != nullptr || (offset == 0 && size == 0));
    PW_ASSERT(offset <= std::numeric_limits<OffsetType>::max());
  }

  friend Buf Slice(Buf&& buf, size_t offset, size_t length);
  friend Buf Reclaim(Buf&& buf, size_t prefix_count, size_t suffix_count);
  friend UniquePtr<std::byte[]> TakeAllocation(Buf&& buf);

  constexpr void MoveFrom(Buf&& other) {
    data_ = cpp20::exchange(other.data_, nullptr);
    size_ = cpp20::exchange(other.size_, 0u);
    prefix_ = cpp20::exchange(other.prefix_, OffsetType{0});
    suffix_ = cpp20::exchange(other.suffix_, OffsetType{0});
    deallocator_ = cpp20::exchange(other.deallocator_, nullptr);
  }

  constexpr void Clear() {
    data_ = nullptr;
    size_ = 0;
    prefix_ = 0;
    suffix_ = 0;
    deallocator_ = nullptr;
  }

  [[nodiscard]] constexpr std::byte* mutable_base() { return data_ - prefix_; }

  std::byte* data_;
  size_t size_;
  OffsetType prefix_;
  OffsetType suffix_;
  Deallocator* deallocator_;
};

/// Slices a mutable `Buf` by shifting its start address and setting its size.
[[nodiscard]] Buf Slice(Buf&& buf, size_t offset, size_t length);

/// Slices a mutable `Buf` by shifting its start address to the end.
///
/// @pre `offset` cannot exceed `buf.size()`.
[[nodiscard]] inline Buf Slice(Buf&& buf, size_t offset) {
  return Slice(std::move(buf), offset, buf.size() - offset);
}

/// Truncates a mutable `Buf` to a smaller size.
[[nodiscard]] inline Buf Truncate(Buf&& buf, size_t length) {
  return Slice(std::move(buf), 0, length);
}

/// Reclaims `prefix_count` bytes at the beginning and `suffix_count` bytes at
/// the end of the buffer.
///
/// @pre `prefix_count` cannot exceed `buf.prefix_size()`, and `suffix_count`
/// cannot exceed `buf.suffix_size()`.
[[nodiscard]] Buf Reclaim(Buf&& buf, size_t prefix_count, size_t suffix_count);

/// Reclaims `count` prefix bytes at the beginning of the buffer.
///
/// @pre `count` cannot exceed `buf.prefix_size()`.
[[nodiscard]] inline Buf ReclaimPrefix(Buf&& buf, size_t count) {
  return Reclaim(std::move(buf), count, 0);
}

/// Reclaims `count` suffix bytes at the end of the buffer.
///
/// @pre `count` cannot exceed `buf.suffix_size()`.
[[nodiscard]] inline Buf ReclaimSuffix(Buf&& buf, size_t count) {
  return Reclaim(std::move(buf), 0, count);
}

/// Transfers ownership of a `Buf`'s underlying memory allocation to a
/// `UniquePtr`. The `Buf` is null after this call.
///
/// The returned `UniquePtr` spans the entire base allocation (`base()` and
/// `base_size()`), not only the bytes in the current view. To preserve the
/// view, record `prefix_size()` and `size()` before calling `TakeAllocation()`,
/// then pass them to `Buf(UniquePtr<std::byte[]>&&, size_t, size_t)`.
///
/// To take sole responsibility for the memory, call `Release()` on the
/// returned `UniquePtr` after saving its `deallocator()` and `size()`.
///
/// If the `Buf` is null, returns a null `UniquePtr`.
///
/// @pre `buf` must be owned (`buf.deallocator() != nullptr`) or null. An
/// unowned `Buf` has no allocation to take.
[[nodiscard]] UniquePtr<std::byte[]> TakeAllocation(Buf&& buf);

inline Buf::Buf(UniquePtr<std::byte[]>&& buffer, size_t offset)
    : Buf(Slice(Buf(std::move(buffer)), offset)) {}

inline Buf::Buf(UniquePtr<std::byte[]>&& buffer, size_t offset, size_t size)
    : Buf(Slice(Buf(std::move(buffer)), offset, size)) {}

inline Buf Buf::Unowned(ByteSpan span, size_t offset, size_t size) {
  return Slice(Unowned(span.data(), span.size()), offset, size);
}

inline Buf Buf::Unowned(ByteSpan span, size_t offset) {
  return Slice(Unowned(span.data(), span.size()), offset);
}

/// Represents a read-only view of a contiguous block of bytes.
///
/// `ConstBuf` is intended for passing ownership of read-only data, or providing
/// an unowned view, optionally slicing the view between layers. Unlike `Buf`,
/// `ConstBuf` does not support `Reclaim()` or expose its base pointer. Once a
/// `ConstBuf` is sliced or truncated, the excluded bytes cannot be recovered.
///
/// An `empty()` `ConstBuf` has no bytes (`size() == 0`). A null `ConstBuf` has
/// no bytes, and its `data()` pointer is `nullptr`. A `ConstBuf` is null when
/// default-constructed, moved-from, or reset.
///
/// An empty `ConstBuf` is not necessarily null (for example, if it has been
/// sliced or truncated to length 0), in which case `data()` returns a non-null
/// pointer.
///
/// Nullness can be checked via comparison to `nullptr` (`buf == nullptr`,
/// `buf != nullptr`). `operator bool` is not provided to prevent confusion
/// between nullness and emptiness.
class ConstBuf {
 public:
  using value_type = std::byte;
  using size_type = size_t;
  using difference_type = ptrdiff_t;
  using pointer = const std::byte*;
  using reference = const std::byte&;
  using iterator = Buf::const_iterator;
  using const_pointer = const std::byte*;
  using const_reference = const std::byte&;
  using const_iterator = Buf::const_iterator;

  /// Constructs a null `ConstBuf`.
  constexpr ConstBuf() = default;

  /// Constructs a null `ConstBuf`.
  constexpr ConstBuf(std::nullptr_t) noexcept : ConstBuf() {}

  /// Integers do not convert to `ConstBuf`. Without this, the literal `0` would
  /// convert through the `nullptr_t` constructor, so passing `0` where a
  /// `ConstBuf` is expected would silently produce a null buffer.
  template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
  ConstBuf(T) = delete;

  /// Move constructor.
  ///
  /// The moved-from `ConstBuf` is left null.
  constexpr ConstBuf(ConstBuf&& other) noexcept : buf_(std::move(other.buf_)) {}

  /// Move-constructs a `ConstBuf` from a mutable `Buf` (move conversion).
  ///
  /// The moved-from `Buf` is left null.
  constexpr ConstBuf(Buf&& other) noexcept : buf_(std::move(other)) {}

  /// Creates an unowned `ConstBuf` from a `ConstByteSpan`.
  [[nodiscard]] static ConstBuf Unowned(ConstByteSpan span) {
    return Unowned(span.data(), span.size());
  }

  /// Creates an unowned `ConstBuf` from a pointer and size in bytes.
  [[nodiscard]] static ConstBuf Unowned(const std::byte* data,
                                        size_t size_bytes) {
    // The underlying Buf is never exposed mutably, so casting away const is
    // safe.
    return Buf::Unowned(const_cast<std::byte*>(data), size_bytes);
  }

  ConstBuf(const ConstBuf&) = delete;

  /// Destructor. Releases the owned memory (if any) back to the allocator.
  ~ConstBuf() = default;

  /// Move assignment operator.
  ///
  /// The moved-from `ConstBuf` is left null.
  ConstBuf& operator=(ConstBuf&& other) noexcept = default;

  /// Move conversion assignment operator from Buf.
  ///
  /// The moved-from `Buf` is left null.
  ConstBuf& operator=(Buf&& other) noexcept {
    buf_ = std::move(other);
    return *this;
  }

  ConstBuf& operator=(const ConstBuf&) = delete;

  /// Frees the owned memory (if any) and sets the buffer to null. Same as
  /// `reset()`.
  ConstBuf& operator=(std::nullptr_t) noexcept {
    reset();
    return *this;
  }

  /// Integers cannot be assigned to a `ConstBuf`, not even the literal `0`.
  template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
  ConstBuf& operator=(T) = delete;

  /// `operator bool` is not provided to prevent confusion between nullness and
  /// emptiness. Use `buf == nullptr` to check for null, or `buf.empty()` for
  /// size 0.
  explicit operator bool() const = delete;

  [[nodiscard]] friend constexpr bool operator==(const ConstBuf& lhs,
                                                 std::nullptr_t) noexcept {
    return lhs.data() == nullptr;
  }
  [[nodiscard]] friend constexpr bool operator==(std::nullptr_t,
                                                 const ConstBuf& rhs) noexcept {
    return rhs.data() == nullptr;
  }
  [[nodiscard]] friend constexpr bool operator!=(const ConstBuf& lhs,
                                                 std::nullptr_t) noexcept {
    return lhs.data() != nullptr;
  }
  [[nodiscard]] friend constexpr bool operator!=(std::nullptr_t,
                                                 const ConstBuf& rhs) noexcept {
    return rhs.data() != nullptr;
  }

  /// Accesses the byte at the specified index.
  reference operator[](size_t index) const { return buf_[index]; }

  /// Returns a pointer to the read-only data, or `nullptr` if the `ConstBuf` is
  /// null.
  ///
  /// @note An empty `ConstBuf` that is not null (e.g. sliced or truncated to
  /// length 0) returns a non-null pointer. `data()` is only `nullptr` when the
  /// buffer is null.
  constexpr pointer data() const { return buf_.data(); }

  /// Returns the number of bytes in the buffer view.
  constexpr size_t size() const { return buf_.size(); }

  /// Returns true if the buffer view is empty (has no bytes).
  ///
  /// `empty()` returns true for both null buffers and non-null buffers of size
  /// zero (e.g. sliced or truncated to length 0).
  [[nodiscard]] constexpr bool empty() const { return buf_.empty(); }

  /// Returns a read-only iterator pointing to the beginning of the data.
  iterator begin() const { return buf_.cbegin(); }

  /// Returns a read-only iterator pointing past the end of the data.
  iterator end() const { return buf_.cend(); }

  /// Returns a read-only iterator pointing to the beginning of the data.
  const_iterator cbegin() const { return buf_.cbegin(); }

  /// Returns a read-only iterator pointing past the end of the data.
  const_iterator cend() const { return buf_.cend(); }

  /// Returns a pointer to the deallocator if this buffer owns the underlying
  /// memory allocation, or `nullptr` if the buffer is unowned or null.
  [[nodiscard]] constexpr Deallocator* deallocator() const {
    return buf_.deallocator();
  }

  /// Frees the owned memory (if any) and sets the buffer to null.
  void reset() { buf_.reset(); }

  /// Exchanges the contents of this `ConstBuf` with `other`.
  void swap(ConstBuf& other) noexcept { buf_.swap(other.buf_); }

  /// Exchanges the contents of two `ConstBuf`s.
  friend void swap(ConstBuf& lhs, ConstBuf& rhs) noexcept { lhs.swap(rhs); }

 private:
  friend class multibuf::v2::internal::GenericMultiBuf;

  friend ConstBuf Slice(ConstBuf&& const_buf, size_t offset, size_t length);

  // A ConstBuf is a Buf that does not expose mutable access to its data or
  // support reclaiming bytes excluded by slicing.
  Buf buf_;
};

/// Slices a read-only `ConstBuf` by shifting its start address and setting its
/// size.
[[nodiscard]] inline ConstBuf Slice(ConstBuf&& const_buf,
                                    size_t offset,
                                    size_t length) {
  return Slice(std::move(const_buf.buf_), offset, length);
}

/// Slices a `ConstBuf` by shifting its start address to the end.
///
/// @pre `offset` cannot exceed `const_buf.size()`.
[[nodiscard]] inline ConstBuf Slice(ConstBuf&& const_buf, size_t offset) {
  return Slice(std::move(const_buf), offset, const_buf.size() - offset);
}

/// Truncates a `ConstBuf` to a smaller size.
[[nodiscard]] inline ConstBuf Truncate(ConstBuf&& const_buf, size_t length) {
  return Slice(std::move(const_buf), 0, length);
}

/// @endmodule

}  // namespace pw
