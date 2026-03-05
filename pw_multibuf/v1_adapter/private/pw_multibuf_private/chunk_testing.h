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

#include "pw_allocator/shared_ptr.h"
#include "pw_allocator/testing.h"
#include "pw_multibuf/multibuf.h"
#include "pw_multibuf_private/chunk_test_base.h"

namespace pw::multibuf::test {

class ChunkTest : public ChunkTestBase<OwnedChunk> {
 private:
  // Arbitrary sizes intended to be large enough to store the Chunk and data
  // slices. These may be increased if `MakeChunk` fails.
  static constexpr size_t kMetaSizeBytes = 512;
  static constexpr size_t kDataSizeBytes = 2048;

  bool DoHasAllocations() const override {
    const auto& metrics = data_allocator_.metrics();
    return metrics.num_allocations.value() != metrics.num_deallocations.value();
  }

  OwnedChunk DoMakeChunk(size_t size) override {
    SharedPtr<std::byte[]> shared;
    if (size != 0) {
      shared = data_allocator_.MakeShared<std::byte[]>(size);
      PW_ASSERT(shared != nullptr);
    }
    return OwnedChunk(meta_allocator_, shared);
  }

  allocator::test::AllocatorForTest<kDataSizeBytes> data_allocator_;
  allocator::test::AllocatorForTest<kMetaSizeBytes> meta_allocator_;
};

}  // namespace pw::multibuf::test
