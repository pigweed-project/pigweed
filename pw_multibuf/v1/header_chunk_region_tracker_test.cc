// Copyright 2023 The Pigweed Authors
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

#include "pw_multibuf/v1/header_chunk_region_tracker.h"

#include <cstddef>
#include <optional>

#include "pw_allocator/testing.h"
#include "pw_multibuf/v1/chunk.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

namespace pw::multibuf::v1 {
namespace {

using allocator::test::AllocatorForTest;

const size_t kArbitraryAllocatorSize = 1024;
const size_t kArbitraryChunkSize = 32;

TEST(HeaderChunkRegionTracker, AllocatesRegionAsChunk) {
  AllocatorForTest<kArbitraryAllocatorSize> alloc;
  std::optional<OwnedChunk> chunk =
      HeaderChunkRegionTracker::AllocateRegionAsChunk(alloc,
                                                      kArbitraryChunkSize);
  ASSERT_TRUE(chunk.has_value());
}

TEST(HeaderChunkRegionTracker, AllocatedRegionAsChunkTooLarge) {
  AllocatorForTest<kArbitraryAllocatorSize> alloc;
  std::optional<OwnedChunk> chunk =
      HeaderChunkRegionTracker::AllocateRegionAsChunk(alloc,
                                                      kArbitraryAllocatorSize);
  ASSERT_FALSE(chunk.has_value());
}

TEST(HeaderChunkRegionTracker, AllocatesRegion) {
  AllocatorForTest<kArbitraryAllocatorSize> alloc;
  auto tracker =
      HeaderChunkRegionTracker::AllocateRegion(alloc, kArbitraryChunkSize);
  ASSERT_NE(tracker, nullptr);
}

TEST(HeaderChunkRegionTracker, AllocatedRegionTooLarge) {
  AllocatorForTest<kArbitraryAllocatorSize> alloc;
  auto tracker =
      HeaderChunkRegionTracker::AllocateRegion(alloc, kArbitraryAllocatorSize);
  ASSERT_EQ(tracker, nullptr);
}

}  // namespace
}  // namespace pw::multibuf::v1
