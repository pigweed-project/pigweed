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

#include "pw_allocator/bump_allocator.h"
#include "pw_containers/dynamic_deque.h"
#include "pw_multibuf/v2/chunks.h"
#include "pw_multibuf/v2/internal/byte_iterator.h"
#include "pw_multibuf/v2/internal/chunk_iterator.h"
#include "pw_multibuf/v2/internal/entry.h"
#include "pw_unit_test/framework.h"

namespace pw::multibuf::v2::test {

/// A test fixture that manually constructs a multibuf sequence of entries.
///
/// The created sequence represents 4 chunks, with three layers applied to them,
/// i.e.
///
///          buffer 0:     buffer 1:     buffer 2:   buffer 3:
/// layer 3: [0x3]={4, 8}  [0x7]={0, 0}  [0xB]={8, 8}  [0xF]={0,16}
/// layer 2: [0x2]={2,12}  [0x6]={0, 8}  [0xA]={4,12}  [0xE]={0,16}
/// layer 1: [0x1]={0,16}  [0x5]={0,16}  [0x9]={0,16}  [0xD]={0,16}
/// layer 0: [0x0].data    [0x4].data    [0x8].data    [0xC].data
///
/// The visible byte spans can be retrieved using `GetData` and `GetSize` with
/// indices in the range of [0, 4).
///
/// @tparam   kIsConst    Indicates whether this fixture is to be used to test
///                       iterators or const_iterators.
class IteratorTest : public ::testing::Test {
 protected:
  using ChunksType = internal::Chunks<DynamicDeque<internal::Entry>>;
  using RawChunksType = internal::RawChunks<DynamicDeque<internal::Entry>>;
  using Entry = internal::Entry;

  constexpr static uint16_t kNumLayers = 3;
  constexpr static uint16_t kNumChunks = 4;
  constexpr static uint16_t kEntriesPerChunk =
      Entry::kMinEntriesPerChunk - 1 + kNumLayers;
  constexpr static uint16_t kBufSize = 16;

  IteratorTest() : deque_mem_(), allocator_(deque_mem_), deque_(allocator_) {
    uint16_t num_entries = kNumChunks * kEntriesPerChunk;
    deque_.reserve(num_entries);

    for (uint16_t chunk = 0; chunk < kNumChunks; ++chunk) {
      for (uint16_t i = 0; i < kEntriesPerChunk; ++i) {
        internal::Entry entry;
        if (i == Entry::kDataIndex) {
          entry.data = &buffer_[chunk * kBufSize];
          for (size_t j = 0; j < kBufSize; ++j) {
            entry.data[j] = std::byte(j);
          }
        } else if (i == Entry::kBaseViewIndex) {
          auto [offset, length] = kViews[0][chunk];
          entry.base_view = {
              .offset = offset,
              .owned = false,
              .length = length,
              .shared = false,
          };
        } else if (i > Entry::kBaseViewIndex) {
          auto [offset, length] = kViews[i - Entry::kBaseViewIndex][chunk];
          entry.view = {
              .offset = offset,
              .sealed = false,
              .length = length,
              .boundary = true,
          };
        }
        deque_.push_back(entry);
      }
    }
    chunks_ = ChunksType(deque_, kEntriesPerChunk);
    raw_chunks_ = RawChunksType(deque_, kEntriesPerChunk);
  }

  ChunksType& chunks() { return chunks_; }
  RawChunksType& raw_chunks() { return raw_chunks_; }

  // Fragment 0 is non-empty.
  // Fragment 1 is empty.
  // Fragments 2 and 3 are contiguous.
  constexpr static uint16_t kNumContiguous = 2;
  ByteSpan GetContiguous(size_t index) {
    switch (index) {
      case 0:
        return ByteSpan(data(0), size(0));
      case 1:
        return ByteSpan(data(2), size(2) + size(3));
      default:
        return ByteSpan();
    }
  }

  constexpr static uint16_t kNumRaw = 4;
  ByteSpan GetRaw(size_t index) {
    switch (index) {
      case 0:
        return {data(0), size(0)};
      case 1:
        return {data(1), size(1)};
      case 2:
        return {data(2), size(2)};
      case 3:
        return {data(3), size(3)};
      default:
        return {};
    }
  }

  std::pair<
      internal::ByteIterator<uint16_t, internal::ChunkMutability::kMutable>,
      internal::ByteIterator<uint16_t, internal::ChunkMutability::kMutable>>
  GetByteIterators() {
    return {{chunks_.begin(), 0}, {chunks_.end(), 0}};
  }

 private:
  static constexpr std::pair<uint16_t, uint16_t>
      kViews[kNumLayers][kNumChunks] = {
          {{0, 16}, {0, 16}, {0, 16}, {0, 16}},  // layer 1
          {{2, 12}, {0, 8}, {4, 12}, {0, 16}},   // layer 2
          {{4, 8}, {0, 0}, {8, 8}, {0, 16}},     // layer 3
      };

  std::byte* data(size_t chunk) {
    return &buffer_[chunk * kBufSize] + kViews[kNumLayers - 1][chunk].first;
  }

  constexpr uint16_t size(size_t chunk) {
    return kViews[kNumLayers - 1][chunk].second;
  }

  std::array<std::byte, kNumChunks * kBufSize> buffer_;

  // Create a minimally sized allocator for the deque.
  std::array<std::byte, kNumChunks * kEntriesPerChunk * sizeof(internal::Entry)>
      deque_mem_;
  allocator::BumpAllocator allocator_;
  DynamicDeque<internal::Entry> deque_;

  ChunksType chunks_;
  RawChunksType raw_chunks_;
};

}  // namespace pw::multibuf::v2::test
