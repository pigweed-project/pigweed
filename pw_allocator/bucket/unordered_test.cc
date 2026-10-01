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

#include "pw_allocator/bucket/unordered.h"

#include <cstddef>
#include <cstdint>

#include "pw_allocator/block/detailed_block.h"
#include "pw_allocator/bucket/testing.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::allocator::UnorderedBucket;
using ::pw::allocator::UnorderedItem;
using ::pw::allocator::test::BucketTest;

using BlockType = ::pw::allocator::DetailedBlock<uint32_t, UnorderedItem>;
using UnorderedBucketTest = BucketTest<UnorderedBucket<BlockType>>;

TEST_F(UnorderedBucketTest, SetsAndGetsMaxInnerSize) {
  SetsAndGetsMaxInnerSize();
}

TEST_F(UnorderedBucketTest, AddsAndRemovesBlocks) { AddsAndRemovesBlocks(); }

TEST_F(UnorderedBucketTest, FailsToAddWhenBlockIsTooSmall) {
  FailsToAddWhenBlockIsTooSmall();
}

TEST_F(UnorderedBucketTest, FindsLargestWhenEmpty) { FindsLargestWhenEmpty(); }

TEST_F(UnorderedBucketTest, FindsLargestWithBlocks) {
  FindsLargestWithBlocks();
}

TEST_F(UnorderedBucketTest, FailsToRemoveBlockWhenNotFound) {
  FailsToRemoveBlockWhenNotFound();
}

TEST_F(UnorderedBucketTest, RemovesUnspecifiedBlock) {
  RemovesUnspecifiedBlock();
}

TEST_F(UnorderedBucketTest, RemovesByLayout) { RemovesByLayout(); }

TEST_F(UnorderedBucketTest, FailsToRemoveByExcessiveSize) {
  FailsToRemoveByExcessiveSize();
}

TEST_F(UnorderedBucketTest, CanIterate) { CanIterate(); }

}  // namespace
