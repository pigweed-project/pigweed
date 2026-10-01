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

#include "pw_allocator/bucket/sorted.h"

#include <cstddef>
#include <cstdint>

#include "pw_allocator/block/detailed_block.h"
#include "pw_allocator/bucket/testing.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::allocator::ForwardSortedBucket;
using ::pw::allocator::SortedItem;
using ::pw::allocator::test::BucketTest;

using BlockType = ::pw::allocator::DetailedBlock<uint32_t, SortedItem>;
using ForwardSortedBucketTest = BucketTest<ForwardSortedBucket<BlockType>>;

TEST_F(ForwardSortedBucketTest, SetsAndGetsMaxInnerSize) {
  SetsAndGetsMaxInnerSize();
}

TEST_F(ForwardSortedBucketTest, AddsAndRemovesBlocks) {
  AddsAndRemovesBlocks();
}

TEST_F(ForwardSortedBucketTest, FailsToAddWhenBlockIsTooSmall) {
  FailsToAddWhenBlockIsTooSmall();
}

TEST_F(ForwardSortedBucketTest, FindsLargestWhenEmpty) {
  FindsLargestWhenEmpty();
}

TEST_F(ForwardSortedBucketTest, FindsLargestWithBlocks) {
  FindsLargestWithBlocks();
}

TEST_F(ForwardSortedBucketTest, FailsToRemoveBlockWhenNotFound) {
  FailsToRemoveBlockWhenNotFound();
}

TEST_F(ForwardSortedBucketTest, RemovesUnspecifiedBlock) {
  RemovesUnspecifiedBlock();
}

TEST_F(ForwardSortedBucketTest, RemovesByLayout) { RemovesByLayout(); }

TEST_F(ForwardSortedBucketTest, FailsToRemoveByExcessiveSize) {
  FailsToRemoveByExcessiveSize();
}

TEST_F(ForwardSortedBucketTest, CanIterate) { CanIterate(); }

TEST_F(ForwardSortedBucketTest, RemovesBlocksInOrderOfIncreasingSize) {
  ForwardSortedBucket<BlockType>& bucket = this->bucket();

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

using ::pw::allocator::ReverseSortedBucket;

using ReverseSortedBucketTest = BucketTest<ReverseSortedBucket<BlockType>>;

TEST_F(ReverseSortedBucketTest, FindsLargestWhenEmpty) {
  FindsLargestWhenEmpty();
}

TEST_F(ReverseSortedBucketTest, FindsLargestWithBlocks) {
  FindsLargestWithBlocks();
}

TEST_F(ReverseSortedBucketTest, CanIterate) { CanIterate(); }

TEST_F(ReverseSortedBucketTest, RemovesByLayout) { RemovesByLayout(); }

TEST_F(ReverseSortedBucketTest, RemovesBlocksInOrderOfDecreasingSize) {
  ReverseSortedBucket<BlockType>& bucket = this->bucket();

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
