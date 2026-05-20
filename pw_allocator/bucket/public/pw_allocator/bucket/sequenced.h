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

#include <algorithm>
#include <cstddef>
#include <iterator>

#include "pw_allocator/bucket/base.h"
#include "pw_containers/intrusive_list.h"

namespace pw::allocator {

/// @submodule{pw_allocator,bucket}

/// Intrusive item type corresponding to a `SequencedBucket`.
///
/// When free blocks are added to a bucket, their usable space is used to store
/// an intrusive item that can be added to the bucket's intrusive container.
///
/// This particular item is derived from pw_container's doubly linked list item,
/// which allows it to be easily inserted and removed from a "sequence".
class SequencedItem
    : public containers::future::IntrusiveList<SequencedItem>::Item {};

/// Container of a sequence of free blocks.
///
/// The container used to hold the blocks is a doubly-linked list. The list is
/// sorted on the memory address of the blocks themselves. Insertion is O(n),
/// while removal is O(1). This bucket type is useful when the order of blocks
/// must be preserved.
template <typename BlockType>
class SequencedBucket final
    : public internal::
          BucketBase<SequencedBucket<BlockType>, BlockType, SequencedItem> {
 private:
  using Base = internal::
      BucketBase<SequencedBucket<BlockType>, BlockType, SequencedItem>;
  friend Base;

 public:
  ~SequencedBucket();

  constexpr size_t threshold() const { return threshold_; }

  /// Sets the threshold for which blocks are considered "large".
  ///
  /// This threshold can improve performance when blocks are partitioned based
  /// on size. Iterating over the free blocks to add or remove a block will
  /// start at the beginning for block with an inner size considered "large",
  /// and the end for blocks with an inner size considered "small".
  void set_threshold(size_t threshold) { threshold_ = threshold; }

 private:
  /// @copydoc internal::BucketBase::Add
  void DoAdd(BlockType& block);

  /// @copydoc internal::BucketBase::FindLargest
  const BlockType* DoFindLargest() const;

  /// @copydoc internal::BucketBase::RemoveAny
  BlockType* DoRemoveAny();

  /// @copydoc internal::BucketBase::Remove
  bool DoRemove(BlockType& block);

  /// @copydoc internal::BucketBase::RemoveCompatible
  BlockType* DoRemoveCompatible(Layout layout);

  containers::future::IntrusiveList<SequencedItem> items_;
  size_t threshold_ = 0;
};

/// @}

// Template method implementations.

template <typename BlockType>
SequencedBucket<BlockType>::~SequencedBucket() {
  Base::Clear();
}

template <typename BlockType>
void SequencedBucket<BlockType>::DoAdd(BlockType& block) {
  auto* item_to_add = new (block.UsableSpace()) SequencedItem();
  containers::future::IntrusiveList<SequencedItem>::iterator iter;
  if (block.InnerSize() < threshold_) {
    // Search from back.
    auto r_iter = std::find_if(
        items_.rbegin(), items_.rend(), [item_to_add](SequencedItem& item) {
          return &item < item_to_add;
        });

    // If r_iter dereferences to the last address that is before than the
    // item to add, then the corresponding forward iterator points to the
    // first address that is after the item to add.
    iter = r_iter.base();

  } else {
    // Search from front.
    iter = std::find_if(
        items_.begin(), items_.end(), [item_to_add](SequencedItem& item) {
          return item_to_add < &item;
        });
  }
  items_.insert(iter, *item_to_add);
}

template <typename BlockType>
const BlockType* SequencedBucket<BlockType>::DoFindLargest() const {
  auto iter = std::max_element(items_.begin(), items_.end(), Base::Compare);
  return BlockType::FromUsableSpace(&(*iter));
}

template <typename BlockType>
BlockType* SequencedBucket<BlockType>::DoRemoveAny() {
  SequencedItem& item = items_.front();
  items_.pop_front();
  return BlockType::FromUsableSpace(&item);
}

template <typename BlockType>
bool SequencedBucket<BlockType>::DoRemove(BlockType& block) {
  SequencedItem& item_to_remove = Base::GetItemFrom(block);
  if (block.InnerSize() >= threshold_) {
    // Search from front and remove.
    return items_.remove(item_to_remove);
  }
  // Search from back and remove.
  auto iter = std::find_if(
      items_.rbegin(), items_.rend(), [&item_to_remove](SequencedItem& item) {
        return &item_to_remove == &item;
      });
  if (iter == items_.rend()) {
    return false;
  }
  items_.erase(*iter);
  return true;
}

template <typename BlockType>
BlockType* SequencedBucket<BlockType>::DoRemoveCompatible(Layout layout) {
  auto predicate = Base::MakeCanAllocPredicate(layout);
  SequencedItem* item = nullptr;
  if (layout.size() < threshold_) {
    // Search from back.
    auto iter = std::find_if(items_.rbegin(), items_.rend(), predicate);
    item = iter != items_.rend() ? &(*iter) : nullptr;
  } else {
    // Search from front.
    auto iter = std::find_if(items_.begin(), items_.end(), predicate);
    item = iter != items_.end() ? &(*iter) : nullptr;
  }
  if (item == nullptr) {
    return nullptr;
  }
  auto* block = BlockType::FromUsableSpace(item);
  items_.erase(*item);
  return block;
}

}  // namespace pw::allocator
