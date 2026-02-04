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

#include "pw_multibuf/v1/multibuf.h"

#include <algorithm>
#include <cstring>

#include "pw_assert/check.h"

namespace pw::multibuf::v1 {

void MultiBufChunks::Release() noexcept {
  while (first_ != nullptr) {
    Chunk* removed = first_;
    first_ = first_->next_in_buf_;
    removed->Free();
  }
}

size_t MultiBufChunks::size_bytes() const {
  size_t len = 0;
  for (const auto& chunk : *this) {
    len += chunk.size();
  }
  return len;
}

bool MultiBuf::empty() const {
  return std::all_of(Chunks().begin(), Chunks().end(), [](const Chunk& c) {
    return c.empty();
  });
}

std::optional<ConstByteSpan> MultiBuf::ContiguousSpan() const {
  ConstByteSpan contiguous;
  for (const Chunk& chunk : Chunks()) {
    if (chunk.empty()) {
      continue;
    }
    if (contiguous.empty()) {
      contiguous = chunk;
    } else if (contiguous.data() + contiguous.size() == chunk.data()) {
      contiguous = {contiguous.data(), contiguous.size() + chunk.size()};
    } else {  // Non-empty chunks are not contiguous
      return std::nullopt;
    }
  }
  // Return either the one non-empty chunk or an empty span.
  return contiguous;
}

bool MultiBuf::ClaimPrefix(size_t bytes_to_claim) {
  if (Chunks().empty()) {
    return false;
  }
  return Chunks().front().ClaimPrefix(bytes_to_claim);
}

bool MultiBuf::ClaimSuffix(size_t bytes_to_claim) {
  if (Chunks().empty()) {
    return false;
  }
  return Chunks().back().ClaimSuffix(bytes_to_claim);
}

void MultiBuf::DiscardPrefix(size_t bytes_to_discard) {
  PW_DCHECK(bytes_to_discard <= size());
  while (bytes_to_discard != 0) {
    if (Chunks().begin()->size() > bytes_to_discard) {
      Chunks().begin()->DiscardPrefix(bytes_to_discard);
      return;
    }
    OwnedChunk front_chunk = TakeFrontChunk();
    bytes_to_discard -= front_chunk.size();
  }
}

void MultiBuf::Slice(size_t begin, size_t end) {
  PW_DCHECK(end >= begin);
  DiscardPrefix(begin);
  Truncate(end - begin);
}

void MultiBuf::Truncate(size_t len) {
  if (len == 0) {
    Release();
    return;
  }
  TruncateAfter(begin() + (len - 1));
}

void MultiBuf::TruncateAfter(iterator pos) {
  PW_DCHECK(pos != end());
  pos.chunk()->Truncate(pos.byte_index() + 1);
  Chunk* remainder = pos.chunk()->next_in_buf_;
  pos.chunk()->next_in_buf_ = nullptr;
  MultiBuf discard(remainder);
}

void MultiBuf::PushPrefix(MultiBuf&& front) {
  front.PushSuffix(std::move(*this));
  *this = std::move(front);
}

void MultiBufChunks::PushSuffix(MultiBufChunks&& tail) {
  if (first_ == nullptr) {
    first_ = tail.first_;
    tail.first_ = nullptr;
    return;
  }
  back().next_in_buf_ = tail.first_;
  tail.first_ = nullptr;
}

StatusWithSize MultiBuf::CopyTo(ByteSpan dest, const size_t position) const {
  const_iterator byte_in_chunk = begin() + position;

  MultiBufChunks::const_iterator chunk(byte_in_chunk.chunk());
  size_t chunk_offset = byte_in_chunk.byte_index();

  size_t bytes_copied = 0;
  for (; chunk != Chunks().end(); ++chunk) {
    const size_t chunk_bytes = chunk->size() - chunk_offset;
    const size_t to_copy = std::min(chunk_bytes, dest.size() - bytes_copied);
    if (to_copy != 0u) {
      std::memcpy(
          dest.data() + bytes_copied, chunk->data() + chunk_offset, to_copy);
      bytes_copied += to_copy;
    }

    if (chunk_bytes > to_copy) {
      return StatusWithSize::ResourceExhausted(bytes_copied);
    }
    chunk_offset = 0;
  }

  return StatusWithSize(bytes_copied);  // all chunks were copied
}

StatusWithSize MultiBuf::CopyFromAndOptionallyTruncate(ConstByteSpan source,
                                                       size_t position,
                                                       bool truncate) {
  if (source.empty()) {
    if (truncate) {
      Truncate(position);
    }
    return StatusWithSize(0u);
  }

  iterator byte_in_chunk = begin() + position;
  size_t chunk_offset = byte_in_chunk.byte_index();

  size_t bytes_copied = 0;
  for (MultiBufChunks::iterator chunk(byte_in_chunk.chunk());
       chunk != Chunks().end();
       ++chunk) {
    if (chunk->empty()) {
      continue;
    }
    const size_t to_copy =
        std::min(chunk->size() - chunk_offset, source.size() - bytes_copied);
    std::memcpy(
        chunk->data() + chunk_offset, source.data() + bytes_copied, to_copy);
    bytes_copied += to_copy;

    if (bytes_copied == source.size()) {
      if (truncate) {
        // to_copy is always at least one byte, since source.empty() is checked
        // above and empty chunks are skipped.
        TruncateAfter(iterator(&*chunk, chunk_offset + to_copy - 1));
      }
      return StatusWithSize(bytes_copied);
    }
    chunk_offset = 0;
  }

  return StatusWithSize::ResourceExhausted(bytes_copied);  // ran out of space
}

std::optional<MultiBuf> MultiBuf::TakePrefix(size_t bytes_to_take) {
  PW_DCHECK(bytes_to_take <= size());
  MultiBuf front;
  if (bytes_to_take == 0) {
    return front;
  }
  // Pointer to the last element of `front`, allowing constant-time appending.
  Chunk* last_front_chunk = nullptr;
  while (bytes_to_take > Chunks().begin()->size()) {
    OwnedChunk new_chunk = TakeFrontChunk().Take();
    Chunk* new_chunk_ptr = &*new_chunk;
    bytes_to_take -= new_chunk.size();
    if (last_front_chunk == nullptr) {
      front.PushFrontChunk(std::move(new_chunk));
    } else {
      last_front_chunk->next_in_buf_ = std::move(new_chunk).Take();
    }
    last_front_chunk = new_chunk_ptr;
  }
  if (bytes_to_take == 0) {
    return front;
  }
  std::optional<OwnedChunk> last_front_bit =
      Chunks().front().TakePrefix(bytes_to_take);
  if (last_front_bit.has_value()) {
    if (last_front_chunk != nullptr) {
      Chunk* last_front_bit_ptr = std::move(*last_front_bit).Take();
      last_front_chunk->next_in_buf_ = last_front_bit_ptr;
    } else {
      front.PushFrontChunk(std::move(*last_front_bit));
    }
    return front;
  }
  // The front chunk could not be split, so we must put the front back on.
  PushPrefix(std::move(front));
  return std::nullopt;
}

std::optional<MultiBuf> MultiBuf::TakeSuffix(size_t bytes_to_take) {
  size_t front_size = size() - bytes_to_take;
  std::optional<MultiBuf> front_opt = TakePrefix(front_size);
  if (!front_opt.has_value()) {
    return std::nullopt;
  }
  MultiBuf front_then_back = std::move(*front_opt);
  std::swap(front_then_back, *this);
  return front_then_back;
}

void MultiBufChunks::push_front(OwnedChunk&& chunk) {
  PW_DCHECK(chunk->next_in_buf_ == nullptr);
  Chunk* new_chunk = std::move(chunk).Take();
  Chunk* old_first = first_;
  new_chunk->next_in_buf_ = old_first;
  first_ = new_chunk;
}

void MultiBufChunks::push_back(OwnedChunk&& chunk) {
  PW_DCHECK(chunk->next_in_buf_ == nullptr);
  Chunk* new_chunk = std::move(chunk).Take();
  if (first_ == nullptr) {
    first_ = new_chunk;
    return;
  }
  Chunk* cur = first_;
  while (cur->next_in_buf_ != nullptr) {
    cur = cur->next_in_buf_;
  }
  cur->next_in_buf_ = new_chunk;
}

MultiBufChunks::iterator MultiBufChunks::insert(iterator position,
                                                OwnedChunk&& chunk) {
  // Note: this also catches the cases where ``first_ == nullptr``
  PW_DCHECK(chunk->next_in_buf_ == nullptr);
  if (position == begin()) {
    push_front(std::move(chunk));
    return iterator(first_);
  }
  Chunk* previous = Previous(position.chunk_);
  Chunk* old_next = previous->next_in_buf_;
  Chunk* new_chunk = std::move(chunk).Take();
  new_chunk->next_in_buf_ = old_next;
  previous->next_in_buf_ = new_chunk;
  return iterator(new_chunk);
}

OwnedChunk MultiBufChunks::take_front() {
  PW_CHECK(!empty());
  Chunk* old_first = first_;
  first_ = old_first->next_in_buf_;
  old_first->next_in_buf_ = nullptr;
  return OwnedChunk(old_first);
}

std::tuple<MultiBufChunks::iterator, OwnedChunk> MultiBufChunks::take(
    iterator position) {
  Chunk* chunk = position.chunk_;
  if (position == begin()) {
    OwnedChunk old_first = take_front();
    return std::make_tuple(iterator(first_), std::move(old_first));
  }
  Chunk* previous = Previous(chunk);
  previous->next_in_buf_ = chunk->next_in_buf_;
  chunk->next_in_buf_ = nullptr;
  return std::make_tuple(iterator(previous->next_in_buf_), OwnedChunk(chunk));
}

Chunk* MultiBufChunks::Previous(Chunk* chunk) const {
  Chunk* previous = first_;
  while (previous != nullptr && previous->next_in_buf_ != chunk) {
    previous = previous->next_in_buf_;
  }
  return previous;
}

MultiBuf::const_iterator& MultiBuf::const_iterator::operator++() {
  if (byte_index_ + 1 == chunk_->size()) {
    chunk_ = chunk_->next_in_buf_;
    byte_index_ = 0;
    AdvanceToData();
  } else {
    ++byte_index_;
  }
  return *this;
}

MultiBuf::const_iterator& MultiBuf::const_iterator::operator+=(size_t advance) {
  if (advance == 0) {
    return *this;
  }

  while (chunk_ != nullptr && advance >= (chunk_->size() - byte_index_)) {
    advance -= (chunk_->size() - byte_index_);
    chunk_ = chunk_->next_in_buf_;
    byte_index_ = 0;
  }

  PW_DCHECK(chunk_ != nullptr || advance == 0u,
            "Iterated past the end of the MultiBuf");
  byte_index_ += advance;
  return *this;
}

const Chunk& MultiBufChunks::back() const {
  Chunk* current = first_;
  while (current->next_in_buf_ != nullptr) {
    current = current->next_in_buf_;
  }
  return *current;
}

}  // namespace pw::multibuf::v1
