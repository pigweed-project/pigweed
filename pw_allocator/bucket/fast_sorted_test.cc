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

#include "pw_allocator/bucket/fast_sorted.h"

#include <cstddef>
#include <cstdint>

#include "pw_allocator/block/detailed_block.h"
#include "pw_allocator/bucket/testing.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::allocator::FastSortedBucket;
using ::pw::allocator::GenericFastSortedItem;
using ::pw::allocator::test::BucketTest;

using BlockType =
    ::pw::allocator::DetailedBlock<uint32_t, GenericFastSortedItem>;
using FastSortedBucketTest = BucketTest<FastSortedBucket<BlockType>>;

TEST_F(FastSortedBucketTest, SetsAndGetsMaxInnerSize) {
  SetsAndGetsMaxInnerSize();
}

TEST_F(FastSortedBucketTest, AddsAndRemovesBlocks) { AddsAndRemovesBlocks(); }

TEST_F(FastSortedBucketTest, FailsToAddWhenBlockIsTooSmall) {
  FailsToAddWhenBlockIsTooSmall();
}

TEST_F(FastSortedBucketTest, FindsLargestWhenEmpty) { FindsLargestWhenEmpty(); }

TEST_F(FastSortedBucketTest, FindsLargestWithBlocks) {
  FindsLargestWithBlocks();
}

TEST_F(FastSortedBucketTest, FailsToRemoveBlockWhenNotFound) {
  FailsToRemoveBlockWhenNotFound();
}

TEST_F(FastSortedBucketTest, RemovesUnspecifiedBlock) {
  RemovesUnspecifiedBlock();
}

TEST_F(FastSortedBucketTest, RemovesByLayout) { RemovesByLayout(); }

TEST_F(FastSortedBucketTest, FailsToRemoveByExcessiveSize) {
  FailsToRemoveByExcessiveSize();
}

TEST_F(FastSortedBucketTest, CanIterate) { CanIterate(); }

TEST_F(FastSortedBucketTest, RemovesBlocksInOrderOfIncreasingSize) {
  FastSortedBucket<BlockType>& bucket = this->bucket();

  BlockType& block1 = CreateBlock(kLayout1);
  BlockType& block2 = CreateBlock(kLayout2);
  BlockType& block3 = CreateBlock(kLayout3);

  // Added out of order.
  EXPECT_TRUE(bucket.Add(block2));
  EXPECT_TRUE(bucket.Add(block3));
  EXPECT_TRUE(bucket.Add(block1));

  // Removed in order.
  EXPECT_EQ(bucket.RemoveAny(), &block1);
  EXPECT_EQ(bucket.RemoveAny(), &block2);
  EXPECT_EQ(bucket.RemoveAny(), &block3);
  EXPECT_TRUE(bucket.empty());
}

using ::pw::allocator::ReverseFastSortedBucket;

using ReverseFastSortedBucketTest =
    BucketTest<ReverseFastSortedBucket<BlockType>>;

TEST_F(ReverseFastSortedBucketTest, FindsLargestWhenEmpty) {
  FindsLargestWhenEmpty();
}

TEST_F(ReverseFastSortedBucketTest, FindsLargestWithBlocks) {
  FindsLargestWithBlocks();
}

TEST_F(ReverseFastSortedBucketTest, CanIterate) { CanIterate(); }

TEST_F(ReverseFastSortedBucketTest, RemovesByLayout) { RemovesByLayout(); }

TEST_F(ReverseFastSortedBucketTest, RemovesBlocksInOrderOfDecreasingSize) {
  ReverseFastSortedBucket<BlockType>& bucket = this->bucket();

  BlockType& block1 = CreateBlock(kLayout1);
  BlockType& block2 = CreateBlock(kLayout2);
  BlockType& block3 = CreateBlock(kLayout3);

  // Added out of order.
  EXPECT_TRUE(bucket.Add(block2));
  EXPECT_TRUE(bucket.Add(block3));
  EXPECT_TRUE(bucket.Add(block1));

  // Removed in order.
  EXPECT_EQ(bucket.RemoveAny(), &block3);
  EXPECT_EQ(bucket.RemoveAny(), &block2);
  EXPECT_EQ(bucket.RemoveAny(), &block1);
  EXPECT_TRUE(bucket.empty());
}

}  // namespace
