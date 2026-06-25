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

#include "pw_allocator/buddy_allocator.h"

#include <cstring>
#include <utility>

#include "lib/stdcompat/bit.h"
#include "pw_allocator/hardening.h"
#include "pw_assert/check.h"
#include "pw_bytes/alignment.h"

namespace pw::allocator::internal {

BuddyBlock::BuddyBlock(size_t outer_size) {
  outer_size_log2_ = CountRZero<size_t, uint8_t>(outer_size);
}

StatusWithSize BuddyBlock::CanAlloc(Layout layout) const {
  return layout.size() > InnerSize() ? StatusWithSize::ResourceExhausted()
                                     : StatusWithSize(0);
}

BuddyBlock* BuddyBlock::Split() {
  outer_size_log2_--;
  std::byte* ptr = UsableSpace() + InnerSize();
  return new (ptr) BuddyBlock(OuterSize());
}

BuddyBlock* BuddyBlock::Merge(BuddyBlock*&& left, BuddyBlock*&& right) {
  if (right < left) {
    return BuddyBlock::Merge(std::move(right), std::move(left));
  }
  left->outer_size_log2_++;
  return left;
}

void GenericBuddyAllocator::InitBuckets(
    span<UnorderedBucket<BuddyBlock>> buckets, size_t min_outer_size) {
  buckets_ = buckets;
  min_outer_size_ = min_outer_size;
  for (auto& bucket : buckets) {
    size_t max_inner_size = BuddyBlock::InnerSizeFromOuterSize(min_outer_size);
    bucket.set_max_inner_size(max_inner_size);
    min_outer_size <<= 1;
  }
}

void GenericBuddyAllocator::Init(ByteSpan region) {
  CrashIfAllocated();

  // Ensure there is a byte preceding the first `BuddyBlock`.
  region = GetAlignedSubspan(region.subspan(1), min_outer_size_);
  region = ByteSpan(region.data() - 1, region.size() + 1);
  if constexpr (Hardening::kIncludesBasicChecks) {
    PW_CHECK_INT_GE(region.size(), min_outer_size_);
  }

  // Build up the available memory by successively freeing (and thus merging)
  // minimum sized blocks.
  std::byte* data = region.data();
  size_t count = 0;
  while (region.size() >= min_outer_size_) {
    new (region.data()) BuddyBlock(min_outer_size_);
    region = region.subspan(min_outer_size_);
    ++count;
  }
  region_ = ByteSpan(data, min_outer_size_ * count);
  data++;
  for (size_t i = 0; i < count; ++i) {
    Deallocate(data);
    data += min_outer_size_;
  }
}

void GenericBuddyAllocator::CrashIfAllocated() {
  size_t total_free = 0;
  // Drain all the buckets before destroying the list. Although O(n), this
  // should be reasonably quick since all memory should have been freed and
  // coalesced prior to calling this method.
  for (auto& bucket : buckets_) {
    while (!bucket.empty()) {
      BuddyBlock* block = bucket.RemoveAny();
      total_free += block->OuterSize();
    }
  }
  if constexpr (Hardening::kIncludesRobustChecks) {
    PW_CHECK_INT_EQ(region_.size(),
                    total_free,
                    "%zu bytes were still in use when an allocator was "
                    "destroyed. All memory allocated by an allocator must be "
                    "released before the allocator goes out of scope.",
                    region_.size() - total_free);
  }
  region_ = ByteSpan();
}

void* GenericBuddyAllocator::DoAllocate(Layout layout) {
  // Reserve one byte to save the bucket index.
  layout = layout.Extend(1);

  if (layout.size() > buckets_.back().max_inner_size()) {
    return nullptr;
  }
  if (layout.alignment() > min_outer_size_) {
    return nullptr;
  }

  for (auto& bucket : buckets_) {
    size_t inner_size = bucket.max_inner_size();
    size_t outer_size = BuddyBlock::OuterSizeFromInnerSize(inner_size);
    if (inner_size < layout.size()) {
      continue;
    }
    layout = Layout(inner_size, layout.alignment());

    // Check if this bucket has a compatible free block available.
    if (auto* block = bucket.RemoveCompatible(layout); block != nullptr) {
      return block->UsableSpace();
    }

    // No compatible free blocks available, allocate one from the next bucket
    // and split it.
    void* ptr = Allocate(layout.Extend(outer_size - 1));
    if (ptr == nullptr) {
      break;
    }

    auto* block = BuddyBlock::FromUsableSpace(ptr);
    BuddyBlock* buddy = block->Split();
    std::ignore = bucket.Add(*buddy);
    return ptr;
  }
  return nullptr;
}

void GenericBuddyAllocator::DoDeallocate(void* ptr) {
  if (ptr == nullptr) {
    return;
  }

  auto* block = BuddyBlock::FromUsableSpace(ptr);
  UnorderedBucket<BuddyBlock>* bucket = nullptr;
  if constexpr (Hardening::kIncludesDebugChecks) {
    PW_CHECK_INT_GT(buckets_.size(), 0);
  }
  for (auto& current : span(buckets_.data(), buckets_.size() - 1)) {
    size_t outer_size =
        BuddyBlock::OuterSizeFromInnerSize(current.max_inner_size());
    if (outer_size < block->OuterSize()) {
      continue;
    }
    bucket = &current;

    // Determine the expected address of this free block's buddy by determining
    // if it would be first or second in a merged block of the next larger size.
    std::byte* item = block->UsableSpace();
    size_t offset = static_cast<size_t>(reinterpret_cast<std::byte*>(block) -
                                        region_.data());
    if (offset % (block->OuterSize() * 2) == 0) {
      item += outer_size;
    } else {
      item -= outer_size;
    }
    // Blocks at the end of the range may not have a buddy.
    if (item < region_.data() || region_.data() + region_.size() < item) {
      break;
    }

    // Look for the buddy block in the previous bucket. If found, remove it from
    // that bucket, and repeat the whole process with the merged block.
    auto* buddy = BuddyBlock::FromUsableSpace(item);
    if (!current.Remove(*buddy)) {
      break;
    }

    block = BuddyBlock::Merge(std::move(block), std::move(buddy));
    bucket = nullptr;
  }
  if (bucket == nullptr) {
    bucket = &buckets_.back();
  }

  // Add the (possibly merged) free block to the correct bucket.
  std::ignore = bucket->Add(*block);
}

/// @copydoc Deallocator::GetInfo
Result<Layout> GenericBuddyAllocator::DoGetInfo(InfoType info_type,
                                                const void* ptr) const {
  switch (info_type) {
    case InfoType::kCapacity:
      return Layout(region_.size(), min_outer_size_);
    case InfoType::kUsableLayoutOf:
    case InfoType::kAllocatedLayoutOf:
    case InfoType::kRecognizes:
      break;
    case InfoType::kRequestedLayoutOf:
    default:
      return Status::Unimplemented();
  }

  if (ptr < region_.data()) {
    return Status::OutOfRange();
  }
  const auto* block = BuddyBlock::FromUsableSpace(ptr);
  size_t block_offset = cpp20::bit_cast<uintptr_t>(block) -
                        cpp20::bit_cast<uintptr_t>(region_.data());
  if (region_.size() <= block_offset || block_offset % min_outer_size_ != 0) {
    return Status::OutOfRange();
  }
  Layout layout(block->InnerSize(), min_outer_size_);

  switch (info_type) {
    case InfoType::kUsableLayoutOf:
      return Layout(layout.size() - 1, layout.alignment());
    case InfoType::kAllocatedLayoutOf:
      return layout;
    case InfoType::kRecognizes:
      return Layout();
    case InfoType::kCapacity:
    case InfoType::kRequestedLayoutOf:
    default:
      return Status::Unimplemented();
  }
}

}  // namespace pw::allocator::internal
