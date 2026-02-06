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
#include <cstdint>
#include <limits>

#include "pw_allocator/deallocator.h"
#include "pw_allocator/internal/control_block.h"
#include "pw_bytes/span.h"
#include "pw_containers/dynamic_deque.h"
#include "pw_preprocessor/compiler.h"

namespace pw::multibuf::v2::internal {

/// Used to indicate whether the contents of a multibuf chunk are modifiable or
/// read-only.
enum class Mutability {
  kMutable,
  kConst,
};

/// Describes either a memory location or a view of an associated location.
///
/// This module stores byte buffers in queues using sequences of entries. The
/// first entry holds an address, and subsequent entries hold the offset and
/// lengths of ever-narrower views of that data. This provides a compact
/// representation of data encoded using nested or layered protocols.
///
/// For example, in a TCP/IP stack:
///  * The first entry hold addresses to Ethernet frames.
///  * The second entry holds a zero offset and the whole frame length.
///  * The third entry holds the offset and length describing the IP data.
///  * The fourth entry holds the offset and length describing the TCP data.
struct Entry {
  using Deque = DynamicDeque<Entry>;
  using size_type = Deque::size_type;
  using difference_type = Deque::difference_type;

  /// Offset and length must fit in 15 bits.
  static constexpr size_t kMaxSize = ~(1U << 15);

  /// Per-chunk index entry that holds the deallocator or control block.
  static constexpr size_type kMemoryContextIndex = 0;

  /// Per-chunk index entry that holds the data pointer.
  static constexpr size_type kDataIndex = 1;

  /// Per-chunk index entry that holds the base view of the data.
  static constexpr size_type kBaseViewIndex = 2;

  /// Minimum number of entries per chunk.
  static constexpr size_type kMinEntriesPerChunk = 3;

  /// Returns the number of chunks in a deque.
  [[nodiscard]] static constexpr size_type num_chunks(
      const Deque& deque, size_type entries_per_chunk) {
    return deque.size() / entries_per_chunk;
  }

  /// Returns the number of layers in a chunk given the number of entries used
  /// per chunk.
  [[nodiscard]] static constexpr size_type num_layers(
      size_type entries_per_chunk) {
    return entries_per_chunk - Entry::kMinEntriesPerChunk + 1;
  }

  /// Returns the index to the data entry of a given chunk.
  [[nodiscard]] static constexpr size_type memory_context_index(
      size_type chunk, size_type entries_per_chunk) {
    return chunk * entries_per_chunk + kMemoryContextIndex;
  }

  /// Returns the index to the data entry of a given chunk.
  [[nodiscard]] static constexpr size_type data_index(
      size_type chunk, size_type entries_per_chunk) {
    return chunk * entries_per_chunk + kDataIndex;
  }

  /// Returns the index to the base view entry of a given chunk.
  [[nodiscard]] static constexpr size_type base_view_index(
      size_type chunk, size_type entries_per_chunk) {
    return chunk * entries_per_chunk + kBaseViewIndex;
  }

  /// Returns the index to a view entry of a given chunk.
  [[nodiscard]] static constexpr size_type view_index(
      size_type chunk, size_type entries_per_chunk, size_type layer) {
    return chunk * entries_per_chunk + kBaseViewIndex + layer - 1;
  }

  /// Returns the index to the top view entry of a given chunk.
  [[nodiscard]] static constexpr size_type top_view_index(
      size_type chunk, size_type entries_per_chunk) {
    return (chunk + 1) * entries_per_chunk - 1;
  }

  /// Returns the memory backing the chunk at the given index.
  [[nodiscard]] static constexpr std::byte* GetData(
      const Deque& deque, size_type chunk, size_type entries_per_chunk) {
    return deque[data_index(chunk, entries_per_chunk)].data;
  }

  /// Returns whether the memory backing the chunk at the given index is owned
  /// by this object.
  [[nodiscard]] static constexpr bool IsOwned(const Deque& deque,
                                              size_type chunk,
                                              size_type entries_per_chunk) {
    return deque[base_view_index(chunk, entries_per_chunk)].base_view.owned;
  }

  /// Returns whether the memory backing the chunk at the given index is shared
  /// with other objects.
  [[nodiscard]] static constexpr bool IsShared(const Deque& deque,
                                               size_type chunk,
                                               size_type entries_per_chunk) {
    return deque[base_view_index(chunk, entries_per_chunk)].base_view.shared;
  }

  /// Returns whether the chunk is part of a sealed layer.
  [[nodiscard]] static constexpr bool IsSealed(const Deque& deque,
                                               size_type chunk,
                                               size_type entries_per_chunk) {
    return entries_per_chunk == Entry::kMinEntriesPerChunk
               ? false
               : deque[top_view_index(chunk, entries_per_chunk)].view.sealed;
  }

  /// Returns the deallocator for a chunk, which must be owned.
  [[nodiscard]] static constexpr Deallocator& GetDeallocator(
      const Deque& deque, size_type chunk, size_type entries_per_chunk) {
    PW_ASSERT(IsOwned(deque, chunk, entries_per_chunk));
    return *(deque[memory_context_index(chunk, entries_per_chunk)].deallocator);
  }

  /// Returns the control block for a chunk, which must be shared.
  [[nodiscard]] static constexpr allocator::internal::ControlBlock&
  GetControlBlock(const Deque& deque,
                  size_type chunk,
                  size_type entries_per_chunk) {
    PW_ASSERT(IsShared(deque, chunk, entries_per_chunk));
    return *(
        deque[memory_context_index(chunk, entries_per_chunk)].control_block);
  }

  /// Returns whether the given layer of a chunk represents a fragment boundary.
  [[nodiscard]] static constexpr bool IsBoundary(const Deque& deque,
                                                 size_type chunk,
                                                 size_type entries_per_chunk,
                                                 size_type layer) {
    return layer == 1 ? true
                      : deque[view_index(chunk, entries_per_chunk, layer)]
                            .view.boundary;
  }

  /// Returns whether the top layer of a chunk represents a fragment boundary.
  [[nodiscard]] static constexpr bool IsBoundary(const Deque& deque,
                                                 size_type chunk,
                                                 size_type entries_per_chunk) {
    return IsBoundary(
        deque, chunk, entries_per_chunk, num_layers(entries_per_chunk));
  }

  /// Returns the offset of the given layer of the chunk.
  [[nodiscard]] static constexpr size_type GetOffset(
      const Deque& deque,
      size_type chunk,
      size_type entries_per_chunk,
      size_type layer) {
    return layer == 1
               ? deque[base_view_index(chunk, entries_per_chunk)]
                     .base_view.offset
               : deque[view_index(chunk, entries_per_chunk, layer)].view.offset;
  }

  /// Returns the offset of the top layer of the chunk.
  [[nodiscard]] static constexpr size_type GetOffset(
      const Deque& deque, size_type chunk, size_type entries_per_chunk) {
    return GetOffset(
        deque, chunk, entries_per_chunk, num_layers(entries_per_chunk));
  }

  /// Returns the offset of the given layer of the chunk relative to the layer
  /// below it.
  [[nodiscard]] static constexpr size_type GetRelativeOffset(
      const Deque& deque,
      size_type chunk,
      size_type entries_per_chunk,
      size_type layer) {
    return layer == 1 ? GetOffset(deque, chunk, entries_per_chunk, layer)
                      : (GetOffset(deque, chunk, entries_per_chunk, layer) -
                         GetOffset(deque, chunk, entries_per_chunk, layer - 1));
  }

  /// Returns the offset of the top layer of the chunk relative to the layer
  /// below it.
  [[nodiscard]] static constexpr size_type GetRelativeOffset(
      const Deque& deque, size_type chunk, size_type entries_per_chunk) {
    return GetRelativeOffset(
        deque, chunk, entries_per_chunk, num_layers(entries_per_chunk));
  }

  /// Returns the length of the given layer of the chunk.
  [[nodiscard]] static constexpr size_type GetLength(
      const Deque& deque,
      size_type chunk,
      size_type entries_per_chunk,
      size_type layer) {
    return layer == 1
               ? deque[base_view_index(chunk, entries_per_chunk)]
                     .base_view.length
               : deque[view_index(chunk, entries_per_chunk, layer)].view.length;
  }

  /// Returns the length of the top layer of the chunk.
  [[nodiscard]] static constexpr size_type GetLength(
      const Deque& deque, size_type chunk, size_type entries_per_chunk) {
    return GetLength(
        deque, chunk, entries_per_chunk, num_layers(entries_per_chunk));
  }

  /// Returns a view of the visible data length of the chunk at the given layer
  [[nodiscard]] static constexpr ByteSpan GetView(const Deque& deque,
                                                  size_type chunk,
                                                  size_type entries_per_chunk,
                                                  size_type layer) {
    std::byte* data = GetData(deque, chunk, entries_per_chunk);
    size_type offset = GetOffset(deque, chunk, entries_per_chunk, layer);
    size_type length = GetLength(deque, chunk, entries_per_chunk, layer);
    return ByteSpan(data + offset, length);
  }

  /// Returns a view of the visible data length of the chunk at the top layer
  [[nodiscard]] static constexpr ByteSpan GetView(const Deque& deque,
                                                  size_type chunk,
                                                  size_type entries_per_chunk) {
    return GetView(
        deque, chunk, entries_per_chunk, num_layers(entries_per_chunk));
  }

  /// Describes the entire memory region.
  struct BaseView {
    /// Starting offset within the buffer of the data to present.
    size_type offset : 15;

    /// Indicates this memory is "owned", i.e. it should be deallocated when the
    /// entry goes out of scope.
    size_type owned : 1;

    /// Amount of data from the buffer to present.
    size_type length : 15;

    /// Indicates this memory is "shared", i.e. there may be other references to
    /// it.
    size_type shared : 1;
  };

  /// Subsequent entries describe the view of that data that makes up part of a
  /// MultiBuf "layer".
  struct View {
    /// Starting offset within the buffer of the data to present.
    size_type offset : 15;

    /// Flag that is set when a layer should not be modified or removed. This
    /// can be used by lower levels of a protocol stack to indicate that upper
    /// or application layers should not modify data. This is informational and
    /// bypassable, and so should not be considered a security mechanism.
    size_type sealed : 1;

    /// Amount of data from the buffer to present.
    size_type length : 15;

    /// Flag that is set when adding an entry or consolidating several entries
    /// in a new layer. It is used to determine how many entries represent a
    /// packet or message fragment at a particular protocol layer.
    size_type boundary : 1;
  };

  /// Contents of the entry.
  ///
  /// Entries fit in a single word on 32-bit platforms and larger. Fields are
  /// ordered in such a way to ensure this is true on supported platforms.
  ///
  /// The index of the entry in the deque modulo the number of entries per
  /// chunk determines what information is stored in the entry. For example,
  /// if the index is congruent to `kDataIndex`, a pointer to bytes is stored.
  ///
  /// As a special case, the information stored in entries with indices
  /// congruent to `kMemoryContextIndex` also depends on the corresponding
  /// entries congruent to `kBaseViewIndex`. If the latter has the `owned` flag
  /// set, a pointer to a deallocator is stored. If it has the `shared` flag
  /// set, a pointer to a control block is stored.
  union {
    /// Optional deallocator involved in freeing owned memory.
    Deallocator* deallocator;

    /// Optional control block involved in freeing shared memory.
    allocator::internal::ControlBlock* control_block;

    /// Pointer to memory.
    std::byte* data;

    BaseView base_view;

    View view;
  };

  // All fields should fit into a pointer's width.
  static_assert(sizeof(BaseView) <= sizeof(void*));
  static_assert(sizeof(View) <= sizeof(void*));
};

}  // namespace pw::multibuf::v2::internal
