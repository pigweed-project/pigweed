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
#pragma once

#include <cstddef>
#include <memory>
#include <optional>

#include "pw_allocator/allocator.h"
#include "pw_bytes/span.h"
#include "pw_multibuf/config.h"
#include "pw_multibuf/v1/chunk.h"

namespace pw::multibuf::v1 {

/// @submodule{pw_multibuf,v1_impl}

/// A ``ChunkRegionTracker`` which stores its ``Chunk`` and region metadata
/// in a ``allocator::Allocator`` allocation alongside the data.
///
/// This is useful when testing and when there is no need for asynchronous
/// allocation.
class PW_MULTIBUF_DEPRECATED HeaderChunkRegionTracker final
    : public ChunkRegionTracker {
 public:
  /// Allocates a new ``Chunk`` region of ``size`` bytes  in ``alloc``.
  ///
  /// The underlyiing allocation will also store the
  /// ``HeaderChunkRegionTracker`` itself. The allocated memory must not outlive
  /// the provided allocator ``alloc``.
  ///
  /// Returns the newly-created ``OwnedChunk`` if successful.
  static std::optional<OwnedChunk> AllocateRegionAsChunk(
      allocator::Allocator& alloc, size_t size) {
    HeaderChunkRegionTracker* tracker = AllocateRegion(alloc, size);
    if (tracker == nullptr) {
      return std::nullopt;
    }
    std::optional<OwnedChunk> chunk = tracker->CreateFirstChunk();
    if (!chunk.has_value()) {
      tracker->Destroy();
      return std::nullopt;
    }
    return chunk;
  }

  /// Allocates a new region of ``size`` bytes  in ``alloc``.
  ///
  /// The underlyiing allocation will also store the
  /// ``HeaderChunkRegionTracker`` itself. The allocated memory must not outlive
  /// the provided allocator ``alloc``.
  ///
  /// Returns a pointer to the newly-created ``HeaderChunkRegionTracker``
  /// or ``nullptr`` if the allocation failed.
  static HeaderChunkRegionTracker* AllocateRegion(allocator::Allocator& alloc,
                                                  size_t size) {
    auto layout =
        allocator::Layout::Of<HeaderChunkRegionTracker>().Extend(size);
    void* ptr = alloc.Allocate(layout);
    if (ptr == nullptr) {
      return nullptr;
    }
    std::byte* data =
        reinterpret_cast<std::byte*>(ptr) + sizeof(HeaderChunkRegionTracker);
    return new (ptr) HeaderChunkRegionTracker(ByteSpan(data, size), alloc);
  }

  ByteSpan Region() const final { return region_; }
  ~HeaderChunkRegionTracker() final {}

 protected:
  void Destroy() final {
    std::byte* ptr = reinterpret_cast<std::byte*>(this);
    auto alloc = alloc_;
    std::destroy_at(this);
    alloc->Deallocate(ptr);
  }

  void* AllocateChunkClass() final {
    return alloc_->Allocate(allocator::Layout::Of<Chunk>());
  }

  void DeallocateChunkClass(void* ptr) final { alloc_->Deallocate(ptr); }

 private:
  ByteSpan region_;
  allocator::Allocator* alloc_;

  // NOTE: `region` must directly follow this `FakeChunkRegionTracker`
  // in memory allocated by allocated by `alloc`.
  HeaderChunkRegionTracker(ByteSpan region, allocator::Allocator& alloc)
      : region_(region), alloc_(&alloc) {}
};

/// @}

}  // namespace pw::multibuf::v1
