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
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_allocator/unique_ptr.h"
#include "pw_assert/assert.h"
#include "pw_bytes/span.h"
#include "pw_containers/ptr_iterator.h"
#include "pw_span/span.h"

namespace pw {
namespace multibuf::v2::internal {

class GenericMultiBuf;

}  // namespace multibuf::v2::internal

class Buf;

/// @module{pw_buf}

/// Represents a read-only view of a contiguous block of bytes.
///
/// Users access a `Buf` through a `ConstBuf` reference to ensure the underlying
/// data is not modified.
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

  /// Constructs an empty `ConstBuf`.
  constexpr ConstBuf() = default;

  /// Move constructor.
  constexpr ConstBuf(ConstBuf&& other) noexcept
      : allocation_(other.allocation_),
        view_(other.view_),
        deallocator_(other.deallocator_) {
    other.allocation_ = nullptr;
    other.view_ = {};
    other.deallocator_ = nullptr;
  }

  /// Move-constructs a `ConstBuf` from a mutable `Buf` (move conversion).
  explicit constexpr ConstBuf(Buf&& other) noexcept;

  ConstBuf(const ConstBuf&) = delete;

  /// Destructor. Releases the owned memory back to the allocator.
  ~ConstBuf() { reset(); }

  /// Move assignment operator.
  ConstBuf& operator=(ConstBuf&& other) noexcept;

  /// Move conversion assignment operator from Buf.
  ConstBuf& operator=(Buf&& other) noexcept;

  ConstBuf& operator=(const ConstBuf&) = delete;

  /// Accesses the byte at the specified index.
  reference operator[](size_t index) const {
    PW_DASSERT(index < view_.size());
    return view_[index];
  }

  /// Returns a pointer to the read-only data.
  pointer data() const { return view_.data(); }

  /// Returns the number of bytes in the buffer view.
  size_t size() const { return view_.size(); }

  /// Returns true if the buffer view is empty.
  [[nodiscard]] bool empty() const { return view_.empty(); }

  /// Returns a read-only iterator pointing to the beginning of the data.
  iterator begin() const { return iterator(view_.data()); }

  /// Returns a read-only iterator pointing past the end of the data.
  iterator end() const { return iterator(view_.data() + view_.size()); }

  /// Returns a read-only iterator pointing to the beginning of the data.
  const_iterator cbegin() const { return const_iterator(view_.data()); }

  /// Returns a read-only iterator pointing past the end of the data.
  const_iterator cend() const {
    return const_iterator(view_.data() + view_.size());
  }

  /// Returns a pointer to the deallocator if this buffer owns the underlying
  /// memory allocation, or `nullptr` if the buffer is unowned.
  [[nodiscard]] Deallocator* deallocator() const { return deallocator_; }

  /// Frees the owned memory, leaving the buffer empty.
  void reset();

 private:
  constexpr explicit ConstBuf(std::byte* allocation,
                              Deallocator* deallocator,
                              pw::span<std::byte> view)
      : allocation_(allocation), view_(view), deallocator_(deallocator) {
    PW_ASSERT(allocation != nullptr || view.empty());
    PW_ASSERT(view.data() >= allocation);
  }

  friend class Buf;
  friend class multibuf::v2::internal::GenericMultiBuf;

  friend ConstBuf Slice(ConstBuf&& const_buf, size_t offset, size_t length);
  friend Buf Slice(Buf&& buf, size_t offset, size_t length);
  friend Buf Reclaim(Buf&& buf, size_t prefix_count, size_t suffix_count);

  void MoveFrom(ConstBuf&& other);

  ConstBuf Slice(size_t offset, size_t length) &&;
  ConstBuf Reclaim(size_t prefix_count, size_t suffix_count) &&;

  std::byte* mut_data() { return view_.data(); }

  static constexpr iterator MakeIterator(const std::byte* ptr) {
    return iterator(ptr);
  }

  std::byte* allocation_ = nullptr;
  pw::span<std::byte> view_;
  Deallocator* deallocator_ = nullptr;
};

/// Represents a mutable view of a contiguous block of bytes.
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
      return Buf::ConstBufIterator(this->operator->());
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
      return Buf::ConstBufIterator(this->operator->());
    }

   private:
    friend class Buf;
    friend containers::internal::PtrIterator<const_iterator, const Buf>;

    explicit constexpr const_iterator(const std::byte* ptr)
        : containers::internal::PtrIterator<const_iterator, const Buf>(ptr) {}
  };
  /// @endcond

  /// Constructs an empty `Buf`.
  constexpr Buf() = default;

  /// Move constructor.
  constexpr Buf(Buf&& other) noexcept
      : const_buf_(std::move(other.const_buf_)) {}

  /// Constructs an owned `Buf` from a `UniquePtr`.
  explicit Buf(UniquePtr<std::byte[]>&& buffer)
      : const_buf_(buffer.get(),
                   buffer.deallocator(),
                   pw::span<std::byte>(buffer.get(), buffer.size())) {
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
      : const_buf_(
            allocation, &deallocator, pw::span<std::byte>(allocation, size)) {}

  /// Constructs an owned `Buf` from a raw buffer, offset, size, and
  /// deallocator.
  explicit constexpr Buf(std::byte* allocation,
                         size_t offset,
                         size_t size,
                         Deallocator& deallocator)
      : const_buf_(allocation,
                   &deallocator,
                   pw::span<std::byte>(allocation + offset, size)) {}

  /// Creates an unowned `Buf` from a `ByteSpan`, offset, and size.
  [[nodiscard]] static Buf Unowned(ByteSpan span, size_t offset, size_t size);

  /// Creates an unowned `Buf` using the remaining length starting from
  /// `offset`.
  [[nodiscard]] static Buf Unowned(ByteSpan span, size_t offset = 0);

  /// Creates an unowned `Buf` from a pointer, offset, and size.
  [[nodiscard]] static Buf Unowned(std::byte* allocation,
                                   size_t offset,
                                   size_t size) {
    return Buf(allocation, pw::span<std::byte>(allocation + offset, size));
  }

  /// Creates an unowned `Buf` from a pointer and size.
  [[nodiscard]] static Buf Unowned(std::byte* allocation, size_t size) {
    return Buf(allocation, pw::span<std::byte>(allocation, size));
  }

  /// Allocates a new owned `Buf` of the specified size.
  ///
  /// Asserts if allocation fails.
  [[nodiscard]] static Buf Allocate(Allocator& allocator, size_t size) {
    return Allocate(allocator, 0, size);
  }

  /// Allocates a new owned `Buf` of the specified allocation size, shifted to
  /// `offset`, with the specified size.
  ///
  /// Asserts if allocation fails.
  [[nodiscard]] static Buf Allocate(Allocator& allocator,
                                    size_t offset,
                                    size_t size);

  /// Allocates a new owned `Buf` of the specified size.
  ///
  /// Returns an empty `Buf` if allocation fails.
  [[nodiscard]] static Buf TryAllocate(Allocator& allocator, size_t size) {
    return TryAllocate(allocator, 0, size);
  }

  /// Allocates a new owned `Buf` of the specified allocation size, shifted to
  /// `offset`, with the specified size.
  ///
  /// Returns an empty `Buf` if allocation fails.
  [[nodiscard]] static Buf TryAllocate(Allocator& allocator,
                                       size_t offset,
                                       size_t size);

  Buf(const Buf&) = delete;

  ~Buf() = default;

  /// Move assignment operator.
  Buf& operator=(Buf&& other) noexcept {
    const_buf_ = std::move(other.const_buf_);
    return *this;
  }

  Buf& operator=(const Buf&) = delete;

  /// Implicit conversion operators to `ConstBuf` reference types.
  operator ConstBuf&() & { return const_buf_; }
  operator const ConstBuf&() const& { return const_buf_; }
  operator ConstBuf&&() && { return std::move(const_buf_); }
  operator const ConstBuf&&() const&& { return std::move(const_buf_); }

  /// Accesses the byte at the specified index as read-only.
  const_reference operator[](size_t index) const { return const_buf_[index]; }

  /// Accesses the byte at the specified index as mutable.
  reference operator[](size_t index) {
    return const_cast<reference>(const_buf_[index]);
  }

  /// Returns a pointer to the read-only data.
  const_pointer data() const { return const_buf_.data(); }

  /// Returns a pointer to the mutable data.
  pointer data() { return const_buf_.mut_data(); }

  /// Returns the size of the buffer.
  size_t size() const { return const_buf_.size(); }

  /// Returns true if the buffer is empty.
  [[nodiscard]] bool empty() const { return const_buf_.empty(); }

  /// Returns a pointer to the deallocator if this buffer owns the underlying
  /// memory allocation, or `nullptr` if the buffer is unowned.
  [[nodiscard]] Deallocator* deallocator() const {
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

  /// Frees the owned memory, leaving the buffer empty.
  void reset() { const_buf_.reset(); }

 private:
  constexpr Buf(std::byte* allocation, pw::span<std::byte> view)
      : const_buf_(allocation, nullptr, view) {}

  explicit constexpr Buf(ConstBuf&& const_buf)
      : const_buf_(std::move(const_buf)) {}

  friend class ConstBuf;
  friend class multibuf::v2::internal::GenericMultiBuf;

  friend Buf Slice(Buf&& buf, size_t offset, size_t length);
  friend Buf Reclaim(Buf&& buf, size_t prefix_count, size_t suffix_count);

  static constexpr ConstBuf::iterator ConstBufIterator(const std::byte* ptr) {
    return ConstBuf::MakeIterator(ptr);
  }

  ConstBuf const_buf_;
};

inline constexpr ConstBuf::ConstBuf(Buf&& other) noexcept
    : ConstBuf(std::move(other.const_buf_)) {}

/// Slices a read-only `ConstBuf` by shifting its start address and setting its
/// size.
[[nodiscard]] inline ConstBuf Slice(ConstBuf&& const_buf,
                                    size_t offset,
                                    size_t length) {
  return std::move(const_buf).Slice(offset, length);
}

/// Slices a read-only `ConstBuf` by shifting its start address to the end.
///
/// Note: if `offset` exceeds size, the subtraction wraps and fails the bounds
/// check in the delegating Slice function.
[[nodiscard]] inline ConstBuf Slice(ConstBuf&& const_buf, size_t offset) {
  return Slice(std::move(const_buf), offset, const_buf.size() - offset);
}

/// Truncates a read-only `ConstBuf` to a smaller size.
[[nodiscard]] inline ConstBuf Truncate(ConstBuf&& const_buf, size_t length) {
  return Slice(std::move(const_buf), 0, length);
}

/// Slices a mutable `Buf` by shifting its start address and setting its size.
[[nodiscard]] inline Buf Slice(Buf&& buf, size_t offset, size_t length) {
  return Buf(std::move(buf.const_buf_).Slice(offset, length));
}

/// Slices a mutable `Buf` by shifting its start address to the end.
///
/// Note: if `offset` exceeds size, the subtraction wraps and fails the bounds
/// check in the delegating Slice function.
[[nodiscard]] inline Buf Slice(Buf&& buf, size_t offset) {
  return Slice(std::move(buf), offset, buf.size() - offset);
}

/// Truncates a mutable `Buf` to a smaller size.
[[nodiscard]] inline Buf Truncate(Buf&& buf, size_t length) {
  return Slice(std::move(buf), 0, length);
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

/// @endmodule

}  // namespace pw
