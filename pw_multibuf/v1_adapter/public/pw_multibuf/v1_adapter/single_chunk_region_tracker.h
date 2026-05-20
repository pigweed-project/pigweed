// Copyright 2025 The Pigweed Authors
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

#include <atomic>
#include <cstddef>
#include <optional>

#include "pw_allocator/allocator.h"
#include "pw_allocator/block/tiny_block.h"
#include "pw_allocator/first_fit.h"
#include "pw_allocator/layout.h"
#include "pw_bytes/span.h"
#include "pw_containers/storage.h"
#include "pw_multibuf/v1_adapter/chunk.h"
#include "pw_multibuf/v1_adapter/internal/chunk_allocator.h"
#include "pw_multibuf/v2/internal/entry.h"

namespace pw::multibuf::v1_adapter {

/// @submodule{pw_multibuf,v1_adapter}

/// Helper type that creates a single `OwnedChunk` from a region of memory and
/// mimics `v1::SingleChunkRegionTracker`.
///
/// This type can be used as a drop-in replacement for
/// `v1::SingleChunkRegionTracker` while migrating to using pw_multibuf/v2.
class SingleChunkRegionTracker : private internal::SingleChunkAllocator {
 private:
  using Base = internal::SingleChunkAllocator;

 public:
  /// Default constructor.
  ///
  /// Callers must call `SetRegion` before calling `GetChunk`.
  SingleChunkRegionTracker()
      : Base(metadata_allocator_), metadata_allocator_(metadata_buffer_) {}

  /// Constructs a region tracker with a single `Chunk` that maps to `region`,
  /// which must outlive this tracker and any `OwnedChunk` it creates.
  explicit SingleChunkRegionTracker(ByteSpan region)
      : SingleChunkRegionTracker() {
    SetRegion(region);
  }

  using Base::Destroy;
  using Base::SetRegion;

  ByteSpan Region() const { return Base::buffer(); }

  /// @copydoc ::v1::SingleChunkRegionTracker::GetChunk
  std::optional<OwnedChunk> GetChunk(size_t size) {
    return Base::AllocateChunk(size);
  }

 private:
  /// The metadata allocator is primarily used to back the v2 multibuf's deque.
  static constexpr size_t kBufSize = sizeof(v2::internal::Entry) * 32;

  std::array<std::byte, kBufSize> metadata_buffer_{};

  allocator::FirstFitAllocator<allocator::TinyBlock> metadata_allocator_;
};

/// @endsubmodule

}  // namespace pw::multibuf::v1_adapter
