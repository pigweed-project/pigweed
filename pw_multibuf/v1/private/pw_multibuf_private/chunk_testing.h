// Copyright 2026 The Pigweed Authors
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
#pragma once

#include <optional>

#include "pw_allocator/testing.h"
#include "pw_multibuf/multibuf.h"
#include "pw_multibuf/v1/header_chunk_region_tracker.h"
#include "pw_multibuf_private/chunk_test_base.h"

namespace pw::multibuf::test {

class ChunkTest : public ChunkTestBase<OwnedChunk> {
 private:
  static constexpr size_t kArbitraryAllocatorSize = 2048;

  bool DoHasAllocations() const override {
    const auto& metrics = allocator_.metrics();
    return metrics.num_allocations.value() != metrics.num_deallocations.value();
  }

  OwnedChunk DoMakeChunk(size_t size) override {
    std::optional<OwnedChunk> chunk =
        v1::HeaderChunkRegionTracker::AllocateRegionAsChunk(allocator_, size);
    // If this check fails, `kArbitraryAllocatorSize` may need increasing.
    PW_ASSERT(chunk.has_value());
    return std::move(*chunk);
  }

  allocator::test::AllocatorForTest<kArbitraryAllocatorSize> allocator_;
};

}  // namespace pw::multibuf::test
