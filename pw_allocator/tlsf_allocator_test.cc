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

#include "pw_allocator/tlsf_allocator.h"

#include <cstdint>

#include "pw_allocator/block/testing.h"
#include "pw_allocator/block_allocator_testing.h"
#include "pw_allocator/layout.h"
#include "pw_unit_test/framework.h"

namespace {

// Test fixtures.

using ::pw::allocator::Layout;
using ::pw::allocator::test::Preallocation;
using BlockType = ::pw::allocator::TlsfBlock<uint16_t>;
using TlsfAllocator = ::pw::allocator::TlsfAllocator<BlockType>;
using BlockAllocatorTest =
    ::pw::allocator::test::BlockAllocatorTest<TlsfAllocator>;

class TlsfAllocatorTest : public BlockAllocatorTest {
 public:
  TlsfAllocatorTest() : BlockAllocatorTest(allocator_) {}

 private:
  TlsfAllocator allocator_;
};

// Unit tests.

TEST_F(TlsfAllocatorTest, AutomaticallyInit) {
  TlsfAllocator allocator(GetBytes());
  AutomaticallyInit(allocator);
}

TEST_F(TlsfAllocatorTest, ExplicitlyInit) {
  TlsfAllocator allocator;
  ExplicitlyInit(allocator);
}

TEST_F(TlsfAllocatorTest, GetCapacity) { GetCapacity(); }

TEST_F(TlsfAllocatorTest, AllocateLarge) { AllocateLarge(); }

TEST_F(TlsfAllocatorTest, AllocateSmall) { AllocateSmall(); }

TEST_F(TlsfAllocatorTest, AllocateLargeAlignment) { AllocateLargeAlignment(); }

TEST_F(TlsfAllocatorTest, AllocateAlignmentFailure) {
  AllocateAlignmentFailure();
}

TEST_F(TlsfAllocatorTest, AllocatesBestCompatible) {
  auto& allocator = GetAllocator({
      {kLargeOuterSize, Preallocation::kFree},
      {kSmallerOuterSize, Preallocation::kUsed},
      {kSmallOuterSize, Preallocation::kFree},
      {kSmallerOuterSize, Preallocation::kUsed},
      {kLargerOuterSize, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });

  void* ptr1 = allocator.Allocate(Layout(kSmallInnerSize, 1));
  EXPECT_LT(Fetch(1), ptr1);
  EXPECT_LT(ptr1, Fetch(3));

  void* ptr2 = allocator.Allocate(Layout(kSmallInnerSize, 1));
  EXPECT_LT(ptr2, Fetch(1));

  // A second small block fits in the leftovers of the first "Large" block.
  void* ptr3 = allocator.Allocate(Layout(kSmallInnerSize, 1));
  EXPECT_LT(ptr3, Fetch(1));

  allocator.Deallocate(ptr1);
  allocator.Deallocate(ptr2);
  allocator.Deallocate(ptr3);
}

TEST_F(TlsfAllocatorTest, DeallocateNull) { DeallocateNull(); }

TEST_F(TlsfAllocatorTest, DeallocateShuffled) { DeallocateShuffled(); }

TEST_F(TlsfAllocatorTest, IterateOverBlocks) { IterateOverBlocks(); }

TEST_F(TlsfAllocatorTest, ResizeNull) { ResizeNull(); }

TEST_F(TlsfAllocatorTest, ResizeLargeSame) { ResizeLargeSame(); }

TEST_F(TlsfAllocatorTest, ResizeLargeSmaller) { ResizeLargeSmaller(); }

TEST_F(TlsfAllocatorTest, ResizeLargeLarger) { ResizeLargeLarger(); }

TEST_F(TlsfAllocatorTest, ResizeLargeLargerFailure) {
  ResizeLargeLargerFailure();
}

TEST_F(TlsfAllocatorTest, ResizeSmallSame) { ResizeSmallSame(); }

TEST_F(TlsfAllocatorTest, ResizeSmallSmaller) { ResizeSmallSmaller(); }

TEST_F(TlsfAllocatorTest, ResizeSmallLarger) { ResizeSmallLarger(); }

TEST_F(TlsfAllocatorTest, ResizeSmallLargerFailure) {
  ResizeSmallLargerFailure();
}

TEST_F(TlsfAllocatorTest, GetMaxAllocatableWhenAllFree) {
  GetMaxAllocatableWhenAllFree();
}

TEST_F(TlsfAllocatorTest, GetMaxAllocatableWhenLargeFreeBlocksAvailable) {
  GetMaxAllocatableWhenLargeFreeBlocksAvailable();
}

TEST_F(TlsfAllocatorTest, GetMaxAllocatableWhenOnlySmallFreeBlocksAvailable) {
  GetMaxAllocatableWhenOnlySmallFreeBlocksAvailable();
}

TEST_F(TlsfAllocatorTest, GetMaxAllocatableWhenMultipleFreeBlocksAvailable) {
  GetMaxAllocatableWhenMultipleFreeBlocksAvailable();
}

TEST_F(TlsfAllocatorTest, GetMaxAllocatableWhenNoBlocksFree) {
  GetMaxAllocatableWhenNoBlocksFree();
}

TEST_F(TlsfAllocatorTest, MeasureFragmentation) { MeasureFragmentation(); }

TEST_F(TlsfAllocatorTest, PoisonPeriodically) { PoisonPeriodically(); }

}  // namespace
