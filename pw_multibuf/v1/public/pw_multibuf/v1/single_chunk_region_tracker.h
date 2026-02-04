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
#pragma once

#include <array>
#include <atomic>
#include <cstring>
#include <optional>

#include "pw_assert/assert.h"
#include "pw_bytes/span.h"
#include "pw_multibuf/config.h"
#include "pw_multibuf/v1/chunk.h"

namespace pw::multibuf::v1 {

/// @submodule{pw_multibuf,v1_impl}

/// A `ChunkRegionTracker` that uses inline memory to create a single `Chunk`
/// with the only caveat that the provided `Chunk` cannot be split. All attempts
/// will result in `std::nullopt`.
class PW_MULTIBUF_DEPRECATED SingleChunkRegionTracker
    : public ChunkRegionTracker {
 public:
  /// Constructs a region tracker with a single `Chunk` that maps to `region`,
  /// which must outlive this tracker and any `OwnedChunk` it creates.
  explicit SingleChunkRegionTracker(ByteSpan region) : region_(region) {}
  ~SingleChunkRegionTracker() override { Destroy(); }

  /// Gets a `Chunk` of a given size, which must be less than or equal to the
  /// provided region.
  ///
  /// Returns:
  ///   An `OwnedChunk` if the `Chunk` is free, otherwise `std::nullopt`, in
  /// which case `GetChunk()` can be called again.
  std::optional<OwnedChunk> GetChunk(size_t size) {
    PW_DASSERT(size <= region_.size());
    // Since this is a single `Chunk` region, re-create the first `Chunk` is
    // allowed if freed.
    std::optional<OwnedChunk> chunk = CreateFirstChunk();
    if (chunk.has_value() && size < region_.size()) {
      (*chunk)->Truncate(size);
    }
    return chunk;
  }

  void Destroy() final {
    // Nothing to release here.
    PW_ASSERT(!chunk_in_use_);
  }

  ByteSpan Region() const final { return region_; }

  void* AllocateChunkClass() final {
    bool in_use = false;
    if (!chunk_in_use_.compare_exchange_strong(in_use, true)) {
      return nullptr;
    }
    return &chunk_storage_;
  }

  void DeallocateChunkClass(void* chunk) final {
    PW_ASSERT(chunk == chunk_storage_.data());
    // Mark the `Chunk` as not in use and zero-out the region and chunk storage.
    std::memset(chunk_storage_.data(), 0, chunk_storage_.size());
    std::memset(region_.data(), 0, region_.size());
    chunk_in_use_ = false;
  }

 private:
  ByteSpan region_;
  std::atomic<bool> chunk_in_use_ = false;
  alignas(Chunk) std::array<std::byte, sizeof(Chunk)> chunk_storage_;
};

/// @}

}  // namespace pw::multibuf::v1
