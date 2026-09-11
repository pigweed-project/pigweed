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

namespace pw::allocator {
namespace internal {

/// Base type that simply provides better assertion messages.
class BaseFramingAllocator : public ForwardingAllocator {
 protected:
  constexpr explicit BaseFramingAllocator(Allocator& allocator)
      : ForwardingAllocator(allocator) {}

  /// Triggers an assertion indicating that the given data pointer is not
  /// properly aligned if ``strict`` is true; otherwise returns ``false``.
  [[nodiscard]] static bool CrashOnUnalignedIfStrict(bool strict,
                                                     const void* data);

  /// Triggers an assertion indicating that the given data pointer cannot be
  /// converted to a frame if ``strict`` is true; otherwise returns ``false``.
  [[nodiscard]] static bool CrashOnBadDataIfStrict(bool strict,
                                                   const void* data);

  /// Triggers an assertion indicating that the given prefix offset appears
  /// corrupted if ``strict`` is true; otherwise returns ``false``.
  [[nodiscard]] static bool CrashOnBadPrefixOffsetIfStrict(bool strict,
                                                           const void* data,
                                                           size_t prefix_offset,
                                                           size_t min_size = 0);

  /// Triggers an assertion indicating that the prefix offsets do not match if
  /// ``strict`` is true; otherwise returns ``false``.
  [[nodiscard]] static bool CrashOnWrongPrefixOffsetIfStrict(
      bool strict,
      const void* data,
      size_t data_prefix_offset,
      const void* frame,
      size_t frame_prefix_offset);

  /// Triggers an assertion indicating that the given prefix offset appears
  /// corrupted if ``strict`` is true; otherwise returns ``false``.
  [[nodiscard]] static bool CrashOnBadSuffixOffsetIfStrict(bool strict,
                                                           const void* data,
                                                           size_t suffix_offset,
                                                           size_t usable_size);

  /// Triggers an assertion indicating that the given frame pointer is not
  /// recognized by the underlying allocator if ``strict`` is true; otherwise
  /// returns ``false``.
  [[nodiscard]] static bool CrashOnUnrecognized(bool strict, const void* frame);
};

}  // namespace internal

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
/// | 0x...10 | size_t      | frame_offset =0x10 | GetFrameOffsetPtr(frame) |
/// +---------+-------------+--------------------+--------------------------+
/// | 0x...14 | N/A         | 8 bytes of padding | N/A                      |
/// +---------+-------------+--------------------+--------------------------+
/// | 0x...1C | size_t      | data_offset  =0x10 | GetDataOffsetPtr(data)   |
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
/// @tparam   Prefix  A default constructible and trivially copyable type to
///                   place before each allocation, or `void`. `alignof(Prefix)`
///                   must be at most `alignof(size_t)`.
/// @tparam   Suffix  A default constructible and trivially copyable type to
///                   place after each allocation, or `void`.
template <typename Prefix, typename Suffix = void>
class FramingAllocator : public internal::BaseFramingAllocator {
 private:
  using Base = internal::BaseFramingAllocator;

  static_assert(!std::is_same_v<Prefix, void> || !std::is_same_v<Suffix, void>,
                "prefix and suffix types cannot both be null");

 protected:
  /// Returns the result of framing the given layout with a prefix and suffix.
  ///
  /// This is conservative and pessimistic; the returned layout is always large
  /// enough for framing to succeed. Methods like `DoAllocate`will free extra
  /// memory that isn't needed.
  static constexpr Layout GetFrameLayout(Layout layout);

  constexpr explicit FramingAllocator(Allocator& allocator)
      : internal::BaseFramingAllocator(allocator) {}

  /// @copydoc Allocator::Allocate
  void* DoAllocate(Layout layout) override;

  /// @copydoc Deallocator::Deallocate
  void DoDeallocate(void* ptr) override;

  /// @copydoc Allocator::Resize
  bool DoResize(void* ptr, size_t new_size) override;

  /// @copydoc Allocator::DoBeforeReallocate
  void DoBeforeReallocate(void* ptr, Layout new_layout) override;

  /// @copydoc Allocator::DoAfterReallocateCopy
  void DoAfterReallocateCopy(void* ptr,
                             Layout new_layout,
                             void* new_ptr) override;

  /// @copydoc Deallocator::GetInfo
  Result<Layout> DoGetInfo(InfoType info_type, const void* ptr) const override;

  /// Returns whether the given data pointer correspond to a valid frame from
  /// this allocator. If ``strict`` is true, does not return when invalid and
  [[nodiscard]] bool IsValid(const void* data) const;

  /// Returns the frame pointer from a data pointer, that is, return a pointer
  /// to memory holding the prefix, usable memory, and suffix from a pointer to
  /// the usable memory.
  void* GetFrame(const void* data) const;

  /// Returns a pointer to prefix.
  ///
  /// It is an error to call this method if the prefix type is ``void``.
  Prefix* GetPrefix(void* data) const;

  /// Returns a data pointer from a frame pointer.
  static void* GetData(void* frame);

  /// Returns a pointer to the suffix.
  ///
  /// It is an error to call this method if the suffix type is ``void``.
  static Suffix* GetSuffix(void* data);

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

  /// Triggers an assertion if the given data pointer does not correspond to a
  /// valid frame from this allocator.
  void CheckFrame(const void* data) const;

  /// Returns whether the given data pointer correspond to a valid frame from
  /// this allocator.
  bool ValidateFrame(const void* data, bool strict) const;

  /// Returns the location of the frame offset for the given frame pointer.
  static size_t* GetFrameOffsetPtr(const void* frame);

  /// Returns the frame offset for the given frame pointer.
  static size_t GetFrameOffset(const void* frame) {
    return *(GetFrameOffsetPtr(frame));
  }

  /// Returns the location of the data offset for the given data pointer.
  static size_t* GetDataOffsetPtr(const void* data);

  /// Returns the data offset for the given data pointer.
  static size_t GetDataOffset(const void* data) {
    return *(GetDataOffsetPtr(data));
  }

  /// Returns the location of the suffix offset for the given data pointer.
  static size_t* GetSuffixOffsetPtr(const void* data);

  /// Returns the suffix offset for the given data pointer.
  ///
  /// It is an error to call this method if the suffix type is ``void``.
  static size_t GetSuffixOffset(const void* data) {
    return *(GetSuffixOffsetPtr(data));
  }
};

// Template method implementations.

template <typename Prefix, typename Suffix>
constexpr Layout FramingAllocator<Prefix, Suffix>::GetFrameLayout(
    Layout layout) {
  // Overestimate and determine a minimum layout that will always succeed.
  // The excess memory can be returned by, e.g., `DoAllocate`.
  // The frame always includes a frame offset. Also include an extra alignment
  // to ensure the data can be aligned within the frame.
  size_t size = sizeof(size_t) + layout.size() + layout.alignment();

  // When a prefix is present, a data offset may also be needed.
  if constexpr (!std::is_same_v<Prefix, void>) {
    static_assert(alignof(Prefix) <= alignof(size_t),
                  "alignof(Prefix) cannot exceed alignof(size_t)");
    size += AlignUp(sizeof(Prefix), alignof(size_t)) + sizeof(size_t);
  }

  // When a suffix is present, the frame includes a suffix offset and may also
  // need extra padding to align the suffix.
  if constexpr (!std::is_same_v<Suffix, void>) {
    size += sizeof(size_t) + alignof(Suffix) + sizeof(Suffix);
  }

  return Layout{size, alignof(size_t)};
}

template <typename Prefix, typename Suffix>
void* FramingAllocator<Prefix, Suffix>::DoAllocate(Layout layout) {
  // The maximum amount of memory needed includes space for the prefix, the
  // offsets, padding to align the data, the data itself, padding to align the
  // suffix, and the suffix itself. Allocate this maximum amount and use it to
  // set up a BumpAllocator that can infallibly allocate the substructures.
  Layout frame_layout = GetFrameLayout(layout);
  auto* frame = static_cast<std::byte*>(allocator().Allocate(frame_layout));
  if (frame == nullptr) {
    return nullptr;
  }
  uintptr_t frame_addr = reinterpret_cast<uintptr_t>(frame);
  BumpAllocator bump_allocator({frame, frame_layout.size()});

  // Allocate the prefix, and the frame-relative prefix offset.
  if constexpr (!std::is_same_v<Prefix, void>) {
    std::ignore = bump_allocator.New<Prefix>();
  }
  size_t* frame_offset_ptr = bump_allocator.New<size_t>();

  // Allocate a suffix offset. Note that this may not be the memory that ends up
  // being used for the suffix offset, depending on how much padding is needed
  // for alignment. All we know for now is that memory for at least one more
  // `size_t` needs to be reserved.
  if constexpr (!std::is_same_v<Suffix, void>) {
    std::ignore = bump_allocator.New<size_t>();
  }

  auto* data = bump_allocator.Allocate(layout);
  uintptr_t data_addr = reinterpret_cast<uintptr_t>(data);
  auto prefix_offset = static_cast<size_t>(data_addr - frame_addr);

  // Now, update the frame-relative prefix offset. Also, determine where the
  // data-relative prefix offset is. It may line up with the frame-relative
  // prefix offset. If if does not, due to data alignment, store the prefix
  // offset there as well.
  *frame_offset_ptr = prefix_offset;
  auto* data_offset_ptr = GetDataOffsetPtr(data);
  if (data_offset_ptr != frame_offset_ptr) {
    *data_offset_ptr = prefix_offset;
  }

  // Allocate the suffix, and store the data-relative suffix offset.
  if constexpr (!std::is_same_v<Suffix, void>) {
    auto* suffix = bump_allocator.New<Suffix>();
    uintptr_t suffix_addr = reinterpret_cast<uintptr_t>(suffix);
    auto suffix_offset = static_cast<size_t>(suffix_addr - data_addr);

    auto* suffix_offset_ptr = GetSuffixOffsetPtr(data);
    *suffix_offset_ptr = suffix_offset;
  }

  // Trim and return any excess memory.
  size_t used = frame_layout.size() - bump_allocator.remaining();
  std::ignore = allocator().Resize(frame, used);
  return data;
}

template <typename Prefix, typename Suffix>
void FramingAllocator<Prefix, Suffix>::DoDeallocate(void* ptr) {
  if constexpr (!std::is_same_v<Prefix, void>) {
    allocator().Destroy(GetPrefix(ptr), 1);
  }
  if constexpr (!std::is_same_v<Suffix, void>) {
    allocator().Destroy(GetSuffix(ptr), 1);
  }
  allocator().Deallocate(GetFrame(ptr));
}

template <typename Prefix, typename Suffix>
bool FramingAllocator<Prefix, Suffix>::DoResize(void* ptr, size_t new_size) {
  void* frame = GetFrame(ptr);
  size_t data_offset = GetDataOffset(ptr);
  new_size += data_offset;

  if constexpr (!std::is_same_v<Suffix, void>) {
    // Cache the suffix, and don't write it until the `Resize` succeeds.
    Suffix tmp_suffix;
    std::memcpy(&tmp_suffix, GetSuffix(ptr), sizeof(Suffix));

    new_size = AlignUp(new_size, alignof(Suffix));
    auto* suffix_offset_ptr = GetSuffixOffsetPtr(ptr);
    size_t new_suffix_offset = new_size - data_offset;
    new_size += sizeof(Suffix);

    if (!allocator().Resize(frame, new_size)) {
      return false;
    }
    *suffix_offset_ptr = new_suffix_offset;
    std::memcpy(GetSuffix(ptr), &tmp_suffix, sizeof(Suffix));
    return true;

  } else {
    return allocator().Resize(frame, new_size);
  }
}

template <typename Prefix, typename Suffix>
void FramingAllocator<Prefix, Suffix>::DoBeforeReallocate(void* ptr,
                                                          Layout new_layout) {
  Base::DoBeforeReallocate(GetFrame(ptr), new_layout);
}

template <typename Prefix, typename Suffix>
void FramingAllocator<Prefix, Suffix>::DoAfterReallocateCopy(void* ptr,
                                                             Layout new_layout,
                                                             void* new_ptr) {
  Base::DoAfterReallocateCopy(GetFrame(ptr), new_layout, GetFrame(new_ptr));
  if constexpr (!std::is_same_v<Prefix, void>) {
    std::memcpy(GetPrefix(new_ptr), GetPrefix(ptr), sizeof(Prefix));
  }
  if constexpr (!std::is_same_v<Suffix, void>) {
    std::memcpy(GetSuffix(new_ptr), GetSuffix(ptr), sizeof(Suffix));
  }
}

template <typename Prefix, typename Suffix>
Result<Layout> FramingAllocator<Prefix, Suffix>::DoGetInfo(
    InfoType info_type, const void* ptr) const {
  if (info_type != InfoType::kCapacity) {
    ptr = GetFrame(ptr);
  }
  return GetInfo(allocator(), info_type, ptr);
}

template <typename Prefix, typename Suffix>
bool FramingAllocator<Prefix, Suffix>::IsValid(const void* data) const {
  return ValidateFrame(data, /*strict=*/false);
}

template <typename Prefix, typename Suffix>
auto FramingAllocator<Prefix, Suffix>::GetFrame(const void* data) const
    -> void* {
  if constexpr (Hardening::kIncludesDebugChecks) {
    CheckFrame(data);
  }
  uintptr_t addr = reinterpret_cast<uintptr_t>(data);
  PW_ASSERT(CheckedDecrement(addr, GetDataOffset(data)));
  return reinterpret_cast<void*>(addr);
}

template <typename Prefix, typename Suffix>
auto FramingAllocator<Prefix, Suffix>::GetPrefix(void* data) const -> Prefix* {
  static_assert(!std::is_same_v<Prefix, void>, "prefix type is void");
  return reinterpret_cast<Prefix*>(GetFrame(data));
}

template <typename Prefix, typename Suffix>
void* FramingAllocator<Prefix, Suffix>::GetData(void* frame) {
  auto addr = reinterpret_cast<uintptr_t>(frame);
  PW_ASSERT(CheckedIncrement(addr, GetFrameOffset(frame)));
  return reinterpret_cast<void*>(addr);
}

template <typename Prefix, typename Suffix>
auto FramingAllocator<Prefix, Suffix>::GetSuffix(void* data) -> Suffix* {
  static_assert(!std::is_same_v<Suffix, void>, "suffix type is void");
  uintptr_t addr = reinterpret_cast<uintptr_t>(data);
  PW_ASSERT(CheckedIncrement(addr, GetSuffixOffset(data)));
  return reinterpret_cast<Suffix*>(addr);
}

template <typename Prefix, typename Suffix>
void FramingAllocator<Prefix, Suffix>::CheckFrame(const void* data) const {
  std::ignore = ValidateFrame(data, /*strict=*/true);
}

template <typename Prefix, typename Suffix>
bool FramingAllocator<Prefix, Suffix>::ValidateFrame(const void* data,
                                                     bool strict) const {
  if (!IsAlignedAs<size_t>(data)) {
    return CrashOnUnalignedIfStrict(strict, data);
  }

  size_t offset =
      std::is_same_v<Suffix, void> ? sizeof(size_t) : sizeof(size_t) * 2;
  if (reinterpret_cast<uintptr_t>(data) < offset) {
    return CrashOnBadDataIfStrict(strict, data);
  }

  size_t data_offset = GetDataOffset(data);
  if (data_offset % alignof(size_t) != 0) {
    return CrashOnBadPrefixOffsetIfStrict(strict, data, data_offset);
  }

  uintptr_t addr = reinterpret_cast<uintptr_t>(data);
  if (!CheckedDecrement(addr, data_offset)) {
    return CrashOnBadPrefixOffsetIfStrict(strict, data, data_offset);
  }
  void* frame = reinterpret_cast<void*>(addr);
  if (allocator().HasCapability(kImplementsRecognizes) &&
      !Recognizes(allocator(), frame)) {
    return CrashOnUnrecognized(strict, frame);
  }
  size_t frame_offset = GetFrameOffset(frame);
  if (data_offset != frame_offset) {
    return CrashOnWrongPrefixOffsetIfStrict(
        strict, data, data_offset, frame, frame_offset);
  }

  if constexpr (!std::is_same_v<Prefix, void>) {
    if (data_offset < sizeof(Prefix)) {
      return CrashOnBadPrefixOffsetIfStrict(
          strict, data, data_offset, sizeof(Prefix));
    }
  }

  if constexpr (!std::is_same_v<Suffix, void>) {
    size_t suffix_offset = GetSuffixOffset(data);
    auto result = GetUsableLayout(allocator(), frame);
    if (result.ok() && (result->size() < sizeof(Suffix) ||
                        result->size() - sizeof(Suffix) < suffix_offset)) {
      return CrashOnBadSuffixOffsetIfStrict(
          strict, data, suffix_offset, result->size());
    }
  }

  return true;
}

template <typename Prefix, typename Suffix>
size_t* FramingAllocator<Prefix, Suffix>::GetFrameOffsetPtr(const void* frame) {
  uintptr_t addr = reinterpret_cast<uintptr_t>(frame);
  if constexpr (!std::is_same_v<Prefix, void>) {
    PW_ASSERT(CheckedIncrement(addr, AlignUp(sizeof(Prefix), alignof(size_t))));
  }
  return reinterpret_cast<size_t*>(addr);
}

template <typename Prefix, typename Suffix>
size_t* FramingAllocator<Prefix, Suffix>::GetDataOffsetPtr(const void* data) {
  size_t offset;
  if constexpr (std::is_same_v<Suffix, void>) {
    offset = sizeof(size_t);
  } else {
    offset = sizeof(size_t) * 2;
  }
  auto addr = reinterpret_cast<uintptr_t>(data);
  PW_ASSERT(CheckedDecrement(addr, offset));
  return reinterpret_cast<size_t*>(addr);
}

template <typename Prefix, typename Suffix>
size_t* FramingAllocator<Prefix, Suffix>::GetSuffixOffsetPtr(const void* data) {
  static_assert(!std::is_same_v<Suffix, void>, "suffix type is void");
  return reinterpret_cast<size_t*>(const_cast<void*>(data)) - 1;
}

}  // namespace pw::allocator
