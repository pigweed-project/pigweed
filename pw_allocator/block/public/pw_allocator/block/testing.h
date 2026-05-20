// Copyright 2024 The Pigweed Authors
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
#include <limits>

#include "lib/stdcompat/bit.h"
#include "pw_allocator/block/allocatable.h"
#include "pw_allocator/block/result.h"
#include "pw_allocator/layout.h"
#include "pw_assert/assert.h"
#include "pw_bytes/alignment.h"
#include "pw_bytes/span.h"

namespace pw::allocator::test {

/// @submodule{pw_allocator,impl_test}

// The size of the memory region used in tests.
static constexpr size_t kDefaultCapacity = 1024;

// The large alignment used in alignment-related tests.
static constexpr size_t kAlign = 64;

/// Convenience alias for creating block-aligned byte buffers.
template <typename BlockType, size_t kBufferSize = kDefaultCapacity>
struct BlockAlignedBuffer {
  constexpr ByteSpan as_span() { return ByteSpan(bytes_); }

  alignas(BlockType::kAlignment) std::array<std::byte, kBufferSize> bytes_;
};

/// Utility function that returns the offset from an addres a given number of
/// bytes `after` a given `ptr` to the next address that has a given
/// `alignment`.
///
/// In other words, if offset is `GetAlignedOffsetAfter(ptr, alignment, after)`,
/// then `((uintptr_t)ptr + after + offset) % alignment` is 0.
///
/// This is useful when dealing with blocks that need their usable space to be
/// aligned, e.g.
///   GetAlignedOffsetAfter(bytes_.data(), layout.alignment(), kBlockOverhead);
inline size_t GetAlignedOffsetAfter(const void* ptr,
                                    size_t alignment,
                                    size_t after) {
  auto addr = cpp20::bit_cast<uintptr_t>(ptr) + after;
  return AlignUp(addr, alignment) - addr;
}

/// Represents an initial state for a memory block.
///
/// Unit tests can specify an initial block layout by passing a list of these
/// structs to `Preallocate`.
///
/// The outer size of each block must be more than `kBlockOverhead` for the
/// block type in use. The special `kSizeRemaining` may be used for at most
/// one block to give it any space not assigned to other blocks.
///
/// The state must be either `kFree` or `kUsed`.
///
/// Example:
/// @code{.cpp}
///   // BlockType = UnpoisonedBlock<uint32_t>, so kBlockOverhead == 8.
///   BlockType* block1 = util_.Preallocate({
///     {32,              kUsed},
///     {24,              kFree},
///     {48,              kUsed},
///     {kSizeRemaining,  kFree},
///     {64,              kUsed},
///   });
/// @endcode
struct Preallocation {
  /// The outer size of the block to preallocate.
  size_t outer_size;

  // Index into the `test_fixture` array where the pointer to the block's
  // space should be cached.
  enum class State {
    kUsed,
    kFree,
  } state;

  static constexpr State kUsed = State::kUsed;
  static constexpr State kFree = State::kFree;

  /// Special value indicating the block should comprise of the all remaining
  /// space not preallocated to any other block. May be used at most once.
  static constexpr size_t kSizeRemaining = std::numeric_limits<size_t>::max();
};

template <typename BlockType, size_t kBufferSize = kDefaultCapacity>
class BlockTestUtilities {
 public:
  BlockTestUtilities() : buffer_(), bytes_(buffer_.as_span()) {}

  ByteSpan bytes() const { return bytes_; }

  // Some tests below need a block with a nonzero inner size to fit within
  // alignment boundaries.
  static_assert(kAlign > BlockType::kBlockOverhead + BlockType::kAlignment);

  /// Adjusts the bytes of this object to refer to the subspan given by `offset`
  /// and `length`.
  constexpr void TrimBytes(size_t offset, size_t length);

  /// Trims the starting bytes so that the span is block-aligned.
  void TrimAligned(size_t extra = 0);

  /// Returns the minimum outer size for a block allocated from a layout with
  /// the given `min_inner_size`.
  constexpr size_t GetOuterSize(size_t min_inner_size);

  /// Creates a sequence of blocks as described by the given `preallocs` using
  /// the given `bytes`.
  ///
  /// See `Preallocation` for more detail.
  BlockType* Preallocate(std::initializer_list<Preallocation> preallocs);

  /// Returns the smallest offset into the given memory region which can be
  /// preceded by a valid block, and at which a block would have properly
  /// aligned usable space of the given size.
  ///
  /// @pre ``bytes`` must not be smaller than the calculated offset plus
  ///      ``layout.size()``.
  size_t GetFirstAlignedOffset(Layout layout);

 private:
  BlockAlignedBuffer<BlockType, kBufferSize> buffer_;
  ByteSpan bytes_;
};

/// @endsubmodule

//  Template method implementations.

template <typename BlockType, size_t kBufferSize>
constexpr void BlockTestUtilities<BlockType, kBufferSize>::TrimBytes(
    size_t offset, size_t length) {
  bytes_ = bytes_.subspan(offset, length);
}

template <typename BlockType, size_t kBufferSize>
void BlockTestUtilities<BlockType, kBufferSize>::TrimAligned(size_t extra) {
  size_t offset =
      GetAlignedOffsetAfter(bytes_.data(), kAlign, BlockType::kBlockOverhead) +
      extra;
  bytes_ = bytes_.subspan(offset);
}

template <typename BlockType, size_t kBufferSize>
constexpr size_t BlockTestUtilities<BlockType, kBufferSize>::GetOuterSize(
    size_t min_inner_size) {
  return BlockType::kBlockOverhead +
         AlignUp(min_inner_size, BlockType::kAlignment);
}

template <typename BlockType, size_t kBufferSize>
BlockType* BlockTestUtilities<BlockType, kBufferSize>::Preallocate(
    std::initializer_list<Preallocation> preallocs) {
  static_assert(is_allocatable_v<BlockType>);

  // First, look if any blocks use kSizeRemaining, and calculate how large
  // that will be.
  auto init_result = BlockType::Init(bytes());
  PW_ASSERT(init_result.ok());
  BlockType* block = *init_result;
  size_t remaining_outer_size = block->OuterSize();
  for (auto& preallocation : preallocs) {
    if (preallocation.outer_size != Preallocation::kSizeRemaining) {
      size_t outer_size =
          AlignUp(preallocation.outer_size, BlockType::kAlignment);
      PW_ASSERT(outer_size > BlockType::kBlockOverhead);
      PW_ASSERT(remaining_outer_size >= outer_size);
      remaining_outer_size -= outer_size;
    }
  }

  // Now, construct objects in place.
  bool next_is_free = false;
  BlockType* next = nullptr;
  for (auto it = std::rbegin(preallocs); it != std::rend(preallocs); ++it) {
    PW_ASSERT(block != nullptr);
    const Preallocation& preallocation = *it;
    size_t outer_size = preallocation.outer_size;
    if (outer_size == Preallocation::kSizeRemaining) {
      outer_size = remaining_outer_size;
      remaining_outer_size = 0;
    } else {
      outer_size = AlignUp(preallocation.outer_size, BlockType::kAlignment);
    }
    Layout layout(outer_size - BlockType::kBlockOverhead, 1);
    auto alloc_result = BlockType::AllocLast(std::move(block), layout);
    PW_ASSERT(alloc_result.ok());

    using Next = internal::GenericBlockResult::Next;
    PW_ASSERT(alloc_result.next() == Next::kUnchanged);

    block = alloc_result.block();

    if (next_is_free) {
      BlockType::Free(std::move(next)).IgnoreUnlessStrict();
    }
    next_is_free = preallocation.state == Preallocation::kFree;
    next = block;
    block = block->Prev();
  }

  // Handle the edge case of the first block being free.
  PW_ASSERT(block == nullptr);
  if (next_is_free) {
    auto free_result = BlockType::Free(std::move(next));
    next = free_result.block();
  }
  return next;
}

template <typename BlockType, size_t kBufferSize>
size_t BlockTestUtilities<BlockType, kBufferSize>::GetFirstAlignedOffset(
    Layout layout) {
  size_t min_block = BlockType::kBlockOverhead + 1;
  size_t offset = GetAlignedOffsetAfter(bytes().data(),
                                        layout.alignment(),
                                        min_block + BlockType::kBlockOverhead);
  return min_block + offset;
}

}  // namespace pw::allocator::test
