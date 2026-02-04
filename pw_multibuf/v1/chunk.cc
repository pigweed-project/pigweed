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

#include "pw_multibuf/v1/chunk.h"

#include <mutex>

#include "pw_assert/check.h"

namespace pw::multibuf::v1 {
namespace {
std::byte* CheckedAdd(std::byte* ptr, size_t offset) {
  uintptr_t ptr_int = reinterpret_cast<uintptr_t>(ptr);
  if (std::numeric_limits<uintptr_t>::max() - ptr_int < offset) {
    return nullptr;
  }
  return reinterpret_cast<std::byte*>(ptr_int + offset);
}

std::byte* CheckedSub(std::byte* ptr, size_t offset) {
  uintptr_t ptr_int = reinterpret_cast<uintptr_t>(ptr);
  if (ptr_int < offset) {
    return nullptr;
  }
  return reinterpret_cast<std::byte*>(ptr_int - offset);
}

std::byte* BeginPtr(ByteSpan span) { return span.data(); }

std::byte* EndPtr(ByteSpan span) { return span.data() + span.size(); }

}  // namespace

bool Chunk::CanMerge(const Chunk& next_chunk) const {
  return region_tracker_ == next_chunk.region_tracker_ &&
         EndPtr(span_) == BeginPtr(next_chunk.span_);
}

bool Chunk::Merge(OwnedChunk& next_chunk_owned) {
  if (!CanMerge(*next_chunk_owned)) {
    return false;
  }
  Chunk* next_chunk = std::move(next_chunk_owned).Take();

  // Note: Both chunks have the same ``region_tracker_``.
  //
  // We lock the one from `next_chunk` to satisfy the automatic
  // checker that ``RemoveFromRegionList`` is safe to call.
  std::lock_guard lock(next_chunk->region_tracker_->lock_);
  PW_DCHECK(next_in_region_ == next_chunk);
  span_ = ByteSpan(data(), size() + next_chunk->size());
  next_chunk->RemoveFromRegionList();
  region_tracker_->DeallocateChunkClass(next_chunk);
  return true;
}

void Chunk::InsertAfterInRegionList(Chunk* new_chunk)
    PW_EXCLUSIVE_LOCKS_REQUIRED(region_tracker_->lock_) {
  new_chunk->next_in_region_ = next_in_region_;
  new_chunk->prev_in_region_ = this;
  if (next_in_region_ != nullptr) {
    next_in_region_->prev_in_region_ = new_chunk;
  }
  next_in_region_ = new_chunk;
}

void Chunk::InsertBeforeInRegionList(Chunk* new_chunk)
    PW_EXCLUSIVE_LOCKS_REQUIRED(region_tracker_->lock_) {
  new_chunk->next_in_region_ = this;
  new_chunk->prev_in_region_ = prev_in_region_;
  if (prev_in_region_ != nullptr) {
    prev_in_region_->next_in_region_ = new_chunk;
  }
  prev_in_region_ = new_chunk;
}

void Chunk::RemoveFromRegionList()
    PW_EXCLUSIVE_LOCKS_REQUIRED(region_tracker_->lock_) {
  if (prev_in_region_ != nullptr) {
    prev_in_region_->next_in_region_ = next_in_region_;
  }
  if (next_in_region_ != nullptr) {
    next_in_region_->prev_in_region_ = prev_in_region_;
  }
  prev_in_region_ = nullptr;
  next_in_region_ = nullptr;
}

std::optional<OwnedChunk> ChunkRegionTracker::CreateFirstChunk() {
  void* memory = AllocateChunkClass();
  if (memory == nullptr) {
    return std::nullopt;
  }
  // Note: `Region()` is `const`, so no lock is required.
  Chunk* chunk = new (memory) Chunk(this, Region());
  return OwnedChunk(chunk);
}

void Chunk::Free() {
  span_ = ByteSpan();
  bool region_empty;
  // Record `region_track` so that it is available for use even after
  // `this` is free'd by the call to `DeallocateChunkClass`.
  ChunkRegionTracker* region_tracker = region_tracker_;
  {
    std::lock_guard lock(region_tracker_->lock_);
    region_empty = prev_in_region_ == nullptr && next_in_region_ == nullptr;
    RemoveFromRegionList();
    // NOTE: do *not* attempt to access any fields of `this` after this point.
    //
    // The lock must be held while deallocating this, otherwise another
    // ``Chunk::Release`` in the same region could race and call
    // ``ChunkRegionTracker::Destroy``, making this call no longer valid.
    region_tracker->DeallocateChunkClass(this);
  }
  if (region_empty) {
    region_tracker->Destroy();
  }
}

void OwnedChunk::Release() {
  if (inner_ == nullptr) {
    return;
  }
  inner_->Free();
  inner_ = nullptr;
}

bool Chunk::ClaimPrefix(size_t bytes_to_claim) {
  if (bytes_to_claim == 0) {
    return true;
  }
  // In order to roll back `bytes_to_claim`, the current chunk must start at
  // least `bytes_to_claim` after the beginning of the current region.
  std::byte* new_start = CheckedSub(data(), bytes_to_claim);
  // Note: `Region()` is `const`, so no lock is required.
  if (new_start == nullptr || new_start < BeginPtr(region_tracker_->Region())) {
    return false;
  }

  // `lock` is acquired in order to traverse the linked list and mutate `span_`.
  std::lock_guard lock(region_tracker_->lock_);

  // If there are any chunks before this one, they must not end after
  // `new_start`.
  Chunk* prev = prev_in_region_;
  if (prev != nullptr && EndPtr(*prev) > new_start) {
    return false;
  }

  size_t old_size = span_.size();
  span_ = ByteSpan(new_start, old_size + bytes_to_claim);
  return true;
}

bool Chunk::ClaimSuffix(size_t bytes_to_claim) {
  if (bytes_to_claim == 0) {
    return true;
  }
  // In order to expand forward `bytes_to_claim`, the current chunk must start
  // at least `subytes` before the end of the current region.
  std::byte* new_end = CheckedAdd(EndPtr(*this), bytes_to_claim);
  // Note: `Region()` is `const`, so no lock is required.
  if (new_end == nullptr || new_end > EndPtr(region_tracker_->Region())) {
    return false;
  }

  // `lock` is acquired in order to traverse the linked list and mutate `span_`.
  std::lock_guard lock(region_tracker_->lock_);

  // If there are any chunks after this one, they must not start before
  // `new_end`.
  Chunk* next = next_in_region_;
  if (next != nullptr && BeginPtr(next->span_) < new_end) {
    return false;
  }

  size_t old_size = span_.size();
  span_ = ByteSpan(data(), old_size + bytes_to_claim);
  return true;
}

void Chunk::DiscardPrefix(size_t bytes_to_discard) {
  Slice(bytes_to_discard, size());
}

void Chunk::Slice(size_t begin, size_t end) {
  PW_DCHECK(begin <= size());
  PW_DCHECK(end <= size());
  PW_DCHECK(end >= begin);
  ByteSpan new_span(data() + begin, end - begin);
  std::lock_guard lock(region_tracker_->lock_);
  span_ = new_span;
}

void Chunk::Truncate(size_t len) { Slice(0, len); }

std::optional<OwnedChunk> Chunk::TakePrefix(size_t bytes_to_take) {
  void* new_chunk_memory = region_tracker_->AllocateChunkClass();
  if (new_chunk_memory == nullptr) {
    return std::nullopt;
  }

  PW_DCHECK(bytes_to_take <= size());
  ByteSpan first_span = ByteSpan(data(), bytes_to_take);
  ByteSpan second_span =
      ByteSpan(data() + bytes_to_take, size() - bytes_to_take);

  std::lock_guard lock(region_tracker_->lock_);
  span_ = second_span;
  Chunk* new_chunk = new (new_chunk_memory) Chunk(region_tracker_, first_span);
  InsertBeforeInRegionList(new_chunk);
  return OwnedChunk(new_chunk);
}

std::optional<OwnedChunk> Chunk::TakeSuffix(size_t bytes_to_take) {
  void* new_chunk_memory = region_tracker_->AllocateChunkClass();
  if (new_chunk_memory == nullptr) {
    return std::nullopt;
  }

  PW_DCHECK(bytes_to_take <= size());
  ByteSpan first_span = ByteSpan(data(), size() - bytes_to_take);
  ByteSpan second_span = ByteSpan(EndPtr(*this) - bytes_to_take, bytes_to_take);

  std::lock_guard lock(region_tracker_->lock_);
  span_ = first_span;
  Chunk* new_chunk = new (new_chunk_memory) Chunk(region_tracker_, second_span);
  InsertAfterInRegionList(new_chunk);
  return OwnedChunk(new_chunk);
}

}  // namespace pw::multibuf::v1
