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
#include <limits>

#include "pw_allocator/block/detailed_block.h"
#include "pw_allocator/block_allocator.h"
#include "pw_allocator/bucket/fast_sorted.h"
#include "pw_allocator/bucket/unordered.h"
#include "pw_allocator/config.h"
#include "pw_allocator/internal/bit.h"

namespace pw::allocator {

/// @submodule{pw_allocator,concrete_block}

/// Alias for a default block type that is compatible with `DlAllocator`.
template <typename OffsetType>
using DlBlock = DetailedBlock<OffsetType, GenericFastSortedItem>;

/// Doug Lea's allocator.
///
/// This allocator uses several types of buckets to quickly satisfy
/// memory allocations with best-fit blocks as described by
/// https://gee.cs.oswego.edu/dl/html/malloc.html
///
/// This implementation is simplified as `sbrk`- and `mmap`-related features are
/// not currently supported.
///
/// Note that Doug Lea's "bins" are provided by pw_allocator's buckets. Both the
/// "fast" and "small" bins hold a single size, and can therefore be implemented
/// using `UnorderedBucket`. The "large" bins hold a range of sizes a use
/// `FastSortedBucket` to quickly return best-fit blocks as requested.
template <typename BlockType = DlBlock<uintptr_t>>
class DlAllocator : public BlockAllocator<BlockType> {
 private:
  using Base = BlockAllocator<BlockType>;
  using FastBin = UnorderedBucket<BlockType>;
  using SmallBin = UnorderedBucket<BlockType>;
  using LargeBin = FastSortedBucket<BlockType>;

  static constexpr size_t kMinSize = 8;

  // Cache free blocks with sizes up to 80 bytes.
  static constexpr size_t kNumFastBins = 10;

  // The number of small bins must be a power of two.
  static constexpr size_t kNumSmallBins = 64;
  static_assert((kNumSmallBins & (kNumSmallBins - 1)) == 0);

  // The number of large bins is the sum of all powers of two smaller than the
  // number of small bins.
  static constexpr size_t kNumLargeBins = kNumSmallBins - 1;

  // Bit maps are implemented as an array of `uintptr_t`s.
  static constexpr size_t kBitmapBits = std::numeric_limits<uintptr_t>::digits;
  static constexpr size_t kNumBitmaps =
      AlignUp(kNumSmallBins + kNumLargeBins, kBitmapBits) / kBitmapBits;

 public:
  /// Constexpr constructor. Callers must explicitly call `Init`.
  constexpr DlAllocator();

  /// Non-constexpr constructor that automatically calls `Init`.
  ///
  /// @param[in]  region      Region of memory to use when satisfying allocation
  ///                         requests. The region MUST be valid as an argument
  ///                         to `BlockType::Init`.
  DlAllocator(ByteSpan region) : DlAllocator() { Base::Init(region); }

  ~DlAllocator() override { Flush(); }

 private:
  /// @copydoc BlockAllocator::GetMaxAllocatable
  size_t DoGetMaxAllocatable() override;

  /// @copydoc BlockAllocator::ChooseBlock
  BlockResult<BlockType> ChooseBlock(Layout layout) override;

  /// @copydoc BlockAllocator::ReserveBlock
  void ReserveBlock(BlockType& block) override;

  /// @copydoc BlockAllocator::RecycleBlock
  void RecycleBlock(BlockType& block) override;

  /// @copydoc BlockAllocator::Flush
  void Flush() override { ReleaseFastBins(); }

  /// @copydoc BlockAllocator::DeallocateBlock
  void DeallocateBlock(BlockType*&& block) override;

  /// Returns the bin index for blocks of a given `size`.
  ///
  /// Bins are sized to be approximately logarithmically spaced. The bins are
  /// grouped into several "rounds", with each round having half as many bins as
  /// that are 8 times the size of the previous round's bins. The final round
  /// consists of a single bin with an unlimited size.
  size_t GetIndex(size_t size);

  /// Performs deferred allocations.
  ///
  /// Small blocks are not immediately freed in order to avoid merging and
  /// splitting when the same size is quickly requested again. A full
  /// deallocation pass is performed whenever a small block is requested, but
  /// none are available in the corresponding bin, or whenever a large block is
  /// requested.
  void ReleaseFastBins();

  /// Starting with the bucket indicated by the given `index`, searches for
  /// the non-empty bucket with the smallest maximum inner size. Updates the
  /// given `index` and returns true if such a bucket is found; otherwise
  /// returns false.
  bool FindNextAvailable(size_t& index);

  /// Updates the shelf and bucket bitmaps to reflect whether the bucket
  /// referenced by the given `index` is `empty`.
  void UpdateBitmaps(size_t index, bool empty);

  std::array<FastBin, kNumFastBins> fast_bins_;
  std::array<SmallBin, kNumSmallBins> small_bins_;
  std::array<LargeBin, kNumLargeBins> large_bins_;
  std::array<uintptr_t, kNumBitmaps> bitmaps_;
};

/// @}

// Template method implementations.

template <typename BlockType>
constexpr DlAllocator<BlockType>::DlAllocator() {
  size_t index = 0;
  size_t size = 0;
  size_t bin_size = kMinSize;
  size_t bins_in_round = kNumSmallBins;
  while (bins_in_round > 1) {
    for (size_t i = 0; i < bins_in_round; ++i) {
      size += bin_size;
      if (index < kNumFastBins) {
        fast_bins_[index].set_max_inner_size(size);
      }
      if (index < kNumSmallBins) {
        small_bins_[index].set_max_inner_size(size);
      } else {
        large_bins_[index - kNumSmallBins].set_max_inner_size(size);
      }
      ++index;
    }
    bin_size *= 8;
    bins_in_round /= 2;
  }
  bitmaps_.fill(0);
}

template <typename BlockType>
size_t DlAllocator<BlockType>::DoGetMaxAllocatable() {
  ReleaseFastBins();
  size_t bitmap_index = bitmaps_.size() - 1;
  uintptr_t bitmap = bitmaps_[bitmap_index];
  while (bitmap_index != 0 && bitmap == 0) {
    --bitmap_index;
    bitmap = bitmaps_[bitmap_index];
  }
  if (bitmap == 0) {
    // No free blocks.
    return 0;
  }
  size_t bitmap_offset = kBitmapBits - internal::CountLZero(bitmap) - 1;
  size_t index = bitmap_index * kBitmapBits + bitmap_offset;
  const BlockType* largest =
      index < kNumSmallBins ? small_bins_[index].FindLargest()
                            : large_bins_[index - kNumSmallBins].FindLargest();
  return largest == nullptr ? 0 : largest->InnerSize();
}

template <typename BlockType>
BlockResult<BlockType> DlAllocator<BlockType>::ChooseBlock(Layout layout) {
  layout = Layout(std::max(layout.size(), kMinSize), layout.alignment());
  size_t index = GetIndex(layout.size());

  if (index < kNumSmallBins) {
    // First, check if there's a chunk available in the fast bucket.
    if (index < kNumFastBins) {
      FastBin& fast_bin = fast_bins_[index];
      BlockType* block = fast_bin.RemoveCompatible(layout);
      if (block != nullptr) {
        return BlockResult<BlockType>(block);
      }
    }
    // If the corresponding small bucket is empty, release the fast bins to
    // consolidate chunks and maybe get the requested size.
    if (small_bins_[index].empty()) {
      ReleaseFastBins();
    }
  } else {
    // Always release fast bins on large requests.
    ReleaseFastBins();
  }

  // Check the small, fixed-size buckets.
  for (; FindNextAvailable(index) && index < kNumSmallBins; ++index) {
    SmallBin& small_bin = small_bins_[index];
    BlockType* block = small_bin.RemoveCompatible(layout);
    if (block != nullptr) {
      UpdateBitmaps(index, small_bin.empty());
      return BlockType::AllocFirst(std::move(block), layout);
    }
  }

  // Check the larger, sorted buckets.
  for (; FindNextAvailable(index); ++index) {
    LargeBin& large_bin = large_bins_[index - kNumSmallBins];
    BlockType* block = large_bin.RemoveCompatible(layout);
    if (block != nullptr) {
      UpdateBitmaps(index, large_bin.empty());
      return BlockType::AllocFirst(std::move(block), layout);
    }
  }

  // No sufficiently large block found.
  return BlockResult<BlockType>(nullptr, Status::NotFound());
}

template <typename BlockType>
void DlAllocator<BlockType>::ReserveBlock(BlockType& block) {
  size_t index = GetIndex(block.InnerSize());
  if (index < kNumSmallBins) {
    SmallBin& small_bin = small_bins_[index];
    std::ignore = small_bin.Remove(block);
    UpdateBitmaps(index, small_bin.empty());
  } else {
    LargeBin& large_bin = large_bins_[index - kNumSmallBins];
    std::ignore = large_bin.Remove(block);
    UpdateBitmaps(index, large_bin.empty());
  }
}

template <typename BlockType>
void DlAllocator<BlockType>::RecycleBlock(BlockType& block) {
  size_t index = GetIndex(block.InnerSize());
  if (index < kNumSmallBins) {
    std::ignore = small_bins_[index].Add(block);
  } else {
    std::ignore = large_bins_[index - kNumSmallBins].Add(block);
  }
  UpdateBitmaps(index, false);
}

template <typename BlockType>
void DlAllocator<BlockType>::DeallocateBlock(BlockType*&& block) {
  // Add small blocks to the fast bins without actually freeing them.
  size_t index = block->InnerSize() / kMinSize;
  if (index < kNumFastBins) {
    std::ignore = fast_bins_[index].Add(*block);
  } else {
    Base::DeallocateBlock(std::move(block));
  }
}

template <typename BlockType>
size_t DlAllocator<BlockType>::GetIndex(size_t size) {
  size_t index = 0;
  size_t bin_size = kMinSize;
  size_t bins_in_round = kNumSmallBins;
  size = AlignUp(size, kMinSize) - kMinSize;
  while (bins_in_round > 1) {
    size_t round_size = bin_size * bins_in_round;
    if (size < round_size) {
      return index + (size / bin_size);
    }
    size -= round_size;
    index += bins_in_round;
    bin_size *= 8;
    bins_in_round /= 2;
  }
  return index;
}

template <typename BlockType>
void DlAllocator<BlockType>::ReleaseFastBins() {
  for (auto& fast_bin : fast_bins_) {
    while (!fast_bin.empty()) {
      BlockType* block = fast_bin.RemoveAny();
      if constexpr (Hardening::kIncludesDebugChecks) {
        PW_ASSERT(block != nullptr);
      }
      Base::DeallocateBlock(std::move(block));
    }
  }
}

template <typename BlockType>
bool DlAllocator<BlockType>::FindNextAvailable(size_t& index) {
  size_t bitmap_index = index / kBitmapBits;
  size_t bitmap_offset = index % kBitmapBits;
  uintptr_t bitmap = bitmaps_[bitmap_index] & (~uintptr_t(0) << bitmap_offset);
  while (bitmap == 0) {
    ++bitmap_index;
    if (bitmap_index >= kNumBitmaps) {
      return false;
    }
    bitmap = bitmaps_[bitmap_index];
  }
  auto bitmap_log2 = internal::CountRZero(bitmap);
  index = (bitmap_index * kBitmapBits) + bitmap_log2;
  return true;
}

template <typename BlockType>
void DlAllocator<BlockType>::UpdateBitmaps(size_t index, bool empty) {
  size_t bitmap_index = index / kBitmapBits;
  size_t bitmap_offset = index % kBitmapBits;
  uintptr_t bitmap = uintptr_t(1) << bitmap_offset;
  if (empty) {
    bitmaps_[bitmap_index] &= ~bitmap;
  } else {
    bitmaps_[bitmap_index] |= bitmap;
  }
}

}  // namespace pw::allocator
