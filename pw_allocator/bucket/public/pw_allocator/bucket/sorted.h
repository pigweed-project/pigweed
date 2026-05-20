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

/// Intrusive item type corresponding to a `SortedBucket`.
///
/// When free blocks are added to a bucket, their usable space is used to store
/// an intrusive item that can be added to the bucket's intrusive container.
///
/// This particular item is derived from pw_container's smallest intrusive item
/// type, hence it is the most "compact".
class SortedItem : public IntrusiveForwardList<SortedItem>::Item {};

// Forward declarations.

template <typename BlockType>
class ForwardSortedBucket;

template <typename BlockType>
class ReverseSortedBucket;

namespace internal {

/// Container of size-sorted free blocks.
///
/// The container used to hold the free blocks is a forward list. As a result,
/// it is able to store small free blocks with inner sizes as small as
/// `sizeof(void*)`. However, holding such small blocks in a sorted list
/// requires that insertion and removal are O(n) operations. As such, this
/// bucket type is only useful for bounded lists of free blocks, such as caches.
///
/// This CRTP-style base class requires a derived class to provide a predicate
/// that indicates where each item should be inserted in the list.
template <typename Derived, typename BlockType>
class SortedBucketBase : public BucketBase<Derived, BlockType, SortedItem> {
 public:
  ~SortedBucketBase() { Base::Clear(); }

 protected:
  using Base = BucketBase<Derived, BlockType, SortedItem>;
  friend Base;

  constexpr SortedBucketBase() = default;

  const IntrusiveForwardList<SortedItem>& items() const { return items_; }

  /// @copydoc ::BucketBase::Add
  void DoAdd(BlockType& block);

  /// @copydoc ::BucketBase::RemoveAny
  BlockType* DoRemoveAny();

  /// @copydoc ::BucketBase::Remove
  bool DoRemove(BlockType& block) {
    return items_.remove(Base::GetItemFrom(block));
  }

  /// @copydoc ::BucketBase::RemoveCompatible
  BlockType* DoRemoveCompatible(Layout layout);

 private:
  IntrusiveForwardList<SortedItem> items_;
};

}  // namespace internal

/// Container of free blocks sorted in order of increasing size.
///
/// As noted in the base class, this class relies on a forward list. This allows
/// the free blocks to be as small as a single pointer, but makes insertion and
/// lookup O(n) on the number of blocks in the bucket.
///
/// Calling `RemoveAny()` on this bucket will return the smallest free block.
template <typename BlockType>
class ForwardSortedBucket final
    : public internal::SortedBucketBase<ForwardSortedBucket<BlockType>,
                                        BlockType> {
 private:
  using Base =
      internal::SortedBucketBase<ForwardSortedBucket<BlockType>, BlockType>;

  friend Base;
  // Suppress `no uniquely matching class member found` Doxygen error
  /// @cond
  friend typename Base::Base;
  /// @endcond

  /// Returns a lambda that tests if the block storing an item has an inner size
  /// larger than the given `inner_size`.
  ///
  /// This lambda can be used with `std::find_if` and `FindPrevIf`.
  static constexpr auto MakeAddPredicate(size_t inner_size);

  /// @copydoc ::internal::BucketBase::FindLargest
  const BlockType* DoFindLargest() const;
};

/// Container of free blocks sorted in order of decreasing size.
///
/// As noted in the base class, this class relies on a forward list. This allows
/// the free blocks to be as small as a single pointer, but makes insertion and
/// lookup O(n) on the number of blocks in the bucket.
///
/// Calling `RemoveAny()` on this bucket will return the largest free block.
template <typename BlockType>
class ReverseSortedBucket final
    : public internal::SortedBucketBase<ReverseSortedBucket<BlockType>,
                                        BlockType> {
 private:
  using Base =
      internal::SortedBucketBase<ReverseSortedBucket<BlockType>, BlockType>;

  friend Base;
  // Suppress `no uniquely matching class member found` Doxygen error
  /// @cond
  friend typename Base::Base;
  /// @endcond

  /// Returns a lambda that tests if the block storing an item has an inner size
  /// smaller than the given `inner_size`.
  ///
  /// This lambda can be used with `std::find_if` and `FindPrevIf`.
  static constexpr auto MakeAddPredicate(size_t inner_size);

  /// @copydoc ::internal::BucketBase::FindLargest
  const BlockType* DoFindLargest() const;
};

/// @}

// Template method implementations.

namespace internal {

template <typename Derived, typename BlockType>
void SortedBucketBase<Derived, BlockType>::DoAdd(BlockType& block) {
  auto* item_to_add = new (block.UsableSpace()) SortedItem();
  auto prev = Base::FindPrevIf(items_.before_begin(),
                               items_.end(),
                               Derived::MakeAddPredicate(block.InnerSize()));
  items_.insert_after(prev, *item_to_add);
}

template <typename Derived, typename BlockType>
BlockType* SortedBucketBase<Derived, BlockType>::DoRemoveAny() {
  SortedItem& item = items_.front();
  items_.pop_front();
  return BlockType::FromUsableSpace(&item);
}

template <typename Derived, typename BlockType>
BlockType* SortedBucketBase<Derived, BlockType>::DoRemoveCompatible(
    Layout layout) {
  auto prev = Base::FindPrevIf(
      items_.before_begin(), items_.end(), Base::MakeCanAllocPredicate(layout));
  auto* block = Base::GetBlockFromPrev(prev, items_.end());
  if (block != nullptr) {
    items_.erase_after(prev);
  }
  return block;
}

}  // namespace internal

template <typename BlockType>
constexpr auto ForwardSortedBucket<BlockType>::MakeAddPredicate(
    size_t inner_size) {
  return [inner_size](SortedItem& item) {
    auto* block = BlockType::FromUsableSpace(&item);
    return inner_size < block->InnerSize();
  };
}

template <typename BlockType>
const BlockType* ForwardSortedBucket<BlockType>::DoFindLargest() const {
  const auto& items = Base::items();
  if (items.empty()) {
    return nullptr;
  }
  auto iter = items.before_begin();
  auto prev = iter;
  do {
    prev = iter++;
  } while (iter != items.end());
  return BlockType::FromUsableSpace(&(*prev));
}

template <typename BlockType>
constexpr auto ReverseSortedBucket<BlockType>::MakeAddPredicate(
    size_t inner_size) {
  return [inner_size](SortedItem& item) {
    auto* block = BlockType::FromUsableSpace(&item);
    return block->InnerSize() < inner_size;
  };
}

template <typename BlockType>
const BlockType* ReverseSortedBucket<BlockType>::DoFindLargest() const {
  const auto& items = Base::items();
  auto iter = items.begin();
  return BlockType::FromUsableSpace(&(*iter));
}

}  // namespace pw::allocator
