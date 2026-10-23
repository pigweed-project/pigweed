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
#include <limits>
#include <tuple>

#include "pw_allocator/block/detailed_block.h"
#include "pw_allocator/block/result.h"
#include "pw_allocator/block_allocator.h"
#include "pw_allocator/bucket/fast_sorted.h"
#include "pw_allocator/bucket/sorted.h"
#include "pw_allocator/config.h"
#include "pw_allocator/layout.h"
#include "pw_bytes/span.h"
#include "pw_status/status.h"

namespace pw::allocator {

/// @submodule{pw_allocator,concrete_block}

/// Alias for a default block type that is compatible with `FirstFitAllocator`.
template <typename OffsetType>
using WorstFitBlock = DetailedBlock<OffsetType, GenericFastSortedItem>;

/// Block allocator that uses a "worst-fit" allocation strategy.
///
/// In this strategy, the allocator handles an allocation request by looking at
/// all unused blocks and finding the smallest one which can satisfy the
/// request.
///
/// This algorithm may make better use of available memory by wasting less on
/// unused fragments, but may also lead to worse fragmentation as those
/// fragments are more likely to be too small to be useful to other requests.
template <typename BlockType = WorstFitBlock<uintptr_t>>
class WorstFitAllocator : public BlockAllocator<BlockType> {
 private:
  using SmallBucket = ReverseSortedBucket<BlockType>;
  using LargeBucket = ReverseFastSortedBucket<BlockType>;

 public:
  using Base = BlockAllocator<BlockType>;

  /// Constexpr constructor. Callers must explicitly call `Init`.
  constexpr WorstFitAllocator() = default;

  /// Non-constexpr constructor that automatically calls `Init`.
  ///
  /// @param[in]  region      Region of memory to use when satisfying allocation
  ///                         requests. The region MUST be valid as an argument
  ///                         to `BlockType::Init`.
  WorstFitAllocator(ByteSpan region) { Base::Init(region); }

 private:
  /// @copydoc BlockAllocator::GetMaxAllocatable
  size_t DoGetMaxAllocatable() override;

  /// @copydoc BlockAllocator::ChooseBlock
  BlockResult<BlockType> ChooseBlock(Layout layout) override;

  /// @copydoc BlockAllocator::ReserveBlock
  void ReserveBlock(BlockType& block) override;

  /// @copydoc BlockAllocator::RecycleBlock
  void RecycleBlock(BlockType& block) override;

  SmallBucket small_bucket_;
  LargeBucket large_bucket_;
};

/// @}

// Template method implementations.

template <typename BlockType>
size_t WorstFitAllocator<BlockType>::DoGetMaxAllocatable() {
  const BlockType* largest = large_bucket_.empty()
                                 ? small_bucket_.FindLargest()
                                 : large_bucket_.FindLargest();
  return largest == nullptr ? 0 : largest->InnerSize();
}

template <typename BlockType>
BlockResult<BlockType> WorstFitAllocator<BlockType>::ChooseBlock(
    Layout layout) {
  BlockType* block = large_bucket_.RemoveCompatible(layout);
  if (block != nullptr) {
    return BlockType::AllocFirst(std::move(block), layout);
  }
  block = small_bucket_.RemoveCompatible(layout);
  if (block != nullptr) {
    return BlockType::AllocFirst(std::move(block), layout);
  }
  return BlockResult<BlockType>(nullptr, Status::NotFound());
}

template <typename BlockType>
void WorstFitAllocator<BlockType>::ReserveBlock(BlockType& block) {
  // The small bucket is slower; skip it if we can.
  if (!large_bucket_.Remove(block)) {
    std::ignore = small_bucket_.Remove(block);
  }
}

template <typename BlockType>
void WorstFitAllocator<BlockType>::RecycleBlock(BlockType& block) {
  if (block.InnerSize() < sizeof(typename LargeBucket::ItemType)) {
    std::ignore = small_bucket_.Add(block);
  } else {
    std::ignore = large_bucket_.Add(block);
  }
}

}  // namespace pw::allocator
