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

#include "pw_multibuf/v1_adapter/multibuf.h"

#include <cstddef>
#include <cstring>
#include <optional>
#include <tuple>
#include <utility>

#include "pw_assert/check.h"
#include "pw_bytes/span.h"
#include "pw_multibuf/v1_adapter/chunk.h"
#include "pw_status/status_with_size.h"

namespace pw::multibuf::v1_adapter {

// v1 API //////////////////////////////////////////////////////////////////////

MultiBuf MultiBuf::FromChunk(OwnedChunk&& chunk) {
  MultiBuf buf;
  buf.PushFrontChunk(std::move(chunk));
  return buf;
}

void MultiBuf::Release() noexcept {
  if (mbv2_.has_value()) {
    mbv2_.reset();
  }
}

std::optional<ByteSpan> MultiBuf::ContiguousSpan() {
  if (!mbv2_.has_value()) {
    return std::make_optional<ByteSpan>();
  }
  auto contiguous = std::as_const(*this).ContiguousSpan();
  if (!contiguous.has_value()) {
    return std::nullopt;
  }
  return std::make_optional<ByteSpan>(
      const_cast<std::byte*>(contiguous->data()), contiguous->size());
}

std::optional<ConstByteSpan> MultiBuf::ContiguousSpan() const {
  if (!mbv2_.has_value()) {
    return std::make_optional<ConstByteSpan>();
  }
  ConstByteSpan contiguous;
  for (const ConstByteSpan chunk : (*mbv2_)->ConstChunks()) {
    if (contiguous.empty()) {
      contiguous = chunk;
    } else if (contiguous.data() + contiguous.size() == chunk.data()) {
      contiguous = {contiguous.data(), contiguous.size() + chunk.size()};
    } else {  // Non-empty chunks are not contiguous
      return std::nullopt;
    }
  }
  // Return either the one non-empty chunk or an empty span.
  return std::make_optional(contiguous);
}

bool MultiBuf::ClaimPrefix(size_t bytes_to_claim) {
  if (!mbv2_.has_value()) {
    return false;
  }
  v2::MultiBuf::Instance& mbv2 = *mbv2_;
  if (mbv2->NumLayers() == 1 || offset_ < bytes_to_claim) {
    return false;
  }
  const size_t length = mbv2->size();
  mbv2->PopLayer();
  offset_ -= bytes_to_claim;
  std::ignore = mbv2->AddLayer(offset_, length + bytes_to_claim);
  return true;
}

bool MultiBuf::ClaimSuffix(size_t bytes_to_claim) {
  if (!mbv2_.has_value()) {
    return false;
  }
  auto& mbv2 = *mbv2_;
  if (mbv2->NumLayers() == 1) {
    return false;
  }
  const size_t length = mbv2->size();
  mbv2->PopLayer();
  if (length + bytes_to_claim > mbv2->size() - offset_) {
    std::ignore = mbv2->AddLayer(offset_, length);
    return false;
  }
  std::ignore = mbv2->AddLayer(offset_, length + bytes_to_claim);
  return true;
}

void MultiBuf::DiscardPrefix(size_t bytes_to_discard) {
  if (bytes_to_discard == 0) {
    return;
  }
  PW_CHECK(mbv2_.has_value());
  v2::MultiBuf::Instance& mbv2 = *mbv2_;
  const size_t length = mbv2->size();
  PW_CHECK_UINT_LE(bytes_to_discard, length);
  if (mbv2->NumLayers() > 1) {
    mbv2->PopLayer();
  }
  offset_ += bytes_to_discard;
  PW_CHECK(mbv2->AddLayer(offset_, length - bytes_to_discard));
}

void MultiBuf::Slice(size_t begin, size_t end) {
  PW_CHECK_UINT_LE(begin, end);
  DiscardPrefix(begin);
  Truncate(end - begin);
}

void MultiBuf::Truncate(size_t len) {
  if (!mbv2_.has_value()) {
    PW_CHECK_UINT_EQ(
        len, 0, "cannot truncate empty multibuf to a non-zero length");
    return;
  }
  v2::MultiBuf::Instance& mbv2 = *mbv2_;
  PW_CHECK_UINT_LE(len, mbv2->size());
  if (mbv2->NumLayers() > 1) {
    mbv2->PopLayer();
  }
  PW_CHECK(mbv2->AddLayer(offset_, len));
}

void MultiBuf::TruncateAfter(iterator position) {
  PW_CHECK(mbv2_.has_value());
  Truncate(static_cast<size_t>(position - (*mbv2_)->begin()) + 1);
}

std::optional<MultiBuf> MultiBuf::TakePrefix(size_t bytes_to_take) {
  PW_CHECK_UINT_LE(bytes_to_take, size());
  if (!mbv2_.has_value()) {
    return std::make_optional(MultiBuf());
  }
  v2::MultiBuf::Instance& mbv2 = *mbv2_;
  Allocator& allocator = mbv2->get_allocator();
  if (bytes_to_take == 0) {
    return std::make_optional(MultiBuf(allocator));
  }
  auto mb = mbv2->Remove(mbv2->begin(), bytes_to_take);
  if (!mb.ok()) {
    return std::nullopt;
  }
  if (mbv2->empty()) {
    mbv2_.reset();
  }
  return std::make_optional(MultiBuf(std::move(**mb)));
}

std::optional<MultiBuf> MultiBuf::TakeSuffix(size_t bytes_to_take) {
  PW_CHECK_UINT_LE(bytes_to_take, size());
  if (!mbv2_.has_value()) {
    return std::make_optional(MultiBuf());
  }
  v2::MultiBuf::Instance& mbv2 = *mbv2_;
  Allocator& allocator = mbv2->get_allocator();
  if (bytes_to_take == 0) {
    return std::make_optional(MultiBuf(allocator));
  }
  const size_t split_pos = size() - bytes_to_take;
  auto position = mbv2->begin() + static_cast<difference_type>(split_pos);
  auto mb = mbv2->Remove(position, bytes_to_take);
  if (!mb.ok()) {
    return std::nullopt;
  }
  if (mbv2->empty()) {
    mbv2_.reset();
  }
  return std::make_optional(MultiBuf(std::move(**mb)));
}

void MultiBuf::PushPrefix(MultiBuf&& front) {
  if (!front.mbv2_.has_value()) {
    return;
  }
  v2::MultiBuf::Instance& prefix = *front.mbv2_;
  if (!mbv2_.has_value()) {
    mbv2_ = std::make_optional(v2::MultiBuf::Instance(prefix->get_allocator()));
  }
  v2::MultiBuf::Instance& mbv2 = *mbv2_;
  mbv2->Insert(mbv2->begin(), std::move(*prefix));
}

void MultiBuf::PushSuffix(MultiBuf&& tail) {
  if (!tail.mbv2_.has_value()) {
    return;
  }
  v2::MultiBuf::Instance& suffix = *tail.mbv2_;
  if (!mbv2_.has_value()) {
    mbv2_ = std::make_optional(v2::MultiBuf::Instance(suffix->get_allocator()));
  }
  v2::MultiBuf::Instance& mbv2 = *mbv2_;
  mbv2->PushBack(std::move(*suffix));
}

StatusWithSize MultiBuf::CopyTo(ByteSpan dest, size_t position) const {
  if (!mbv2_.has_value()) {
    return StatusWithSize(0);
  }
  const v2::MultiBuf::Instance& mbv2 = *mbv2_;
  const size_t copied = mbv2->CopyTo(dest, position);
  const size_t available = size() - position;
  return copied == available ? StatusWithSize(copied)
                             : StatusWithSize::ResourceExhausted(copied);
}

StatusWithSize MultiBuf::CopyFrom(ConstByteSpan source, size_t position) {
  if (source.empty()) {
    return StatusWithSize(0);
  }
  if (!mbv2_.has_value()) {
    return StatusWithSize::ResourceExhausted(0);
  }
  v2::MultiBuf::Instance& mbv2 = *mbv2_;
  const size_t copied = mbv2->CopyFrom(source, position);
  return copied == source.size() ? StatusWithSize(copied)
                                 : StatusWithSize::ResourceExhausted(copied);
}

StatusWithSize MultiBuf::CopyFromAndTruncate(ConstByteSpan source,
                                             size_t position) {
  auto copied = CopyFrom(source, position);
  if (copied.ok()) {
    Truncate(position + copied.size());
  }
  return copied;
}

void MultiBuf::PushFrontChunk(OwnedChunk&& chunk) {
  auto inner = std::move(chunk).Take();
  if (!inner.has_value()) {
    return;
  }
  Allocator* metadata_allocator = inner->metadata_allocator();
  SharedPtr<std::byte[]> region = inner->region();
  size_t offset = static_cast<size_t>(inner->data() - region.get());
  if (!mbv2_.has_value()) {
    mbv2_ = std::make_optional(v2::MultiBuf::Instance(*metadata_allocator));
  }
  v2::MultiBuf::Instance& mbv2 = *mbv2_;
  mbv2->Insert(mbv2->begin(), region, offset, inner->size());
}

void MultiBuf::PushBackChunk(OwnedChunk&& chunk) {
  auto inner = std::move(chunk).Take();
  if (!inner.has_value()) {
    return;
  }
  Allocator* metadata_allocator = inner->metadata_allocator();
  SharedPtr<std::byte[]> region = inner->region();
  size_t offset = static_cast<size_t>(inner->data() - region.get());
  if (!mbv2_.has_value()) {
    mbv2_ = std::make_optional(v2::MultiBuf::Instance(*metadata_allocator));
  }
  (*mbv2_)->PushBack(region, offset, inner->size());
}

MultiBufChunks::iterator MultiBuf::InsertChunk(
    MultiBufChunks::iterator position, OwnedChunk&& chunk) {
  // If the chunk is empty, don't modify anything.
  auto inner = std::move(chunk).Take();
  if (!inner.has_value()) {
    return position;
  }

  // Is this the first chunk to be added?
  bool appending = position == Chunks().end();
  if (!mbv2_.has_value()) {
    Allocator* metadata_allocator = inner->metadata_allocator();
    mbv2_ = std::make_optional(v2::MultiBuf::Instance(*metadata_allocator));
  }

  // Add the chunk.
  v2::MultiBuf::Instance& mbv2 = *mbv2_;
  MultiBuf::iterator iter = appending ? mbv2->end() : ToByteIterator(position);
  SharedPtr<std::byte[]> region = inner->region();
  size_t offset = static_cast<size_t>(inner->data() - region.get());
  iter = mbv2->Insert(iter, region, offset, inner->size());
  return ToChunksIterator(iter);
}

std::tuple<MultiBufChunks::iterator, OwnedChunk> MultiBuf::TakeChunk(
    MultiBufChunks::iterator position) {
  PW_CHECK(mbv2_.has_value());
  v2::MultiBuf::Instance& mbv2 = *mbv2_;
  size_t size = position->size();
  auto iter = ToByteIterator(position);
  auto chunk = mbv2->Share(iter);
  auto result = mbv2->Discard(iter, size);
  PW_CHECK_OK(result.status());
  OwnedChunk owned(mbv2->get_allocator(), chunk);
  if (mbv2->empty()) {
    mbv2_.reset();
  }
  return std::make_tuple(ToChunksIterator(*result), std::move(owned));
}

MultiBuf::iterator MultiBuf::ToByteIterator(
    const MultiBufChunks::iterator& position) {
  v2::MultiBuf::Instance& mbv2 = *mbv2_;
  iterator iter = mbv2->begin();

  // First, check if this is the non-dereferencable `end()` iterator.
  const auto& chunks = Chunks();
  if (position == chunks.end()) {
    return mbv2->end();
  }

  // Next, scan the chunks for one that matches.
  for (auto raw_chunk : chunks) {
    if (raw_chunk.data() == position->data()) {
      return iter;
    }
    iter += static_cast<iterator::difference_type>(raw_chunk.size());
  }
  PW_CHECK(false, "position not found in multibuf");
}

MultiBufChunks::iterator MultiBuf::ToChunksIterator(
    const MultiBuf::const_iterator& position) {
  v2::MultiBuf::Instance& mbv2 = *mbv2_;
  MultiBufChunks chunks = Chunks();
  if (position == mbv2->end()) {
    return chunks.end();
  }
  const std::byte* data = &(*position);
  for (auto c_iter = chunks.begin(); c_iter != chunks.end(); ++c_iter) {
    if (data == c_iter->data()) {
      return c_iter;
    }
  }
  PW_CHECK(false, "position not found in multibuf");
}

// v2 API //////////////////////////////////////////////////////////////////////

bool MultiBuf::TryReserveChunks(size_t num_chunks) {
  if (!mbv2_.has_value()) {
    return false;
  }
  v2::MultiBuf::Instance& mbv2 = *mbv2_;
  return mbv2->TryReserveChunks(num_chunks);
}

}  // namespace pw::multibuf::v1_adapter
