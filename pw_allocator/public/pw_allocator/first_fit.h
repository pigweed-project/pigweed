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
#include "pw_allocator/bucket/sequenced.h"
#include "pw_allocator/layout.h"
#include "pw_bytes/span.h"
#include "pw_status/status.h"

namespace pw::allocator {

/// @submodule{pw_allocator,concrete_block}

/// Alias for a default block type that is compatible with `FirstFitAllocator`.
template <typename OffsetType>
using FirstFitBlock = DetailedBlock<OffsetType, SequencedItem>;

/// Block allocator that uses a "first-fit" allocation strategy split
/// between large and small allocations.
///
/// In this strategy, the allocator handles an allocation request by starting at
/// the beginning of the range of blocks and looking for the first one which can
/// satisfy the request.
///
/// Optionally, callers may set a "threshold" value. If set, requests smaller
/// than the threshold are satisfied using the *last* compatible block. This
/// separates large and small requests and can reduce overall fragmentation.
template <typename BlockType = FirstFitBlock<uintptr_t>>
class FirstFitAllocator : public BlockAllocator<BlockType> {
 public:
  using Base = BlockAllocator<BlockType>;

  /// Constexpr constructor. Callers must explicitly call `Init`.
  constexpr FirstFitAllocator() = default;

  /// Non-constexpr constructor that automatically calls `Init`.
  ///
  /// @param[in]  region      Region of memory to use when satisfying allocation
  ///                         requests. The region MUST be valid as an argument
  ///                         to `BlockType::Init`.
  FirstFitAllocator(ByteSpan region) { Base::Init(region); }

  /// Non-constexpr constructor that automatically calls `Init`.
  ///
  /// @param[in]  region      Region of memory to use when satisfying allocation
  ///                         requests. The region MUST be valid as an argument
  ///                         to `BlockType::Init`.
  /// @param[in]  threshold   Value for which requests are considered "large".
  FirstFitAllocator(ByteSpan region, size_t threshold);

  /// Sets the threshold value for which requests are considered "large".
  void set_threshold(size_t threshold) { bucket_.set_threshold(threshold); }

 private:
  /// @copydoc BlockAllocator::GetMaxAllocatable
  size_t DoGetMaxAllocatable() override;

  /// @copydoc BlockAllocator::ChooseBlock
  BlockResult<BlockType> ChooseBlock(Layout layout) override;

  /// @copydoc BlockAllocator::ReserveBlock
  void ReserveBlock(BlockType& block) override {
    std::ignore = bucket_.Remove(block);
  }

  /// @copydoc BlockAllocator::RecycleBlock
  void RecycleBlock(BlockType& block) override {
    std::ignore = bucket_.Add(block);
  }

  SequencedBucket<BlockType> bucket_;
};

/// @}

// Template method implementations.

template <typename BlockType>
FirstFitAllocator<BlockType>::FirstFitAllocator(ByteSpan region,
                                                size_t threshold) {
  Base::Init(region);
  bucket_.set_threshold(threshold);
}

template <typename BlockType>
size_t FirstFitAllocator<BlockType>::DoGetMaxAllocatable() {
  const BlockType* largest = bucket_.FindLargest();
  return largest == nullptr ? 0 : largest->InnerSize();
}

template <typename BlockType>
BlockResult<BlockType> FirstFitAllocator<BlockType>::ChooseBlock(
    Layout layout) {
  BlockType* block = bucket_.RemoveCompatible(layout);
  if (block == nullptr) {
    return BlockResult<BlockType>(nullptr, Status::NotFound());
  }
  if (layout.size() < bucket_.threshold()) {
    return BlockType::AllocLast(std::move(block), layout);
  } else {
    return BlockType::AllocFirst(std::move(block), layout);
  }
}

}  // namespace pw::allocator
