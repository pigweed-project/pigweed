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

class Buf;

/// @module{pw_buf}

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
  using iterator = containers::ConstPtrIterator<ConstBuf>;
  using const_pointer = const std::byte*;
  using const_reference = const std::byte&;
  using const_iterator = containers::ConstPtrIterator<ConstBuf>;

  /// Constructs a null `ConstBuf`.
  constexpr ConstBuf()
      : data_(nullptr),
        size_(0),
        deallocator_(nullptr),
        prefix_(0),
        suffix_(0) {}

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
  constexpr ConstBuf(ConstBuf&& other) noexcept
  // MoveFrom initializes all members, so skip initializing them twice to reduce
  // code size. This is not supported in C++17, however.
#if __cpp_constexpr < 201907L
      : ConstBuf()
#endif  // __cpp_constexpr < 201907L
  {
    MoveFrom(std::move(other));
  }

  /// Move-constructs a `ConstBuf` from a mutable `Buf` (move conversion).
  ///
  /// The moved-from `Buf` is left null.
  constexpr ConstBuf(Buf&& other) noexcept;

  /// Creates an unowned `ConstBuf` from a `ConstByteSpan`.
  [[nodiscard]] static ConstBuf Unowned(ConstByteSpan span) {
    return Unowned(span.data(), span.size());
  }

  /// Creates an unowned `ConstBuf` from a pointer and size in bytes.
  [[nodiscard]] static ConstBuf Unowned(const std::byte* data,
                                        size_t size_bytes) {
    return ConstBuf(const_cast<std::byte*>(data), size_bytes);
  }

  ConstBuf(const ConstBuf&) = delete;

  /// Destructor. Releases the owned memory (if any) back to the allocator.
  ~ConstBuf() { reset(); }

  /// Move assignment operator.
  ///
  /// The moved-from `ConstBuf` is left null.
  ConstBuf& operator=(ConstBuf&& other) noexcept;

  /// Move conversion assignment operator from Buf.
  ///
  /// The moved-from `Buf` is left null.
  ConstBuf& operator=(Buf&& other) noexcept;

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
  reference operator[](size_t index) const {
    PW_DASSERT(index < size_);
    return data_[index];
  }

  /// Returns a pointer to the read-only data, or `nullptr` if the `ConstBuf` is
  /// null.
  ///
  /// @note An empty `ConstBuf` that is not null (e.g. sliced or truncated to
  /// length 0) returns a non-null pointer. `data()` is only `nullptr` when the
  /// buffer is null.
  constexpr pointer data() const { return data_; }

  /// Returns the number of bytes in the buffer view.
  constexpr size_t size() const { return size_; }

  /// Returns true if the buffer view is empty (has no bytes).
  ///
  /// `empty()` returns true for both null buffers and non-null buffers of size
  /// zero (e.g. sliced or truncated to length 0).
  [[nodiscard]] constexpr bool empty() const { return size_ == 0; }

  /// Returns a read-only iterator pointing to the beginning of the data.
  iterator begin() const { return iterator(data_); }

  /// Returns a read-only iterator pointing past the end of the data.
  iterator end() const { return iterator(data_ + size_); }

  /// Returns a read-only iterator pointing to the beginning of the data.
  const_iterator cbegin() const { return const_iterator(data_); }

  /// Returns a read-only iterator pointing past the end of the data.
  const_iterator cend() const { return const_iterator(data_ + size_); }

  /// Returns a pointer to the deallocator if this buffer owns the underlying
  /// memory allocation, or `nullptr` if the buffer is unowned or null.
  [[nodiscard]] constexpr Deallocator* deallocator() const {
    return deallocator_;
  }

  /// Frees the owned memory (if any) and sets the buffer to null.
  void reset();

 private:
  using OffsetType =
      std::conditional_t<sizeof(size_t) >= 8, uint32_t, uint16_t>;

  constexpr explicit ConstBuf(std::byte* data,
                              size_t size,
                              Deallocator* deallocator = nullptr)
      : data_(data),
        size_(size),
        deallocator_(deallocator),
        prefix_(0),
        suffix_(0) {
    PW_ASSERT(data != nullptr || size == 0);
  }

  constexpr explicit ConstBuf(std::byte* base,
                              size_t offset,
                              size_t size,
                              Deallocator* deallocator)
      : data_(base + offset),
        size_(size),
        deallocator_(deallocator),
        prefix_(static_cast<OffsetType>(offset)),
        suffix_(0) {
    PW_ASSERT(base != nullptr || (offset == 0 && size == 0));
    PW_ASSERT(offset <= std::numeric_limits<OffsetType>::max());
  }

  friend class Buf;
  friend class multibuf::v2::internal::GenericMultiBuf;

  friend ConstBuf Slice(ConstBuf&& const_buf, size_t offset, size_t length);
  friend Buf Slice(Buf&& buf, size_t offset, size_t length);
  friend Buf Reclaim(Buf&& buf, size_t prefix_count, size_t suffix_count);

  constexpr void MoveFrom(ConstBuf&& other) {
    data_ = cpp20::exchange(other.data_, nullptr);
    size_ = cpp20::exchange(other.size_, 0);
    deallocator_ = cpp20::exchange(other.deallocator_, nullptr);
    prefix_ = cpp20::exchange(other.prefix_, 0);
    suffix_ = cpp20::exchange(other.suffix_, 0);
  }

  ConstBuf Slice(size_t offset, size_t length) &&;
  ConstBuf Reclaim(size_t prefix_count, size_t suffix_count) &&;

  constexpr std::byte* mut_data() { return data_; }

  constexpr std::byte* base() const { return data_ - prefix_; }

  constexpr size_t base_size() const { return prefix_ + size_ + suffix_; }

  static constexpr iterator MakeIterator(const std::byte* ptr) {
    return iterator(ptr);
  }

  std::byte* data_;
  size_t size_;
  Deallocator* deallocator_;
  OffsetType prefix_;
  OffsetType suffix_;
};

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

  // Define customized iterators to support conversions to ConstBuf iterators.
  /// @cond
  class iterator : public containers::internal::PtrIterator<iterator, Buf> {
   public:
    constexpr iterator() = default;

    constexpr operator containers::ConstPtrIterator<ConstBuf>() const {
      return ConstBuf::MakeIterator(this->operator->());
    }

   private:
    friend class Buf;
    friend containers::internal::PtrIterator<iterator, Buf>;

    explicit constexpr iterator(std::byte* ptr)
        : containers::internal::PtrIterator<iterator, Buf>(ptr) {}
  };

  class const_iterator
      : public containers::internal::PtrIterator<const_iterator, const Buf> {
   public:
    constexpr const_iterator() = default;

    constexpr const_iterator(iterator other)
        : containers::internal::PtrIterator<const_iterator, const Buf>(
              other.operator->()) {}

    constexpr operator containers::ConstPtrIterator<ConstBuf>() const {
      return ConstBuf::MakeIterator(this->operator->());
    }

   private:
    friend class Buf;
    friend containers::internal::PtrIterator<const_iterator, const Buf>;

    explicit constexpr const_iterator(const std::byte* ptr)
        : containers::internal::PtrIterator<const_iterator, const Buf>(ptr) {}
  };
  /// @endcond

  /// Constructs a null `Buf`.
  constexpr Buf() = default;

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
      : const_buf_(std::move(other.const_buf_)) {}

  /// Constructs an owned `Buf` from a `UniquePtr`.
  ///
  /// If `buffer` is empty/null, constructs a null `Buf`.
  explicit Buf(UniquePtr<std::byte[]>&& buffer)
      : const_buf_(buffer.get(), buffer.size(), buffer.deallocator()) {
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
      : const_buf_(allocation, size, &deallocator) {}

  /// Constructs an owned `Buf` from a raw buffer, offset, size, and
  /// deallocator.
  explicit constexpr Buf(std::byte* allocation,
                         size_t offset,
                         size_t size,
                         Deallocator& deallocator)
      : const_buf_(allocation, offset, size, &deallocator) {}

  /// Creates an unowned `Buf` from a `ByteSpan`, offset, and size.
  [[nodiscard]] static Buf Unowned(ByteSpan span, size_t offset, size_t size);

  /// Creates an unowned `Buf` using the remaining length starting from
  /// `offset`.
  [[nodiscard]] static Buf Unowned(ByteSpan span, size_t offset = 0);

  /// Creates an unowned `Buf` from a pointer, offset, and size.
  [[nodiscard]] static Buf Unowned(std::byte* base,
                                   size_t offset,
                                   size_t size) {
    return Buf(ConstBuf(base, offset, size, nullptr));
  }

  /// Creates an unowned `Buf` from a pointer and size.
  [[nodiscard]] static Buf Unowned(std::byte* base, size_t size) {
    return Buf(ConstBuf(base, size));
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

  ~Buf() = default;

  /// Move assignment operator.
  ///
  /// The moved-from `Buf` is left null.
  Buf& operator=(Buf&& other) noexcept {
    const_buf_ = std::move(other.const_buf_);
    return *this;
  }

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

  /// Implicit conversion to `const ConstBuf&` and `const ConstBuf&&`.
  /// Conversion to `ConstBuf&` is not supported, since truncating the
  /// `ConstBuf&` would nullify this `Buf`.
  ///
  /// @note Prefer accepting `pw::ConstByteSpan` (or `pw::span<const
  /// std::byte>`) by value instead of `const ConstBuf&` for functions that only
  /// require read-only access to the buffer's contents.
  operator const ConstBuf&() const& { return const_buf_; }
  operator const ConstBuf&&() const&& { return std::move(const_buf_); }

  /// Accesses the byte at the specified index as read-only.
  const_reference operator[](size_t index) const { return const_buf_[index]; }

  /// Accesses the byte at the specified index as mutable.
  reference operator[](size_t index) {
    return const_cast<reference>(const_buf_[index]);
  }

  /// Returns a pointer to the read-only data, or `nullptr` if the `Buf` is
  /// null.
  ///
  /// @note An empty `Buf` that is not null (e.g. sliced or truncated to length
  /// 0) returns a non-null pointer. `data()` is only `nullptr` when the buffer
  /// is null.
  constexpr const_pointer data() const { return const_buf_.data(); }

  /// Returns a pointer to the mutable data, or `nullptr` if the `Buf` is null.
  ///
  /// @note An empty `Buf` that is not null (e.g. sliced or truncated to length
  /// 0) returns a non-null pointer. `data()` is only `nullptr` when the buffer
  /// is null.
  constexpr pointer data() { return const_buf_.mut_data(); }

  /// Returns the size of the buffer.
  constexpr size_t size() const { return const_buf_.size(); }

  /// Returns true if the buffer is empty (has no bytes).
  ///
  /// `empty()` returns true for both null buffers and non-null buffers of size
  /// zero (e.g. after being sliced or truncated to length 0).
  [[nodiscard]] constexpr bool empty() const { return const_buf_.empty(); }

  /// Returns a pointer to the base address of the original, underlying memory
  /// buffer, or `nullptr` if the `Buf` is null.
  [[nodiscard]] constexpr const std::byte* base() const {
    return const_buf_.base();
  }

  /// Returns the size of the original, underlying memory buffer, or 0 if the
  /// `Buf` is null.
  [[nodiscard]] constexpr size_t base_size() const {
    return const_buf_.base_size();
  }

  /// Returns a pointer to the deallocator if this buffer owns the underlying
  /// memory allocation, or `nullptr` if the buffer is unowned or null.
  [[nodiscard]] constexpr Deallocator* deallocator() const {
    return const_buf_.deallocator();
  }

  /// Returns a read-only iterator pointing to the beginning of the data.
  const_iterator begin() const { return const_iterator(const_buf_.data()); }

  /// Returns a read-only iterator pointing past the end of the data.
  const_iterator end() const {
    return const_iterator(const_buf_.data() + const_buf_.size());
  }

  /// Returns a read-only iterator pointing to the beginning of the data.
  const_iterator cbegin() const { return const_iterator(const_buf_.data()); }

  /// Returns a read-only iterator pointing past the end of the data.
  const_iterator cend() const {
    return const_iterator(const_buf_.data() + const_buf_.size());
  }

  /// Returns a mutable iterator pointing to the beginning of the data.
  iterator begin() { return iterator(const_buf_.mut_data()); }

  /// Returns a mutable iterator pointing past the end of the data.
  iterator end() { return iterator(const_buf_.mut_data() + const_buf_.size()); }

  /// Frees the owned memory (if any) and sets the buffer to null.
  void reset() { const_buf_.reset(); }

 private:
  explicit constexpr Buf(ConstBuf&& const_buf)
      : const_buf_(std::move(const_buf)) {}

  friend class ConstBuf;

  friend Buf Slice(Buf&& buf, size_t offset, size_t length);
  friend Buf Reclaim(Buf&& buf, size_t prefix_count, size_t suffix_count);

  ConstBuf const_buf_;
};

inline constexpr ConstBuf::ConstBuf(Buf&& other) noexcept
    : ConstBuf(std::move(other.const_buf_)) {}

inline ConstBuf& ConstBuf::operator=(Buf&& other) noexcept {
  return *this = std::move(other.const_buf_);
}

/// Slices a read-only `ConstBuf` by shifting its start address and setting its
/// size.
[[nodiscard]] inline ConstBuf Slice(ConstBuf&& const_buf,
                                    size_t offset,
                                    size_t length) {
  return std::move(const_buf).Slice(offset, length);
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

/// Slices a mutable `Buf` by shifting its start address and setting its size.
[[nodiscard]] inline Buf Slice(Buf&& buf, size_t offset, size_t length) {
  return Buf(std::move(buf.const_buf_).Slice(offset, length));
}

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

/// Reclaims up to `prefix_count` bytes at the beginning and `suffix_count`
/// bytes at the end of the buffer.
[[nodiscard]] inline Buf Reclaim(Buf&& buf,
                                 size_t prefix_count,
                                 size_t suffix_count) {
  return Buf(std::move(buf.const_buf_).Reclaim(prefix_count, suffix_count));
}

/// Reclaims up to `count` prefix bytes at the beginning of the buffer.
[[nodiscard]] inline Buf ReclaimPrefix(Buf&& buf, size_t count) {
  return Reclaim(std::move(buf), count, 0);
}

/// Reclaims up to `count` suffix bytes at the end of the buffer.
[[nodiscard]] inline Buf ReclaimSuffix(Buf&& buf, size_t count) {
  return Reclaim(std::move(buf), 0, count);
}

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

/// @endmodule

}  // namespace pw
