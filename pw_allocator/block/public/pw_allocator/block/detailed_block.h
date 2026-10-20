// Copyright 2020 The Pigweed Authors
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
#include <new>

#include "pw_allocator/block/alignable.h"
#include "pw_allocator/block/allocatable.h"
#include "pw_allocator/block/basic.h"
#include "pw_allocator/block/contiguous.h"
#include "pw_allocator/block/iterable.h"
#include "pw_allocator/block/poisonable.h"
#include "pw_allocator/block/result.h"
#include "pw_allocator/block/with_layout.h"
#include "pw_allocator/hardening.h"
#include "pw_allocator/layout.h"
#include "pw_assert/assert.h"
#include "pw_bytes/span.h"
#include "pw_status/status.h"

namespace pw::allocator {

/// @submodule{pw_allocator,block_impl}

/// Parameters type that encapsulates the block parameters.
///
/// @tparam   OffsetType  Unsigned integral type used to encode offsets. Larger
///                       types can address more memory, but consume greater
///                       overhead.
/// @tparam   WhenFree    Optional intrusive type that uses the block's usable
///                       space to track the block when free. This will affect
///                       the minimum alignment, and what portion of the usable
///                       space is skipped when poisoning.
template <typename OffsetType_, typename WhenFree>
struct DetailedBlockParameters {
  using OffsetType = std::make_unsigned_t<OffsetType_>;
  static constexpr Layout kLayoutWhenFree = Layout::Of<WhenFree>();
};

template <typename OffsetType_>
struct DetailedBlockParameters<OffsetType_, void> {
  using OffsetType = std::make_unsigned_t<OffsetType_>;
  static constexpr Layout kLayoutWhenFree = Layout(0, 1);
};

/// A block implementation that implements most optional block mix-ins.
///
/// This block dervies from various block mix-ins, allowing it to perform
/// aligned allocations, iterate over blocks, poison and check free blocks,
/// retrieve requested memory layouts, and more.
///
/// The amount of memory that can be addressed by a block of this type depends
/// on it given `OffsetType`. This type is used to describe the size of both the
/// current and previous block, so the maximum addressable range is given by
/// `std::numeric_limits<OffsetType> * alignof(OffsetType)`.
///
/// An additional 4 bytes are used to store details about an allocation,
/// including whether it is in use or free, whether it is poisoned, and what the
/// originally requested layout for a block was.
///
/// See also the `DetailedBlock` alias which provides the `Parameters` type
/// automatically.
///
/// @tparam   Parameters  Block traits, such as `DetailedBlockParameters`, which
///                   encapsulate a set of template parameters.
template <typename Parameters>
class DetailedBlockImpl
    : public BasicBlock<DetailedBlockImpl<Parameters>>,
      public IterableBlock<DetailedBlockImpl<Parameters>>,
      public ContiguousBlock<DetailedBlockImpl<Parameters>>,
      public AllocatableBlock<DetailedBlockImpl<Parameters>>,
      public AlignableBlock<DetailedBlockImpl<Parameters>>,
      public BlockWithLayout<DetailedBlockImpl<Parameters>>,
      public PoisonableBlock<DetailedBlockImpl<Parameters>> {
 private:
  using BlockType = DetailedBlockImpl<Parameters>;
  using Basic = BasicBlock<BlockType>;

 public:
  using OffsetType = typename Parameters::OffsetType;

  using Range = typename IterableBlock<BlockType>::Range;
  using Iterator = typename IterableBlock<BlockType>::Iterator;

 private:
  constexpr explicit DetailedBlockImpl(size_t outer_size);

  // `Basic` required methods.
  friend Basic;
  static constexpr size_t DefaultAlignment();
  static constexpr size_t BlockOverhead();
  static constexpr size_t MinInnerSize();
  static constexpr size_t ReservedWhenFree();
  constexpr size_t OuterSizeUnchecked() const;

  // `Basic` overrides.
  constexpr bool DoCheckInvariants(bool strict) const;

  // `Contiguous` required methods.
  using Contiguous = ContiguousBlock<BlockType>;
  friend Contiguous;

  static constexpr size_t MaxAddressableSize();
  constexpr bool IsLastUnchecked() const { return info_.last != 0; }
  static constexpr BlockType* AsBlock(ByteSpan bytes);
  constexpr void SetNext(size_t outer_size, BlockType* next);
  constexpr size_t PrevOuterSizeUnchecked() const;

  // `Contiguous` overrides.
  constexpr BlockType* DoSplitFirst(size_t new_inner_size);
  constexpr BlockType* DoSplitLast(size_t new_inner_size);
  constexpr void DoMergeNext();

  // `Allocatable` required methods.
  using Allocatable = AllocatableBlock<BlockType>;
  friend Allocatable;
  constexpr bool IsFreeUnchecked() const { return info_.used == 0; }
  constexpr void SetFree(bool is_free);

  // `Alignable` overrides.
  using Alignable = AlignableBlock<BlockType>;
  friend Alignable;
  constexpr StatusWithSize DoCanAlloc(Layout layout) const;
  static constexpr BlockResult<BlockType> DoAllocFirst(BlockType*&& block,
                                                       Layout layout);
  static constexpr BlockResult<BlockType> DoAllocLast(BlockType*&& block,
                                                      Layout layout);
  constexpr BlockResult<BlockType> DoResize(size_t new_inner_size,
                                            bool shifted = false);
  static constexpr BlockResult<BlockType> DoFree(BlockType*&& block);

  // `WithLayout` required methods.
  using WithLayout = BlockWithLayout<BlockType>;
  friend WithLayout;
  constexpr size_t RequestedSize() const;
  constexpr size_t RequestedAlignment() const { return info_.alignment; }
  constexpr void SetRequestedSize(size_t size);
  constexpr void SetRequestedAlignment(size_t alignment);

  // `Poisonable` required methods.
  using Poisonable = PoisonableBlock<BlockType>;
  friend Poisonable;
  constexpr bool IsPoisonedUnchecked() const { return info_.poisoned != 0; }
  constexpr void SetPoisoned(bool is_poisoned) { info_.poisoned = is_poisoned; }

  /// Offset (in increments of the minimum alignment) from this block to the
  /// previous block. 0 if this is the first block.
  OffsetType prev_ = 0;

  /// Offset (in increments of the minimum alignment) from this block to the
  /// next block. Valid even if this is the last block, since it equals the
  /// size of the block.
  OffsetType next_ = 0;

  /// Information about the current state of the block:
  /// * If the `used` flag is set, the block's usable memory has been allocated
  ///   and is being used.
  /// * If the `poisoned` flag is set and the `used` flag is clear, the block's
  ///   usable memory contains a poison pattern that will be checked when the
  ///   block is allocated.
  /// * If the `last` flag is set, the block does not have a next block.
  /// * If the `used` flag is set, the alignment represents the requested value
  ///   when the memory was allocated, which may be less strict than the actual
  ///   alignment.
  struct {
    uint16_t used : 1;
    uint16_t poisoned : 1;
    uint16_t last : 1;
    uint16_t alignment : 13;
  } info_;

  /// Number of bytes allocated beyond what was requested. This will be at most
  /// the minimum alignment, i.e. `alignof(OffsetType).`
  uint16_t padding_ = 0;
};

// Convenience alias that constructs the block traits automatically.
template <typename OffsetType = uintptr_t, typename WhenFree = void>
using DetailedBlock =
    DetailedBlockImpl<DetailedBlockParameters<OffsetType, WhenFree>>;

/// @}

// Template method implementations.

template <typename Parameters>
constexpr DetailedBlockImpl<Parameters>::DetailedBlockImpl(size_t outer_size)
    : info_{} {
  next_ = static_cast<OffsetType>(outer_size / Basic::kAlignment);
  info_.last = 1;
  info_.alignment = Basic::kAlignment;
}

// `Basic` methods.

template <typename Parameters>
constexpr size_t DetailedBlockImpl<Parameters>::DefaultAlignment() {
  return std::max(alignof(OffsetType), Parameters::kLayoutWhenFree.alignment());
}

template <typename Parameters>
constexpr size_t DetailedBlockImpl<Parameters>::BlockOverhead() {
  return sizeof(BlockType);
}

template <typename Parameters>
constexpr size_t DetailedBlockImpl<Parameters>::MinInnerSize() {
  return 1;
}

template <typename Parameters>
constexpr size_t DetailedBlockImpl<Parameters>::ReservedWhenFree() {
  return Parameters::kLayoutWhenFree.size();
}

template <typename Parameters>
constexpr size_t DetailedBlockImpl<Parameters>::OuterSizeUnchecked() const {
  size_t outer_size = static_cast<size_t>(next_);
  Hardening::Multiply(outer_size, Basic::kAlignment);
  return outer_size;
}

template <typename Parameters>
constexpr bool DetailedBlockImpl<Parameters>::DoCheckInvariants(
    bool strict) const {
  return Basic::DoCheckInvariants(strict) &&
         Contiguous::DoCheckInvariants(strict) &&
         Poisonable::DoCheckInvariants(strict);
}

// `Contiguous` methods.

template <typename Parameters>
constexpr size_t DetailedBlockImpl<Parameters>::MaxAddressableSize() {
  auto kOffsetMax = static_cast<size_t>(std::numeric_limits<OffsetType>::max());
  auto kSizeMax = std::numeric_limits<size_t>::max();
  return std::min(kSizeMax / Basic::kAlignment, kOffsetMax) * Basic::kAlignment;
}

template <typename Parameters>
constexpr DetailedBlockImpl<Parameters>* DetailedBlockImpl<Parameters>::AsBlock(
    ByteSpan bytes) {
  return ::new (bytes.data()) DetailedBlockImpl(bytes.size());
}

template <typename Parameters>
constexpr void DetailedBlockImpl<Parameters>::SetNext(size_t outer_size,
                                                      BlockType* next) {
  next_ = static_cast<OffsetType>(outer_size / Basic::kAlignment);
  if (next == nullptr) {
    info_.last = 1;
    return;
  }
  info_.last = 0;
  next->prev_ = next_;
}

template <typename Parameters>
constexpr size_t DetailedBlockImpl<Parameters>::PrevOuterSizeUnchecked() const {
  size_t outer_size = static_cast<size_t>(prev_);
  Hardening::Multiply(outer_size, Basic::kAlignment);
  return outer_size;
}

template <typename Parameters>
constexpr DetailedBlockImpl<Parameters>*
DetailedBlockImpl<Parameters>::DoSplitFirst(size_t new_inner_size) {
  return Poisonable::DoSplitFirst(new_inner_size);
}

template <typename Parameters>
constexpr DetailedBlockImpl<Parameters>*
DetailedBlockImpl<Parameters>::DoSplitLast(size_t new_inner_size) {
  return Poisonable::DoSplitLast(new_inner_size);
}

template <typename Parameters>
constexpr void DetailedBlockImpl<Parameters>::DoMergeNext() {
  Poisonable::DoMergeNext();
}

// `Alignable` methods.

template <typename Parameters>
constexpr void DetailedBlockImpl<Parameters>::SetFree(bool is_free) {
  info_.used = !is_free;
  padding_ = 0;
  Poisonable::SetFree(is_free);
}

// `Alignable` methods.

template <typename Parameters>
constexpr StatusWithSize DetailedBlockImpl<Parameters>::DoCanAlloc(
    Layout layout) const {
  return Alignable::DoCanAlloc(layout);
}

template <typename Parameters>
constexpr BlockResult<DetailedBlockImpl<Parameters>>
DetailedBlockImpl<Parameters>::DoAllocFirst(DetailedBlockImpl*&& block,
                                            Layout layout) {
  return WithLayout::DoAllocFirst(std::move(block), layout);
}

template <typename Parameters>
constexpr BlockResult<DetailedBlockImpl<Parameters>>
DetailedBlockImpl<Parameters>::DoAllocLast(DetailedBlockImpl*&& block,
                                           Layout layout) {
  return WithLayout::DoAllocLast(std::move(block), layout);
}

template <typename Parameters>
constexpr BlockResult<DetailedBlockImpl<Parameters>>
DetailedBlockImpl<Parameters>::DoResize(size_t new_inner_size, bool shifted) {
  return WithLayout::DoResize(new_inner_size, shifted);
}

template <typename Parameters>
constexpr BlockResult<DetailedBlockImpl<Parameters>>
DetailedBlockImpl<Parameters>::DoFree(DetailedBlockImpl*&& block) {
  return WithLayout::DoFree(std::move(block));
}

// `WithLayout` methods.

template <typename Parameters>
constexpr size_t DetailedBlockImpl<Parameters>::RequestedSize() const {
  if constexpr (Hardening::kIncludesDebugChecks) {
    PW_ASSERT(padding_ <= Basic::InnerSize());
  }
  return Basic::InnerSize() - padding_;
}

template <typename Parameters>
constexpr void DetailedBlockImpl<Parameters>::SetRequestedSize(size_t size) {
  size_t inner_size = Basic::InnerSize();
  size_t padding = inner_size;
  Hardening::Decrement(padding, size);
  if constexpr (Hardening::kIncludesDebugChecks) {
    PW_ASSERT(padding <= std::numeric_limits<uint16_t>::max());
  }
  padding_ = static_cast<uint16_t>(padding);
}

template <typename Parameters>
constexpr void DetailedBlockImpl<Parameters>::SetRequestedAlignment(
    size_t alignment) {
  if constexpr (Hardening::kIncludesDebugChecks) {
    PW_ASSERT((alignment & (alignment - 1)) == 0);
    PW_ASSERT(alignment < 0x2000);
  }
  info_.alignment = static_cast<uint16_t>(alignment & 0x1fff);
}

}  // namespace pw::allocator
