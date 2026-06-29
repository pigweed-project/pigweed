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

#include "lib/stdcompat/bit.h"
#include "pw_allocator/block/detailed_block.h"
#include "pw_allocator/block/testing.h"
#include "pw_allocator/block_allocator.h"
#include "pw_allocator/deallocator.h"
#include "pw_allocator/fuzzing.h"
#include "pw_allocator/layout.h"
#include "pw_allocator/test_harness.h"
#include "pw_assert/assert.h"
#include "pw_bytes/alignment.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

namespace pw::allocator::test {

/// @submodule{pw_allocator,impl_test}

/// Test fixture responsible for managing a memory region and an allocator that
/// allocates block of memory from it.
///
/// @tparam   BlockAllocatorType  Type of the `BlockAllocator` being tested.
/// @tparam   kCapacity           Size of the memory region to use in the tests.
///                               Must be large enough so that `BlockType::Init`
///                               does not fail.
template <typename BlockAllocatorType, size_t kCapacity = kDefaultCapacity>
class BlockAllocatorTest : public ::testing::Test {
 public:
  using BlockType = typename BlockAllocatorType::BlockType;

  static constexpr size_t OuterSize(size_t inner_size) {
    return BlockType::OuterSizeFromInnerSize(inner_size);
  }

  // The number of allocated pointers cached by the test fixture.
  static constexpr size_t kNumPtrs = 16;

  // Represents the sizes of various allocations.
  static constexpr size_t kLargeInnerSize = kCapacity / 8;
  static constexpr size_t kLargeOuterSize = OuterSize(kLargeInnerSize);

  static constexpr size_t kSmallInnerSize = BlockType::kBlockOverhead * 2;
  static constexpr size_t kSmallOuterSize = OuterSize(kSmallInnerSize);

  static constexpr size_t kSmallerOuterSize = kSmallInnerSize;
  static constexpr size_t kLargerOuterSize =
      kLargeOuterSize + kSmallerOuterSize;

 protected:
  // Test fixtures.
  BlockAllocatorTest(BlockAllocatorType& allocator) : allocator_(allocator) {
    ptrs_.fill(nullptr);
  }

  /// Returns the underlying memory region.
  ByteSpan GetBytes() { return util_.bytes(); }

  /// Initialize the allocator with a region of memory and return it as a
  /// generic Allocator.
  pw::Allocator& GetGenericAllocator() { return GetAllocator(); }

  /// Initialize the allocator with a sequence of preallocated blocks and return
  /// it as a generic allocator.
  ///
  /// See also ``Preallocation``.
  pw::Allocator& GetGenericAllocator(
      std::initializer_list<Preallocation> preallocations) {
    return GetAllocator(preallocations);
  }

  /// Initialize the allocator with a region of memory and return it as a block
  /// allocator.
  BlockAllocatorType& GetAllocator();

  /// Initialize the allocator with a sequence of preallocated blocks and return
  /// it as a block allocator.
  ///
  /// See also ``Preallocation``.
  BlockAllocatorType& GetAllocator(
      std::initializer_list<Preallocation> preallocations);

  /// Store an allocated pointer in the test's cache of pointers.
  void Store(size_t index, void* ptr);

  /// Retrieve an allocated pointer from the test's cache of pointers.
  void* Fetch(size_t index);

  /// Gets the next allocation from an allocated pointer.
  void* NextAfter(size_t index);

  /// Swaps the pointer at indices `i` and `j`.
  void Swap(size_t i, size_t j);

  /// Ensures the memory is usable by writing to it.
  void UseMemory(void* ptr, size_t size);

  /// Frees any allocated memory.
  void TearDown() override;

  // Unit tests.
  static void AutomaticallyInit(BlockAllocatorType& allocator);
  void ExplicitlyInit(BlockAllocatorType& allocator);
  void GetCapacity(size_t expected = kCapacity);
  void AllocateLarge();
  void AllocateSmall();
  void AllocateTooLarge();
  void AllocateLargeAlignment();
  void AllocateAlignmentFailure();
  void DeallocateNull();
  void DeallocateShuffled();
  void ResizeNull();
  void ResizeLargeSame();
  void ResizeLargeSmaller();
  void ResizeLargeLarger();
  void ResizeLargeLargerFailure();
  void ResizeSmallSame();
  void ResizeSmallSmaller();
  void ResizeSmallLarger();
  void ResizeSmallLargerFailure();
  void ResizeGhostBlock();
  void IterateOverBlocks();
  void GetMaxAllocatableWhenAllFree();
  void GetMaxAllocatableWhenLargeFreeBlocksAvailable();
  void GetMaxAllocatableWhenOnlySmallFreeBlocksAvailable();
  void GetMaxAllocatableWhenMultipleFreeBlocksAvailable();
  void GetMaxAllocatableWhenNoBlocksFree();
  void MeasureFragmentation();
  void PoisonPeriodically();

  // Fuzz tests.
  void NoCorruptedBlocks();

 private:
  BlockAllocatorType& allocator_;
  BlockTestUtilities<BlockType, kCapacity> util_;
  std::array<void*, kNumPtrs> ptrs_;
};

/// @endsubmodule

////////////////////////////////////////////////////////////////////////////////
// Test fixture template method implementations.

template <typename BlockAllocatorType, size_t kCapacity>
BlockAllocatorType&
BlockAllocatorTest<BlockAllocatorType, kCapacity>::GetAllocator() {
  allocator_.Init(GetBytes());
  return allocator_;
}

template <typename BlockAllocatorType, size_t kCapacity>
BlockAllocatorType&
BlockAllocatorTest<BlockAllocatorType, kCapacity>::GetAllocator(
    std::initializer_list<Preallocation> preallocations) {
  auto* first = util_.Preallocate(preallocations);
  size_t index = 0;
  for (BlockType* block = first; block != nullptr; block = block->Next()) {
    Store(index, block->IsFree() ? nullptr : block->UsableSpace());
    ++index;
  }

  BlockAllocator<BlockType>& allocator = allocator_;
  allocator.Init(first);

  return allocator_;
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType, kCapacity>::Store(size_t index,
                                                              void* ptr) {
  PW_ASSERT(index < kNumPtrs);
  PW_ASSERT(ptr == nullptr || ptrs_[index] == nullptr);
  ptrs_[index] = ptr;
}

template <typename BlockAllocatorType, size_t kCapacity>
void* BlockAllocatorTest<BlockAllocatorType, kCapacity>::Fetch(size_t index) {
  return index < kNumPtrs ? ptrs_[index] : nullptr;
}

template <typename BlockAllocatorType, size_t kCapacity>
void* BlockAllocatorTest<BlockAllocatorType, kCapacity>::NextAfter(
    size_t index) {
  void* ptr = Fetch(index);
  if (ptr == nullptr) {
    return nullptr;
  }

  auto* block = BlockType::FromUsableSpace(ptr);
  block = block->Next();
  while (block != nullptr && block->IsFree()) {
    block = block->Next();
  }
  return block == nullptr ? nullptr : block->UsableSpace();
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType, kCapacity>::Swap(size_t i,
                                                             size_t j) {
  std::swap(ptrs_[i], ptrs_[j]);
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType, kCapacity>::UseMemory(void* ptr,
                                                                  size_t size) {
  std::memset(ptr, 0x5a, size);
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType, kCapacity>::TearDown() {
  for (size_t i = 0; i < kNumPtrs; ++i) {
    void* ptr = Fetch(i);
    if (ptr != nullptr) {
      allocator_.Deallocate(ptr);
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
// Unit tests template method implementations.

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType, kCapacity>::AutomaticallyInit(
    BlockAllocatorType& allocator) {
  EXPECT_NE(*(allocator.blocks().begin()), nullptr);
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType, kCapacity>::ExplicitlyInit(
    BlockAllocatorType& allocator) {
  EXPECT_EQ(*(allocator.blocks().begin()), nullptr);
  allocator.Init(GetBytes());
  EXPECT_NE(*(allocator.blocks().begin()), nullptr);
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType, kCapacity>::GetCapacity(
    size_t expected) {
  pw::Allocator& allocator = GetGenericAllocator();
  StatusWithSize capacity = allocator.GetCapacity();
  EXPECT_EQ(capacity.status(), OkStatus());
  EXPECT_EQ(capacity.size(), expected);
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType, kCapacity>::AllocateLarge() {
  pw::Allocator& allocator = GetGenericAllocator();
  constexpr Layout layout = Layout::Of<std::byte[kLargeInnerSize]>();
  Store(0, allocator.Allocate(layout));
  ASSERT_NE(Fetch(0), nullptr);
  ByteSpan bytes = GetBytes();
  EXPECT_GE(Fetch(0), bytes.data());
  EXPECT_LE(Fetch(0), bytes.data() + bytes.size());
  UseMemory(Fetch(0), layout.size());
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType, kCapacity>::AllocateSmall() {
  pw::Allocator& allocator = GetGenericAllocator();
  constexpr Layout layout = Layout::Of<std::byte[kSmallInnerSize]>();
  Store(0, allocator.Allocate(layout));
  ASSERT_NE(Fetch(0), nullptr);
  ByteSpan bytes = GetBytes();
  EXPECT_GE(Fetch(0), bytes.data());
  EXPECT_LE(Fetch(0), bytes.data() + bytes.size());
  UseMemory(Fetch(0), layout.size());
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType, kCapacity>::AllocateTooLarge() {
  pw::Allocator& allocator = GetGenericAllocator();
  Store(0, allocator.Allocate(Layout::Of<std::byte[kCapacity * 2]>()));
  EXPECT_EQ(Fetch(0), nullptr);
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType,
                        kCapacity>::AllocateLargeAlignment() {
  if constexpr (is_alignable_v<BlockType>) {
    pw::Allocator& allocator = GetGenericAllocator();

    constexpr size_t kAlignment = 64;
    Store(0, allocator.Allocate(Layout(kLargeInnerSize, kAlignment)));
    ASSERT_NE(Fetch(0), nullptr);
    EXPECT_TRUE(IsAlignedAs(Fetch(0), kAlignment));
    UseMemory(Fetch(0), kLargeInnerSize);

    Store(1, allocator.Allocate(Layout(kLargeInnerSize, kAlignment)));
    ASSERT_NE(Fetch(1), nullptr);
    EXPECT_TRUE(IsAlignedAs(Fetch(1), kAlignment));
    UseMemory(Fetch(1), kLargeInnerSize);
  } else {
    static_assert(is_alignable_v<BlockType>);
  }
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType,
                        kCapacity>::AllocateAlignmentFailure() {
  if constexpr (is_alignable_v<BlockType>) {
    // Allocate a two blocks with an unaligned region between them.
    constexpr size_t kAlignment = 128;
    ByteSpan bytes = GetBytes();
    size_t outer_size =
        GetAlignedOffsetAfter(bytes.data(), kAlignment, kSmallInnerSize) +
        kAlignment;
    pw::Allocator& allocator = GetGenericAllocator({
        {outer_size, Preallocation::kUsed},
        {kLargeOuterSize, Preallocation::kFree},
        {Preallocation::kSizeRemaining, Preallocation::kUsed},
    });

    // The allocator should be unable to create an aligned region..
    Store(1, allocator.Allocate(Layout(kLargeInnerSize, kAlignment)));
    EXPECT_EQ(Fetch(1), nullptr);
  } else {
    static_assert(is_alignable_v<BlockType>);
  }
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType, kCapacity>::DeallocateNull() {
  pw::Allocator& allocator = GetGenericAllocator();
  allocator.Deallocate(nullptr);
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType, kCapacity>::DeallocateShuffled() {
  pw::Allocator& allocator = GetGenericAllocator();
  constexpr Layout layout = Layout::Of<std::byte[kSmallInnerSize]>();
  for (size_t i = 0; i < kNumPtrs; ++i) {
    Store(i, allocator.Allocate(layout));
    if (Fetch(i) == nullptr) {
      break;
    }
  }

  // Mix up the order of allocations.
  for (size_t i = 0; i < kNumPtrs; ++i) {
    if (i % 2 == 0 && i + 1 < kNumPtrs) {
      Swap(i, i + 1);
    }
    if (i % 3 == 0 && i + 2 < kNumPtrs) {
      Swap(i, i + 2);
    }
  }

  // Deallocate everything.
  for (size_t i = 0; i < kNumPtrs; ++i) {
    allocator.Deallocate(Fetch(i));
    Store(i, nullptr);
  }
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType, kCapacity>::ResizeNull() {
  pw::Allocator& allocator = GetGenericAllocator();
  size_t new_size = 1;
  EXPECT_FALSE(allocator.Resize(nullptr, new_size));
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType, kCapacity>::ResizeLargeSame() {
  pw::Allocator& allocator = GetGenericAllocator({
      {kLargeOuterSize, Preallocation::kUsed},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  size_t new_size = kLargeInnerSize;
  ASSERT_TRUE(allocator.Resize(Fetch(0), new_size));
  UseMemory(Fetch(0), kLargeInnerSize);
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType, kCapacity>::ResizeLargeSmaller() {
  pw::Allocator& allocator = GetGenericAllocator({
      {kLargeOuterSize, Preallocation::kUsed},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  size_t new_size = kSmallInnerSize;
  ASSERT_TRUE(allocator.Resize(Fetch(0), new_size));
  UseMemory(Fetch(0), kSmallInnerSize);
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType, kCapacity>::ResizeLargeLarger() {
  pw::Allocator& allocator = GetGenericAllocator({
      {kLargeOuterSize, Preallocation::kUsed},
      {kLargeOuterSize, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  size_t new_size = kLargeInnerSize * 2;
  ASSERT_TRUE(allocator.Resize(Fetch(0), new_size));
  UseMemory(Fetch(0), kLargeInnerSize * 2);
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType,
                        kCapacity>::ResizeLargeLargerFailure() {
  pw::Allocator& allocator = GetGenericAllocator({
      {kLargeOuterSize, Preallocation::kUsed},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  // Memory after ptr is already allocated, so `Resize` should fail.
  size_t new_size = kLargeInnerSize * 2;
  EXPECT_FALSE(allocator.Resize(Fetch(0), new_size));
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType, kCapacity>::ResizeSmallSame() {
  pw::Allocator& allocator = GetGenericAllocator({
      {kSmallOuterSize, Preallocation::kUsed},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  size_t new_size = kSmallInnerSize;
  ASSERT_TRUE(allocator.Resize(Fetch(0), new_size));
  UseMemory(Fetch(0), kSmallInnerSize);
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType, kCapacity>::ResizeSmallSmaller() {
  pw::Allocator& allocator = GetGenericAllocator({
      {kSmallOuterSize, Preallocation::kUsed},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  size_t new_size = kSmallInnerSize / 2;
  ASSERT_TRUE(allocator.Resize(Fetch(0), new_size));
  UseMemory(Fetch(0), kSmallInnerSize / 2);
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType, kCapacity>::ResizeSmallLarger() {
  pw::Allocator& allocator = GetGenericAllocator({
      {kSmallOuterSize, Preallocation::kUsed},
      {kSmallOuterSize, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  size_t new_size = kSmallInnerSize * 2;
  ASSERT_TRUE(allocator.Resize(Fetch(0), new_size));
  UseMemory(Fetch(0), kSmallInnerSize * 2);
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType,
                        kCapacity>::ResizeSmallLargerFailure() {
  pw::Allocator& allocator = GetGenericAllocator({
      {kSmallOuterSize, Preallocation::kUsed},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  // Memory after ptr is already allocated, so `Resize` should fail.
  size_t new_size = kSmallInnerSize * 2 + BlockType::kBlockOverhead;
  EXPECT_FALSE(allocator.Resize(Fetch(0), new_size));
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType, kCapacity>::ResizeGhostBlock() {
  pw::Allocator& allocator = GetGenericAllocator({
      {kSmallOuterSize, Preallocation::kUsed},
      {kSmallOuterSize, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });

  size_t failed_new_size = kSmallInnerSize * 2 + BlockType::kBlockOverhead + 1;
  EXPECT_FALSE(allocator.Resize(Fetch(0), failed_new_size));

  void* ptr = allocator.Allocate(Layout(kSmallInnerSize, 1));
  EXPECT_NE(ptr, nullptr);

  if (ptr != nullptr) {
    Store(1, ptr);
  }
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType, kCapacity>::IterateOverBlocks() {
  pw::Allocator& allocator = GetGenericAllocator({
      {kSmallOuterSize, Preallocation::kFree},
      {kLargeOuterSize, Preallocation::kUsed},
      {kSmallOuterSize, Preallocation::kFree},
      {kLargeOuterSize, Preallocation::kUsed},
      {kSmallOuterSize, Preallocation::kFree},
      {kLargeOuterSize, Preallocation::kUsed},
      {Preallocation::kSizeRemaining, Preallocation::kFree},
  });

  // Count the blocks. The unallocated ones vary in size, but the allocated ones
  // should all be the same.
  size_t free_count = 0;
  size_t used_count = 0;
  auto& block_allocator = static_cast<BlockAllocatorType&>(allocator);
  for (auto* block : block_allocator.blocks()) {
    if (block->IsFree()) {
      ++free_count;
    } else {
      EXPECT_EQ(block->OuterSize(), kLargeOuterSize);
      ++used_count;
    }
  }
  EXPECT_EQ(used_count, 3U);
  EXPECT_EQ(free_count, 4U);
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType,
                        kCapacity>::GetMaxAllocatableWhenAllFree() {
  BlockAllocatorType& allocator = GetAllocator();
  EXPECT_EQ(allocator.GetMaxAllocatable(),
            kCapacity - BlockType::kBlockOverhead);
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType, kCapacity>::
    GetMaxAllocatableWhenLargeFreeBlocksAvailable() {
  BlockAllocatorType& allocator = GetAllocator({
      {kSmallOuterSize, Preallocation::kUsed},
      {kLargeOuterSize, Preallocation::kFree},
      {kSmallOuterSize, Preallocation::kUsed},
      {kLargeOuterSize, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  EXPECT_EQ(allocator.GetMaxAllocatable(), kLargeInnerSize);
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType, kCapacity>::
    GetMaxAllocatableWhenOnlySmallFreeBlocksAvailable() {
  BlockAllocatorType& allocator = GetAllocator({
      {kLargeOuterSize, Preallocation::kUsed},
      {kSmallOuterSize, Preallocation::kFree},
      {kLargeOuterSize, Preallocation::kUsed},
      {kSmallOuterSize, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  EXPECT_EQ(allocator.GetMaxAllocatable(), kSmallInnerSize);
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType, kCapacity>::
    GetMaxAllocatableWhenMultipleFreeBlocksAvailable() {
  BlockAllocatorType& allocator = GetAllocator({
      {OuterSize(kSmallInnerSize * 3), Preallocation::kFree},
      {OuterSize(kSmallInnerSize * 1), Preallocation::kUsed},
      {OuterSize(kSmallInnerSize * 4), Preallocation::kFree},
      {OuterSize(kSmallInnerSize * 1), Preallocation::kUsed},
      {OuterSize(kSmallInnerSize * 5), Preallocation::kFree},
      {OuterSize(kSmallInnerSize * 9), Preallocation::kUsed},
      {OuterSize(kSmallInnerSize * 2), Preallocation::kFree},
      {OuterSize(kSmallInnerSize * 6), Preallocation::kUsed},
      {OuterSize(kSmallInnerSize * 5), Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  EXPECT_EQ(allocator.GetMaxAllocatable(), kSmallInnerSize * 5);
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType,
                        kCapacity>::GetMaxAllocatableWhenNoBlocksFree() {
  BlockAllocatorType& allocator = GetAllocator({
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  EXPECT_EQ(allocator.GetMaxAllocatable(), 0U);
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType, kCapacity>::MeasureFragmentation() {
  pw::Allocator& allocator = GetGenericAllocator({
      {0x020, Preallocation::kFree},
      {0x040, Preallocation::kUsed},
      {0x080, Preallocation::kFree},
      {0x100, Preallocation::kUsed},
      {0x200, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });

  auto& block_allocator = static_cast<BlockAllocatorType&>(allocator);
  constexpr size_t alignment = BlockAllocatorType::BlockType::kAlignment;
  size_t sum_of_squares = 0;
  size_t sum = 0;
  for (const auto block : block_allocator.blocks()) {
    if (block->IsFree()) {
      size_t inner_size = block->InnerSize() / alignment;
      sum_of_squares += inner_size * inner_size;
      sum += inner_size;
    }
  }

  auto fragmentation = block_allocator.MeasureFragmentation();
  ASSERT_TRUE(fragmentation.has_value());
  EXPECT_EQ(fragmentation->sum_of_squares.hi, 0U);
  EXPECT_EQ(fragmentation->sum_of_squares.lo, sum_of_squares);
  EXPECT_EQ(fragmentation->sum, sum);
}

template <typename BlockAllocatorType, size_t kCapacity>
void BlockAllocatorTest<BlockAllocatorType, kCapacity>::PoisonPeriodically() {
  if constexpr (is_poisonable_v<BlockType>) {
    // Allocate 8 blocks to prevent every other from being merged when freed.
    pw::Allocator& allocator = GetGenericAllocator({
        {kLargeOuterSize, Preallocation::kUsed},
        {kLargeOuterSize, Preallocation::kUsed},
        {kLargeOuterSize, Preallocation::kUsed},
        {kLargeOuterSize, Preallocation::kUsed},
        {kLargeOuterSize, Preallocation::kUsed},
        {kLargeOuterSize, Preallocation::kUsed},
        {kLargeOuterSize, Preallocation::kUsed},
        {Preallocation::kSizeRemaining, Preallocation::kUsed},
    });
    ASSERT_LT(BlockType::kPoisonOffset, kLargeInnerSize);

    // Since the test poisons blocks, it cannot iterate over the blocks without
    // crashing. Use `Fetch` instead.
    for (size_t i = 0; i < 8; ++i) {
      if (i % 2 != 0) {
        continue;
      }
      auto* bytes = cpp20::bit_cast<std::byte*>(Fetch(i));
      auto* block = BlockType::FromUsableSpace(bytes);
      allocator.Deallocate(bytes);
      EXPECT_TRUE(block->IsFree());
      EXPECT_TRUE(block->IsValid());
      bytes[BlockType::kPoisonOffset] = ~bytes[BlockType::kPoisonOffset];

      if (i == 6) {
        // The test_config is defined to only detect corruption is on every
        // fourth freed block. Fix up the block to avoid crashing on teardown.
        EXPECT_FALSE(block->IsValid());
        bytes[BlockType::kPoisonOffset] = ~bytes[BlockType::kPoisonOffset];
      } else {
        EXPECT_TRUE(block->IsValid());
      }
      Store(i, nullptr);
    }
  } else {
    static_assert(is_poisonable_v<BlockType>);
  }
}

// Fuzz test support.

template <typename BlockAllocatorType>
class BlockAllocatorFuzzer : public TestHarness {
 public:
  explicit BlockAllocatorFuzzer(BlockAllocatorType& allocator)
      : TestHarness(allocator), allocator_(allocator) {}

  void DoesNotCorruptBlocks(const pw::Vector<Request>& requests) {
    HandleRequests(requests);
    for (const auto& block : allocator_.blocks()) {
      ASSERT_TRUE(block->IsValid());
    }
  }

 private:
  BlockAllocatorType& allocator_;
};

template <typename BlockAllocatorType>
BlockAllocatorFuzzer(BlockAllocatorType&)
    -> BlockAllocatorFuzzer<BlockAllocatorType>;

}  // namespace pw::allocator::test
