// Copyright 2024 The Pigweed Authors
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
#include <new>
#include <type_traits>

#include "lib/stdcompat/bit.h"
#include "pw_allocator/hardening.h"
#include "pw_bytes/alignment.h"
#include "pw_bytes/span.h"
#include "pw_result/result.h"
#include "pw_status/status.h"

namespace pw::allocator {
namespace internal {

/// Helper type that copies const-qualifers between types.
template <typename From, typename To>
struct CopyConst {
  using type = std::conditional_t<std::is_const_v<From>,
                                  std::add_const_t<To>,
                                  std::remove_const_t<To>>;
};

template <typename From, typename To>
using copy_const_t = typename CopyConst<From, To>::type;

template <typename From, typename To>
using copy_const_ptr_t =
    std::add_pointer_t<typename CopyConst<std::remove_pointer_t<From>,
                                          std::remove_pointer_t<To>>::type>;

// Trivial base class for trait support.
struct BasicBase {};

}  // namespace internal

/// @submodule{pw_allocator,block_mixins}

/// Base mix-in for block implementations.
///
/// This CRTP-style type can be combined with block mix-in types. Block mix-ins
/// are stateless and trivially constructible. Mix-ins require the derived class
/// to implement methods to access and modify state, such has how to return a
/// block's size or a pointer to its usable memory.
///
/// The mix-ins also follow the NVI pattern. This allows mix-ins to layer
/// behavior for a particular method. For example, the implementation of
/// `AllocFirst` in `AlignableBlock` handles allocation with larger than default
/// alignment requirements, and delegates to `AllocatableBlock` for other
/// requests. The derived type provided by the concrete block implementation can
/// implement the public method that delegates to the correct mix-in.
/// Overridable methods are named `Do...`.
///
/// These mix-ins guarantee a number of invariants hold at the beginning and end
/// of each regular public API call. Each mix-in can enforce invariants by
/// overriding `DoCheckInvariants`. The concrete block implementation should
/// override the same method by calling each of the relevant mix-in methods.
/// Calling a public API method within an implementation of `DoCheckInvariants`
/// will lead to infinite recursion. To avoid this, mix-ins provide and/or
/// require versions of methods named `...Unchecked` that skip checking
/// invariants. These should only be used within the context of
/// `DoCheckInvariants` or other `...Unchecked` methods.
///
/// This mix-in requires its derived type provide the following symbols:
///
/// - static constexpr size_t DefaultAlignment()
///   - Returns the alignment of the block type. Must be a power of two.
/// - static constexpr size_t BlockOverhead()
///   - Returns the size of the metadata at the start of a block, before its
///     usable space.
/// - static constexpr size_t MaxAddressableSize()
///   - Size of the largest region that can be addressed by a block.
/// - static constexpr size_t MinInnerSize()
///   - Returns the minimum inner size of a block. Can be 1 unless the usable
///     space is used to track blocks when they are free.
/// - static Derived* AsBlock(BytesSpan)
///   - Instantiates and returns a block for the given region of memory.
/// - size_t OuterSizeUnchecked() const
///   - Returns the size of the block. Must be multiple of `kAlignment`.
template <typename Derived>
class BasicBlock : public internal::BasicBase {
 public:
  static constexpr size_t kAlignment = Derived::DefaultAlignment();
  static constexpr size_t kBlockOverhead =
      AlignUp(Derived::BlockOverhead(), kAlignment);
  static constexpr size_t kMinOuterSize =
      kBlockOverhead + AlignUp(Derived::MinInnerSize(), kAlignment);

  ~BasicBlock() = default;

  // No copy or move.
  BasicBlock(const BasicBlock& other) = delete;
  BasicBlock& operator=(const BasicBlock& other) = delete;

  /// @brief Creates the first block for a given memory region.
  ///
  /// @returns @Result{a block representing the region}
  /// * @OK: Returns a block representing the region.
  /// * @INVALID_ARGUMENT: The region is null.
  /// * @RESOURCE_EXHAUSTED: The region is too small for a block.
  /// * @OUT_OF_RANGE: The region is larger than `kMaxAddressableSize`.
  static constexpr Result<Derived*> Init(ByteSpan region);

  /// @returns  A pointer to a `Block`, given a pointer to the start of the
  ///           usable space inside the block.
  ///
  /// This is the inverse of `UsableSpace()`.
  ///
  /// @warning  This method does not do any checking; passing a random
  ///           pointer will return a non-null pointer.
  template <typename Ptr>
  static constexpr internal::copy_const_ptr_t<Ptr, Derived*> FromUsableSpace(
      Ptr usable_space);

  /// @returns A pointer to the usable space inside this block.
  constexpr std::byte* UsableSpace();
  constexpr const std::byte* UsableSpace() const;
  constexpr std::byte* UsableSpaceUnchecked() {
    return UsableSpaceUncheckedImpl(this);
  }
  constexpr const std::byte* UsableSpaceUnchecked() const {
    return UsableSpaceUncheckedImpl(this);
  }

  /// @returns The outer size of a block from the corresponding inner size.
  static constexpr size_t OuterSizeFromInnerSize(size_t inner_size);

  /// @returns The inner size of a block from the corresponding outer size.
  static constexpr size_t InnerSizeFromOuterSize(size_t outer_size);

  /// @returns The total size of the block in bytes, including the header.
  constexpr size_t OuterSize() const;

  /// @returns The number of usable bytes inside the block.
  constexpr size_t InnerSize() const;
  constexpr size_t InnerSizeUnchecked() const;

  /// @return whether a block is valid.
  constexpr bool IsValid() const;

  /// Like `IsValid`, but crashes if invalid.
  constexpr bool CheckInvariants() const;

 protected:
  constexpr BasicBlock() = default;

  /// Checks that the various block conditions that should always be true are
  /// indeed true.
  ///
  /// Triggers a fatal error if `strict` is true.
  constexpr bool DoCheckInvariants(bool strict) const;

 private:
  constexpr const Derived* derived() const {
    return static_cast<const Derived*>(this);
  }

  /// Static version of `UsableSpace` that preserves constness.
  template <typename Ptr>
  static constexpr internal::copy_const_ptr_t<Ptr, std::byte*>
  UsableSpaceUncheckedImpl(Ptr block);
};

/// Trait type that allows interrogating whether a type is a block.
///
template <typename T>
struct is_block : std::is_base_of<internal::BasicBase, T> {};

/// Helper variable template for `is_block<T>::value`.
template <typename T>
constexpr bool is_block_v = is_block<T>::value;

/// @}

namespace internal {

/// Crashes with an error message about the given block being misaligned if
/// `is_aligned` is false.
void CheckMisaligned(const void* block, bool is_aligned);

}  // namespace internal

// Template method implementations.

template <typename Derived>
constexpr Result<Derived*> BasicBlock<Derived>::Init(ByteSpan region) {
  region = GetAlignedSubspan(region, Derived::kAlignment);
  if (region.size() <= Derived::kBlockOverhead) {
    return Status::ResourceExhausted();
  }
  if (region.size() > Derived::MaxAddressableSize()) {
    return Status::OutOfRange();
  }
  auto* block = Derived::AsBlock(region);
  if constexpr (Hardening::kIncludesDebugChecks) {
    block->CheckInvariants();
  }
  return block;
}

template <typename Derived>
template <typename Ptr>
constexpr internal::copy_const_ptr_t<Ptr, Derived*>
BasicBlock<Derived>::FromUsableSpace(Ptr usable_space) {
  using BlockPtr = internal::copy_const_ptr_t<Ptr, Derived*>;
  auto addr = cpp20::bit_cast<uintptr_t>(usable_space);
  Hardening::Decrement(addr, kBlockOverhead);
  auto* block = std::launder(reinterpret_cast<BlockPtr>(addr));
  if constexpr (Hardening::kIncludesBasicChecks) {
    block->CheckInvariants();
  }
  return block;
}

template <typename Derived>
constexpr std::byte* BasicBlock<Derived>::UsableSpace() {
  if constexpr (Hardening::kIncludesDebugChecks) {
    CheckInvariants();
  }
  return UsableSpaceUnchecked();
}

template <typename Derived>
constexpr const std::byte* BasicBlock<Derived>::UsableSpace() const {
  if constexpr (Hardening::kIncludesDebugChecks) {
    CheckInvariants();
  }
  return UsableSpaceUnchecked();
}

template <typename Derived>
template <typename Ptr>
constexpr internal::copy_const_ptr_t<Ptr, std::byte*>
BasicBlock<Derived>::UsableSpaceUncheckedImpl(Ptr block) {
  using BytePtr = internal::copy_const_ptr_t<Derived, std::byte*>;
  auto addr = cpp20::bit_cast<uintptr_t>(block);
  Hardening::Increment(addr, kBlockOverhead);
  return cpp20::bit_cast<BytePtr>(addr);
}

template <typename Derived>
constexpr size_t BasicBlock<Derived>::OuterSizeFromInnerSize(
    size_t inner_size) {
  size_t outer_size = inner_size;
  Hardening::Increment(outer_size, kBlockOverhead);
  return outer_size;
}

template <typename Derived>
constexpr size_t BasicBlock<Derived>::InnerSizeFromOuterSize(
    size_t outer_size) {
  size_t inner_size = outer_size;
  Hardening::Decrement(inner_size, kBlockOverhead);
  return inner_size;
}

template <typename Derived>
constexpr size_t BasicBlock<Derived>::OuterSize() const {
  if constexpr (Hardening::kIncludesDebugChecks) {
    CheckInvariants();
  }
  return derived()->OuterSizeUnchecked();
}

template <typename Derived>
constexpr size_t BasicBlock<Derived>::InnerSize() const {
  if constexpr (Hardening::kIncludesDebugChecks) {
    CheckInvariants();
  }
  return InnerSizeUnchecked();
}

template <typename Derived>
constexpr size_t BasicBlock<Derived>::InnerSizeUnchecked() const {
  return InnerSizeFromOuterSize(derived()->OuterSizeUnchecked());
}

template <typename Derived>
constexpr bool BasicBlock<Derived>::IsValid() const {
  return derived()->DoCheckInvariants(/*strict=*/false);
}

template <typename Derived>
constexpr bool BasicBlock<Derived>::CheckInvariants() const {
  return derived()->DoCheckInvariants(/*strict=*/true);
}

template <typename Derived>
constexpr bool BasicBlock<Derived>::DoCheckInvariants(bool strict) const {
  bool is_aligned = (cpp20::bit_cast<uintptr_t>(this) % kAlignment) == 0;
  if constexpr (Hardening::kIncludesDebugChecks) {
    internal::CheckMisaligned(this, is_aligned || !strict);
  }
  return is_aligned;
}

}  // namespace pw::allocator
