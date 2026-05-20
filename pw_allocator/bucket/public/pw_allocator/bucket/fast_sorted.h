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
#include "pw_containers/intrusive_multimap.h"
#include "pw_function/function.h"

namespace pw::allocator {

/// @submodule{pw_allocator,bucket}

/// Intrusive item type corresponding to a `FastSortedBucket`.
///
/// When free blocks are added to a bucket, their usable space is used to store
/// an intrusive item that can be added to the bucket's intrusive container.
///
/// This particular item is derived from pw_container's `AATreeItem`, which
/// allows O(log(n)) insertion and lookup and is thus "fast".
template <typename BlockType>
class FastSortedItem
    : public IntrusiveMultiMap<size_t, FastSortedItem<BlockType>>::Item {
 public:
  size_t key() const {
    const auto* block = BlockType::FromUsableSpace(this);
    return block->InnerSize();
  }
};

/// Generic type with the same layout as a `FastSortedItem<BlockType>`.
///
/// `FastSortedItem` depends on a `BlockType`, in order to return the block's
/// inner size as a sorting key. `BlockType` definitions like `DetailedBlock`
/// take a `WhenFree` parameter that describes the layout of memoryused to track
/// the block when free. The `WhenFree` parameter then _should_ be
/// `FastSortedItem`, but cannot be due to the circular dependency. Instead,
/// this type provides same layout without depending on `BlockType`, and thus
/// can be used when defining the block.
struct GenericFastSortedItem
    : public IntrusiveMultiMap<size_t, GenericFastSortedItem>::Item {};

/// Container of size-sorted free blocks.
///
/// The container used to hold the blocks is a multimap. Insertion and removal
/// are O(log(n)) operations. However, the multimap nodes require more space
/// than the "compact" items. As such, this bucket type is a good general
/// purpose container for items above a minimum size.
template <typename BlockType>
class FastSortedBucket final
    : public internal::BucketBase<FastSortedBucket<BlockType>,
                                  BlockType,
                                  FastSortedItem<BlockType>> {
 private:
  using Base = internal::BucketBase<FastSortedBucket<BlockType>,
                                    BlockType,
                                    FastSortedItem<BlockType>>;
  friend Base;

  template <typename>
  friend class ReverseFastSortedBucket;

  using Compare = Function<bool(size_t, size_t)>;

 public:
  constexpr FastSortedBucket() = default;
  ~FastSortedBucket();

 private:
  // Constructor used by `ReverseFastSortedBucket`.
  constexpr explicit FastSortedBucket(Compare&& compare)
      : items_(std::move(compare)) {}

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

  template <typename Iterator>
  BlockType* RemoveImpl(Iterator iter, Layout layout);

  IntrusiveMultiMap<size_t, FastSortedItem<BlockType>> items_;
};

/// Like `FastSortedBucket`, but ordered largest to smallest.
///
/// In particular, `Remove()` will return the largest free block.
template <typename BlockType>
class ReverseFastSortedBucket final
    : public internal::BucketBase<ReverseFastSortedBucket<BlockType>,
                                  BlockType,
                                  FastSortedItem<BlockType>> {
 private:
  using Base = internal::BucketBase<ReverseFastSortedBucket<BlockType>,
                                    BlockType,
                                    FastSortedItem<BlockType>>;
  friend Base;

 public:
  constexpr ReverseFastSortedBucket()
      : impl_(std::greater<>()), items_(impl_.items_) {}

 private:
  /// @copydoc internal::BucketBase::Add
  void DoAdd(BlockType& block);

  /// @copydoc internal::BucketBase::FindLargest
  const BlockType* DoFindLargest() const;

  /// @copydoc internal::BucketBase::RemoveAny
  BlockType* DoRemoveAny();

  /// @copydoc internal::BucketBase::Remove
  bool DoRemove(BlockType& block) { return impl_.DoRemove(block); }

  /// @copydoc internal::BucketBase::RemoveCompatible
  BlockType* DoRemoveCompatible(Layout layout) {
    return impl_.RemoveImpl(impl_.items_.begin(), layout);
  }

  FastSortedBucket<BlockType> impl_;
  IntrusiveMultiMap<size_t, FastSortedItem<BlockType>>& items_;
};

/// @}

// Template method implementations.

template <typename BlockType>
FastSortedBucket<BlockType>::~FastSortedBucket() {
  Base::Clear();
}

template <typename BlockType>
void FastSortedBucket<BlockType>::DoAdd(BlockType& block) {
  auto* item = new (block.UsableSpace()) FastSortedItem<BlockType>();
  items_.insert(*item);
}

template <typename BlockType>
const BlockType* FastSortedBucket<BlockType>::DoFindLargest() const {
  auto iter = items_.end();
  --iter;
  return BlockType::FromUsableSpace(&(*iter));
}

template <typename BlockType>
BlockType* FastSortedBucket<BlockType>::DoRemoveAny() {
  auto iter = items_.begin();
  FastSortedItem<BlockType>& item = *iter;
  items_.erase(iter);
  return BlockType::FromUsableSpace(&item);
}

template <typename BlockType>
bool FastSortedBucket<BlockType>::DoRemove(BlockType& block) {
  FastSortedItem<BlockType>& item_to_remove = Base::GetItemFrom(block);
  auto iters = items_.equal_range(block.InnerSize());
  auto iter = std::find_if(iters.first,
                           iters.second,
                           [&item_to_remove](FastSortedItem<BlockType>& item) {
                             return &item_to_remove == &item;
                           });
  if (iter == items_.end()) {
    return false;
  }
  items_.erase(iter);
  return true;
}

template <typename BlockType>
BlockType* FastSortedBucket<BlockType>::DoRemoveCompatible(Layout layout) {
  auto iter = items_.lower_bound(layout.size());
  return RemoveImpl(iter, layout);
}

template <typename BlockType>
template <typename Iterator>
BlockType* FastSortedBucket<BlockType>::RemoveImpl(Iterator iter,
                                                   Layout layout) {
  iter = std::find_if(iter, items_.end(), Base::MakeCanAllocPredicate(layout));
  auto* block = Base::GetBlockFromIterator(iter, items_.end());
  if (block != nullptr) {
    items_.erase(iter);
  }
  return block;
}

template <typename BlockType>
void ReverseFastSortedBucket<BlockType>::DoAdd(BlockType& block) {
  impl_.DoAdd(block);
}

template <typename BlockType>
const BlockType* ReverseFastSortedBucket<BlockType>::DoFindLargest() const {
  auto iter = impl_.items_.begin();
  return BlockType::FromUsableSpace(&(*iter));
}

template <typename BlockType>
BlockType* ReverseFastSortedBucket<BlockType>::DoRemoveAny() {
  auto iter = items_.begin();
  FastSortedItem<BlockType>& item = *iter;
  items_.erase(iter);
  return BlockType::FromUsableSpace(&item);
}

}  // namespace pw::allocator
