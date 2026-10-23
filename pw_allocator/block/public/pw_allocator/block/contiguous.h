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
#include <type_traits>

#include "lib/stdcompat/bit.h"
#include "pw_allocator/block/basic.h"
#include "pw_allocator/hardening.h"
#include "pw_bytes/span.h"

namespace pw::allocator {
namespace internal {

// Trivial base class for trait support.
struct ContiguousBase {};

}  // namespace internal

/// @submodule{pw_allocator,block_mixins}

/// Mix-in for blocks that collectively represent a contiguous region of memory.
///
/// Contiguous blocks can be split into smaller blocks and merged when adjacent.
///
/// Block mix-ins are stateless and trivially constructible. See `BasicBlock`
/// for details on how mix-ins can be combined to implement blocks.
///
/// This mix-in requires its derived type also derive from `BasicBlock`, and
/// provide the following symbols:
///
/// - size_t PrevOuterSizeUnchecked() const
///   - Returns the outer size of the previous block, if any, or zero.  Must be
///     multiple of `kAlignment`.
/// - bool IsLastUnchecked() const
///   - Returns whether this block is the last block.
template <typename Derived>
class ContiguousBlock : public internal::ContiguousBase {
 protected:
  constexpr explicit ContiguousBlock() {
    static_assert(
        is_block_v<Derived>,
        "Types derived from ContiguousBlock must also derive from BasicBlock");
  }

 public:
  /// @returns the block immediately before this one, or null if this is the
  /// first block.
  constexpr Derived* Prev() const;

  /// @returns the block immediately after this one, or null if this is the last
  /// block.
  constexpr Derived* Next() const;

 protected:
  /// Split a block into two smaller blocks.
  ///
  /// This method splits a block into a leading block of the given
  /// `new_inner_size` and a trailing block, and returns the trailing space as a
  /// new block.
  ///
  /// @pre The block must not be in use.
  /// @pre The block must have enough usable space for the requested size.
  /// @pre The space remaining after a split can hold a new block.
  constexpr Derived* DoSplitFirst(size_t new_inner_size);

  /// Split a block into two smaller blocks.
  ///
  /// This method splits a block into a leading block and a trailing block of
  /// the given `new_inner_size`, and returns the trailing space is returned as
  /// a new block.
  ///
  /// @pre The block must not be in use.
  /// @pre The block must have enough usable space for the requested size.
  /// @pre The space remaining after a split can hold a new block.
  constexpr Derived* DoSplitLast(size_t new_inner_size);

  /// Merges this block with next block.
  ///
  /// This method is static in order to consume and replace the given block
  /// pointer with a pointer to the new, larger block.
  ///
  /// @pre The blocks must not be in use.
  constexpr void DoMergeNext();

  /// Performs the ContiguousBlock invariant checks.
  constexpr bool DoCheckInvariants(bool strict) const;

 private:
  constexpr Derived* derived() { return static_cast<Derived*>(this); }
  constexpr const Derived* derived() const {
    return static_cast<const Derived*>(this);
  }

  /// @copydoc Prev
  constexpr Derived* PrevUnchecked() const;

  /// @copydoc Next
  constexpr Derived* NextUnchecked() const;

  /// Split a block into two smaller blocks.
  ///
  /// This method is static in order to consume and replace the given block
  /// pointer with a pointer to the new, smaller block with an inner size of
  /// `new_inner_size` The remaining space will be returned as a new block.
  ///
  /// @pre The block must not be in use.
  /// @pre The block must have enough usable space for the requested size.
  /// @pre The space remaining after a split can hold a new block.
  static constexpr Derived* Split(Derived*& block, size_t new_inner_size);

  // PoisonableBlock calls DoSplitFirst, DoSplitLast, and DoMergeNext
  template <typename>
  friend class PoisonableBlock;
};

/// Trait type that allow interrogating a block as to whether it is contiguous.
template <typename BlockType>
struct is_contiguous : std::is_base_of<internal::ContiguousBase, BlockType> {};

/// Helper variable template for `is_contiguous<BlockType>::value`.
template <typename BlockType>
constexpr bool is_contiguous_v = is_contiguous<BlockType>::value;

/// @}

namespace internal {

/// Functions to crash with an error message describing which block invariant
/// has been violated. These functions are implemented independent of any
/// template parameters to allow them to use `PW_CHECK`.

/// Crashes with an error message about the next block being misaligned if
/// `next_is_aligned` is false.
void CheckNextMisaligned(const void* block,
                         const void* next,
                         bool next_is_aligned);

/// Crashes with an error message about the next block being corrupted if
/// `next_prev_matches` is false.
void CheckNextPrevMismatched(const void* block,
                             const void* next,
                             const void* next_prev,
                             bool next_prev_matches);

/// Crashes with an error message about the previous block being misaligned if
/// `prev_is_aligned` is false.
void CheckPrevMisaligned(const void* block,
                         const void* prev,
                         bool prev_is_aligned);

/// Crashes with an error message about the previous block being corrupted if
/// `prev_next_matches` is false.
void CheckPrevNextMismatched(const void* block,
                             const void* prev,
                             const void* prev_next,
                             bool prev_next_matches);

}  // namespace internal

// Template method implementations.

template <typename Derived>
constexpr Derived* ContiguousBlock<Derived>::Prev() const {
  if constexpr (Hardening::kIncludesDebugChecks) {
    derived()->CheckInvariants();
  }
  return PrevUnchecked();
}

template <typename Derived>
constexpr Derived* ContiguousBlock<Derived>::PrevUnchecked() const {
  size_t prev_outer_size = derived()->PrevOuterSizeUnchecked();
  if (prev_outer_size == 0) {
    return nullptr;
  }
  auto addr = cpp20::bit_cast<uintptr_t>(this);
  Hardening::Decrement(addr, prev_outer_size);
  return std::launder(reinterpret_cast<Derived*>(addr));
}

template <typename Derived>
constexpr Derived* ContiguousBlock<Derived>::Next() const {
  if constexpr (Hardening::kIncludesDebugChecks) {
    derived()->CheckInvariants();
  }
  return NextUnchecked();
}

template <typename Derived>
constexpr Derived* ContiguousBlock<Derived>::NextUnchecked() const {
  if (derived()->IsLastUnchecked()) {
    return nullptr;
  }
  size_t outer_size = derived()->OuterSizeUnchecked();
  auto addr = cpp20::bit_cast<uintptr_t>(this);
  Hardening::Increment(addr, outer_size);
  return std::launder(reinterpret_cast<Derived*>(addr));
}

template <typename Derived>
constexpr Derived* ContiguousBlock<Derived>::DoSplitFirst(
    size_t new_inner_size) {
  Derived* next = derived()->Next();
  size_t new_outer_size = Derived::kBlockOverhead + new_inner_size;
  ByteSpan bytes(derived()->UsableSpace(), derived()->InnerSize());
  bytes = bytes.subspan(new_inner_size);
  auto* trailing = Derived::AsBlock(bytes);
  derived()->SetNext(new_outer_size, trailing);
  trailing->SetNext(bytes.size(), next);
  return trailing;
}

template <typename Derived>
constexpr Derived* ContiguousBlock<Derived>::DoSplitLast(
    size_t new_inner_size) {
  size_t new_outer_size = Derived::kBlockOverhead + new_inner_size;
  return DoSplitFirst(derived()->InnerSize() - new_outer_size);
}

template <typename Derived>
constexpr void ContiguousBlock<Derived>::DoMergeNext() {
  Derived* next = derived()->Next();
  if (next != nullptr) {
    size_t outer_size = derived()->OuterSize() + next->OuterSize();
    derived()->SetNext(outer_size, next->Next());
  }
}

template <typename Derived>
constexpr bool ContiguousBlock<Derived>::DoCheckInvariants(bool strict) const {
  bool valid = true;

  Derived* next = derived()->NextUnchecked();
  if (next != nullptr) {
    valid &= (cpp20::bit_cast<uintptr_t>(next) % Derived::kAlignment) == 0;
    if constexpr (Hardening::kIncludesDebugChecks) {
      internal::CheckNextMisaligned(this, next, valid || !strict);
    }

    Derived* next_prev = next->PrevUnchecked();
    valid &= this == next_prev;
    if constexpr (Hardening::kIncludesDebugChecks) {
      internal::CheckNextPrevMismatched(
          this, next, next_prev, valid || !strict);
    }
  }

  Derived* prev = derived()->PrevUnchecked();
  if (prev != nullptr) {
    valid &= (cpp20::bit_cast<uintptr_t>(prev) % Derived::kAlignment) == 0;
    if constexpr (Hardening::kIncludesDebugChecks) {
      internal::CheckPrevMisaligned(this, prev, valid || !strict);
    }

    Derived* prev_next = prev->NextUnchecked();
    valid &= this == prev_next;
    if constexpr (Hardening::kIncludesDebugChecks) {
      internal::CheckPrevNextMismatched(
          this, prev, prev_next, valid || !strict);
    }
  }

  return valid;
}

}  // namespace pw::allocator
