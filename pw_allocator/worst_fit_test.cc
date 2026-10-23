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

#include "pw_allocator/worst_fit.h"

#include <cstdint>

#include "pw_allocator/block/testing.h"
#include "pw_allocator/block_allocator_testing.h"
#include "pw_allocator/fuzzing.h"
#include "pw_allocator/layout.h"
#include "pw_allocator/test_harness.h"
#include "pw_containers/vector.h"
#include "pw_fuzzer/fuzztest.h"
#include "pw_unit_test/framework.h"

namespace {

// Test fixtures.

using ::pw::allocator::Layout;
using ::pw::allocator::test::BlockAllocatorTest;
using ::pw::allocator::test::Preallocation;
using BlockType = ::pw::allocator::WorstFitBlock<uint16_t>;
using WorstFitAllocator = ::pw::allocator::WorstFitAllocator<BlockType>;

class WorstFitAllocatorTest : public BlockAllocatorTest<WorstFitAllocator> {
 public:
  WorstFitAllocatorTest() : BlockAllocatorTest(allocator_) {}

 private:
  WorstFitAllocator allocator_;
};

// Unit tests.

TEST_F(WorstFitAllocatorTest, AutomaticallyInit) {
  WorstFitAllocator allocator(GetBytes());
  AutomaticallyInit(allocator);
}

TEST_F(WorstFitAllocatorTest, ExplicitlyInit) {
  WorstFitAllocator allocator;
  ExplicitlyInit(allocator);
}

TEST_F(WorstFitAllocatorTest, GetCapacity) { GetCapacity(); }

TEST_F(WorstFitAllocatorTest, AllocateLarge) { AllocateLarge(); }

TEST_F(WorstFitAllocatorTest, AllocateSmall) { AllocateSmall(); }

TEST_F(WorstFitAllocatorTest, AllocateLargeAlignment) {
  AllocateLargeAlignment();
}

TEST_F(WorstFitAllocatorTest, AllocateAlignmentFailure) {
  AllocateAlignmentFailure();
}

TEST_F(WorstFitAllocatorTest, AllocatesWorstCompatible) {
  auto& allocator = GetAllocator({
      {kLargeOuterSize, Preallocation::kFree},    // 0
      {kSmallerOuterSize, Preallocation::kUsed},  // 1
      {kSmallOuterSize, Preallocation::kFree},    // 2
      {kSmallerOuterSize, Preallocation::kUsed},  // 3
      {kLargeOuterSize, Preallocation::kFree},    // 4
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });

  void* ptr1 = allocator.Allocate(Layout(kSmallInnerSize, 1));
  EXPECT_LT(ptr1, Fetch(1));

  void* ptr2 = allocator.Allocate(Layout(kSmallInnerSize, 1));
  EXPECT_LT(Fetch(3), ptr2);
  EXPECT_LT(ptr2, Fetch(5));

  // A second small block fits in the leftovers of the first "Large" block.
  void* ptr3 = allocator.Allocate(Layout(kSmallInnerSize, 1));
  EXPECT_LT(ptr3, Fetch(1));

  allocator.Deallocate(ptr1);
  allocator.Deallocate(ptr2);
  allocator.Deallocate(ptr3);
}

TEST_F(WorstFitAllocatorTest, DeallocateNull) { DeallocateNull(); }

TEST_F(WorstFitAllocatorTest, DeallocateShuffled) { DeallocateShuffled(); }

TEST_F(WorstFitAllocatorTest, IterateOverBlocks) { IterateOverBlocks(); }

TEST_F(WorstFitAllocatorTest, ResizeNull) { ResizeNull(); }

TEST_F(WorstFitAllocatorTest, ResizeLargeSame) { ResizeLargeSame(); }

TEST_F(WorstFitAllocatorTest, ResizeLargeSmaller) { ResizeLargeSmaller(); }

TEST_F(WorstFitAllocatorTest, ResizeLargeLarger) { ResizeLargeLarger(); }

TEST_F(WorstFitAllocatorTest, ResizeLargeLargerFailure) {
  ResizeLargeLargerFailure();
}

TEST_F(WorstFitAllocatorTest, ResizeSmallSame) { ResizeSmallSame(); }

TEST_F(WorstFitAllocatorTest, ResizeSmallSmaller) { ResizeSmallSmaller(); }

TEST_F(WorstFitAllocatorTest, ResizeSmallLarger) { ResizeSmallLarger(); }

TEST_F(WorstFitAllocatorTest, ResizeSmallLargerFailure) {
  ResizeSmallLargerFailure();
}

TEST_F(WorstFitAllocatorTest, GetMaxAllocatableWhenAllFree) {
  GetMaxAllocatableWhenAllFree();
}

TEST_F(WorstFitAllocatorTest, GetMaxAllocatableWhenLargeFreeBlocksAvailable) {
  GetMaxAllocatableWhenLargeFreeBlocksAvailable();
}

TEST_F(WorstFitAllocatorTest,
       GetMaxAllocatableWhenOnlySmallFreeBlocksAvailable) {
  GetMaxAllocatableWhenOnlySmallFreeBlocksAvailable();
}

TEST_F(WorstFitAllocatorTest,
       GetMaxAllocatableWhenMultipleFreeBlocksAvailable) {
  GetMaxAllocatableWhenMultipleFreeBlocksAvailable();
}

TEST_F(WorstFitAllocatorTest, GetMaxAllocatableWhenNoBlocksFree) {
  GetMaxAllocatableWhenNoBlocksFree();
}

TEST_F(WorstFitAllocatorTest, MeasureFragmentation) { MeasureFragmentation(); }

TEST_F(WorstFitAllocatorTest, PoisonPeriodically) { PoisonPeriodically(); }

// Fuzz tests.

using ::pw::allocator::test::BlockAlignedBuffer;
using ::pw::allocator::test::BlockAllocatorFuzzer;
using ::pw::allocator::test::DefaultArbitraryRequests;
using ::pw::allocator::test::Request;

void DoesNotCorruptBlocks(const pw::Vector<Request>& requests) {
  static BlockAlignedBuffer<BlockType> buffer;
  static WorstFitAllocator allocator(buffer.as_span());
  static BlockAllocatorFuzzer fuzzer(allocator);
  fuzzer.DoesNotCorruptBlocks(requests);
}

FUZZ_TEST(WorstFitAllocatorFuzzTest, DoesNotCorruptBlocks)
    .WithDomains(DefaultArbitraryRequests());

}  // namespace
