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

#include "pw_allocator/dl_allocator.h"

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
using ::pw::allocator::test::Preallocation;
using BlockType = ::pw::allocator::DlBlock<uint16_t>;
using DlAllocator = ::pw::allocator::DlAllocator<BlockType>;
using BlockAllocatorTest =
    ::pw::allocator::test::BlockAllocatorTest<DlAllocator, 8192>;

class DlAllocatorTest : public BlockAllocatorTest {
 public:
  DlAllocatorTest() : BlockAllocatorTest(allocator_) {}

 private:
  DlAllocator allocator_;
};

// Unit tests.

TEST_F(DlAllocatorTest, AutomaticallyInit) {
  DlAllocator allocator(GetBytes());
  AutomaticallyInit(allocator);
}

TEST_F(DlAllocatorTest, ExplicitlyInit) {
  DlAllocator allocator;
  ExplicitlyInit(allocator);
}

TEST_F(DlAllocatorTest, GetCapacity) { GetCapacity(8192); }

TEST_F(DlAllocatorTest, AllocateLarge) { AllocateLarge(); }

TEST_F(DlAllocatorTest, AllocateSmall) { AllocateSmall(); }

TEST_F(DlAllocatorTest, AllocateLargeAlignment) { AllocateLargeAlignment(); }

TEST_F(DlAllocatorTest, AllocateAlignmentFailure) {
  AllocateAlignmentFailure();
}

TEST_F(DlAllocatorTest, AllocatesBestCompatible) {
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

TEST_F(DlAllocatorTest, DeallocateNull) { DeallocateNull(); }

TEST_F(DlAllocatorTest, DeallocateShuffled) { DeallocateShuffled(); }

TEST_F(DlAllocatorTest, IterateOverBlocks) { IterateOverBlocks(); }

TEST_F(DlAllocatorTest, ResizeNull) { ResizeNull(); }

TEST_F(DlAllocatorTest, ResizeLargeSame) { ResizeLargeSame(); }

TEST_F(DlAllocatorTest, ResizeLargeSmaller) { ResizeLargeSmaller(); }

TEST_F(DlAllocatorTest, ResizeLargeLarger) { ResizeLargeLarger(); }

TEST_F(DlAllocatorTest, ResizeLargeLargerFailure) {
  ResizeLargeLargerFailure();
}

TEST_F(DlAllocatorTest, ResizeSmallSame) { ResizeSmallSame(); }

TEST_F(DlAllocatorTest, ResizeSmallSmaller) { ResizeSmallSmaller(); }

TEST_F(DlAllocatorTest, ResizeSmallLarger) { ResizeSmallLarger(); }

TEST_F(DlAllocatorTest, ResizeSmallLargerFailure) {
  ResizeSmallLargerFailure();
}

TEST_F(DlAllocatorTest, GetMaxAllocatableWhenAllFree) {
  GetMaxAllocatableWhenAllFree();
}

TEST_F(DlAllocatorTest, GetMaxAllocatableWhenLargeFreeBlocksAvailable) {
  GetMaxAllocatableWhenLargeFreeBlocksAvailable();
}

TEST_F(DlAllocatorTest, GetMaxAllocatableWhenOnlySmallFreeBlocksAvailable) {
  GetMaxAllocatableWhenOnlySmallFreeBlocksAvailable();
}

TEST_F(DlAllocatorTest, GetMaxAllocatableWhenMultipleFreeBlocksAvailable) {
  GetMaxAllocatableWhenMultipleFreeBlocksAvailable();
}

TEST_F(DlAllocatorTest, GetMaxAllocatableWhenNoBlocksFree) {
  GetMaxAllocatableWhenNoBlocksFree();
}

TEST_F(DlAllocatorTest, MeasureFragmentation) { MeasureFragmentation(); }

// Fuzz tests.

using ::pw::allocator::test::BlockAlignedBuffer;
using ::pw::allocator::test::BlockAllocatorFuzzer;
using ::pw::allocator::test::DefaultArbitraryRequests;
using ::pw::allocator::test::Request;

void DoesNotCorruptBlocks(const pw::Vector<Request>& requests) {
  static BlockAlignedBuffer<BlockType> buffer;
  static DlAllocator allocator(buffer.as_span());
  static BlockAllocatorFuzzer fuzzer(allocator);
  fuzzer.DoesNotCorruptBlocks(requests);
}

FUZZ_TEST(DlAllocatorFuzzTest, DoesNotCorruptBlocks)
    .WithDomains(DefaultArbitraryRequests());

}  // namespace
