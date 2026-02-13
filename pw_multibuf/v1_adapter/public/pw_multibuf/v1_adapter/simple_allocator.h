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

#include <cstddef>
#include <optional>

#include "pw_bytes/span.h"
#include "pw_multibuf/v1_adapter/allocator.h"
#include "pw_multibuf/v1_adapter/internal/chunk_allocator.h"
#include "pw_multibuf/v1_adapter/multibuf.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/mutex.h"

namespace pw::multibuf::v1_adapter {

/// A `MultiBufAllocator` that provides its best-fit allocator and mimics
/// `v1::SimpleAllocator`.
///
/// This type can be used as a drop-in replacement for `v1::SimpleAllocator`
/// while migrating to using pw_multibuf/v2.
///
/// Like the v1 version, this type is thread-safe.
class SimpleAllocator : public MultiBufAllocator {
 public:
  SimpleAllocator(ByteSpan region,
                  Allocator& metadata_allocator,
                  size_t alignment = 1)
      : chunk_allocator_(region, metadata_allocator, alignment) {}

  /// Attempts to allocate enough capacity to track `num_regions`.
  ///
  /// This method can be used to pre-allocate space for region metadata to
  /// mitigate allocation failure due to insufficient space for metadata.
  ///
  /// The guarantueed minimum number of allocated regions for a given value of
  /// `num_regions` is `floor(num_regions / 2)`, as consecutive free regions are
  /// merged.
  [[nodiscard]] bool TryReserveRegions(size_t num_regions)
      PW_LOCKS_EXCLUDED(mutex_);

 private:
  /// A chunk allocator that can split and merge free sub-regions.
  class SimpleChunkAllocator : public internal::ChunkAllocator {
   public:
    SimpleChunkAllocator(ByteSpan region,
                         Allocator& metadata_allocator,
                         size_t alignment);

    ~SimpleChunkAllocator() override;

    /// Adds the overall region as a free sub-region if the list of sub-regions
    /// is empty. This allows the object to avoid allocating in the constructor.
    ///
    /// Returns whether the region was added to the list of sub-regions.
    [[nodiscard]] bool Init();

    /// Returns the number of the current free chunks needed to accumulate the
    /// desired number of bytes.
    size_t CountChunks(size_t desired_size);

    /// @copydoc SimpleAllocator::TryReserveRegions
    [[nodiscard]] bool TryReserveRegions(size_t num_regions);

   private:
    using iterator = DynamicVector<Region>::iterator;

    /// @copydoc ChunkAllocator::AllocateRegion
    Region* AllocateRegion(size_t min_size, size_t desired_size) override;

    /// @copydoc ChunkAllocator::TryDeallocateRegion
    size_t TryDeallocateRegion(void* ptr) override;

    /// Splits the region given by `iter` at the given `offeset`, and returns
    /// iterators to both sub-regions.
    std::tuple<iterator, iterator> Split(iterator iter, size_t offset);

    /// Merges the regions given by `left` and `right`, and returns an iterator
    /// to the merged region.
    iterator Merge(iterator left, iterator right);

    size_t available_;

    /// A heap-allocated sequence of possibly-allocated memory regions. The
    /// regions are contiguous and in aggregate are equivalent to the arena.
    DynamicVector<Region> subregions_;
  };

  /// @copydoc pw::multibuf::v1::MultiBufAllocator::Allocate
  std::optional<MultiBuf> DoAllocate(size_t min_size,
                                     size_t desired_size,
                                     bool contiguous) override
      PW_LOCKS_EXCLUDED(mutex_);

  /// @copydoc pw::multibuf::v1::MultiBufAllocator::GetBackingCapacity
  std::optional<size_t> DoGetBackingCapacity() override
      PW_LOCKS_EXCLUDED(mutex_);

  pw::sync::Mutex mutex_;

  SimpleChunkAllocator chunk_allocator_ PW_GUARDED_BY(mutex_);
};

/// @}

}  // namespace pw::multibuf::v1_adapter
