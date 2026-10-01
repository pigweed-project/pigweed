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

#include "pw_allocator/bucket/sequenced.h"

#include <cstddef>
#include <cstdint>

#include "pw_allocator/block/detailed_block.h"
#include "pw_allocator/bucket/testing.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::allocator::SequencedBucket;
using ::pw::allocator::SequencedItem;
using ::pw::allocator::test::BucketTest;

using BlockType = ::pw::allocator::DetailedBlock<uint32_t, SequencedItem>;
using SequencedBucketTest = BucketTest<SequencedBucket<BlockType>>;

TEST_F(SequencedBucketTest, SetsAndGetsMaxInnerSize) {
  SetsAndGetsMaxInnerSize();
}

TEST_F(SequencedBucketTest, AddsAndRemovesBlocks) { AddsAndRemovesBlocks(); }

TEST_F(SequencedBucketTest, FailsToAddWhenBlockIsTooSmall) {
  FailsToAddWhenBlockIsTooSmall();
}

TEST_F(SequencedBucketTest, FindsLargestWhenEmpty) { FindsLargestWhenEmpty(); }

TEST_F(SequencedBucketTest, FindsLargestWithBlocks) {
  FindsLargestWithBlocks();
}

TEST_F(SequencedBucketTest, FailsToRemoveBlockWhenNotFound) {
  FailsToRemoveBlockWhenNotFound();
}

TEST_F(SequencedBucketTest, RemovesUnspecifiedBlock) {
  RemovesUnspecifiedBlock();
}

TEST_F(SequencedBucketTest, RemovesByLayout) { RemovesByLayout(); }

TEST_F(SequencedBucketTest, FailsToRemoveByExcessiveSize) {
  FailsToRemoveByExcessiveSize();
}

TEST_F(SequencedBucketTest, CanIterate) { CanIterate(); }

TEST_F(SequencedBucketTest, CanAddAndRemoveWithThreshold) {
  SequencedBucket<BlockType>& bucket = this->bucket();
  bucket.set_threshold(kLayout2.size());

  // Create blocks, and use the some duplicate sizes.
  BlockType& block1 = CreateBlockAndAddToBucket(kLayout1);
  BlockType& block2 = CreateBlockAndAddToBucket(kLayout2);
  BlockType& block3 = CreateBlockAndAddToBucket(kLayout3);
  BlockType& block4 = CreateBlockAndAddToBucket(kLayout1);
  BlockType& block5 = CreateBlockAndAddToBucket(kLayout3);

  // Since `kBlockSize3` is greater the threshold, the bucket should be searched
  // from the beginning and find block 3 before block 5.
  EXPECT_EQ(bucket.RemoveCompatible(kLayout3), &block3);
  EXPECT_EQ(bucket.RemoveCompatible(kLayout3), &block5);

  EXPECT_EQ(bucket.RemoveCompatible(kLayout2), &block2);

  // Since `kBlockSize1` is less the threshold, the bucket should be searched
  // from the end and find block 4 before block 1.
  EXPECT_EQ(bucket.RemoveCompatible(kLayout1), &block4);
  EXPECT_EQ(bucket.RemoveCompatible(kLayout1), &block1);
  EXPECT_TRUE(bucket.empty());
}

}  // namespace
