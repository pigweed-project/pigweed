// Copyright 2020 The Pigweed Authors
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

#include "pw_allocator/block/detailed_block.h"

#include <cstdint>

#include "pw_allocator/block/unit_tests.h"
#include "pw_unit_test/framework.h"

namespace {

// Use a very small offset type to test failure of an overly large block without
// requiring excessive memory for the test.

using TinyOffsetDetailedBlock = ::pw::allocator::DetailedBlock<uint8_t>;
using TinyOffsetDetailedBlockTest =
    ::pw::allocator::test::BlockTest<TinyOffsetDetailedBlock>;

TEST_F(TinyOffsetDetailedBlockTest, CannotCreateTooLargeBlock) {
  CannotCreateTooLargeBlock();
}

// Unit tests for DetailedBlock with a small offset field.

using SmallOffsetDetailedBlock = ::pw::allocator::DetailedBlock<uint16_t>;
using SmallOffsetDetailedBlockTest =
    ::pw::allocator::test::BlockTest<SmallOffsetDetailedBlock>;

PW_ALLOCATOR_BASIC_BLOCK_TESTS(SmallOffsetDetailedBlockTest)
PW_ALLOCATOR_ALLOCATABLE_BLOCK_TESTS(SmallOffsetDetailedBlockTest)
PW_ALLOCATOR_ALIGNABLE_BLOCK_TESTS(SmallOffsetDetailedBlockTest)
PW_ALLOCATOR_POISONABLE_BLOCK_TESTS(SmallOffsetDetailedBlockTest)
PW_ALLOCATOR_BLOCK_WITH_LAYOUT_TESTS(SmallOffsetDetailedBlockTest)

// Unit tests for DetailedBlock with a large offset field.

using LargeOffsetDetailedBlock = ::pw::allocator::DetailedBlock<uint64_t>;
using LargeOffsetDetailedBlockTest =
    ::pw::allocator::test::BlockTest<LargeOffsetDetailedBlock>;

PW_ALLOCATOR_BASIC_BLOCK_TESTS(LargeOffsetDetailedBlockTest)
PW_ALLOCATOR_ALLOCATABLE_BLOCK_TESTS(LargeOffsetDetailedBlockTest)
PW_ALLOCATOR_ALIGNABLE_BLOCK_TESTS(LargeOffsetDetailedBlockTest)
PW_ALLOCATOR_POISONABLE_BLOCK_TESTS(LargeOffsetDetailedBlockTest)
PW_ALLOCATOR_BLOCK_WITH_LAYOUT_TESTS(LargeOffsetDetailedBlockTest)

}  // namespace
