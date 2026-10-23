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
#include <type_traits>

#include "pw_allocator/block/contiguous.h"
#include "pw_allocator/block/result.h"
#include "pw_allocator/hardening.h"
#include "pw_allocator/layout.h"
#include "pw_bytes/alignment.h"
#include "pw_numeric/checked_arithmetic.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"

namespace pw::allocator {
namespace internal {

// Trivial base class for trait support.
struct AllocatableBase {};

}  // namespace internal

/// @submodule{pw_allocator,block_mixins}

/// Mix-in for blocks that can be allocated and freed.
///
/// Block mix-ins are stateless and trivially constructible. See `BasicBlock`
/// for details on how mix-ins can be combined to implement blocks.
///
/// This mix-in requires its derived type also derive from `ContiguousBlock`
/// and provide the following symbols:
///
/// - size_t kMinOuterSize
///   - Size of the smallest block that can be allocated.
/// - bool IsFreeUnchecked() const
///   - Returns whether the block is free or in use.
/// - void SetFree(bool)
///   - Sets whether the block is free or in use.
template <typename Derived>
class AllocatableBlock : public internal::AllocatableBase {
 protected:
  constexpr explicit AllocatableBlock() {
    static_assert(is_contiguous_v<Derived>,
                  "Types derived from AllocatableBlock must also derive from "
                  "ContiguousBlock");
  }

 public:
  /// @returns whether this block is free or is in use.
  constexpr bool IsFree() const;

  // clang-format off
  /// Checks if a block could be split from the block.
  ///
  /// On error, this method will return the same status as `AllocFirst` or
  /// `AllocLast` without performing any modifications.
  ///
  /// @pre The block must not be in use.
  ///
  /// @returns @Result{the number of bytes to shift this block in order to align its usable space}
  /// * @FAILED_PRECONDITION: This block is in use and cannot be split.
  /// * @RESOURCE_EXHAUSTED: The available space is insufficient to fulfill the
  ///   request. This may be due to a large requested size, or insufficient
  ///   remaining space to fulfill the requested alignment create a valid
  ///   leading block, and/or create a valid trailing block.
  // clang-format on
  constexpr StatusWithSize CanAlloc(Layout layout) const;

  /// Splits an aligned block from the start of the block, and marks it as used.
  ///
  /// If successful, `block` will be replaced by a block that has an inner
  /// size of at least `inner_size`, and whose starting address is aligned to an
  /// `alignment` boundary. If unsuccessful, `block` will be unmodified.
  ///
  /// This method is static in order to consume the given block pointer.
  /// On success, a pointer to the new, smaller block is returned. In total, up
  /// to two additional blocks may be created: one to pad the returned block to
  /// an alignment boundary and one for the trailing space. On error, the
  /// original pointer is returned.
  ///
  /// For larger alignments, the `AllocLast` method is generally preferable to
  /// this method, as this method may create an additional fragments both before
  /// and after the returned block in order to align the usable space.
  ///
  /// @pre The block must not be in use.
  ///
  /// @returns
  /// * @OK: The split completed successfully. The `BlockAllocType` indicates
  ///   how extra memory was distributed to other blocks.
  /// * @FAILED_PRECONDITION: This block is in use and cannot be split.
  /// * @RESOURCE_EXHAUSTED: The available space is insufficient to fulfill the
  ///   request. This may be due to a large requested size, or insufficient
  ///   remaining space to fulfill the requested alignment create a valid
  ///   leading block, and/or create a valid trailing block.
  static constexpr BlockResult<Derived> AllocFirst(Derived*&& block,
                                                   Layout layout);

  /// Splits an aligned block from the end of the block, and marks it as used.
  ///
  /// If successful, `block` will be replaced by a block that has an inner
  /// size of at least `inner_size`, and whose starting address is aligned to an
  /// `alignment` boundary. If unsuccessful, `block` will be unmodified.
  ///
  /// This method is static in order to consume the given block pointer.
  /// On success, a pointer to the new, smaller block is returned. In total, up
  /// to two additional blocks may be created: one to pad the returned block to
  /// an alignment boundary and one for the trailing space. On error, the
  /// original pointer is returned.
  ///
  /// @pre The block must not be in use.
  ///
  /// @returns
  /// * @OK: The split completed successfully. The `BlockAllocType` indicates
  ///   how extra memory was distributed to other blocks.
  /// * @FAILED_PRECONDITION: This block is in use and cannot be split.
  /// * @RESOURCE_EXHAUSTED: The available space is insufficient to fulfill the
  ///   request. This may be due to a large requested size, or insufficient
  ///   remaining space to fulfill the requested alignment create a valid
  ///   leading block, and/or create a valid trailing block.
  static constexpr BlockResult<Derived> AllocLast(Derived*&& block,
                                                  Layout layout);

  /// Grows or shrinks the block.
  ///
  /// If successful, `block` may be merged with the block after it in order to
  /// provide additional memory (when growing) or to merge released memory (when
  /// shrinking). If unsuccessful, `block` will be unmodified.
  ///
  /// Note: Resizing may modify the block following this one if it is free.
  /// Allocators that track free blocks based on their size must be prepared to
  /// handle this size change.
  ///
  /// @pre The block must be in use.
  ///
  /// @returns
  /// * @OK: The resize completed successfully.
  /// * @FAILED_PRECONDITION: This block is not in use.
  /// * @RESOURCE_EXHAUSTED: The available space is insufficient to fulfill the
  ///   request. This may be due to a large requested size, or insufficient
  ///   remaining space to fulfill the requested alignment create a valid
  ///   leading block, and/or create a valid trailing block.
  constexpr BlockResult<Derived> Resize(size_t new_inner_size);

  /// Marks the block as free.
  ///
  /// This method is static in order to consume the given block pointer. It
  /// returns a pointer to a freed block that is the result of merging the given
  /// block with either or both of its neighbors, if they were free.
  ///
  /// Note: Freeing may modify the adjacent blocks if they are free.
  /// Allocators that track free blocks must be prepared to handle this merge.
  static constexpr BlockResult<Derived> Free(Derived*&& block);

 protected:
  /// @copydoc CanAlloc
  constexpr StatusWithSize DoCanAlloc(Layout layout) const;

  /// @copydoc AllocFirst
  static constexpr BlockResult<Derived> DoAllocFirst(Derived*&& block,
                                                     Layout layout);

  /// @copydoc AllocLast
  static constexpr BlockResult<Derived> DoAllocLast(Derived*&& block,
                                                    Layout layout);

  /// @copydoc Resize
  constexpr BlockResult<Derived> DoResize(size_t new_inner_size,
                                          bool shifted = false);

  /// @copydoc Free
  static constexpr BlockResult<Derived> DoFree(Derived*&& block);

 private:
  using BlockResultPrev = internal::GenericBlockResult::Prev;
  using BlockResultNext = internal::GenericBlockResult::Next;

  constexpr Derived* derived() { return static_cast<Derived*>(this); }
  constexpr const Derived* derived() const {
    return static_cast<const Derived*>(this);
  }

  // AlignableBlock calls DoCanAlloc, DoAllocLast, DoResize
  template <typename>
  friend class AlignableBlock;

  // BlockWithLayout calls DoFree, DoResize
  template <typename>
  friend class BlockWithLayout;
};

/// Trait type that allows interrogating a block as to whether it is
/// allocatable.
template <typename BlockType>
struct is_allocatable : std::is_base_of<internal::AllocatableBase, BlockType> {
};

/// Helper variable template for `is_allocatable<BlockType>::value`.
template <typename BlockType>
constexpr bool is_allocatable_v = is_allocatable<BlockType>::value;

/// @}

// Template method implementations.

template <typename Derived>
constexpr bool AllocatableBlock<Derived>::IsFree() const {
  if constexpr (Hardening::kIncludesDebugChecks) {
    derived()->CheckInvariants();
  }
  return derived()->IsFreeUnchecked();
}

template <typename Derived>
constexpr StatusWithSize AllocatableBlock<Derived>::CanAlloc(
    Layout layout) const {
  if constexpr (Hardening::kIncludesDebugChecks) {
    derived()->CheckInvariants();
  }
  return derived()->DoCanAlloc(layout);
}

template <typename Derived>
constexpr StatusWithSize AllocatableBlock<Derived>::DoCanAlloc(
    Layout layout) const {
  if (!derived()->IsFree()) {
    return StatusWithSize::FailedPrecondition();
  }
  if (layout.size() == 0) {
    return StatusWithSize::InvalidArgument();
  }
  size_t extra = derived()->InnerSize();
  size_t new_inner_size = AlignUp(layout.size(), Derived::kAlignment);
  if (!CheckedSub(extra, new_inner_size, extra)) {
    return StatusWithSize::ResourceExhausted();
  }
  return StatusWithSize(extra);
}

template <typename Derived>
constexpr BlockResult<Derived> AllocatableBlock<Derived>::AllocFirst(
    Derived*&& block, Layout layout) {
  if (block == nullptr || layout.size() == 0) {
    return BlockResult<Derived>(block, Status::InvalidArgument());
  }
  if constexpr (Hardening::kIncludesRobustChecks) {
    block->CheckInvariants();
  }
  if (!block->IsFree()) {
    return BlockResult<Derived>(block, Status::FailedPrecondition());
  }
  return Derived::DoAllocFirst(std::move(block), layout);
}

template <typename Derived>
constexpr BlockResult<Derived> AllocatableBlock<Derived>::DoAllocFirst(
    Derived*&& block, Layout layout) {
  size_t size = AlignUp(layout.size(), Derived::kAlignment);
  layout = Layout(size, layout.alignment());
  StatusWithSize can_alloc = block->DoCanAlloc(layout);
  if (!can_alloc.ok()) {
    return BlockResult<Derived>(block, can_alloc.status());
  }
  size_t extra = can_alloc.size();
  BlockResult<Derived> result(block);
  if (extra >= Derived::kMinOuterSize) {
    // Split the large padding off the back.
    block->DoSplitFirst(block->InnerSize() - extra);
    result = BlockResult<Derived>(block, BlockResultNext::kSplitNew);
  }
  block->SetFree(false);
  return result;
}

template <typename Derived>
constexpr BlockResult<Derived> AllocatableBlock<Derived>::AllocLast(
    Derived*&& block, Layout layout) {
  if (block == nullptr || layout.size() == 0) {
    return BlockResult<Derived>(block, Status::InvalidArgument());
  }
  if constexpr (Hardening::kIncludesRobustChecks) {
    block->CheckInvariants();
  }
  if (!block->IsFree()) {
    return BlockResult<Derived>(block, Status::FailedPrecondition());
  }
  return Derived::DoAllocLast(std::move(block), layout);
}

template <typename Derived>
constexpr BlockResult<Derived> AllocatableBlock<Derived>::DoAllocLast(
    Derived*&& block, Layout layout) {
  size_t size = AlignUp(layout.size(), Derived::kAlignment);
  layout = Layout(size, layout.alignment());
  StatusWithSize can_alloc = block->DoCanAlloc(layout);
  if (!can_alloc.ok()) {
    return BlockResult<Derived>(block, can_alloc.status());
  }
  size_t extra = can_alloc.size();
  BlockResult<Derived> result(block);
  Derived* prev = block->Prev();
  if (extra >= Derived::kMinOuterSize) {
    // Split the large padding off the front.
    block = block->DoSplitLast(layout.size());
    prev = block->Prev();
    result = BlockResult<Derived>(block, BlockResultPrev::kSplitNew);

  } else if (extra != 0 && prev != nullptr) {
    // The small amount of padding can be appended to the previous block.
    prev->DoResize(prev->InnerSize() + extra, true).IgnoreUnlessStrict();
    block = prev->Next();
    result =
        BlockResult<Derived>(block, BlockResultPrev::kResizedLarger, extra);
  }
  block->SetFree(false);
  return result;
}

template <typename Derived>
constexpr BlockResult<Derived> AllocatableBlock<Derived>::Resize(
    size_t new_inner_size) {
  if constexpr (Hardening::kIncludesRobustChecks) {
    derived()->CheckInvariants();
  }
  if (derived()->IsFree()) {
    return BlockResult<Derived>(derived(), Status::FailedPrecondition());
  }
  return derived()->DoResize(new_inner_size, /* shifted: */ false);
}

template <typename Derived>
constexpr BlockResult<Derived> AllocatableBlock<Derived>::DoResize(
    size_t new_inner_size, bool) {
  size_t old_inner_size = derived()->InnerSize();
  new_inner_size = AlignUp(new_inner_size, Derived::kAlignment);
  if (old_inner_size == new_inner_size) {
    return BlockResult<Derived>(derived());
  }

  // Treat the block as free and try to combine it with the next block. At most
  // one free block is expected to follow this block.
  derived()->SetFree(true);
  Derived* next = derived()->Next();
  BlockResult<Derived> result(derived());
  if (next != nullptr && next->IsFree()) {
    derived()->DoMergeNext();
    result = BlockResult<Derived>(derived(), BlockResultNext::kMerged);
  }
  size_t merged_inner_size = derived()->InnerSize();
  if (merged_inner_size < new_inner_size) {
    // The merged block is too small for the resized block. Restore the original
    // blocks as needed.
    if (merged_inner_size != old_inner_size) {
      derived()->DoSplitFirst(old_inner_size);
    }
    derived()->SetFree(false);
    return BlockResult<Derived>(derived(), Status::ResourceExhausted());
  }
  if (new_inner_size + Derived::kMinOuterSize <= merged_inner_size) {
    // There is enough room after the resized block for another trailing block.
    derived()->DoSplitFirst(new_inner_size);
    result = result.next() == BlockResultNext::kMerged
                 ? BlockResult<Derived>(derived(), BlockResultNext::kResized)
                 : BlockResult<Derived>(derived(), BlockResultNext::kSplitNew);
  }
  derived()->SetFree(false);
  return result;
}

template <typename Derived>
constexpr BlockResult<Derived> AllocatableBlock<Derived>::Free(
    Derived*&& block) {
  if (block == nullptr) {
    return BlockResult<Derived>(block, Status::InvalidArgument());
  }
  if constexpr (Hardening::kIncludesRobustChecks) {
    block->CheckInvariants();
  }
  return Derived::DoFree(std::move(block));
}

template <typename Derived>
constexpr BlockResult<Derived> AllocatableBlock<Derived>::DoFree(
    Derived*&& block) {
  block->SetFree(true);
  BlockResult<Derived> result(block);

  // Try to merge the previous block with this one.
  Derived* prev = block->Prev();
  if (prev != nullptr && prev->IsFree()) {
    prev->DoMergeNext();
    block = prev;
    result = BlockResult<Derived>(block, BlockResultNext::kMerged);
  }

  // Try to merge this block with the next one.
  Derived* next = block->Next();
  if (next != nullptr && next->IsFree()) {
    block->DoMergeNext();
    result = BlockResult<Derived>(block, BlockResultNext::kMerged);
  }

  if constexpr (Hardening::kIncludesDebugChecks) {
    block->CheckInvariants();
  }
  return result;
}

}  // namespace pw::allocator
