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

#include "pw_allocator/bucket/base.h"
#include "pw_containers/intrusive_forward_list.h"

namespace pw::allocator {

/// @submodule{pw_allocator,bucket}

/// Intrusive item type corresponding to an `UnorderedBucket`.
///
/// When free blocks are added to a bucket, their usable space is used to store
/// an intrusive item that can be added to the bucket's intrusive container.
///
/// This particular item is derived from pw_container's smallest intrusive item
/// type.
class UnorderedItem : public IntrusiveForwardList<UnorderedItem>::Item {};

/// Container of free blocks that use minimal usable space.
///
/// The container used to hold the blocks is a singly-linked list. As a result,
/// it is able to store free blocks as small as `sizeof(void*)`. Insertion and
/// removal of an unspecified block is O(1). Removal internal::of a specific
/// block is O(n) since the whole list may need to be walked to find the block.
/// As such, this bucket type is useful for pools of blocks of a single size.
template <typename BlockType>
class UnorderedBucket final
    : public internal::
          BucketBase<UnorderedBucket<BlockType>, BlockType, UnorderedItem> {
 private:
  using Base = internal::
      BucketBase<UnorderedBucket<BlockType>, BlockType, UnorderedItem>;
  friend Base;

 public:
  ~UnorderedBucket() { Base::Clear(); }

 private:
  /// @copydoc internal::BucketBase::Add
  void DoAdd(BlockType& block);

  /// @copydoc internal::BucketBase::RemoveAny
  BlockType* DoRemoveAny();

  /// @copydoc internal::BucketBase::FindLargest
  const BlockType* DoFindLargest() const;

  /// @copydoc internal::BucketBase::Remove
  bool DoRemove(BlockType& block) {
    return items_.remove(Base::GetItemFrom(block));
  }

  /// @copydoc internal::BucketBase::RemoveCompatible
  BlockType* DoRemoveCompatible(Layout layout);

  IntrusiveForwardList<UnorderedItem> items_;
};

/// @}

// Template method implementations.

template <typename BlockType>
void UnorderedBucket<BlockType>::DoAdd(BlockType& block) {
  auto* item = new (block.UsableSpace()) UnorderedItem();
  items_.push_front(*item);
}

template <typename BlockType>
BlockType* UnorderedBucket<BlockType>::DoRemoveAny() {
  UnorderedItem& item = items_.front();
  items_.pop_front();
  return BlockType::FromUsableSpace(&item);
}

template <typename BlockType>
const BlockType* UnorderedBucket<BlockType>::DoFindLargest() const {
  auto iter = std::max_element(items_.begin(), items_.end(), Base::Compare);
  return BlockType::FromUsableSpace(&(*iter));
}

template <typename BlockType>
BlockType* UnorderedBucket<BlockType>::DoRemoveCompatible(Layout layout) {
  auto prev = Base::FindPrevIf(
      items_.before_begin(), items_.end(), Base::MakeCanAllocPredicate(layout));
  auto* block = Base::GetBlockFromPrev(prev, items_.end());
  if (block != nullptr) {
    items_.erase_after(prev);
  }
  return block;
}

}  // namespace pw::allocator
