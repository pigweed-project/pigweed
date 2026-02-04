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

namespace pw::multibuf::v2::internal {

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
union Entry {
  /// Entries fit in a single word on 32-bit platforms and larger. Fields are
  /// ordered in such a way to ensure this is true on supported platforms.
  using size_type = uint16_t;

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

  /// Returns the index to the data entry of a given chunk.
  static constexpr size_type memory_context_index(size_type chunk,
                                                  size_type entries_per_chunk) {
    return chunk * entries_per_chunk + kMemoryContextIndex;
  }

  /// Returns the index to the data entry of a given chunk.
  static constexpr size_type data_index(size_type chunk,
                                        size_type entries_per_chunk) {
    return chunk * entries_per_chunk + kDataIndex;
  }

  /// Returns the index to the base view entry of a given chunk.
  static constexpr size_type base_view_index(size_type chunk,
                                             size_type entries_per_chunk) {
    return chunk * entries_per_chunk + kBaseViewIndex;
  }

  /// Returns the index to a view entry of a given chunk.
  static constexpr size_type view_index(size_type chunk,
                                        size_type entries_per_chunk,
                                        size_type layer) {
    return chunk * entries_per_chunk + kBaseViewIndex + layer - 1;
  }

  /// Returns the index to the top view entry of a given chunk.
  static constexpr size_type top_view_index(size_type chunk,
                                            size_type entries_per_chunk) {
    return (chunk + 1) * entries_per_chunk - 1;
  }

  /// Optional deallocator involved in freeing owned memory.
  Deallocator* deallocator;

  /// Optional control block involved in freeing shared memory.
  allocator::internal::ControlBlock* control_block;

  /// Pointer to memory.
  std::byte* data;

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
  } base_view;

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
  } view;
};

}  // namespace pw::multibuf::v2::internal
