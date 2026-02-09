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

#include "pw_multibuf/v1_adapter/chunk.h"

#include "pw_assert/check.h"

namespace pw::multibuf::v1_adapter {

bool Chunk::CanMerge(const Chunk& next_chunk) const {
  return region_ == next_chunk.region_ &&
         metadata_allocator_ == next_chunk.metadata_allocator_ &&
         span_.data() + span_.size() == next_chunk.span_.data();
}

bool Chunk::Merge(OwnedChunk& next_chunk) {
  if (!next_chunk.inner_.has_value()) {
    return false;
  }
  auto& chunk = next_chunk.inner_.value();
  if (!CanMerge(chunk)) {
    return false;
  }
  auto* start = std::min(span_.data(), chunk.span_.data());
  span_ = ByteSpan(start, span_.size() + chunk.span_.size());
  chunk.region_.reset();
  chunk.span_ = ByteSpan();
  return true;
}

bool Chunk::ClaimPrefix(size_t bytes_to_claim) {
  size_t available = static_cast<size_t>(span_.data() - region_.get());
  if (available < bytes_to_claim) {
    return false;
  }
  span_ =
      ByteSpan(span_.data() - bytes_to_claim, span_.size() + bytes_to_claim);
  return true;
}

bool Chunk::ClaimSuffix(size_t bytes_to_claim) {
  std::byte* span_end = span_.data() + span_.size();
  std::byte* region_end = region_.get() + region_.size();
  size_t available = static_cast<size_t>(region_end - span_end);
  if (available < bytes_to_claim) {
    return false;
  }
  span_ = ByteSpan(span_.data(), span_.size() + bytes_to_claim);
  return true;
}

void Chunk::DiscardPrefix(size_t bytes_to_discard) {
  Slice(bytes_to_discard, size());
}

void Chunk::Slice(size_t begin, size_t end) {
  PW_CHECK_UINT_LE(begin, end);
  span_ = span_.subspan(begin, end - begin);
}

void Chunk::Truncate(size_t len) { Slice(0, len); }

std::optional<OwnedChunk> Chunk::TakePrefix(size_t bytes_to_take) {
  if (span_.size() < bytes_to_take) {
    return std::nullopt;
  }
  OwnedChunk chunk(*metadata_allocator_, region_);
  chunk.inner_->span_ = span_;
  chunk.inner_->Truncate(bytes_to_take);
  DiscardPrefix(bytes_to_take);
  return chunk;
}

std::optional<OwnedChunk> Chunk::TakeSuffix(size_t bytes_to_take) {
  if (span_.size() < bytes_to_take) {
    return std::nullopt;
  }
  size_t bytes_to_keep = size() - bytes_to_take;
  OwnedChunk chunk(*metadata_allocator_, region_);
  chunk.inner_->span_ = span_;
  chunk.inner_->DiscardPrefix(bytes_to_keep);
  Truncate(bytes_to_keep);
  return chunk;
}

}  // namespace pw::multibuf::v1_adapter
