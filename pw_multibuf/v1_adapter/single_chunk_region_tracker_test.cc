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

#include "pw_multibuf/single_chunk_region_tracker.h"

#include <cstddef>
#include <optional>

#include "pw_multibuf/chunk.h"
#include "pw_unit_test/framework.h"

namespace pw::multibuf {
namespace {

const size_t kArbitraryRegionSize = 1024;
const size_t kArbitraryChunkSize = 32;

TEST(SingleChunkRegionTrackerTest, GetChunkSmallerThanRegionSuccess) {
  std::array<std::byte, kArbitraryRegionSize> chunk_storage{};
  SingleChunkRegionTracker chunk_tracker(chunk_storage);
  std::optional<OwnedChunk> chunk = chunk_tracker.GetChunk(kArbitraryChunkSize);
  ASSERT_TRUE(chunk.has_value());
  EXPECT_EQ(chunk->size(), kArbitraryChunkSize);
}

TEST(SingleChunkRegionTrackerTest, GetChunkSameSizeAsRegionSuccess) {
  std::array<std::byte, kArbitraryRegionSize> chunk_storage{};
  SingleChunkRegionTracker chunk_tracker(chunk_storage);
  std::optional<OwnedChunk> chunk =
      chunk_tracker.GetChunk(kArbitraryRegionSize);
  ASSERT_TRUE(chunk.has_value());
  EXPECT_EQ(chunk->size(), kArbitraryRegionSize);
}

TEST(SingleChunkRegionTrackerTest, GetChunkFailChunkInUse) {
  std::array<std::byte, kArbitraryRegionSize> chunk_storage{};
  SingleChunkRegionTracker chunk_tracker(chunk_storage);
  std::optional<OwnedChunk> chunk1 =
      chunk_tracker.GetChunk(kArbitraryChunkSize);
  ASSERT_TRUE(chunk1.has_value());

  std::optional<OwnedChunk> chunk2 =
      chunk_tracker.GetChunk(kArbitraryChunkSize);
  EXPECT_FALSE(chunk2.has_value());
}

TEST(SingleChunkRegionTrackerTest, GetChunkAfterReleasedChunkSuccess) {
  std::array<std::byte, kArbitraryRegionSize> chunk_storage{};
  SingleChunkRegionTracker chunk_tracker(chunk_storage);
  std::optional<OwnedChunk> chunk1 =
      chunk_tracker.GetChunk(kArbitraryChunkSize);
  ASSERT_TRUE(chunk1.has_value());

  std::optional<OwnedChunk> chunk2 =
      chunk_tracker.GetChunk(kArbitraryChunkSize);
  ASSERT_FALSE(chunk2.has_value());

  chunk1->Release();

  std::optional<OwnedChunk> chunk3 =
      chunk_tracker.GetChunk(kArbitraryChunkSize);
  EXPECT_TRUE(chunk3.has_value());
}

}  // namespace
}  // namespace pw::multibuf
