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

#include "pw_allocator/block/detailed_block.h"
#include "pw_allocator/block_allocator.h"
#include "pw_allocator/bucket/fast_sorted.h"
#include "pw_allocator/bucket/sorted.h"
#include "pw_allocator/internal/bit.h"

namespace pw::allocator {

/// @submodule{pw_allocator,concrete_block}

/// Alias for a default block type that is compatible with `TlsfAllocator`.
template <typename OffsetType>
using TlsfBlock = DetailedBlock<OffsetType, GenericFastSortedItem>;

/// Default values for the template parameters of `TlsfAllocator`.
///
/// By default, this is tuned for allocations between 64B and 64KB.
struct TlsfDefaults {
  /// Default maximum inner size of the smallest bucket in a TLSF allocator's
  /// two-dimensional array of buckets.
  static constexpr size_t kMinSize = 64;

  /// Default number of rows in a TLSF allocator's two-dimensional array of
  /// buckets.
  static constexpr size_t kNumShelves = 10;
};

/// Pair used to index a bucket in a two dimensional array.
struct TlsfIndices {
  size_t shelf;
  size_t bucket;
};

/// Two-layered, segregated fit allocator.
///
/// This allocator uses a two-dimensional array of buckets to quickly satisfy
/// memory allocations with best-fit blocks as described by
/// http://www.gii.upv.es/tlsf/files/papers/ecrts04_tlsf.pdf
///
/// This class refers to the "second-level arrays" in that paper as "shelves".
/// Each shelf holds an array of Buckets, and an instance of this class holds an
/// array of shelves. Conceptually, buckets can be thought of as being
/// organized on a set of "shelves", with each shelf having 16 buckets arranged
/// from smallest maximum inner size to largest. The smallest maximum inner size
/// on a shelf is a power of 2, and the shelves are arranged from the `kMinSize`
/// on the "bottom" to the largest maximum inner sizes on the "top". The last
/// bucket on the topmost shelf is unbounded to handle any blocks of arbitrary
/// size.
///
/// For example, if `kMinSize` is 64, and `kNumShelves` is 10, than the maximum
/// inner sizes of buckets on each shelf could be represented as:
///
/// @code
/// {
///   shelves_[9]: { 32k, 34k, ..., 62k, inf },
///           ...: { ..., ..., ..., ..., ... },
///   shelves_[1]: { 128, 136, ..., 240, 248 },
///   shelves_[0]: {  64,  68, ..., 120, 124 },
/// }
/// @endcode
///
/// @tparam   BlockType     Block implementation
/// @tparam   kMinSize      Maximum inner size of blocks in the first bucket on
///                         lowest shelf.
/// @tparam   kNumShelves   Number of rows in the two-dimensional array.
template <typename BlockType = TlsfBlock<uint32_t>,
          size_t kMinSize = TlsfDefaults::kMinSize,
          size_t kNumShelves = TlsfDefaults::kNumShelves>
class TlsfAllocator : public BlockAllocator<BlockType> {
 private:
  using Base = BlockAllocator<BlockType>;

  static constexpr size_t kNumBucketsPerShelf = 16;
  static constexpr size_t kBucketBits =
      internal::CountRZero(kNumBucketsPerShelf);

  using SmallBucket = ForwardSortedBucket<BlockType>;
  using LargeBucket = FastSortedBucket<BlockType>;
  using Shelf = std::array<LargeBucket, kNumBucketsPerShelf>;

  static_assert(kMinSize >= kNumBucketsPerShelf,
                "kMinSize must be at least 16.");
  static_assert(
      kMinSize >= sizeof(GenericFastSortedItem),
      "kMinSize must be large enough to hold a FastSortedBucket item.");
  static_assert((kMinSize & (kMinSize - 1)) == 0,
                "kMinSize must be a power of two.");

  static_assert(kNumShelves <= 32, "kNumShelves cannot be larger than 32");

 public:
  /// Constexpr constructor. Callers must explicitly call `Init`.
  constexpr TlsfAllocator();

  /// Non-constexpr constructor that automatically calls `Init`.
  ///
  /// @param[in]  region      Region of memory to use when satisfying allocation
  ///                         requests. The region MUST be valid as an argument
  ///                         to `BlockType::Init`.
  explicit TlsfAllocator(ByteSpan region) : TlsfAllocator() {
    Base::Init(region);
  }

 private:
  /// @copydoc BlockAllocator::GetMaxAllocatable
  size_t DoGetMaxAllocatable() override;

  /// @copydoc BlockAllocator::ChooseBlock
  BlockResult<BlockType> ChooseBlock(Layout layout) override;

  /// @copydoc BlockAllocator::ReserveBlock
  void ReserveBlock(BlockType& block) override;

  /// @copydoc BlockAllocator::RecycleBlock
  void RecycleBlock(BlockType& block) override;

  /// Returns the shelf and bucket indices for the bucket with the smallest
  /// maximum inner size greater than the given size.
  static TlsfIndices MapToIndices(size_t size);

  /// Starting with the bucket indicated by the given `indices`, searches for
  /// the non-empty bucket with the smallest maximum inner size. Updates the
  /// given `indices` and returns true if such a bucket is found; otherwise
  /// returns false.
  bool FindNextAvailable(TlsfIndices& indices);

  /// Updates the shelf and bucket bitmaps to reflect whether the bucket
  /// referenced by the given `indices` is `empty`.
  void UpdateBitmaps(const TlsfIndices& indices, bool empty);

  uint32_t shelf_bitmap_ = 0;
  std::array<uint16_t, kNumShelves> bucket_bitmaps_;
  std::array<Shelf, kNumShelves> shelves_;
  SmallBucket small_bucket_;
};

/// @}

// Template method implementations.

template <typename BlockType, size_t kMinSize, size_t kNumShelves>
constexpr TlsfAllocator<BlockType, kMinSize, kNumShelves>::TlsfAllocator() {
  size_t size = kMinSize;
  size_t step = kMinSize / kNumBucketsPerShelf;
  for (Shelf& shelf : shelves_) {
    for (LargeBucket& bucket : shelf) {
      size += step;
      bucket.set_max_inner_size(size - 1);
    }
    step *= 2;
  }

  // The largest bucket is unbounded.
  LargeBucket& largest = shelves_[kNumShelves - 1][kNumBucketsPerShelf - 1];
  largest.set_max_inner_size(std::numeric_limits<size_t>::max());

  bucket_bitmaps_.fill(0);
}

template <typename BlockType, size_t kMinSize, size_t kNumShelves>
size_t TlsfAllocator<BlockType, kMinSize, kNumShelves>::DoGetMaxAllocatable() {
  size_t shelf_index =
      shelf_bitmap_ == 0 ? 0 : (31 - internal::CountLZero(shelf_bitmap_));
  uint16_t bucket_bitmap = bucket_bitmaps_[shelf_index];
  size_t bucket_index =
      bucket_bitmap == 0 ? 0 : (15 - internal::CountLZero(bucket_bitmap));
  const LargeBucket& bucket = shelves_[shelf_index][bucket_index];
  const BlockType* largest =
      bucket.empty() ? small_bucket_.FindLargest() : bucket.FindLargest();
  return largest == nullptr ? 0 : largest->InnerSize();
}

template <typename BlockType, size_t kMinSize, size_t kNumShelves>
BlockResult<BlockType>
TlsfAllocator<BlockType, kMinSize, kNumShelves>::ChooseBlock(Layout layout) {
  // Check the small bucket.
  if (layout.size() < small_bucket_.max_inner_size()) {
    BlockType* block = small_bucket_.RemoveCompatible(layout);
    if (block != nullptr) {
      return BlockType::AllocFirst(std::move(block), layout);
    }
  }

  // Check the buckets on the shelves.
  for (TlsfIndices indices = MapToIndices(layout.size());
       FindNextAvailable(indices);
       indices.bucket++) {
    LargeBucket& bucket = shelves_[indices.shelf][indices.bucket];
    BlockType* block = bucket.RemoveCompatible(layout);
    if (block != nullptr) {
      UpdateBitmaps(indices, bucket.empty());
      return BlockType::AllocFirst(std::move(block), layout);
    }
  }

  // No sufficiently large block found.
  return BlockResult<BlockType>(nullptr, Status::NotFound());
}

template <typename BlockType, size_t kMinSize, size_t kNumShelves>
void TlsfAllocator<BlockType, kMinSize, kNumShelves>::ReserveBlock(
    BlockType& block) {
  if (block.InnerSize() < sizeof(typename LargeBucket::ItemType)) {
    std::ignore = small_bucket_.Remove(block);
    return;
  }
  TlsfIndices indices = MapToIndices(block.InnerSize());
  LargeBucket& large_bucket = shelves_[indices.shelf][indices.bucket];
  if (large_bucket.Remove(block)) {
    UpdateBitmaps(indices, large_bucket.empty());
  }
}

template <typename BlockType, size_t kMinSize, size_t kNumShelves>
void TlsfAllocator<BlockType, kMinSize, kNumShelves>::RecycleBlock(
    BlockType& block) {
  if (block.InnerSize() < sizeof(typename LargeBucket::ItemType)) {
    std::ignore = small_bucket_.Add(block);
    return;
  }
  TlsfIndices indices = MapToIndices(block.InnerSize());
  LargeBucket& large_bucket = shelves_[indices.shelf][indices.bucket];
  std::ignore = large_bucket.Add(block);
  UpdateBitmaps(indices, false);
}

template <typename BlockType, size_t kMinSize, size_t kNumShelves>
TlsfIndices TlsfAllocator<BlockType, kMinSize, kNumShelves>::MapToIndices(
    size_t size) {
  if (size <= kMinSize) {
    return TlsfIndices{.shelf = 0, .bucket = 0};
  }

  // Most significant bit set determines the shelf.
  auto shelf = internal::CountRZero(cpp20::bit_floor(size));
  // Each shelf has 16 buckets, so next 4 bits determine the bucket.
  auto bucket = static_cast<uint16_t>((size >> (shelf - kBucketBits)) & 0xF);

  // Adjust for minimum size, and clamp to the valid range.
  shelf -= internal::CountRZero(kMinSize);
  if (shelf >= kNumShelves) {
    shelf = kNumShelves - 1;
    bucket = kNumBucketsPerShelf - 1;
  }
  return TlsfIndices{.shelf = static_cast<uint32_t>(shelf), .bucket = bucket};
}

template <typename BlockType, size_t kMinSize, size_t kNumShelves>
bool TlsfAllocator<BlockType, kMinSize, kNumShelves>::FindNextAvailable(
    TlsfIndices& indices) {
  // Are we past the end of a shelf? If so, move up a shelf.
  if (indices.bucket == kNumBucketsPerShelf) {
    indices.shelf++;
    indices.bucket = 0;
  }

  // Have we passed the top shelf? If so, no larger blocks are available.
  if (indices.shelf >= kNumShelves) {
    return false;
  }

  // Use the bitmaps to find the next largest non-empty bucket.
  uint16_t bucket_bitmap =
      bucket_bitmaps_[indices.shelf] & (~uint32_t(0) << indices.bucket);
  if (bucket_bitmap != 0) {
    // There's at least one non-empty bucket on the current shelf whose
    // blocks are at least as large as the requested size.
    indices.bucket = internal::CountRZero(bucket_bitmap);
    return true;
  }

  // The buckets for large enough blocks on this shelf are all empty.
  // Move up to the first shelf with non-empty buckets and find the
  // non-empty bucket with the smallest blocks.
  uint32_t shelf_bitmap = shelf_bitmap_ & (~uint32_t(0) << (indices.shelf + 1));
  if (shelf_bitmap != 0) {
    indices.shelf = internal::CountRZero(shelf_bitmap);
    indices.bucket = internal::CountRZero(bucket_bitmaps_[indices.shelf]);
    return true;
  }

  // No larger blocks are available.
  return false;
}

template <typename BlockType, size_t kMinSize, size_t kNumShelves>
void TlsfAllocator<BlockType, kMinSize, kNumShelves>::UpdateBitmaps(
    const TlsfIndices& indices, bool empty) {
  auto bucket_bitmap = static_cast<uint16_t>(1 << indices.bucket);
  if (empty) {
    bucket_bitmaps_[indices.shelf] &= ~bucket_bitmap;
  } else {
    bucket_bitmaps_[indices.shelf] |= bucket_bitmap;
  }

  uint32_t shelf_bitmap = uint32_t(1) << indices.shelf;
  if (bucket_bitmaps_[indices.shelf] == 0) {
    shelf_bitmap_ &= ~shelf_bitmap;
  } else {
    shelf_bitmap_ |= shelf_bitmap;
  }
}

}  // namespace pw::allocator
