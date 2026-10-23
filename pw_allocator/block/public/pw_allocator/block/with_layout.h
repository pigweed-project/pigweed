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

#include "pw_allocator/block/alignable.h"
#include "pw_allocator/block/allocatable.h"
#include "pw_allocator/block/result.h"
#include "pw_allocator/hardening.h"
#include "pw_allocator/layout.h"
#include "pw_assert/assert.h"

namespace pw::allocator {
namespace internal {

// Trivial base class for trait support.
struct BaseWithLayout {};

}  // namespace internal

/// @submodule{pw_allocator,block_mixins}

/// Mix-in for blocks that can retrieve the layout used to allocate them.
///
/// Block mix-ins are stateless and trivially constructible. See `BasicBlock`
/// for details on how mix-ins can be combined to implement blocks.
///
/// This mix-in requires its derived type also derive from `AlignableBlock`
/// and provide the following symbols:
///
/// - size_t RequestedSize() const
///   - Returns the size of the original layout
/// - size_t RequestedAlignment() const
///   - Returns the alignment of the original layout
/// - void SetRequestedSize(size_t)
///   - Records the size of the original layout
/// - void SetRequestedAlignment(size_t)
///   - Records the alignment from the original layout
template <typename Derived>
class BlockWithLayout : public internal::BaseWithLayout {
 protected:
  constexpr explicit BlockWithLayout() {
    // Assert within a function, since `Derived` is not complete when this type
    // is defined.
    static_assert(is_alignable_v<Derived>,
                  "Types derived from BlockWithLayout must also derive from "
                  "AlignableBlock");
  }

 public:
  /// @returns The memory layout that was requested using AllocFirst, AllocLast,
  /// or Resize.
  ///
  /// @pre The block must be in use.
  constexpr Layout RequestedLayout() const;

 protected:
  /// @copydoc AllocatableBlock::AllocFirst
  static constexpr BlockResult<Derived> DoAllocFirst(Derived*&& block,
                                                     Layout layout);

  /// @copydoc AllocatableBlock::AllocLast
  static constexpr BlockResult<Derived> DoAllocLast(Derived*&& block,
                                                    Layout layout);

  /// @copydoc AllocatableBlock::Resize
  constexpr BlockResult<Derived> DoResize(size_t new_inner_size,
                                          bool shifted = false);

  /// @copydoc AllocatableBlock::Free
  static constexpr BlockResult<Derived> DoFree(Derived*&& block);

 private:
  using BlockResultPrev = internal::GenericBlockResult::Prev;

  constexpr Derived* derived() { return static_cast<Derived*>(this); }
  constexpr const Derived* derived() const {
    return static_cast<const Derived*>(this);
  }
};

/// Trait type that allow interrogating a block as to whether it records the
/// requested layout.
template <typename BlockType>
struct has_layout : std::is_base_of<internal::BaseWithLayout, BlockType> {};

/// Helper variable template for `has_layout<BlockType>::value`.
template <typename BlockType>
constexpr bool has_layout_v = has_layout<BlockType>::value;

/// @}

// Template method implementations.

template <typename Derived>
constexpr Layout BlockWithLayout<Derived>::RequestedLayout() const {
  if constexpr (Hardening::kIncludesDebugChecks) {
    derived()->CheckInvariants();
  }
  if constexpr (Hardening::kIncludesRobustChecks) {
    PW_ASSERT(!derived()->IsFree());
  }
  return Layout(derived()->RequestedSize(), derived()->RequestedAlignment());
}

template <typename Derived>
constexpr BlockResult<Derived> BlockWithLayout<Derived>::DoAllocFirst(
    Derived*&& block, Layout layout) {
  auto result = AlignableBlock<Derived>::DoAllocFirst(std::move(block), layout);
  if (!result.ok()) {
    return result;
  }
  block = result.block();
  block->SetRequestedSize(layout.size());
  block->SetRequestedAlignment(layout.alignment());
  return result;
}

template <typename Derived>
constexpr BlockResult<Derived> BlockWithLayout<Derived>::DoAllocLast(
    Derived*&& block, Layout layout) {
  auto result = AlignableBlock<Derived>::DoAllocLast(std::move(block), layout);
  if (!result.ok()) {
    return result;
  }
  block = result.block();
  block->SetRequestedSize(layout.size());
  block->SetRequestedAlignment(layout.alignment());
  return result;
}

template <typename Derived>
constexpr BlockResult<Derived> BlockWithLayout<Derived>::DoResize(
    size_t new_inner_size, bool shifted) {
  size_t old_size = derived()->RequestedSize();
  auto result =
      derived()->AllocatableBlock<Derived>::DoResize(new_inner_size, shifted);
  if (result.ok() && !shifted) {
    derived()->SetRequestedSize(new_inner_size);
  } else {
    derived()->SetRequestedSize(old_size);
  }
  return result;
}

template <typename Derived>
constexpr BlockResult<Derived> BlockWithLayout<Derived>::DoFree(
    Derived*&& block) {
  auto result = AllocatableBlock<Derived>::DoFree(std::move(block));
  if (!result.ok()) {
    return result;
  }
  block = result.block();
  Derived* prev = block->Prev();
  if (prev == nullptr) {
    return result;
  }
  size_t prev_size = prev->RequestedSize();
  if (prev->InnerSize() - prev_size < Derived::kAlignment) {
    return result;
  }
  // Reclaim bytes that were shifted to prev when the block allocated.
  size_t old_prev_size = prev->OuterSize();
  prev->DoResize(prev_size, true).IgnoreUnlessStrict();
  return BlockResult<Derived>(prev->Next(),
                              BlockResultPrev::kResizedSmaller,
                              old_prev_size - prev->OuterSize());
}

}  // namespace pw::allocator
