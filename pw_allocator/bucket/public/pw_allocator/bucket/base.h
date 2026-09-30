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

#include "pw_allocator/block/poisonable.h"
#include "pw_allocator/config.h"
#include "pw_allocator/hardening.h"
#include "pw_allocator/layout.h"
#include "pw_assert/assert.h"
#include "pw_bytes/alignment.h"

namespace pw::allocator::internal {

/// @submodule{pw_allocator,bucket}

/// A container of free blocks.
///
/// Allocators can use buckets to manage their free blocks. This may include
/// using buckets to sort free blocks based on their size, partitioning them
/// into several buckets, etc., for faster searching when allocating. Each
/// bucket may have a maximum block inner size set which indicates the largest
/// free block allowable in the bucket, or be unbounded.
///
/// This class is a CRTP-style base class. The specific derived class includes
/// an intrusive container type from `pw_containers`. The usable space of free
/// blocks added to the bucket is used to store the intrusive item corresponding
/// to the container.
///
/// This implies that a block must be large enough to hold the item type in
/// order to be added to a bucket. Since intrusive items can be part of at most
/// one container at any point in time, free blocks can be in at most ONE
/// bucket at any time. However, a sufficiently large block may be sequentially
/// added to more than one *type* of bucket. This can be useful for allocators
/// that may track blocks in more than one way, e.g. an allocator that caches
/// recently freed blocks.
///
/// @tparam   Derived     Bucket type derived from this base class.
/// @tparam   BlockType   Type of free blocks that can be  held in this bucket.
/// @tparam   ItemType    Intrusive `pw_containers` type. The usable space of
///                       each free block in the bucket will hold an item of
///                       this type.
template <typename Derived, typename BlockType_, typename ItemType_>
class BucketBase {
 public:
  using BlockType = BlockType_;
  using ItemType = ItemType_;

  constexpr BucketBase();

  /// Returns whether this buckets contains any free blocks.
  [[nodiscard]] constexpr bool empty() const {
    return derived()->items_.empty();
  }

  /// Returns the configured maximum inner size for blocks in this bucket.
  [[nodiscard]] constexpr size_t max_inner_size() const {
    return max_inner_size_;
  }

  /// Sets the maximum inner size for blocks in this bucket.
  ///
  /// This can only be called when the bucket is empty.
  constexpr void set_max_inner_size(size_t max_inner_size);

  /// Adds a block to this bucket if the block can hold an item of the bucket's
  /// `ItemType`, otherwise does nothing.
  ///
  /// @param  block   The block to be added.
  [[nodiscard]] bool Add(BlockType& block);

  /// Returns the block of the largest inner size in the bucket, or null if the
  /// bucket is empty.
  [[nodiscard]] const BlockType* FindLargest() const {
    return empty() ? nullptr : derived()->DoFindLargest();
  }

  /// Removes and returns a block if the bucket is not empty; otherwise returns
  /// null. Exactly which block is returned depends on the specific bucket
  /// implementation.
  [[nodiscard]] BlockType* RemoveAny() {
    return empty() ? nullptr : derived()->DoRemoveAny();
  }

  /// If the given `block` is in this bucket, removes it and returns true;
  /// otherwise, returns false.
  ///
  /// @param  block   The block to be removed.
  [[nodiscard]] bool Remove(BlockType& block) {
    return block.InnerSize() >= sizeof(ItemType) && derived()->DoRemove(block);
  }

  /// Removes and returns a block that can be allocated with the given `layout`,
  /// or null if no such block is present in the bucket.
  ///
  /// Bucket implementations must only return a block if
  /// `block->CanAlloc(layout)` would succeed.
  ///
  /// @param  layout  Allocatable layout of the block to be removed.
  [[nodiscard]] BlockType* RemoveCompatible(Layout layout) {
    return derived()->DoRemoveCompatible(layout);
  }

  /// Removes all blocks from this bucket.
  void Clear() { derived()->items_.clear(); }

 protected:
  /// Returns an iterator the first element in a range whose successor satisfies
  /// a given `predicate`.
  ///
  /// The returned iterator will be in the range [`before_first`, `last`),
  /// and will be the element **before** the first element that satisfies the
  /// predicate.
  ///
  /// This is intended to act similar to `std::find_if` in order to return
  /// iterators that can be used with sorted forward lists like
  /// `std::forward_list<T>` or `pw::IntrusiveForwardList<T>. In particular,
  /// methods like `insert_after` and `erase_after` need the iterator that
  /// precedes a desired item.
  template <typename Iterator, typename Predicate>
  static Iterator FindPrevIf(Iterator before_first,
                             Iterator last,
                             Predicate predicate);

  /// Returns a lambda that tests if the blocks storing an item can be allocated
  /// with the given `layout`.
  ///
  /// This lambda can be used with `std::find_if` and `FindPrevIf`.
  static auto MakeCanAllocPredicate(Layout layout);

  /// Returns whether the first item represents a block with a smaller inner
  /// size than the block represented by the second item.
  static bool Compare(const ItemType& item1, const ItemType& item2);

  /// Returns the block storing the item pointed to by the provided `iter`, or
  /// null if the iterator is the `last` iterator.
  template <typename Iterator>
  constexpr static BlockType* GetBlockFromIterator(Iterator iter,
                                                   Iterator last) {
    return iter != last ? BlockType::FromUsableSpace(&(*iter)) : nullptr;
  }

  /// Returns the block storing the item after the provided `prev` iterator, or
  /// null if the iterator points to the element before the `last` iterator.
  template <typename Iterator>
  constexpr static BlockType* GetBlockFromPrev(Iterator prev, Iterator last) {
    return GetBlockFromIterator(++prev, last);
  }

  /// Returns an existing item stored in a free block's usable space.
  ///
  /// The item is created when adding a block to a bucket, that is, in the
  /// derived type's implementation of `DoAdd`.
  ///
  /// @pre the block must have been added to a bucket of type `Derived`.
  static ItemType& GetItemFrom(BlockType& block) {
    return *(std::launder(reinterpret_cast<ItemType*>(block.UsableSpace())));
  }

 private:
  constexpr Derived* derived() { return static_cast<Derived*>(this); }
  constexpr const Derived* derived() const {
    return static_cast<const Derived*>(this);
  }

  /// The maximum inner size of blocks in this bucket.
  size_t max_inner_size_ = std::numeric_limits<size_t>::max();
};

/// @}

// Template method implementations.

template <typename Derived, typename BlockType, typename ItemType>
constexpr BucketBase<Derived, BlockType, ItemType>::BucketBase() {
  if constexpr (is_poisonable_v<BlockType>) {
    static_assert(BlockType::kPoisonOffset >= sizeof(ItemType),
                  "Block type does not reserve sufficient space for an item");
  }
}

template <typename Derived, typename BlockType, typename ItemType>
constexpr void BucketBase<Derived, BlockType, ItemType>::set_max_inner_size(
    size_t max_inner_size) {
  if constexpr (Hardening::kIncludesDebugChecks) {
    PW_ASSERT(empty());
  }
  max_inner_size_ = max_inner_size;
}

template <typename Derived, typename BlockType, typename ItemType>
bool BucketBase<Derived, BlockType, ItemType>::Add(BlockType& block) {
  if (block.InnerSize() < sizeof(ItemType)) {
    return false;
  }
  if constexpr (Hardening::kIncludesDebugChecks) {
    PW_ASSERT(block.InnerSize() <= max_inner_size_);
    PW_ASSERT(IsAlignedAs<ItemType>(block.UsableSpace()));
  }
  derived()->DoAdd(block);
  return true;
}

template <typename Derived, typename BlockType, typename ItemType>
template <typename Iterator, typename Predicate>
Iterator BucketBase<Derived, BlockType, ItemType>::FindPrevIf(
    Iterator before_first, Iterator last, Predicate predicate) {
  Iterator prev = before_first;
  Iterator iter = prev;
  ++iter;
  for (; iter != last; ++iter) {
    if (predicate(*iter)) {
      break;
    }
    prev = iter;
  }
  return prev;
}

template <typename Derived, typename BlockType, typename ItemType>
auto BucketBase<Derived, BlockType, ItemType>::MakeCanAllocPredicate(
    Layout layout) {
  return [layout](ItemType& item) {
    auto* block = BlockType::FromUsableSpace(&item);
    return block->CanAlloc(layout).ok();
  };
}

template <typename Derived, typename BlockType, typename ItemType>
bool BucketBase<Derived, BlockType, ItemType>::Compare(const ItemType& item1,
                                                       const ItemType& item2) {
  const BlockType* block1 = BlockType::FromUsableSpace(&item1);
  const BlockType* block2 = BlockType::FromUsableSpace(&item2);
  return block1->InnerSize() < block2->InnerSize();
}

}  // namespace pw::allocator::internal
