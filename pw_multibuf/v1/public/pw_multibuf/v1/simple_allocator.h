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

#include "pw_allocator/allocator.h"
#include "pw_containers/intrusive_list.h"
#include "pw_multibuf/config.h"
#include "pw_multibuf/v1/allocator.h"
#include "pw_multibuf/v1/multibuf.h"

namespace pw::multibuf::v1 {

class SimpleAllocator;

namespace internal {

/// A ``ChunkRegionTracker`` for the allocated regions within a
/// ``SimpleAllocator``'s data area.
class PW_MULTIBUF_DEPRECATED LinkedRegionTracker final
    : public ChunkRegionTracker,
      public IntrusiveList<LinkedRegionTracker>::Item {
 public:
  LinkedRegionTracker(SimpleAllocator& parent, ByteSpan region)
      : parent_(parent), region_(region) {}
  ~LinkedRegionTracker() final;

  // LinkedRegionTracker is not copyable nor movable.
  LinkedRegionTracker(const LinkedRegionTracker&) = delete;
  LinkedRegionTracker& operator=(const LinkedRegionTracker&) = delete;
  LinkedRegionTracker(LinkedRegionTracker&&) = delete;
  LinkedRegionTracker& operator=(LinkedRegionTracker&&) = delete;

 protected:
  void Destroy() final;
  ByteSpan Region() const final { return region_; }
  void* AllocateChunkClass() final;
  void DeallocateChunkClass(void*) final;

 private:
  SimpleAllocator& parent_;
  const ByteSpan region_;

  friend class ::pw::multibuf::v1::SimpleAllocator;
};

}  // namespace internal

/// @submodule{pw_multibuf,v1_impl}

/// A simple first-fit ``MultiBufAllocator``.
class PW_MULTIBUF_DEPRECATED SimpleAllocator : public MultiBufAllocator {
 public:
  /// Creates a new ``SimpleAllocator``.
  ///
  /// @param[in] data_area         The region to use for storing chunk memory.
  ///
  /// @param[in] metadata_alloc    The allocator to use for metadata tracking
  ///  the in-use regions. This allocator *must* be thread-safe if the resulting
  ///  buffers may travel to another thread. ``SynchronizedAllocator`` can be
  ///  used to create a thread-safe allocator from a non-thread-safe allocator.
  ///
  /// @param[in] alignment         The alignment to use. All chunks allocated
  ///  by this allocator will start aligned with the specified alignment. The
  ///  alignment can change if the prefix Chunk methods are used. The supplied
  ///  `data_area` *must* be aligned (both start and end) to the specified
  ///  alignment. Defaults to 1.
  SimpleAllocator(ByteSpan data_area,
                  pw::allocator::Allocator& metadata_alloc,
                  size_t alignment = 1);

 private:
  pw::Result<MultiBuf> DoAllocate(
      size_t min_size,
      size_t desired_size,
      ContiguityRequirement contiguity_requirement) final;

  std::optional<size_t> DoGetBackingCapacity() final {
    return data_area_.size();
  }

  /// Allocates a contiguous buffer of exactly ``size`` bytes.
  pw::Result<MultiBuf> InternalAllocateContiguous(size_t size)
      PW_EXCLUSIVE_LOCKS_REQUIRED(lock_);

  /// An unused block of memory in the allocator's data area.
  ///
  /// This describes a single contiguous chunk of memory in the allocator's data
  /// area that is not yet tracked by a ``LinkedRegionTracker`` and therefore
  /// not referenced by any ``Chunk``s.
  struct FreeBlock final {
    /// An ``iterator`` pointing just before this block.
    /// This is meant for use with ``insert_after`` to add new elements
    /// within the block.
    IntrusiveList<internal::LinkedRegionTracker>::iterator iter;

    /// The span of unused memory.
    ByteSpan span;
  };

  pw::Result<OwnedChunk> InsertRegion(const FreeBlock&)
      PW_EXCLUSIVE_LOCKS_REQUIRED(lock_);

  /// A description of the unused memory within this allocator's data area.
  struct AvailableMemorySize final {
    /// The total number of unused bytes.
    size_t total;
    /// The number of bytes in the largest contiguous unused section.
    size_t contiguous;
  };

  /// Returns information about the amount of unused memory within this
  /// allocator's data area.
  AvailableMemorySize GetAvailableMemorySize()
      PW_EXCLUSIVE_LOCKS_REQUIRED(lock_);

  /// Whether to continue or stop iterating.
  enum class ControlFlow {
    Continue,
    Break,
  };

  /// Iterates over each unused section of memory in the data area.
  ///
  /// @param[in] function   A function which accepts ``const FreeBlock&`` and
  ///    returns ``ControlFlow``. This function will be invoked once for every
  ///    unused section of memory in the data area.
  template <typename Fn>
  void ForEachFreeBlock(Fn function) PW_EXCLUSIVE_LOCKS_REQUIRED(lock_) {
    std::byte* last_used_end = data_area_.data();
    // We need to track only the `prev_iter` in order to ensure that we don't
    // miss any blocks that ``function`` inserts.
    auto prev_iter = regions_.before_begin();
    while (true) {
      // Compute ``cur_iter`` by incrementing ``prev_iter``.
      auto cur_iter = prev_iter;
      cur_iter++;
      if (cur_iter == regions_.end()) {
        break;
      }
      size_t unused =
          static_cast<size_t>(cur_iter->region_.data() - last_used_end);
      if (unused != 0) {
        ControlFlow cf = function({prev_iter, ByteSpan(last_used_end, unused)});
        if (cf == ControlFlow::Break) {
          return;
        }
      }
      last_used_end = cur_iter->region_.data() + cur_iter->region_.size();
      prev_iter = cur_iter;
    }
    size_t unused = static_cast<size_t>(
        (data_area_.data() + data_area_.size()) - last_used_end);
    if (unused != 0) {
      function({prev_iter, ByteSpan(last_used_end, unused)});
    }
  }

  pw::sync::Mutex lock_;
  IntrusiveList<internal::LinkedRegionTracker> regions_ PW_GUARDED_BY(lock_);
  pw::allocator::Allocator& metadata_alloc_;
  const ByteSpan data_area_;
  const size_t alignment_;

  friend class internal::LinkedRegionTracker;
};

/// @}

}  // namespace pw::multibuf::v1
