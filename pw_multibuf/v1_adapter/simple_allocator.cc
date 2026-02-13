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

#include "pw_multibuf/v1_adapter/simple_allocator.h"

#include <mutex>

#include "pw_assert/check.h"
#include "pw_bytes/alignment.h"

namespace pw::multibuf::v1_adapter {

////////////////////////////////////////////////////////////////////////////////
// SimpleAllocator methods.

bool SimpleAllocator::TryReserveRegions(size_t num_regions) {
  std::lock_guard lock(mutex_);
  return chunk_allocator_.TryReserveRegions(num_regions);
}

std::optional<MultiBuf> SimpleAllocator::DoAllocate(size_t min_size,
                                                    size_t desired_size,
                                                    bool contiguous) {
  // First, check if this request is even feasible.
  if (desired_size == 0) {
    return MultiBuf();
  }

  std::lock_guard lock(mutex_);
  if (!chunk_allocator_.Init() || chunk_allocator_.available() < min_size) {
    return std::nullopt;
  }

  // Next, prepare a multibuf that can hold the necessary number of chunks.
  size_t num_chunks = 1;
  if (!contiguous) {
    min_size = 1;
    num_chunks = chunk_allocator_.CountChunks(desired_size);
  }
  MultiBuf buf(chunk_allocator_.metadata_allocator());
  if (!buf.TryReserveChunks(num_chunks)) {
    return std::nullopt;
  }

  // Finally, scan the free sub-regions and collect chunks into the multibuf.
  while (true) {
    std::optional<OwnedChunk> chunk =
        chunk_allocator_.AllocateChunk(min_size, desired_size);
    if (!chunk.has_value()) {
      break;
    }
    contiguous |= desired_size <= chunk->size();
    desired_size -= chunk->size();
    buf.PushBackChunk(std::move(*chunk));
    if (contiguous) {
      break;
    }
  }
  if (buf.empty()) {
    return std::nullopt;
  }
  return buf;
}

std::optional<size_t> SimpleAllocator::DoGetBackingCapacity() {
  std::lock_guard lock(mutex_);
  auto sws = chunk_allocator_.GetCapacity();
  if (!sws.ok()) {
    return std::nullopt;
  }
  return std::make_optional(sws.size());
}

////////////////////////////////////////////////////////////////////////////////
// SimpleAllocator::SimpleChunkAllocator methods.

SimpleAllocator::SimpleChunkAllocator::SimpleChunkAllocator(
    ByteSpan region, Allocator& metadata_allocator, size_t alignment)
    : ChunkAllocator(region, metadata_allocator, alignment),
      available_(buffer().size()),
      subregions_(metadata_allocator) {}

SimpleAllocator::SimpleChunkAllocator::~SimpleChunkAllocator() {
  PW_CHECK_UINT_LE(subregions_.size(), 1);
  if (!subregions_.empty()) {
    ByteSpan buffer = ChunkAllocator::buffer();
    Region& first = subregions_.front();
    PW_CHECK_PTR_EQ(first.data, buffer.data());
    PW_CHECK_UINT_EQ(first.size, buffer.size());
    PW_CHECK(first.free);
  }
}

bool SimpleAllocator::SimpleChunkAllocator::Init() {
  if (!subregions_.empty()) {
    return true;
  }
  ByteSpan buffer = ChunkAllocator::buffer();
  if (!TryReserveRegions(4)) {
    return false;
  }
  subregions_.push_back(Region());
  Region& first = subregions_.back();
  first.data = buffer.data();
  first.size = static_cast<uint32_t>(buffer.size());
  return true;
}

size_t SimpleAllocator::SimpleChunkAllocator::CountChunks(size_t desired_size) {
  size_t count = 0;
  for (const auto& region : subregions_) {
    if (!region.free) {
      continue;
    }
    ++count;
    if (desired_size <= region.size) {
      break;
    }
    desired_size -= region.size;
  }
  return count;
}

bool SimpleAllocator::SimpleChunkAllocator::TryReserveRegions(
    size_t num_regions) {
  PW_CHECK_UINT_LE(num_regions, std::numeric_limits<uint16_t>::max());
  return subregions_.try_reserve(static_cast<uint16_t>(num_regions));
}

auto SimpleAllocator::SimpleChunkAllocator::AllocateRegion(size_t min_size,
                                                           size_t desired_size)
    -> Region* {
  // Ensure sufficient capacity to split chunks as needed.
  if (subregions_.size() == subregions_.capacity()) {
    if (!TryReserveRegions(subregions_.capacity() * 2)) {
      return nullptr;
    }
  }

  // Search for a compatible free chunk.
  for (auto iter = subregions_.begin(); iter != subregions_.end(); ++iter) {
    // Is the region available?
    if (!iter->free) {
      continue;
    }

    // Is it big enough for the allocation?
    if (iter->size < min_size) {
      continue;
    }
    if (desired_size < iter->size) {
      std::tie(iter, std::ignore) = Split(iter, desired_size);
    }

    // Mark the region as being used and return the data.
    available_ -= iter->size;
    iter->free = false;
    return &(*iter);
  }
  return nullptr;
}

size_t SimpleAllocator::SimpleChunkAllocator::TryDeallocateRegion(void* ptr) {
  auto prev = subregions_.end();
  for (auto iter = subregions_.begin(); iter != subregions_.end(); ++iter) {
    // Scan until we find the corresponding region.
    if (iter->data != ptr) {
      prev = iter;
      continue;
    }

    iter->free = true;
    size_t size = iter->size;
    iter->size = static_cast<uint32_t>(AlignUp(iter->size, alignment()));

    // Can we merge with the previous region?
    if (prev != subregions_.end() && prev->free) {
      iter = Merge(prev, iter);
    }

    // Can we merge with the next region?
    auto next = iter;
    ++next;
    if (next != subregions_.end() && next->free) {
      Merge(iter, next);
    }
    return size;
  }
  return 0;
}

auto SimpleAllocator::SimpleChunkAllocator::Split(iterator iter, size_t offset)
    -> std::tuple<iterator, iterator> {
  ByteSpan bytes(iter->data, iter->size);
  iter = subregions_.insert(iter, Region());
  iter->data = bytes.data();
  iter->size = static_cast<uint32_t>(offset);
  auto next = iter;
  ++next;
  bytes = bytes.subspan(offset);
  next->data = bytes.data();
  next->size = static_cast<uint32_t>(bytes.size());
  return std::make_tuple(iter, next);
}

auto SimpleAllocator::SimpleChunkAllocator::Merge(iterator left, iterator right)
    -> iterator {
  left->size += right->size;
  subregions_.erase(right);
  return left;
}

}  // namespace pw::multibuf::v1_adapter
