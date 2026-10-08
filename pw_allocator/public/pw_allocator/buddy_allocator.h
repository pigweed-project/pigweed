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

#include "pw_allocator/abstract_allocator.h"
#include "pw_allocator/block/basic.h"
#include "pw_allocator/bucket/unordered.h"
#include "pw_bytes/span.h"
#include "pw_containers/vector.h"
#include "pw_status/try.h"

namespace pw::allocator {
namespace internal {

/// A specialized block used by the BuddyAllocator.
///
/// Buddy blocks have outer sizes that are powers of two. When smaller blocks
/// are needed, a block is split into left and right halves of equal size. These
/// half-blocks are "buddies", and when both are freed they are merged back into
/// a single block.
class BuddyBlock : public BasicBlock<BuddyBlock> {
 public:
  BuddyBlock(size_t outer_size);

  /// Simplified version of `Allocatable::CanAlloc`, as needed for
  /// `UnorderedBucket::Remove`.
  StatusWithSize CanAlloc(Layout layout) const;

  /// Splits a block in half and returns the new block.
  BuddyBlock* Split();

  /// Merges two blocks together.
  static BuddyBlock* Merge(BuddyBlock*&& left, BuddyBlock*&& right);

 private:
  // `Basic` required methods.
  using Basic = BasicBlock<BuddyBlock>;
  friend Basic;
  static constexpr size_t DefaultAlignment() { return 1; }
  static constexpr size_t BlockOverhead() { return sizeof(BuddyBlock); }
  static constexpr size_t MinInnerSize() { return sizeof(UnorderedItem); }
  constexpr size_t OuterSizeUnchecked() const { return 1U << outer_size_log2_; }

  uint8_t outer_size_log2_;
};

/// Size-independent buddy allocator.
///
/// This allocator allocates blocks of memory whose sizes are powers of two.
/// See also https://en.wikipedia.org/wiki/Buddy_memory_allocation.
///
/// Compared to `BuddyAllocator`, this implementation is size-agnostic with
/// respect to the number of buckets.
class GenericBuddyAllocator : public AbstractAllocator {
 public:
  static constexpr Capabilities kCapabilities =
      kImplementsGetUsableLayout | kImplementsGetAllocatedLayout |
      kImplementsGetCapacity | kImplementsRecognizes;

  static constexpr size_t kDefaultMinOuterSize = 16;
  static constexpr size_t kDefaultNumBuckets = 16;

  /// Sets up buckets for a buddy allocator.
  ///
  /// @param[in] buckets        Storage for buckets of free blocks.
  /// @param[in] min_outer_size Outer size of the blocks in the first bucket.
  void InitBuckets(span<UnorderedBucket<BuddyBlock>> buckets,
                   size_t min_outer_size);

  /// Sets the memory used to allocate blocks.
  void Init(ByteSpan region);

 protected:
  constexpr GenericBuddyAllocator() : AbstractAllocator(kCapabilities) {}

  /// @copydoc Allocator::Allocate
  void* DoAllocate(Layout layout) override;

  /// @copydoc Deallocator::DoDeallocate
  void DoDeallocate(void* ptr) override;

  /// @copydoc Allocator::MeasureFragmentation
  std::optional<Fragmentation> DoMeasureFragmentation() const override;

  /// @copydoc Deallocator::GetInfo
  Result<Layout> DoGetInfo(InfoType info_type, const void* ptr) const override;

  /// Ensures all allocations have been freed. Crashes with a diagnostic message
  /// If any allocations remain outstanding.
  void CrashIfAllocated();

 private:
  span<UnorderedBucket<BuddyBlock>> buckets_;
  size_t min_outer_size_ = 0;
  ByteSpan region_;
};

}  // namespace internal

/// @submodule{pw_allocator,concrete}

/// Allocator that uses the buddy memory allocation algorithm.
///
/// This allocator allocates blocks of memory whose sizes are powers of two.
/// This allows the allocator to satisfy requests to acquire and release memory
/// very quickly, at the possible cost of higher internal fragmentation. In
/// particular:
///
/// * The maximum alignment for this allocator is `kMinInnerSize`.
/// * The minimum size of an allocation is `kMinInnerSize`. Less may be
///   requested, but it will be satisfied by a block of that inner size.
/// * The maximum size of an allocation is `kMinInnerSize << (kNumBuckets - 1)`.
///
/// Use this allocator if you know the needed sizes are close to but less than
/// the block inner sizes and you need high allocator performance.
///
/// @tparam   kMinOuterSize   Outer size of the smallest allocatable block. Must
///                           be a power of two. All allocations will use at
///                           least this much memory.
/// @tparam   kNumBuckets     Number of buckets. Must be at least 1. Each
///                           additional bucket allows combining blocks into
///                           larger blocks.
template <size_t kMinOuterSize_ =
              internal::GenericBuddyAllocator::kDefaultMinOuterSize,
          size_t kNumBuckets =
              internal::GenericBuddyAllocator::kDefaultNumBuckets>
class BuddyAllocator : public internal::GenericBuddyAllocator {
 private:
  using Base = internal::GenericBuddyAllocator;

 public:
  static constexpr size_t kMinOuterSize = kMinOuterSize_;

  static_assert((kMinOuterSize & (kMinOuterSize - 1)) == 0,
                "kMinOuterSize must be a power of 2");

  /// Constructs an allocator. Callers must call `Init`.
  constexpr BuddyAllocator() { Base::InitBuckets(buckets_, kMinOuterSize); }

  /// Constructs an allocator, and initializes it with the given memory region.
  ///
  /// @param[in]  region  Region of memory to use when satisfying allocation
  ///                     requests. The region MUST be large enough to fit a
  ///                     least one minimally-size `BuddyBlock`.
  BuddyAllocator(ByteSpan region) : BuddyAllocator() { Base::Init(region); }

  ~BuddyAllocator() override { Base::CrashIfAllocated(); }

 private:
  std::array<UnorderedBucket<internal::BuddyBlock>, kNumBuckets> buckets_;
};

/// @}

}  // namespace pw::allocator
