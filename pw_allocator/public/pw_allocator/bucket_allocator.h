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

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "pw_allocator/block/detailed_block.h"
#include "pw_allocator/block/result.h"
#include "pw_allocator/block_allocator.h"
#include "pw_allocator/bucket/unordered.h"
#include "pw_allocator/layout.h"
#include "pw_bytes/span.h"
#include "pw_span/span.h"
#include "pw_status/try.h"

namespace pw::allocator {

/// @submodule{pw_allocator,concrete_block}

/// Alias for a default block type that is compatible with
/// `BucketAllocator`.
template <typename OffsetType = uintptr_t>
using BucketBlock = DetailedBlock<OffsetType, UnorderedItem>;

/// Block allocator that uses sized buckets of free blocks.
///
/// In this strategy, the allocator handles an allocation request by starting
/// with the bucket with the smallest size that is larger than the requested
/// size. It tries to allocate using the blocks in that block, if any, before
/// trying the bucket with the next largest size.
///
/// On deallocation, blocks are placed in the bucket of the smallest size that
/// is larger than usable space of the block being freed.
///
/// The last bucket always has an unbounded size.
///
/// As an example, assume that the allocator is configured with a minimum block
/// inner size of 64 and 5 buckets. The internal state may look like the
/// following:
///
/// @code{.unparsed}
/// bucket[0] (64B) --> block[12B] --> block[42B] --> block[64B] --> NULL
/// bucket[1] (128B) --> block[65B] --> block[72B] --> NULL
/// bucket[2] (256B) --> NULL
/// bucket[3] (512B) --> block[312B] --> block[512B] --> block[416B] --> NULL
/// bucket[4] (implicit) --> block[1024B] --> block[513B] --> NULL
/// @endcode
template <typename BlockType = BucketBlock<>>
class BasicBucketAllocator : public BlockAllocator<BlockType> {
 private:
  using Base = BlockAllocator<BlockType>;

 public:
  constexpr BasicBucketAllocator() = default;

  /// Configures bucket sizes.
  constexpr void InitBuckets(span<UnorderedBucket<BlockType>> buckets,
                             size_t max_inner_size);

 private:
  /// @copydoc BlockAllocator::GetMaxAllocatable
  size_t DoGetMaxAllocatable() override;

  /// @copydoc BlockAllocator::ChooseBlock
  BlockResult<BlockType> ChooseBlock(Layout layout) override;

  /// @copydoc BlockAllocator::ReserveBlock
  void ReserveBlock(BlockType& block) override;

  /// @copydoc BlockAllocator::RecycleBlock
  void RecycleBlock(BlockType& block) override;

  span<UnorderedBucket<BlockType>> buckets_;
};

/// A BasicBucketAllocator that provides storage for its buckets.
///
/// Except for the constructors, methods are implemented on the base class in
/// order to reduce code duplication.
template <typename BlockType = BucketBlock<>,
          size_t kMinInnerSize = 32,
          size_t kNumBuckets = 5>
class BucketAllocator : public BasicBucketAllocator<BlockType> {
 private:
  using Base = BasicBucketAllocator<BlockType>;

 public:
  /// Constexpr constructor. Callers must explicitly call `Init`.
  constexpr BucketAllocator() { Base::InitBuckets(buckets_, kMinInnerSize); }

  /// Non-constexpr constructor that automatically calls `Init`.
  ///
  /// @param[in]  region  Region of memory to use when satisfying allocation
  ///                     requests. The region MUST be large enough to fit an
  ///                     aligned block with overhead. It MUST NOT be larger
  ///                     than what is addressable by `OffsetType`.
  explicit BucketAllocator(ByteSpan region) : BucketAllocator() {
    Base::Init(region);
  }

  ~BucketAllocator() override;

 private:
  std::array<UnorderedBucket<BlockType>, kNumBuckets> buckets_;
};

/// @}

// Template method implementations.

template <typename BlockType>
constexpr void BasicBucketAllocator<BlockType>::InitBuckets(
    span<UnorderedBucket<BlockType>> buckets, size_t max_inner_size) {
  buckets_ = buckets;
  for (auto& bucket : buckets_.subspan(0, buckets_.size() - 1)) {
    bucket.set_max_inner_size(max_inner_size);
    max_inner_size <<= 1;
  }
}

template <typename BlockType>
size_t BasicBucketAllocator<BlockType>::DoGetMaxAllocatable() {
  for (auto b = buckets_.rbegin(); b != buckets_.rend(); ++b) {
    const BlockType* largest = b->FindLargest();
    if (largest != nullptr) {
      return largest->InnerSize();
    }
  }
  return 0;
}

template <typename BlockType>
BlockResult<BlockType> BasicBucketAllocator<BlockType>::ChooseBlock(
    Layout layout) {
  for (auto& bucket : buckets_) {
    if (bucket.max_inner_size() < layout.size()) {
      continue;
    }
    BlockType* block = bucket.RemoveCompatible(layout);
    if (block != nullptr) {
      return BlockType::AllocFirst(std::move(block), layout);
    }
  }
  return BlockResult<BlockType>(nullptr, Status::NotFound());
}

template <typename BlockType>
void BasicBucketAllocator<BlockType>::ReserveBlock(BlockType& block) {
  for (auto& bucket : buckets_) {
    if (block.InnerSize() <= bucket.max_inner_size()) {
      std::ignore = bucket.Remove(block);
      break;
    }
  }
}

template <typename BlockType>
void BasicBucketAllocator<BlockType>::RecycleBlock(BlockType& block) {
  for (auto& bucket : buckets_) {
    if (block.InnerSize() <= bucket.max_inner_size()) {
      std::ignore = bucket.Add(block);
      break;
    }
  }
}

template <typename BlockType, size_t kMinInnerSize, size_t kNumBuckets>
BucketAllocator<BlockType, kMinInnerSize, kNumBuckets>::~BucketAllocator() {
  for (auto& bucket : buckets_) {
    bucket.Clear();
  }
}

}  // namespace pw::allocator
