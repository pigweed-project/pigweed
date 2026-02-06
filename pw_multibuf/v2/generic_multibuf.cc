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

#include <cstring>
#include <utility>

#include "pw_assert/check.h"
#include "pw_containers/algorithm.h"
#include "pw_multibuf/v2/internal/byte_iterator.h"
#include "pw_multibuf/v2/multibuf.h"
#include "pw_status/try.h"

namespace pw::multibuf::v2::internal {

template <typename Out>
static constexpr Out CheckedCast(size_t val) {
  size_t max = size_t(std::numeric_limits<Out>::max());
  PW_CHECK_UINT_LE(val, max);
  return static_cast<Out>(val);
}

GenericMultiBuf& GenericMultiBuf::operator=(GenericMultiBuf&& other) {
  deque_ = std::move(other.deque_);
  entries_per_chunk_ =
      std::exchange(other.entries_per_chunk_, Entry::kMinEntriesPerChunk);
  observer_ = std::exchange(other.observer_, nullptr);
  return *this;
}

bool GenericMultiBuf::TryReserveForInsert(const_iterator pos,
                                          const GenericMultiBuf& mb) {
  size_type entries_per_chunk = entries_per_chunk_;
  while (entries_per_chunk_ < mb.entries_per_chunk_) {
    if (!AddLayer(0)) {
      break;
    }
  }
  if (pos.offset_ == 0 || !IsOwned(pos.chunk_) ||
      TryConvertToShared(pos.chunk_)) {
    if (entries_per_chunk_ >= mb.entries_per_chunk_ &&
        TryReserveEntries(entries_per_chunk_ * mb.num_chunks(),
                          pos.offset_ != 0)) {
      return true;
    }
  }
  while (entries_per_chunk_ > entries_per_chunk) {
    PopLayer();
  }
  return false;
}

bool GenericMultiBuf::TryReserveForInsert(const_iterator pos) {
  return (pos.offset_ == 0 || !IsOwned(pos.chunk_) ||
          TryConvertToShared(pos.chunk_)) &&
         TryReserveEntries(entries_per_chunk_, pos.offset_ != 0);
}

auto GenericMultiBuf::Insert(const_iterator pos, GenericMultiBuf&& mb)
    -> iterator {
  // Make room for the other object's entries.
  PW_CHECK(TryReserveForInsert(pos, mb));
  size_type chunk = InsertChunks(pos, mb.num_chunks());

  // Merge the entries into this object.
  size_t size = 0;
  size_type index = chunk * entries_per_chunk_;
  while (!mb.empty()) {
    size_type i = 0;
    size_type offset = mb.GetOffset(0);
    size_type length = mb.GetLength(0);
    for (; i < mb.entries_per_chunk_; ++i) {
      deque_[index + i] = mb.deque_.front();
      mb.deque_.pop_front();
    }

    // If this object is deeper than `mb`, pad it with extra entries.
    for (; i < entries_per_chunk_; ++i) {
      deque_[index + i].view = {
          .offset = offset,
          .sealed = false,
          .length = length,
          .boundary = true,
      };
    }
    size += size_t{length};
    index += entries_per_chunk_;
  }
  if (mb.observer_ != nullptr) {
    mb.observer_->Notify(Observer::Event::kBytesRemoved, size);
  }
  if (observer_ != nullptr) {
    observer_->Notify(Observer::Event::kBytesAdded, size);
  }
  return MakeIterator(chunk);
}

auto GenericMultiBuf::Insert(const_iterator pos, ConstByteSpan bytes)
    -> iterator {
  PW_CHECK(TryReserveForInsert(pos));
  [[maybe_unused]] auto [iter, unused] = Insert(pos, bytes, 0, bytes.size());
  return iter;
}

auto GenericMultiBuf::Insert(const_iterator pos,
                             ConstByteSpan bytes,
                             size_t offset,
                             size_t length,
                             Deallocator* deallocator) -> iterator {
  PW_CHECK(TryReserveForInsert(pos));
  auto [iter, chunk] = Insert(pos, bytes, offset, length);
  if (deallocator != nullptr) {
    deque_[memory_context_index(chunk)].deallocator = deallocator;
    deque_[base_view_index(chunk)].base_view.owned = true;
  }
  return iter;
}

auto GenericMultiBuf::Insert(const_iterator pos,
                             ConstByteSpan bytes,
                             size_t offset,
                             size_t length,
                             ControlBlock* control_block) -> iterator {
  PW_CHECK(TryReserveForInsert(pos));
  auto [iter, chunk] = Insert(pos, bytes, offset, length);
  if (control_block != nullptr) {
    control_block->IncrementShared();
    deque_[memory_context_index(chunk)].control_block = control_block;
    deque_[base_view_index(chunk)].base_view.shared = true;
  }
  return iter;
}

Result<GenericMultiBuf> GenericMultiBuf::Remove(const_iterator pos,
                                                size_t size) {
  PW_CHECK(IsRemovable(pos, size));
  GenericMultiBuf out(deque_.get_allocator());
  if (!TryReserveForRemove(pos, size, &out)) {
    return Status::ResourceExhausted();
  }
  MoveRange(pos, size, out);
  if (observer_ != nullptr) {
    observer_->Notify(Observer::Event::kBytesRemoved, size);
  }
  return Result<GenericMultiBuf>(std::move(out));
}

Result<GenericMultiBuf> GenericMultiBuf::PopFrontFragment() {
  PW_CHECK(!empty());
  size_t size = 0;
  for (size_type chunk = 0; chunk < num_chunks(); ++chunk) {
    size_type length = GetLength(chunk);
    if (length == 0) {
      continue;
    }
    size += size_t{length};
    if (IsBoundary(chunk)) {
      break;
    }
  }
  return Remove(begin(), size);
}

Result<GenericMultiBuf::const_iterator> GenericMultiBuf::Discard(
    const_iterator pos, size_t size) {
  if (!TryReserveForRemove(pos, size, nullptr)) {
    return Status::ResourceExhausted();
  }
  difference_type out_offset = pos - begin();
  ClearRange(pos, size);
  if (observer_ != nullptr) {
    observer_->Notify(Observer::Event::kBytesRemoved, size);
  }
  return cbegin() + out_offset;
}

bool GenericMultiBuf::IsReleasable(const_iterator pos) const {
  PW_CHECK(pos != cend());
  return IsOwned(pos.chunk_);
}

UniquePtr<std::byte[]> GenericMultiBuf::Release(const_iterator pos) {
  PW_CHECK(IsReleasable(pos));
  ByteSpan bytes = GetView(pos.chunk_, 1);
  Deallocator& deallocator = GetDeallocator(pos.chunk_);
  EraseRange(pos - pos.offset_, size_t{GetLength(pos.chunk_)});
  if (observer_ != nullptr) {
    observer_->Notify(Observer::Event::kBytesRemoved, bytes.size());
  }
  return UniquePtr<std::byte[]>(bytes.data(), bytes.size(), deallocator);
}

bool GenericMultiBuf::IsShareable(const_iterator pos) const {
  PW_CHECK(pos != cend());
  return IsShared(pos.chunk_);
}

SharedPtr<std::byte[]> GenericMultiBuf::Share(const_iterator pos) const {
  PW_CHECK(IsShareable(pos));
  ControlBlock& control_block = GetControlBlock(pos.chunk_);
  control_block.IncrementShared();
  return SharedPtr<std::byte[]>(GetData(pos.chunk_), &control_block);
}

size_t GenericMultiBuf::CopyFrom(ConstByteSpan src, size_t offset) {
  size_t total = 0;
  for (size_type chunk = 0; chunk < num_chunks(); ++chunk) {
    if (src.empty()) {
      break;
    }
    ByteSpan view = GetView(chunk);
    if (offset < view.size()) {
      size_t size = std::min(view.size() - offset, src.size());
      std::memcpy(view.data() + offset, src.data(), size);
      src = src.subspan(size);
      offset = 0;
      total += size;
    } else {
      offset -= view.size();
    }
  }
  return total;
}

ConstByteSpan GenericMultiBuf::Get(ByteSpan copy, size_t offset) const {
  ByteSpan buffer;
  std::optional<size_type> start;
  for (size_type chunk = 0; chunk < num_chunks(); ++chunk) {
    ByteSpan view = GetView(chunk);
    if (buffer.empty() && offset >= view.size()) {
      // Still looking for start of data.
      offset -= view.size();
    } else if (buffer.empty()) {
      // Found the start of data.
      buffer = view.subspan(offset);
      start = chunk;
    } else if (buffer.data() + buffer.size() == view.data()) {
      // Current view is contiguous with previous; append.
      buffer = ByteSpan(buffer.data(), buffer.size() + view.size());
    } else {
      // Span is discontiguous and needs to be copied.
      size_t copied = CopyToImpl(copy, offset, start.value());
      return copy.subspan(0, copied);
    }
  }
  // Requested span is contiguous and can be directly passed to the visitor.
  return buffer.size() <= copy.size() ? buffer : buffer.subspan(0, copy.size());
}

void GenericMultiBuf::Clear() {
  while (num_layers() > 1) {
    UnsealTopLayer();
    PopLayer();
  }
  size_t num_bytes = size();
  ClearRange(begin(), num_bytes);
  if (observer_ != nullptr) {
    observer_->Notify(Observer::Event::kBytesRemoved, num_bytes);
    observer_ = nullptr;
  }
}

void GenericMultiBuf::ShrinkToFit() { deque_.shrink_to_fit(); }

bool GenericMultiBuf::TryReserveLayers(size_t num_layers, size_t num_chunks) {
  if (num_layers == 0 || num_chunks == 0) {
    return true;
  }
  size_type num_entries = 0;
  PW_CHECK(CheckedIncrement(num_layers, Entry::kMinEntriesPerChunk - 1));
  PW_CHECK(CheckedMul(num_layers, num_chunks, num_entries));
  if (num_entries <= deque_.size()) {
    return true;
  }
  return TryReserveEntries(num_entries - deque_.size());
}

bool GenericMultiBuf::AddLayer(size_t offset, size_t length) {
  // Given entries with layers A and B, to which we want to add layer C:
  //     A1 B1 A2 B2 A3 B3 A4 B4
  // 1). Add `shift` empty buffers:
  //     A1 B1 A2 B2 A3 B3 A4 B4 -- -- -- --
  size_type shift = num_chunks();
  if (!TryReserveEntries(shift)) {
    return false;
  }
  ++entries_per_chunk_;
  for (size_t i = 0; i < shift; ++i) {
    deque_.push_back({.data = nullptr});
  }

  // 2). Shift the existing layers over. This is expensive, but slicing usually
  //     happens with `shift == 1`:
  //     A1 B1 -- A2 B2 -- A3 B3 -- A4 B4 --
  for (size_type i = deque_.size(); i != 0; --i) {
    if (i % entries_per_chunk_ == 0) {
      --shift;
      deque_[i - 1].view = {
          .offset = 0,
          .sealed = false,
          .length = 0,
          .boundary = false,
      };
    } else {
      deque_[i - 1] = deque_[i - 1 - shift];
    }
  }

  // 3). Fill in the new layer C with subspans of layer B:
  //     A1 B1 C1 A2 B2 C2 A3 B3 C3 A4 B4 C4
  size_type off = CheckedCast<size_type>(offset);
  size_type len = length == dynamic_extent ? 0 : CheckedCast<size_type>(length);
  size_type lower_layer = num_layers() - 1;
  size_type num_fragments = 0;
  Entry* last = nullptr;
  for (size_type chunk = 0; chunk < num_chunks(); ++chunk) {
    size_type lower_off = GetOffset(chunk, lower_layer);
    size_type lower_len = GetLength(chunk, lower_layer);

    if (lower_len != 0 && IsBoundary(chunk, lower_layer)) {
      ++num_fragments;
    }

    // Skip over entries until we reach `offset`.
    Entry& entry = deque_[top_view_index(chunk)];
    if (off >= lower_len) {
      off -= lower_len;
      entry.view.offset = 0;
      entry.view.length = 0;
      continue;
    }
    entry.view.offset = lower_off + off;
    lower_len -= off;

    // This is similar to `entry.view.length = std::min(lower_len, len);`, but
    // with extra cases to correctly handle `dynamic_extent` and boundaries.
    if (lower_len == 0) {
      entry.view.length = 0;
    } else if (length == dynamic_extent) {
      entry.view.length = lower_len;
      last = &entry;
    } else if (len == 0) {
      entry.view.length = 0;
    } else if (len <= lower_len) {
      entry.view.length = len;
      entry.view.boundary = true;
      len = 0;
    } else {
      entry.view.length = lower_len;
      len -= lower_len;
    }
    off = 0;
  }
  PW_CHECK_UINT_EQ(len, 0, "Requested layer exceeds available data");

  if (last != nullptr) {
    last->view.boundary = true;
  }
  if (observer_ != nullptr) {
    observer_->Notify(Observer::Event::kLayerAdded, num_fragments);
  }
  return true;
}

void GenericMultiBuf::SealTopLayer() {
  PW_CHECK_UINT_GT(num_layers(), 1);
  for (size_type chunk = 0; chunk < num_chunks(); ++chunk) {
    deque_[top_view_index(chunk)].view.sealed = true;
  }
}

void GenericMultiBuf::UnsealTopLayer() {
  PW_CHECK_UINT_GT(num_layers(), 1);
  for (size_type chunk = 0; chunk < num_chunks(); ++chunk) {
    deque_[top_view_index(chunk)].view.sealed = false;
  }
}

void GenericMultiBuf::TruncateTopLayer(size_t length) {
  PW_CHECK_UINT_GT(num_layers(), 1);

  size_type len = CheckedCast<size_type>(length);
  for (size_type chunk = 0; chunk < num_chunks(); ++chunk) {
    PW_CHECK(!IsSealed(chunk),
             "MultiBuf::TruncateTopLayer() was called on a sealed layer; call "
             "UnsealTopLayer first");
    size_type chunk_len = GetLength(chunk);
    if (len >= chunk_len) {
      len -= chunk_len;
      continue;
    }
    deque_[top_view_index(chunk)].view.length = len;
    len = 0;
  }
  PW_CHECK_UINT_EQ(len,
                   0,
                   "MultiBuf::TruncateTopLayer() was called with a length "
                   "longer than the MultiBuf");
}

void GenericMultiBuf::PopLayer() {
  PW_CHECK_UINT_GT(num_layers(), 1);

  // Given entries with layers A, B, and C, to remove layer C:
  //     A1 B1 C1 A2 B2 C2 A3 B3 C3 A4 B4 C4
  // 1). Check that the layer is not sealed.
  size_t num_fragments = 0;
  for (size_type chunk = 0; chunk < num_chunks(); ++chunk) {
    PW_CHECK(!IsSealed(chunk),
             "MultiBuf::PopLayer() was called on a sealed layer; call "
             "UnsealTopLayer first");
    if (GetLength(chunk) != 0 && IsBoundary(chunk)) {
      ++num_fragments;
    }
  }

  // 2). Compress lower layers backward.
  //     -- -- -- -- A1 B1 A2 B2 A3 B3 A4 B4
  size_type shift = 0;
  size_type discard = deque_.size() / entries_per_chunk_;
  size_type keep = deque_.size() - discard;
  --entries_per_chunk_;
  for (size_type i = 1; i <= keep; ++i) {
    size_type j = deque_.size() - i;
    if ((i - 1) % entries_per_chunk_ == 0) {
      ++shift;
    }
    deque_[j] = deque_[j - shift];
    if ((j - discard) % entries_per_chunk_ != entries_per_chunk_ - 1) {
      continue;
    }
  }

  // 3). Discard the first elements
  //     A1 B1 A2 B2 A3 B3 A4 B4
  for (size_type i = 0; i < discard; ++i) {
    deque_.pop_front();
  }
  if (observer_ != nullptr) {
    observer_->Notify(Observer::Event::kLayerRemoved, num_fragments);
  }
}

// Implementation methods

size_t GenericMultiBuf::CheckRange(size_t offset, size_t length, size_t size) {
  PW_CHECK_UINT_LE(size, Entry::kMaxSize);
  PW_CHECK_UINT_LE(offset, size);
  if (length == dynamic_extent) {
    return size - offset;
  }
  PW_CHECK_UINT_LE(length, size - offset);
  return length;
}

GenericMultiBuf::size_type GenericMultiBuf::NumFragments() const {
  size_type num_fragments = 0;
  for (size_type chunk = 0; chunk < num_chunks(); ++chunk) {
    if (GetLength(chunk) != 0 && IsBoundary(chunk)) {
      ++num_fragments;
    }
  }
  return num_fragments;
}

bool GenericMultiBuf::TryConvertToShared(size_type chunk) {
  Deallocator& deallocator = GetDeallocator(chunk);
  std::byte* data = GetData(chunk);
  Entry::BaseView& base_view = deque_[base_view_index(chunk)].base_view;
  auto* control_block =
      ControlBlock::Create(&deallocator, data, base_view.length);
  if (control_block == nullptr) {
    return false;
  }
  deque_[memory_context_index(chunk)].control_block = control_block;
  base_view.owned = false;
  base_view.shared = true;
  return true;
}

bool GenericMultiBuf::TryReserveEntries(size_type num_entries, bool split) {
  if (split) {
    PW_CHECK(CheckedAdd(num_entries, entries_per_chunk_, num_entries));
  }
  PW_CHECK(CheckedAdd(num_entries, deque_.size(), num_entries));
  return deque_.try_reserve_exact(num_entries);
}

GenericMultiBuf::size_type GenericMultiBuf::InsertChunks(const_iterator pos,
                                                         size_type num_chunks) {
  size_type chunk = pos.chunk_;
  size_type offset = pos.offset_;
  if (offset != 0) {
    num_chunks++;
  } else if (chunk < this->num_chunks()) {
    // Insert before any empty chunks at `pos`.
    while (chunk != 0 && GetLength(chunk - 1) == 0) {
      --chunk;
    }
  }
  size_type num_entries = num_chunks * entries_per_chunk_;
  PW_CHECK(TryReserveEntries(num_entries, offset != 0));
  Entry entry;
  entry.data = nullptr;
  for (size_type i = 0; i < num_entries; ++i) {
    deque_.push_back(entry);
  }
  size_type index = chunk * entries_per_chunk_;
  for (size_type i = deque_.size() - 1; i >= index + num_entries; --i) {
    deque_[i] = deque_[i - num_entries];
  }

  if (offset == 0) {
    // New chunk falls between existing chunks.
    return chunk;
  }
  // New chunk within an existing chunk, which must be split.
  SplitAfter(chunk, offset, deque_, chunk + num_chunks);
  SplitBefore(chunk, offset);
  return chunk + 1;
}

auto GenericMultiBuf::Insert(const_iterator pos,
                             ConstByteSpan bytes,
                             size_t offset,
                             size_t length) -> std::tuple<iterator, size_type> {
  size_type chunk = InsertChunks(pos, 1);
  deque_[memory_context_index(chunk)].deallocator = nullptr;
  deque_[data_index(chunk)].data = const_cast<std::byte*>(bytes.data());
  auto off = CheckedCast<size_type>(offset);
  if (length == dynamic_extent) {
    length = bytes.size() - offset;
  }
  auto len = CheckedCast<size_type>(length);
  deque_[base_view_index(chunk)].base_view = {
      .offset = off,
      .owned = false,
      .length = len,
      .shared = false,
  };
  for (size_type layer = 2; layer <= num_layers(); ++layer) {
    deque_[view_index(chunk, layer)].view = {
        .offset = off,
        .sealed = false,
        .length = len,
        .boundary = true,
    };
  }
  if (observer_ != nullptr) {
    observer_->Notify(Observer::Event::kBytesAdded, length);
  }
  return std::make_tuple(MakeIterator(chunk), chunk);
}

void GenericMultiBuf::SplitBase(size_type chunk,
                                Deque& out_deque,
                                size_type out_chunk) {
  if (&deque_ == &out_deque && chunk == out_chunk) {
    return;
  }
  PW_CHECK(!IsOwned(chunk));
  size_type index = chunk * entries_per_chunk_;
  size_type out_index = out_chunk * entries_per_chunk_;
  for (size_type i = 0; i < entries_per_chunk_; ++i) {
    out_deque[out_index + i] = deque_[index + i];
  }
  if (IsShared(chunk)) {
    GetControlBlock(chunk).IncrementShared();
  }
}

void GenericMultiBuf::SplitBefore(size_type chunk,
                                  size_type split,
                                  Deque& out_deque,
                                  size_type out_chunk) {
  SplitBase(chunk, out_deque, out_chunk);
  split += GetOffset(chunk);
  Entry::BaseView src_base_view = deque_[base_view_index(chunk)].base_view;
  Entry::BaseView& dst_base_view =
      out_deque[base_view_index(out_chunk)].base_view;
  dst_base_view.offset = src_base_view.offset;
  dst_base_view.length = split - src_base_view.offset;
  for (size_type layer = 2; layer <= num_layers(); ++layer) {
    Entry::View src_view = deque_[view_index(chunk, layer)].view;
    Entry::View& dst_view = out_deque[view_index(out_chunk, layer)].view;
    dst_view.offset = src_view.offset;
    dst_view.length = split - src_view.offset;
  }
}

void GenericMultiBuf::SplitBefore(size_type chunk, size_type split) {
  SplitBefore(chunk, split, deque_, chunk);
}

void GenericMultiBuf::SplitAfter(size_type chunk,
                                 size_type split,
                                 Deque& out_deque,
                                 size_type out_chunk) {
  SplitBase(chunk, out_deque, out_chunk);
  split += GetOffset(chunk);
  Entry::BaseView src_base_view = deque_[base_view_index(chunk)].base_view;
  Entry::BaseView& dst_base_view =
      out_deque[base_view_index(out_chunk)].base_view;
  dst_base_view.offset = split;
  dst_base_view.length = src_base_view.offset + src_base_view.length - split;
  for (size_type layer = 2; layer <= num_layers(); ++layer) {
    Entry::View src_view = deque_[view_index(chunk, layer)].view;
    Entry::View& dst_view = out_deque[view_index(out_chunk, layer)].view;
    dst_view.offset = split;
    dst_view.length = src_view.offset + src_view.length - split;
  }
}

void GenericMultiBuf::SplitAfter(size_type chunk, size_type split) {
  SplitAfter(chunk, split, deque_, chunk);
}

bool GenericMultiBuf::TryReserveForRemove(const_iterator pos,
                                          size_t size,
                                          GenericMultiBuf* out) {
  PW_CHECK_UINT_NE(size, 0u);
  auto end = pos + CheckedCast<difference_type>(size);
  size_type shift = end.chunk_ - pos.chunk_;

  // If removing part of an owned chunk, make it shared.
  if (pos.offset_ != 0 && IsOwned(pos.chunk_) &&
      !TryConvertToShared(pos.chunk_)) {
    return false;
  }

  // Removing a sub-chunk.
  if (shift == 0 && pos.offset_ != 0) {
    return (out == nullptr || out->TryReserveEntries(entries_per_chunk_)) &&
           TryReserveEntries(0, /*split=*/true);
  }

  // If removing part of an owned chunk, make it shared.
  if (end.offset_ != 0 && IsOwned(end.chunk_) &&
      !TryConvertToShared(end.chunk_)) {
    return false;
  }

  // Discarding entries, no room needed.
  if (out == nullptr) {
    return true;
  }

  // Make room in `out`.
  if (end.offset_ != 0) {
    ++shift;
  }
  return out->TryReserveEntries(shift * entries_per_chunk_);
}

void GenericMultiBuf::MoveRange(const_iterator pos,
                                size_t size,
                                GenericMultiBuf& out) {
  size_type chunk = pos.chunk_;
  size_type offset = pos.offset_;
  auto end = pos + CheckedCast<difference_type>(size);
  out.entries_per_chunk_ = entries_per_chunk_;

  // Determine how many entries needs to be moved.
  size_type shift = end.chunk_ - chunk;

  // Are we removing the prefix of a single chunk?
  if (shift == 0 && offset == 0) {
    out.InsertChunks(begin(), 1);
    SplitBefore(chunk, end.offset_, out.deque_, 0);
    EraseRange(pos, size);
    return;
  }

  // Are we removing a sub-chunk? If so, split the chunk in two.
  if (shift == 0) {
    out.InsertChunks(begin(), 1);
    SplitBefore(end.chunk_, end.offset_, out.deque_, 0);
    out.SplitAfter(0, offset);
    EraseRange(pos, size);
    return;
  }

  // Otherwise, start by copying entries to the new deque, if provided.
  size_type out_chunk = 0;
  size_type reserve = end.offset_ == 0 ? shift : shift + 1;
  out.InsertChunks(cend(), reserve);

  // Move the suffix of the first chunk.
  if (offset != 0) {
    SplitAfter(chunk, offset, out.deque_, out_chunk);
    --shift;
    ++chunk;
    ++out_chunk;
  }

  // Move the complete chunks.
  size_type index = chunk * entries_per_chunk_;
  size_type end_index = end.chunk_ * entries_per_chunk_;
  size_type out_index = out_chunk * entries_per_chunk_;
  pw::copy(deque_.begin() + index,
           deque_.begin() + end_index,
           out.deque_.begin() + out_index);
  chunk += shift;
  out_chunk += shift;

  // Copy the prefix of the last chunk.
  if (end.offset_ != 0) {
    SplitBefore(end.chunk_, end.offset_, out.deque_, out_chunk);
  }
  EraseRange(pos, size);
}

void GenericMultiBuf::ClearRange(const_iterator pos, size_t size) {
  size_type chunk = pos.chunk_;
  size_type offset = pos.offset_;
  auto end = pos + CheckedCast<difference_type>(size);
  if (offset != 0) {
    ++chunk;
  }
  for (; chunk < end.chunk_; ++chunk) {
    std::byte* data = GetData(chunk);
    if (IsOwned(chunk)) {
      Deallocator& deallocator = GetDeallocator(chunk);
      deallocator.Deallocate(data);
      continue;
    }
    if (!IsShared(chunk)) {
      continue;
    }
    // To avoid races with other shared or weak pointers to the data, put the
    // data pointer back into a SharedPtr and let it go out scope.
    ControlBlock& control_block = GetControlBlock(chunk);
    SharedPtr<std::byte[]> shared(data, &control_block);
  }
  EraseRange(pos, size);
}

void GenericMultiBuf::EraseRange(const_iterator pos, size_t size) {
  size_type chunk = pos.chunk_;
  size_type offset = pos.offset_;
  auto end = pos + CheckedCast<difference_type>(size);

  // Are we removing a sub-chunk? If so, split the chunk in two.
  if (chunk == end.chunk_ && offset != 0) {
    size_type new_chunk = InsertChunks(pos, 0);
    SplitAfter(new_chunk, end.offset_ - offset);
    return;
  }

  // Discard suffix of first chunk.
  if (offset != 0) {
    SplitBefore(chunk, offset);
    ++chunk;
  }

  // Discard prefix of last chunk.
  if (end.offset_ != 0) {
    SplitAfter(end.chunk_, end.offset_);
  }

  // Discard complete chunks.
  if (chunk < end.chunk_) {
    deque_.erase(deque_.begin() + (chunk * entries_per_chunk_),
                 deque_.begin() + (end.chunk_ * entries_per_chunk_));
  }
}

size_t GenericMultiBuf::CopyToImpl(ByteSpan dst,
                                   size_t offset,
                                   size_type start) const {
  size_t total = 0;
  for (size_type chunk = start; chunk < num_chunks(); ++chunk) {
    if (dst.empty()) {
      break;
    }
    ConstByteSpan view = GetView(chunk);
    if (offset < view.size()) {
      size_t size = std::min(view.size() - offset, dst.size());
      std::memcpy(dst.data(), view.data() + offset, size);
      dst = dst.subspan(size);
      offset = 0;
      total += size;
    } else {
      offset -= view.size();
    }
  }
  return total;
}

bool GenericMultiBuf::IsTopLayerSealed() const {
  for (size_type chunk = 0; chunk < num_chunks(); ++chunk) {
    if (IsSealed(chunk)) {
      return true;
    }
  }
  return false;
}

}  // namespace pw::multibuf::v2::internal
