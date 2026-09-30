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
#include <type_traits>
#include <utility>

#include "pw_allocator/bump_allocator.h"
#include "pw_allocator/forwarding_allocator.h"
#include "pw_allocator/hardening.h"
#include "pw_allocator/layout.h"
#include "pw_assert/assert.h"
#include "pw_bytes/alignment.h"
#include "pw_preprocessor/compiler.h"
#include "pw_status/try.h"

namespace pw::allocator {
namespace internal {

/// Extensible wrapper for frame validation errors.
///
/// This type can be extended with additional errors; for an example see
/// pw::allocator::internal::GuardError.
class FrameError {
 public:
  enum Value : uint32_t {
    /// The provided data pointer is not aligned correctly.
    kDataNotAligned,

    /// The provided data pointer is to an address too low to have a prefix
    /// offset, e.g. a data pointer of 0x00000004.
    kDataTooSmall,

    /// The provided data pointer has a prefix offset that's too large, e.g. a
    /// data pointer of 0x0010000 has an offset of 0xffffffff.
    kDataTooSmallForPrefix,

    /// The provided frame pointer is null.
    kFrameNull,

    /// The provided frame pointer is not aligned correctly.
    kFrameNotAligned,

    /// The provided data pointer is to an address too small to have a prefix
    /// offset, e.g. a frame pointer of 0x00000004.
    kFrameTooSmall,

    /// The provided frame pointer has a prefix offset that's too large, e.g. a
    /// frame pointer of 0x0010000 has an offset of 0xffffffff.
    kFrameOffsetTooSmall,

    /// The suffix located from the provided data pointer is not aligned
    /// correctly.
    kSuffixNotAligned,

    /// The suffix located from the provided data pointer is conflicts with the
    /// usable memory region.
    kDataTooSmallForSuffix,

    /// The underlying allocator does not recognize the frame pointer.
    kUnrecognizedFrame,

    /// Deriving a frame pointer from a data pointer that is derived from a
    /// provided frame pointer yields a different frame pointer than provided.
    kFramePointerMismatch,

    /// Deriving a data pointer from a frame pointer that is derived from a
    /// provided data pointer yields a different data pointer than provided.
    kDataPointerMismatch,

    /// Can be used to dispatch to chained error types, e.g.
    /// pw::allocator::internal::GuardError.
    kMaxValue,
  };

  constexpr FrameError(Value v) : value_(static_cast<uint32_t>(v)) {}
  constexpr uint32_t value() const { return value_; }

  constexpr bool operator==(FrameError other) const {
    return value_ == other.value_;
  }
  constexpr bool operator!=(FrameError other) const {
    return value_ != other.value_;
  }

 protected:
  constexpr explicit FrameError(uint32_t v) : value_(v) {}
  uint32_t value_;
};

/// Handles detected errors by calling PW_CRASH with a diagnostic message.
struct DefaultFrameErrorHandler final {
  static void HandleError(FrameError error,
                          const void* ptr1 = nullptr,
                          size_t val1 = 0,
                          const void* ptr2 = nullptr,
                          size_t val2 = 0);
};

/// Provides generic framing logic for type-erased prefixes and suffixes.
///
/// @tparam   ErrorHandler  Invoked when a frame error is encountered.
template <typename ErrorHandler>
class BaseFramingAllocator : public ForwardingAllocator {
 protected:
  constexpr explicit BaseFramingAllocator(Allocator& allocator)
      : ForwardingAllocator(allocator) {}

  /// Returns the distance between two pointers in bytes.
  static constexpr size_t Distance(const void* start, const void* end);

  /// Updates the frame-relative and data-relative prefix offsets, and the
  /// suffix offset as needed for the given pointers to locations in the frame.
  ///
  /// @param  frame       Pointer to the underlying allocation.
  /// @param  data        Pointer to the usable memory.
  /// @param  suffix      Pointer to the type-erased suffix, or null.
  static void UpdateOffsets(void* frame, void* data, void* suffix);

  /// Allocates a frame that can hold the requested data framed by a prefix
  /// and/or suffix, and returns it as a span of bytes.
  ///
  /// @param  prefix      Layout of the type-erased prefix.
  /// @param  data        Requested layout.
  /// @param  suffix      Layout of the type-erased suffix.
  ByteSpan AllocateFrame(Layout prefix, Layout data, Layout suffix);

  /// Resizes a frame that holds the provided data to be at least the new size
  /// while keeping the (type-erased) prefix and/or suffix.
  ///
  /// @returns Whether the frame was successfully resized.
  /// @param  data        Pointer to usable memory.
  /// @param  new_size    Requested new size of the usable memory.
  /// @param  suffix      Layout of the type-erased suffix, or a default layout.
  /// @param  tmp_suffix  Pointer to a temporary suffix, or null.
  bool ResizeFrame(void* data,
                   size_t new_size,
                   Layout suffix,
                   void* tmp_suffix);

  /// Returns info like `Deallocator::GetInfo`.
  ///
  /// @param  info_type   See `Deallocator::InfoType`.
  /// @param  data        Pointer to usable memory.
  /// @param  suffix      Layout of the type-erased suffix, or a default layout.
  Result<Layout> GetFrameInfo(InfoType info_type,
                              const void* data,
                              Layout suffix) const;

  /// Returns a frame pointer from a data pointer.
  ///
  /// Unlike many of the other pointer conversion methods, this method includes
  /// a `checked` parameter that can be set to `false` to avoid calling the
  /// error handler. This is useful when the returned frame pointer is intended
  /// to be passed to a method that accepts any pointer, e.g. a Deallocator
  /// method that returns `pw::Status::NotFound()` for invalid pointers.
  ///
  /// @param  data        Pointer to usable memory.
  /// @param  has_suffix  Used to locate the data-relative prefix offset.
  /// @param  checked     If false, the ErrorHandler is never called.
  /// @{
  static void* GetFrame(void* data, bool has_suffix, bool checked = true);
  static const void* GetFrame(const void* data,
                              bool has_suffix,
                              bool checked = true) {
    return GetFrame(const_cast<void*>(data), has_suffix, checked);
  }
  /// @}

  /// Returns the usable size of an underlying allocation.
  ///
  /// @returns @Result{size of the frame}
  /// * @OK: Returns the size of the frame in bytes.
  /// * @NOT_FOUND: Frame is not recognized by the underlying allocator.
  /// * @UNIMPLEMENTED: The underlying allocator cannot get usable sizes.
  Result<size_t> GetFrameSize(const void* frame) const;

  /// Returns a data pointer from a frame pointer.
  ///
  /// @param  frame       Pointer to an underlying allocation.
  /// @param  frame_size  Size of the frame.
  /// @param  prefix      Layout of the type-erased prefix, or a default layout.
  /// @{
  static void* GetData(void* frame, Result<size_t> frame_size, Layout prefix);
  static const void* GetData(const void* frame,
                             Result<size_t> frame_size,
                             Layout prefix) {
    return GetData(const_cast<void*>(frame), frame_size, prefix);
  }
  /// @}

  /// Returns a frame pointer from a data pointer.
  /// @param  data        Pointer to usable memory.
  /// @param  data_size   Size of the memory from `data` to the end of the
  ///                     frame, **including** the suffix.
  /// @param  suffix      Layout of the type-erased suffix, or a default layout.
  /// @{
  static void* GetSuffix(void* data, size_t data_size, Layout suffix);
  static const void* GetSuffix(const void* data,
                               size_t data_size,
                               Layout suffix) {
    return GetSuffix(const_cast<void*>(data), data_size, suffix);
  }
  /// @}
};

}  // namespace internal

/// @submodule{pw_allocator,forwarding}

/// An allocator that can "frame" its allocation with a leading prefix type, a
/// trailing suffix type, or both.
///
/// At least one of `Prefix` and `Suffix` must be a type other than `void`,
/// since in that case there is no need for a frame.
///
/// In addition to the template parameter types, each allocation will include
/// up to three additional `size_t` fields.
///
///  - The offset of the **suffix** field from the **data**, i.e. from the
///    allocated pointer. This field is omitted if the suffix type is `void`.
///    When present, this field is located just before the usable memory.
///
///  - The offset of the **data** from the start of the **frame**. This field is
///    located just after the prefix, if the prefix type is not `void`, or at
///    the start of the frame.
///
///  - The offset of the **frame** from the start of the **data**. This has
///    the same value as the frame offset, but can be located using only the
///    data pointer. This field is located just before the suffix offset, if the
///    suffix type is not `void`, or just before the usable memory.
///
/// The frame offset and data offset locations may match, i.e. the field just
/// after the prefix is right before the suffix offset or usable memory. In this
/// case, only one offset is stored as both the frame and data offset.
///
/// Thus assuming ``sizeof(size_t) == 4``, requesting a 64-byte allocation with
/// a 16-byte alignment from a ``FramingAllocator<uint32_t, uint16_t>`` might
/// result in an allocation that looks like:
///
/// @code
///   Address   Type          Contents             Pointed at by:
/// +---------+-------------+--------------------+--------------------------+
/// | 0x...00 | Prefix      | user-defined       | frame                    |
/// |         | (uint32_t)  |                    | GetFrame(data)           |
/// |         |             |                    | GetPrefix(data)          |
/// +---------+-------------+--------------------+--------------------------+
/// | 0x...04 | size_t      | frame_offset =0x10 | GetFrameOffsetPtr(frame) |
/// +---------+-------------+--------------------+--------------------------+
/// | 0x...08 | size_t      | data_offset  =0x10 | GetDataOffsetPtr(data)   |
/// +---------+-------------+--------------------+--------------------------+
/// | 0x...0C | size_t      | suffix_offset=0x40 | GetSuffixOffsetPtr(data) |
/// +---------+-------------+--------------------+--------------------------+
/// | 0x...10 | std::byte[] | usable space       | data                     |
/// |         |             |                    | GetData(frame)           |
/// +---------+-------------+--------------------+--------------------------+
/// | 0x...50 | Suffix      | user-defined       | GetSuffix(data)          |
/// |         | (uint16_t)  |                    |                          |
/// +---------+-------------+--------------------+--------------------------+
/// @endcode
///
/// Alternatively, if the selected memory for the same allocation happens to
/// start at `0x...04`, the prefix offsets will be combined:
/// @code
///   Address   Type          Contents             Pointed at by:
/// +---------+-------------+--------------------+--------------------------+
/// | 0x...04 | Prefix      | user-defined       | frame                    |
/// |         | (uint32_t)  |                    | GetFrame(data)           |
/// |         |             |                    | GetPrefix(data)          |
/// +---------+-------------+--------------------+--------------------------+
/// | 0x...08 | size_t      | frame_offset,      | GetFrameOffsetPtr(frame) |
/// |         |             | data_offset  =0x0C | GetDataOffsetPtr(data)   |
/// +---------+-------------+--------------------+--------------------------+
/// | 0x...0C | size_t      | suffix_offset=0x40 | GetSuffixOffsetPtr(data) |
/// +---------+-------------+--------------------+--------------------------+
/// | 0x...10 | std::byte[] | usable space       | data                     |
/// |         |             |                    | GetData(frame)           |
/// +---------+-------------+--------------------+--------------------------+
/// | 0x...50 | Suffix      | user-defined       | GetSuffix(data)          |
/// |         | (uint16_t)  |                    |                          |
/// +---------+-------------+--------------------+--------------------------+
/// @endcode
///
/// Let's drop the suffix for simplicity. If we make a similar allocation from a
/// ``FramingAllocator<uint32_t, void>`` that happens to start at `0x...0C`, we
/// get the worst case scenario for padding to maintain alignment:
///
/// @code
///   Address   Type          Contents             Pointed at by:
/// +---------+-------------+--------------------+--------------------------+
/// | 0x...0C | Prefix      | user-defined       | frame                    |
/// |         | (uint32_t)  |                    | GetFrame(data)           |
/// |         |             |                    | GetPrefix(data)          |
/// +---------+-------------+--------------------+--------------------------+
/// | 0x...10 | size_t      | frame_offset =0x14 | GetFrameOffsetPtr(frame) |
/// +---------+-------------+--------------------+--------------------------+
/// | 0x...14 | N/A         | 8 bytes of padding | N/A                      |
/// +---------+-------------+--------------------+--------------------------+
/// | 0x...1C | size_t      | data_offset  =0x14 | GetDataOffsetPtr(data)   |
/// +---------+-------------+--------------------+--------------------------+
/// | 0x...20 | std::byte[] | usable space       | data                     |
/// |         |             |                    | GetData(frame)           |
/// +---------+-------------+--------------------+--------------------------+
/// @endcode
///
/// Finally, requesting a 64-byte allocation with a 16-byte alignment from
/// a suffix-only ``FramingAllocator<void, uint16_t>`` might result in an
/// allocation that looks like:
///
/// @code
///   Address   Type          Contents             Pointed at by:
/// +---------+-------------+--------------------+--------------------------+
/// | 0x...00 | size_t      | frame_offset =0x10 | frame                    |
/// |         |             |                    | GetFrame(data)           |
/// |         |             |                    | GetFrameOffsetPtr(frame) |
/// +---------+-------------+--------------------+--------------------------+
/// | 0x...04 | N/A         | 4 bytes of padding | N/A                      |
/// +---------+-------------+--------------------+--------------------------+
/// | 0x...08 | size_t      | data_offset  =0x10 | GetDataOffsetPtr(data)   |
/// +---------+-------------+--------------------+--------------------------+
/// | 0x...0C | size_t      | suffix_offset=0x40 | GetSuffixOffsetPtr(data) |
/// +---------+-------------+--------------------+--------------------------+
/// | 0x...10 | std::byte[] | usable space       | data                     |
/// |         |             |                    | GetData(frame)           |
/// +---------+-------------+--------------------+--------------------------+
/// | 0x...50 | Suffix      | user-defined       | GetSuffix(data)          |
/// |         | (uint16_t)  |                    |                          |
/// +---------+-------------+--------------------+--------------------------+
/// @endcode
///
/// \note As illustrated above, the choice of prefix and suffix types and
/// overall alignment can significantly affect the amount of overhead required.
/// When possible, keep alignment requirements low. Ideally, `Suffix` and the
/// allocated data both have an alignment of `alignof(size_t)` or less.
///
/// @tparam   Prefix        A default constructible and trivially copyable type
///                         to place before each allocation, or `void`.
///                         `alignof(Prefix)` must be at most `alignof(size_t)`.
/// @tparam   Suffix        A default constructible and trivially copyable type
///                         to place after each allocation, or `void`.
/// @tparam   ErrorHandler  Used to report frame validation errors.
template <typename Prefix,
          typename Suffix = void,
          typename ErrorHandler = internal::DefaultFrameErrorHandler>
class FramingAllocator : public internal::BaseFramingAllocator<ErrorHandler> {
 protected:
  using Base = internal::BaseFramingAllocator<ErrorHandler>;

  static_assert(!std::is_same_v<Prefix, void> || !std::is_same_v<Suffix, void>,
                "prefix and suffix types cannot both be null");

  constexpr explicit FramingAllocator(pw::Allocator& allocator)
      : Base(allocator) {
    if constexpr (!std::is_same_v<Suffix, void>) {
      // GetUsableLayout is required to validate suffix offsets.
      PW_ASSERT(Base::GetUsableLayout(allocator, nullptr).status() !=
                Status::Unimplemented());
    }
  }

  /// @copydoc Allocator::Allocate
  void* DoAllocate(Layout layout) override;

  /// @copydoc Deallocator::Deallocate
  void DoDeallocate(void* ptr) override;

  /// @copydoc Allocator::Resize
  bool DoResize(void* ptr, size_t new_size) override;

  /// @copydoc Allocator::DoBeforeReallocate
  [[nodiscard]] bool DoBeforeReallocate(void* ptr, Layout new_layout) override;

  /// @copydoc Allocator::DoAfterReallocateCopy
  void DoAfterReallocateCopy(void* ptr,
                             Layout new_layout,
                             void* new_ptr) override;

  /// @copydoc Deallocator::GetInfo
  Result<Layout> DoGetInfo(Deallocator::InfoType info_type,
                           const void* ptr) const override {
    return Base::GetFrameInfo(info_type, ptr, Layout::Of<Suffix>());
  }

  /// Returns the frame pointer from a data pointer, that is, return a pointer
  /// to memory holding the prefix, usable memory, and suffix from a pointer to
  /// the usable memory.
  /// @{
  void* GetFrame(void* data) const;
  const void* GetFrame(const void* data) const {
    return GetFrame(const_cast<void*>(data));
  }
  /// @}

  /// Returns a pointer to prefix.
  ///
  /// It is an error to call this method if the prefix type is ``void``.
  /// @{
  Prefix* GetPrefix(void* data) const;
  const Prefix* GetPrefix(const void* data) const {
    return GetPrefix(const_cast<void*>(data));
  }
  /// @}

  /// Returns a data pointer from a frame pointer.
  /// @{
  void* GetData(void* frame) const;
  const void* GetData(const void* data) const {
    return GetData(const_cast<void*>(data));
  }
  /// @}

  /// Returns a pointer to the suffix.
  ///
  /// It is an error to call this method if the suffix type is ``void``.
  /// @{
  Suffix* GetSuffix(void* data) const;
  const Suffix* GetSuffix(const void* data) const {
    return GetSuffix(const_cast<void*>(data));
  }
  /// @}

 private:
  static_assert(std::is_same_v<Prefix, void> ||
                    std::is_default_constructible_v<Prefix>,
                "prefix type must be void or default constructible");
  static_assert(std::is_same_v<Suffix, void> ||
                    std::is_default_constructible_v<Suffix>,
                "suffix type must be void or default constructible");

  static_assert(std::is_same_v<Prefix, void> ||
                    std::is_trivially_copyable_v<Prefix>,
                "prefix type must be void or trivially copyable");
  static_assert(std::is_same_v<Suffix, void> ||
                    std::is_trivially_copyable_v<Suffix>,
                "suffix type must be void or trivially copyable");

  // If Suffix is not void; this must always be true. If Suffix is void, it may
  // be true depending on whether the underlying allocator supports getting the
  // usable size of an allocation. When false, this allocator will not be able
  // to validate
  bool can_get_frame_size_;
};

/// @}

// Template method implementations.

// BaseFramingAllocator methods ////////////////////////////////////////////////

namespace internal {

template <typename ErrorHandler>
constexpr size_t BaseFramingAllocator<ErrorHandler>::Distance(const void* start,
                                                              const void* end) {
  auto* first = cpp20::bit_cast<const std::byte*>(start);
  auto* last = cpp20::bit_cast<const std::byte*>(end);
  return static_cast<size_t>(std::distance(first, last));
}

template <typename ErrorHandler>
ByteSpan BaseFramingAllocator<ErrorHandler>::AllocateFrame(Layout prefix,
                                                           Layout data,
                                                           Layout suffix) {
  // The maximum amount of memory needed includes space for the prefix, the
  // offsets, padding to align the data, the data itself, padding to align the
  // suffix, and the suffix itself. Allocate this maximum amount and use it to
  // set up a BumpAllocator that can infallibly allocate the substructures.
  //
  // The frame always includes a frame offset. Also include an extra alignment
  // to ensure the data can be aligned within the frame.
  size_t size = sizeof(size_t) + data.size() + data.alignment();

  // When a prefix is present, a data offset may also be needed.
  if (prefix.size() != 0) {
    size += AlignUp(prefix.size(), alignof(size_t)) + sizeof(size_t);
  }

  // When a suffix is present, the frame includes a suffix offset and may also
  // need extra padding to align the suffix.
  if (suffix.size() != 0) {
    size += sizeof(size_t) + suffix.alignment() + suffix.size();
  }

  Layout frame_layout{size, std::max(prefix.alignment(), alignof(size_t))};
  void* ptr = allocator().Allocate(frame_layout);
  if (ptr == nullptr) {
    return ByteSpan();
  }
  return ByteSpan(static_cast<std::byte*>(ptr), frame_layout.size());
}

template <typename ErrorHandler>
void BaseFramingAllocator<ErrorHandler>::UpdateOffsets(void* frame,
                                                       void* data,
                                                       void* suffix) {
  size_t prefix_offset = Distance(frame, data);
  auto* data_ptr = cpp20::bit_cast<size_t*>(data);
  if (suffix == nullptr) {
    *(data_ptr - 1) = prefix_offset;
  } else {
    size_t suffix_offset = Distance(data, suffix);
    *(data_ptr - 1) = suffix_offset;
    *(data_ptr - 2) = prefix_offset;
  }
}

template <typename ErrorHandler>
bool BaseFramingAllocator<ErrorHandler>::ResizeFrame(void* data,
                                                     size_t new_size,
                                                     Layout suffix,
                                                     void* tmp_suffix) {
  void* frame = GetFrame(data, tmp_suffix != nullptr);
  size_t prefix_offset = Distance(frame, data);
  if (suffix.size() == 0) {
    return CheckedIncrement(new_size, prefix_offset) &&
           allocator().Resize(frame, new_size);
  }

  auto data_addr = cpp20::bit_cast<uintptr_t>(data);
  uintptr_t new_suffix_addr = data_addr;
  if (!CheckedIncrement(new_suffix_addr, new_size)) {
    return false;
  }
  new_suffix_addr = AlignUp(new_suffix_addr, suffix.alignment());

  size_t new_suffix_offset = new_suffix_addr - data_addr;
  new_size = new_suffix_offset;

  // Cache the suffix, and don't write it until the `Resize` succeeds.
  Result<size_t> frame_size = GetFrameSize(frame);
  if (!frame_size.ok()) {
    return false;
  }
  size_t data_size = *frame_size - Distance(frame, data);
  void* suffix_ptr = GetSuffix(data, data_size, suffix);
  if (suffix_ptr == nullptr) {
    return false;
  }
  std::memcpy(tmp_suffix, suffix_ptr, suffix.size());

  if (!CheckedIncrement(new_size, prefix_offset) ||
      !CheckedIncrement(new_size, suffix.size()) ||
      !allocator().Resize(frame, new_size)) {
    return false;
  }

  auto* suffix_offset_ptr = cpp20::bit_cast<size_t*>(data) - 1;
  *suffix_offset_ptr = new_suffix_offset;

  auto* new_suffix_ptr = cpp20::bit_cast<void*>(new_suffix_addr);
  std::memcpy(new_suffix_ptr, tmp_suffix, suffix.size());
  return true;
}
template <typename ErrorHandler>
Result<Layout> BaseFramingAllocator<ErrorHandler>::GetFrameInfo(
    InfoType info_type, const void* data, Layout suffix) const {
  if (info_type == InfoType::kCapacity) {
    return GetInfo(allocator(), InfoType::kCapacity, nullptr);
  }
  if (info_type == InfoType::kRequestedLayoutOf) {
    return Status::Unimplemented();
  }

  const void* frame = GetFrame(data, suffix.size() != 0, /*checked=*/false);
  if (frame == nullptr) {
    return Status::NotFound();
  }

  PW_TRY_ASSIGN(Layout layout, GetInfo(allocator(), info_type, frame));
  if (info_type != InfoType::kUsableLayoutOf) {
    return layout;
  }

  size_t data_size = layout.size() - Distance(frame, data);
  if (suffix.size() != 0) {
    data_size = Distance(data, GetSuffix(data, data_size, suffix));
  }

  // The frame alignment may be greater than the data alignment. The largest
  // power of 2 dividing both the layout alignment and data address is the
  // rightmost bit set in those values combined.
  auto data_addr = cpp20::bit_cast<uintptr_t>(data);
  uintptr_t combined = data_addr | layout.alignment();
  return Layout(data_size, combined & (~combined + 1));
}

template <typename ErrorHandler>
void* BaseFramingAllocator<ErrorHandler>::GetFrame(void* data,
                                                   bool has_suffix,
                                                   bool checked) {
  auto data_addr = cpp20::bit_cast<uintptr_t>(data);
  if ((data_addr % alignof(size_t)) != 0) {
    if (checked) {
      ErrorHandler::HandleError(
          FrameError::kDataNotAligned, data, alignof(size_t));
    }
    return nullptr;
  }

  uintptr_t data_offset_addr = data_addr;
  size_t offset = has_suffix ? sizeof(size_t) * 2 : sizeof(size_t);
  if (!CheckedDecrement(data_offset_addr, offset)) {
    if (checked) {
      ErrorHandler::HandleError(FrameError::kDataTooSmall, data);
    }
    return nullptr;
  }

  uintptr_t frame_addr = data_addr;
  size_t data_offset = *(cpp20::bit_cast<const size_t*>(data_offset_addr));
  if (!CheckedDecrement(frame_addr, data_offset)) {
    if (checked) {
      ErrorHandler::HandleError(
          FrameError::kDataTooSmallForPrefix, data, data_offset);
    }
    return nullptr;
  }

  return cpp20::bit_cast<void*>(frame_addr);
}

template <typename ErrorHandler>
void* BaseFramingAllocator<ErrorHandler>::GetData(void* frame,
                                                  Result<size_t> frame_size,
                                                  Layout prefix) {
  if (frame == nullptr || frame_size.status() == Status::NotFound()) {
    ErrorHandler::HandleError(FrameError::kFrameNull);
    return nullptr;
  }

  auto frame_addr = cpp20::bit_cast<uintptr_t>(frame);
  if ((frame_addr % prefix.alignment()) != 0) {
    ErrorHandler::HandleError(
        FrameError::kFrameNotAligned, frame, prefix.alignment());
    return nullptr;
  }

  size_t offset = AlignUp(prefix.size(), alignof(size_t));
  if (frame_size.ok() && offset >= *frame_size) {
    ErrorHandler::HandleError(FrameError::kFrameTooSmall, frame, *frame_size);
    return nullptr;
  }

  size_t frame_offset = *(cpp20::bit_cast<const size_t*>(frame_addr + offset));
  if (frame_size.ok() && frame_offset >= *frame_size) {
    ErrorHandler::HandleError(FrameError::kFrameOffsetTooSmall,
                              frame,
                              *frame_size,
                              nullptr,
                              frame_offset);
    return nullptr;
  }
  return cpp20::bit_cast<void*>(frame_addr + frame_offset);
}

template <typename ErrorHandler>
void* BaseFramingAllocator<ErrorHandler>::GetSuffix(void* data,
                                                    size_t data_size,
                                                    Layout suffix) {
  auto data_addr = cpp20::bit_cast<uintptr_t>(data);

  // To provide both `data` and `data_size`, the caller had to call either
  // `GetData(frame)` or `GetFrame(data)`, both of which ensure this is safe.
  size_t suffix_offset = *(cpp20::bit_cast<const size_t*>(data_addr) - 1);
  if (data_size < suffix.size() || suffix_offset > data_size - suffix.size()) {
    ErrorHandler::HandleError(FrameError::kDataTooSmallForSuffix,
                              data,
                              data_size,
                              nullptr,
                              suffix_offset);
    return nullptr;
  }

  uintptr_t suffix_addr = data_addr + suffix_offset;
  if ((suffix_addr % suffix.alignment()) != 0) {
    ErrorHandler::HandleError(FrameError::kSuffixNotAligned,
                              nullptr,
                              static_cast<size_t>(suffix_addr),
                              nullptr,
                              suffix.alignment());
    return nullptr;
  }
  return cpp20::bit_cast<void*>(suffix_addr);
}

template <typename ErrorHandler>
Result<size_t> BaseFramingAllocator<ErrorHandler>::GetFrameSize(
    const void* frame) const {
  auto result = GetUsableLayout(allocator(), frame);
  if (!result.ok()) {
    if (result.status() == Status::NotFound()) {
      ErrorHandler::HandleError(FrameError::kUnrecognizedFrame, frame);
    }
    return result.status();
  }
  return result->size();
}

}  // namespace internal

// FramingAllocator methods ////////////////////////////////////////////////////

template <typename Prefix, typename Suffix, typename ErrorHandler>
void* FramingAllocator<Prefix, Suffix, ErrorHandler>::DoAllocate(
    Layout layout) {
  ByteSpan frame =
      Base::AllocateFrame(Layout::Of<Prefix>(), layout, Layout::Of<Suffix>());
  if (frame.empty()) {
    return nullptr;
  }
  BumpAllocator bump_allocator(frame);

  // Allocate the prefix, and the frame-relative prefix offset.
  if constexpr (!std::is_same_v<Prefix, void>) {
    std::ignore = bump_allocator.New<Prefix>();
  }
  auto* frame_offset_ptr = bump_allocator.New<size_t>();

  // Allocate a suffix offset. Note that this may not be the memory that ends up
  // being used for the suffix offset, depending on how much padding is needed
  // for alignment. All we know for now is that memory for at least one more
  // `size_t` needs to be reserved.
  if constexpr (!std::is_same_v<Suffix, void>) {
    std::ignore = bump_allocator.New<size_t>();
  }

  // Allocate the data itself.
  auto* data = bump_allocator.Allocate(layout);
  *frame_offset_ptr = Base::Distance(frame.data(), data);

  // Allocate the suffix.
  void* suffix = nullptr;
  if constexpr (!std::is_same_v<Suffix, void>) {
    suffix = bump_allocator.New<Suffix>();
  }

  // Store the data-relative and suffix offsets.
  Base::UpdateOffsets(frame.data(), data, suffix);

  // Trim and return any excess memory.
  size_t used = frame.size() - bump_allocator.remaining();
  std::ignore = Base::allocator().Resize(frame.data(), used);
  return data;
}

template <typename Prefix, typename Suffix, typename ErrorHandler>
void FramingAllocator<Prefix, Suffix, ErrorHandler>::DoDeallocate(void* ptr) {
  // Prefix and Suffix are void or trivially copyable, which implies they are
  // also trivially destructible. As a result, there is no need to Destroy them.
  Base::allocator().Deallocate(GetFrame(ptr));
}

template <typename Prefix, typename Suffix, typename ErrorHandler>
bool FramingAllocator<Prefix, Suffix, ErrorHandler>::DoResize(void* ptr,
                                                              size_t new_size) {
  if constexpr (std::is_same_v<Suffix, void>) {
    return Base::ResizeFrame(ptr, new_size, Layout(), nullptr);

  } else {
    Suffix tmp;
    return Base::ResizeFrame(ptr, new_size, Layout::Of<Suffix>(), &tmp);
  }
}

template <typename Prefix, typename Suffix, typename ErrorHandler>
bool FramingAllocator<Prefix, Suffix, ErrorHandler>::DoBeforeReallocate(
    void* ptr, Layout new_layout) {
  void* frame = GetFrame(ptr);
  if (frame == nullptr) {
    return false;
  }
  return Base::DoBeforeReallocate(frame, new_layout);
}

template <typename Prefix, typename Suffix, typename ErrorHandler>
void FramingAllocator<Prefix, Suffix, ErrorHandler>::DoAfterReallocateCopy(
    void* ptr, Layout new_layout, void* new_ptr) {
  void* frame = Base::GetFrame(ptr, !std::is_same_v<Suffix, void>);
  if (ptr == new_ptr) {
    // Prefix unchanged, ResizeFrame already moved the Suffix.
    Base::DoAfterReallocateCopy(frame, new_layout, frame);
    return;
  }

  void* new_frame = Base::GetFrame(new_ptr, !std::is_same_v<Suffix, void>);
  Base::DoAfterReallocateCopy(frame, new_layout, new_frame);

  if constexpr (!std::is_same_v<Prefix, void>) {
    std::memcpy(new_frame, frame, sizeof(Prefix));
  }
  if constexpr (!std::is_same_v<Suffix, void>) {
    Result<size_t> frame_size = Base::GetFrameSize(frame);
    PW_ASSERT(frame_size.ok());
    size_t data_size = *frame_size - Base::Distance(frame, ptr);
    void* suffix = Base::GetSuffix(ptr, data_size, Layout::Of<Suffix>());

    Result<size_t> new_frame_size = Base::GetFrameSize(new_frame);
    PW_ASSERT(new_frame_size.ok());
    size_t new_data_size = *new_frame_size - Base::Distance(new_frame, new_ptr);
    void* new_suffix =
        Base::GetSuffix(new_ptr, new_data_size, Layout::Of<Suffix>());

    std::memcpy(new_suffix, suffix, sizeof(Suffix));
  }
}

template <typename Prefix, typename Suffix, typename ErrorHandler>
void* FramingAllocator<Prefix, Suffix, ErrorHandler>::GetFrame(
    void* data) const {
  void* frame = Base::GetFrame(data, !std::is_same_v<Suffix, void>);
  if (frame == nullptr) {
    return nullptr;
  }
  Result<size_t> frame_size = Base::GetFrameSize(frame);
  if (frame_size.status() == Status::NotFound()) {
    return nullptr;
  }
  return frame;
}

template <typename Prefix, typename Suffix, typename ErrorHandler>
void* FramingAllocator<Prefix, Suffix, ErrorHandler>::GetData(
    void* frame) const {
  return Base::GetData(frame, Base::GetFrameSize(frame), Layout::Of<Prefix>());
}

template <typename Prefix, typename Suffix, typename ErrorHandler>
auto FramingAllocator<Prefix, Suffix, ErrorHandler>::GetPrefix(void* data) const
    -> Prefix* {
  static_assert(!std::is_same_v<Prefix, void>, "prefix type is void");
  return cpp20::bit_cast<Prefix*>(GetFrame(data));
}

template <typename Prefix, typename Suffix, typename ErrorHandler>
auto FramingAllocator<Prefix, Suffix, ErrorHandler>::GetSuffix(void* data) const
    -> Suffix* {
  static_assert(!std::is_same_v<Suffix, void>, "suffix type is void");
  void* frame = Base::GetFrame(data, true);
  Result<size_t> frame_size = Base::GetFrameSize(frame);
  if (!frame_size.ok()) {
    return nullptr;
  }
  size_t data_size = *frame_size - Base::Distance(frame, data);
  void* suffix = Base::GetSuffix(data, data_size, Layout::Of<Suffix>());
  return cpp20::bit_cast<Suffix*>(suffix);
}

}  // namespace pw::allocator
