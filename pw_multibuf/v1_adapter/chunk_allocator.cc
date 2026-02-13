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

#include "pw_multibuf/v1_adapter/internal/chunk_allocator.h"

#include "pw_assert/check.h"
#include "pw_bytes/alignment.h"
#include "pw_numeric/checked_arithmetic.h"

namespace pw::multibuf::v1_adapter::internal {

////////////////////////////////////////////////////////////////////////////////
// ChunkAllocator methods.

ChunkAllocator::ChunkAllocator(ByteSpan region,
                               Allocator& metadata_allocator,
                               size_t alignment)
    : Allocator(allocator::kImplementsGetCapacity |
                allocator::kImplementsRecognizes),
      metadata_allocator_(metadata_allocator) {
  // `region` must fit in a `Region`.
  PW_CHECK_UINT_LT(region.size(), 1ULL << 31);

  // Alignment must be a power of two.
  alignment_log2_ = static_cast<uint8_t>(cpp20::countr_zero(alignment));
  PW_CHECK_UINT_EQ(alignment, this->alignment());

  // Only store the usable portion.
  buffer_ = GetAlignedSubspan(region, alignment);
  available_ = static_cast<uint32_t>(buffer_.size());
}

std::optional<OwnedChunk> ChunkAllocator::AllocateChunk(size_t min_size,
                                                        size_t desired_size) {
  PW_CHECK_UINT_LE(min_size, desired_size);
  Region* region = AllocateRegion(AlignUp(min_size, alignment()),
                                  AlignUp(desired_size, alignment()));
  if (region == nullptr) {
    return std::nullopt;
  }
  region->size = std::min(region->size, static_cast<uint32_t>(desired_size));
  auto* control_block = ControlBlock::Create(this, region->data, region->size);
  if (control_block == nullptr) {
    size_t size = region->size;
    PW_CHECK_UINT_EQ(TryDeallocateRegion(region->data), size);
    return std::nullopt;
  }
  PW_CHECK(CheckedDecrement(available_, region->size));
  return std::make_optional(
      OwnedChunk(metadata_allocator_,
                 SharedPtr<std::byte[]>(region->data, control_block)));
}

void* ChunkAllocator::DoAllocate(allocator::Layout layout) {
  void* ptr = metadata_allocator_.Allocate(layout);
  if (ptr != nullptr) {
    PW_CHECK(CheckedIncrement(num_allocations_, 1));
  }
  return ptr;
}

void ChunkAllocator::DoDeallocate(void* ptr) {
  size_t released = TryDeallocateRegion(ptr);
  if (released == 0) {
    PW_CHECK(CheckedDecrement(num_allocations_, 1));
    metadata_allocator_.Deallocate(ptr);
  } else {
    PW_CHECK_UINT_LT(released, 1ULL << 31);
    auto released32 = static_cast<uint32_t>(AlignUp(released, alignment()));
    PW_CHECK(CheckedIncrement(available_, released32));
  }
}

auto ChunkAllocator::DoGetInfo(InfoType info_type, const void* ptr) const
    -> Result<Layout> {
  if (info_type == InfoType::kCapacity) {
    return Layout(buffer_.size(), 1);
  }
  if (info_type != InfoType::kRecognizes) {
    return Status::Unimplemented();
  }
  if (ptr < buffer_.data() || buffer_.data() + buffer_.size() < ptr) {
    return Status::NotFound();
  }
  return Layout();
}

////////////////////////////////////////////////////////////////////////////////
// SingleChunkAllocator methods.

SingleChunkAllocator::SingleChunkAllocator(ByteSpan region,
                                           Allocator& metadata_allocator)
    : ChunkAllocator(region, metadata_allocator, 1) {
  region_.data = buffer().data();
}

void* SingleChunkAllocator::DoAllocate(allocator::Layout layout) {
  if (layout == allocator::Layout::Of<ControlBlock>() && control_block_free_) {
    control_block_free_ = false;
    return control_block_.data();
  }
  return ChunkAllocator::DoAllocate(layout);
}

void SingleChunkAllocator::DoDeallocate(void* ptr) {
  if (ptr == control_block_.data()) {
    control_block_free_ = true;
  } else {
    ChunkAllocator::DoDeallocate(ptr);
  }
}

auto SingleChunkAllocator::AllocateRegion(size_t min_size, size_t desired_size)
    -> Region* {
  if (!region_.free) {
    return nullptr;
  }
  if (available() < min_size) {
    return nullptr;
  }
  region_.size = static_cast<uint32_t>(std::min(available(), desired_size));
  region_.free = false;
  return &region_;
}

size_t SingleChunkAllocator::TryDeallocateRegion(void* ptr) {
  if (ptr != region_.data || region_.free) {
    return 0;
  }
  region_.free = true;
  return region_.size;
}

}  // namespace pw::multibuf::v1_adapter::internal
