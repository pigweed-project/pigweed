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

#include "pw_allocator/first_fit.h"

#include <cstddef>
#include <cstdint>
#include <limits>

#include "pw_allocator/block/detailed_block.h"
#include "pw_allocator/block/testing.h"
#include "pw_allocator/block_allocator_testing.h"
#include "pw_allocator/fuzzing.h"
#include "pw_allocator/layout.h"
#include "pw_allocator/test_harness.h"
#include "pw_containers/vector.h"
#include "pw_fuzzer/fuzztest.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::allocator::Layout;
using ::pw::allocator::test::Preallocation;
using BlockType = ::pw::allocator::FirstFitBlock<uint16_t>;
using FirstFitAllocator = ::pw::allocator::FirstFitAllocator<BlockType>;
using ::pw::allocator::test::BlockAllocatorTest;

// Minimum size of a "large" allocation; allocation less than this size are
// considered "small" when using the "dual first fit" strategy.
static constexpr size_t kThreshold =
    BlockAllocatorTest<FirstFitAllocator>::kSmallInnerSize * 2;

class FirstFitAllocatorTest : public BlockAllocatorTest<FirstFitAllocator> {
 public:
  FirstFitAllocatorTest() : BlockAllocatorTest(allocator_) {}

 private:
  FirstFitAllocator allocator_;
};

TEST_F(FirstFitAllocatorTest, AutomaticallyInit) {
  FirstFitAllocator allocator(GetBytes());
  AutomaticallyInit(allocator);
}

TEST_F(FirstFitAllocatorTest, ExplicitlyInit) {
  FirstFitAllocator allocator;
  ExplicitlyInit(allocator);
}

TEST_F(FirstFitAllocatorTest, GetCapacity) { GetCapacity(); }

TEST_F(FirstFitAllocatorTest, AllocateLarge) { AllocateLarge(); }

TEST_F(FirstFitAllocatorTest, AllocateSmall) { AllocateSmall(); }

TEST_F(FirstFitAllocatorTest, AllocateLargeAlignment) {
  AllocateLargeAlignment();
}

TEST_F(FirstFitAllocatorTest, AllocateAlignmentFailure) {
  AllocateAlignmentFailure();
}

TEST_F(FirstFitAllocatorTest, AllocatesFirstCompatible) {
  FirstFitAllocator& allocator = GetAllocator({
      {kSmallOuterSize, Preallocation::kFree},
      {kSmallerOuterSize, Preallocation::kUsed},
      {kSmallOuterSize, Preallocation::kFree},
      {kSmallerOuterSize, Preallocation::kUsed},
      {kLargeOuterSize, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });

  Store(0, allocator.Allocate(Layout(kSmallInnerSize, 1)));
  EXPECT_EQ(NextAfter(0), Fetch(1));
  Store(4, allocator.Allocate(Layout(kLargeInnerSize, 1)));
  EXPECT_EQ(NextAfter(3), Fetch(4));
  EXPECT_EQ(NextAfter(4), Fetch(5));
}

TEST_F(FirstFitAllocatorTest, AllocatesLastCompatible) {
  FirstFitAllocator& allocator = GetAllocator({
      {kLargeOuterSize, Preallocation::kFree},
      {kSmallerOuterSize, Preallocation::kUsed},
      {kSmallOuterSize, Preallocation::kFree},
      {kSmallerOuterSize, Preallocation::kUsed},
      {kSmallOuterSize, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
  });
  allocator.set_threshold(std::numeric_limits<size_t>::max());

  Store(0, allocator.Allocate(Layout(kLargeInnerSize, 1)));
  EXPECT_EQ(NextAfter(0), Fetch(1));
  Store(4, allocator.Allocate(Layout(kSmallInnerSize, 1)));
  EXPECT_EQ(NextAfter(3), Fetch(4));
  EXPECT_EQ(NextAfter(4), Fetch(5));
}

TEST_F(FirstFitAllocatorTest, AllocatesUsingThreshold) {
  FirstFitAllocator& allocator = GetAllocator({
      {kLargeOuterSize, Preallocation::kFree},
      {kSmallerOuterSize, Preallocation::kUsed},
      {kSmallOuterSize, Preallocation::kFree},
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
      {kLargeOuterSize, Preallocation::kFree},
      {kSmallerOuterSize, Preallocation::kUsed},
      {kSmallOuterSize, Preallocation::kFree},
  });
  allocator.set_threshold(kThreshold);

  Store(0, allocator.Allocate(Layout(kLargeInnerSize, 1)));
  EXPECT_EQ(NextAfter(0), Fetch(1));
  Store(4, allocator.Allocate(Layout(kLargeInnerSize, 1)));
  EXPECT_EQ(NextAfter(3), Fetch(4));
  EXPECT_EQ(NextAfter(4), Fetch(5));
  Store(6, allocator.Allocate(Layout(kSmallInnerSize, 1)));
  EXPECT_EQ(NextAfter(5), Fetch(6));
  EXPECT_EQ(NextAfter(6), Fetch(7));
  Store(2, allocator.Allocate(Layout(kSmallInnerSize, 1)));
  EXPECT_EQ(NextAfter(1), Fetch(2));
  EXPECT_EQ(NextAfter(2), Fetch(3));
}

TEST_F(FirstFitAllocatorTest, AllocatesFromCorrectEnd) {
  FirstFitAllocator allocator(GetBytes());
  allocator.set_threshold(kThreshold);

  void* ptr1 = allocator.Allocate(Layout(kSmallInnerSize, 1));
  ASSERT_NE(ptr1, nullptr);

  void* ptr2 = allocator.Allocate(Layout(kLargeInnerSize, 1));
  ASSERT_NE(ptr2, nullptr);

  auto* block_a = BlockType::FromUsableSpace(ptr2);
  auto* block_b = block_a->Next();
  auto* block_c = BlockType::FromUsableSpace(ptr1);

  ASSERT_NE(block_b, nullptr);
  EXPECT_TRUE(block_b->IsFree());
  EXPECT_EQ(block_b->Next(), block_c);

  allocator.Deallocate(ptr1);
  allocator.Deallocate(ptr2);
}

TEST_F(FirstFitAllocatorTest, DeallocateNull) { DeallocateNull(); }

TEST_F(FirstFitAllocatorTest, DeallocateShuffled) { DeallocateShuffled(); }

TEST_F(FirstFitAllocatorTest, IterateOverBlocks) { IterateOverBlocks(); }

TEST_F(FirstFitAllocatorTest, ResizeNull) { ResizeNull(); }

TEST_F(FirstFitAllocatorTest, ResizeLargeSame) { ResizeLargeSame(); }

TEST_F(FirstFitAllocatorTest, ResizeLargeSmaller) { ResizeLargeSmaller(); }

TEST_F(FirstFitAllocatorTest, ResizeLargeLarger) { ResizeLargeLarger(); }

TEST_F(FirstFitAllocatorTest, ResizeLargeLargerFailure) {
  ResizeLargeLargerFailure();
}

TEST_F(FirstFitAllocatorTest, ResizeSmallSame) { ResizeSmallSame(); }

TEST_F(FirstFitAllocatorTest, ResizeSmallSmaller) { ResizeSmallSmaller(); }

TEST_F(FirstFitAllocatorTest, ResizeSmallLarger) { ResizeSmallLarger(); }

TEST_F(FirstFitAllocatorTest, ResizeSmallLargerFailure) {
  ResizeSmallLargerFailure();
}

TEST_F(FirstFitAllocatorTest, ResizeLargeSmallerAcrossThreshold) {
  FirstFitAllocator& allocator = GetAllocator({
      {kThreshold * 2, Preallocation::kUsed},
      {Preallocation::kSizeRemaining, Preallocation::kFree},
  });
  allocator.set_threshold(kThreshold);

  // Shrinking succeeds, and the pointer is unchanged even though it is now
  // below the threshold.
  size_t new_size = kThreshold / 2;
  ASSERT_TRUE(allocator.Resize(Fetch(0), new_size));
  UseMemory(Fetch(0), kThreshold / 2);
}

TEST_F(FirstFitAllocatorTest, ResizeSmallLargerAcrossThreshold) {
  FirstFitAllocator& allocator = GetAllocator({
      {Preallocation::kSizeRemaining, Preallocation::kUsed},
      {kThreshold / 2, Preallocation::kUsed},
      {kThreshold * 2, Preallocation::kFree},
  });
  allocator.set_threshold(kThreshold);

  // Growing succeeds, and the pointer is unchanged even though it is now
  // above the threshold.
  size_t new_size = kThreshold * 2;
  ASSERT_TRUE(allocator.Resize(Fetch(1), new_size));
  UseMemory(Fetch(1), kThreshold * 2);
}

TEST_F(FirstFitAllocatorTest, GetMaxAllocatableWhenAllFree) {
  GetMaxAllocatableWhenAllFree();
}

TEST_F(FirstFitAllocatorTest, GetMaxAllocatableWhenLargeFreeBlocksAvailable) {
  GetMaxAllocatableWhenLargeFreeBlocksAvailable();
}

TEST_F(FirstFitAllocatorTest,
       GetMaxAllocatableWhenOnlySmallFreeBlocksAvailable) {
  GetMaxAllocatableWhenOnlySmallFreeBlocksAvailable();
}

TEST_F(FirstFitAllocatorTest, GetMaxAllocatableWhenNoBlocksFree) {
  GetMaxAllocatableWhenNoBlocksFree();
}

TEST_F(FirstFitAllocatorTest, MeasureFragmentation) { MeasureFragmentation(); }

TEST_F(FirstFitAllocatorTest, PoisonPeriodically) { PoisonPeriodically(); }

// Fuzz tests.

using ::pw::allocator::test::BlockAlignedBuffer;
using ::pw::allocator::test::BlockAllocatorFuzzer;
using ::pw::allocator::test::DefaultArbitraryRequests;
using ::pw::allocator::test::Request;

void DoesNotCorruptBlocks(const pw::Vector<Request>& requests) {
  static BlockAlignedBuffer<BlockType> buffer;
  static FirstFitAllocator allocator(buffer.as_span());
  static BlockAllocatorFuzzer fuzzer(allocator);
  fuzzer.DoesNotCorruptBlocks(requests);
}

FUZZ_TEST(FirstFitAllocatorFuzzTest, DoesNotCorruptBlocks)
    .WithDomains(DefaultArbitraryRequests());

void WorksWithAnyThreshold(const pw::Vector<Request>& requests,
                           size_t threshold) {
  static BlockAlignedBuffer<BlockType> buffer;
  FirstFitAllocator allocator(buffer.as_span(), threshold);
  BlockAllocatorFuzzer fuzzer(allocator);
  fuzzer.DoesNotCorruptBlocks(requests);
}

FUZZ_TEST(FirstFitAllocatorFuzzTest, WorksWithAnyThreshold)
    .WithDomains(DefaultArbitraryRequests(), fuzztest::Arbitrary<size_t>());

}  // namespace
