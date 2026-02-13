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

#include <cstddef>
#include <cstdint>
#include <tuple>

#include "pw_allocator/allocator.h"
#include "pw_allocator/capability.h"
#include "pw_allocator/internal/control_block.h"
#include "pw_allocator/layout.h"
#include "pw_assert/assert.h"
#include "pw_bytes/span.h"
#include "pw_containers/dynamic_vector.h"
#include "pw_containers/storage.h"
#include "pw_multibuf/v1_adapter/chunk.h"
#include "pw_preprocessor/compiler.h"
#include "pw_result/result.h"

namespace pw::multibuf::v1_adapter::internal {

/// @submodule{pw_multibuf,v1_adapter}

/// Allocator interface that specializes in producing v1-compatible chunks.
///
/// This allocator does not store allocation inline with allocated memory, like
/// a BlockAllocator. Instead, it has a reference to a second allocator that it
/// can use provide space for metadata tracking which regions of an overall
/// memory arena are allocated. This allows the allocator to fully allocate the
/// arena without using any of it for overhead.
///
/// The details of how the second allocator is used are implemented by the
/// derived classes.
class ChunkAllocator : public Allocator {
 public:
  ~ChunkAllocator() override = default;

  constexpr Allocator& metadata_allocator() const {
    return metadata_allocator_;
  }

  constexpr size_t alignment() const { return 1u << alignment_log2_; }

  constexpr size_t available() const { return available_; }

  /// Allocates a chunk of at least `min_size` and up to `desired_size` bytes
  /// and returns it, or return `std::nullopt` if allocation fails.
  std::optional<OwnedChunk> AllocateChunk(size_t min_size, size_t desired_size);

  /// Allocates a chunk of `size` bytes and returns it, or return `std::nullopt`
  /// if allocation fails.
  std::optional<OwnedChunk> AllocateChunk(size_t size) {
    return AllocateChunk(size, size);
  }

 protected:
  using ControlBlock = allocator::internal::ControlBlock;

  // A compact representation of a span that may be allocated or free.
  PW_PACKED(struct) Region {
    std::byte* data;
    uint32_t size : 31;
    uint32_t free : 1;

    constexpr Region() : data(nullptr), size(0), free(true) {}
  };

  ChunkAllocator(ByteSpan region,
                 Allocator& metadata_allocator,
                 size_t alignment);

  constexpr ByteSpan buffer() const { return buffer_; }

  constexpr size_t num_allocations() const {
    return static_cast<size_t>(num_allocations_);
  }

 protected:
  /// @copydoc Allocator::Allocate
  void* DoAllocate(allocator::Layout layout) override;

  /// @copydoc Deallocator::Deallocate
  void DoDeallocate(void* ptr) override;

  /// @copydoc Deallocator::GetInfo
  Result<Layout> DoGetInfo(InfoType info_type, const void* ptr) const override;

  /// Returns a pointer to a region describing allocated memory of at least
  /// `min_size` and up to `desired_size`, or null if allocation failed.
  virtual Region* AllocateRegion(size_t min_size, size_t desired_size) = 0;

  /// Returns the number of bytes deallocated if the given pointer refers to
  /// memory described by a region previously returned by ` AllocateRegion`, or
  /// 0 if it does not.
  virtual size_t TryDeallocateRegion(void* ptr) = 0;

 private:
  /// Allocator used for metadata, including shared pointer control blocks and
  /// v2 multibuf deques.
  Allocator& metadata_allocator_;

  /// Memory buffer used to allocate memory regions from this allocator.
  ByteSpan buffer_;

  /// Number of bytes available to be allocated.
  uint32_t available_;

  /// Number of metadata allocations made.
  uint16_t num_allocations_ = 0;

  /// Log2 of the minimum alignment of allocations. Must be a power of two, so
  /// simply storing the exponent is more compact.
  uint8_t alignment_log2_;
};

/// Allocator designed to allocate a single chunk at any given time.
///
/// This restriction allows the allocator to manage its chunk metadata directly,
/// without relying on the metadata allocator.
class SingleChunkAllocator : public ChunkAllocator {
 public:
  SingleChunkAllocator(ByteSpan region, Allocator& metadata_allocator);

 protected:
  constexpr bool control_block_free() const { return control_block_free_; }

  /// Release the chunk shared pointer metadata.
  void DeallocateControlBlock(void* ptr);

  /// @copydoc Allocator::Allocate
  void* DoAllocate(Layout layout) override;

  /// @copydoc Allocator::Deallocate
  void DoDeallocate(void* ptr) override;

  /// @copydoc ChunkAllocator::AllocateRegion
  Region* AllocateRegion(size_t min_size, size_t desired_size) override;

  /// @copydoc ChunkAllocator::TryDeallocateRegion
  size_t TryDeallocateRegion(void* ptr) override;

 private:
  /// Describes the memory available for allocation.
  Region region_;

  /// Indicates whether a control block has been allocated from this object.
  bool control_block_free_ = true;

  /// Memory used to store chunk shared pointer metadata.
  containers::StorageFor<ControlBlock> control_block_;
};

/// @}

}  // namespace pw::multibuf::v1_adapter::internal
