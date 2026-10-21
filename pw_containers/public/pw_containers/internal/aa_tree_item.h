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
#include <cstdint>

#include "pw_assert/assert.h"
#include "pw_bytes/packed_ptr.h"

namespace pw::containers::internal {

/// Base type for items stored in an AA tree, as described by Arne Andersson in
/// https://user.it.uu.se/~arneande/ps/simp.pdf. AA trees are simplified
/// red-black which offer almost as much performance with much simpler and
/// smaller code.
///
/// The major difference between the nodes described by Andersson and this
/// implementation is the addition of a back-reference from an item to its
/// parent, allowing additional methods that operate on ranges of nodes. This
/// allows methods that more closely match the interfaces of `std::map` and
/// `std::multimap`.
class AATreeItem {
 public:
  /// Destructor. An item cannot be part of a tree when it is destroyed.
  ~AATreeItem() { PW_ASSERT(!IsMapped()); }

  // AATreeItems are not copyable.
  AATreeItem(const AATreeItem&) = delete;
  AATreeItem& operator=(const AATreeItem&) = delete;

 protected:
  constexpr explicit AATreeItem() = default;

 private:
  // Allow the tree and its iterators to walk and  maniplate the node-related
  // fields of items.
  friend class GenericAATree;

  template <typename>
  friend class KeyedAATree;

  template <typename>
  friend class AATreeIterator;

  /// Gets the level of an item, i.e. its depth in the tree.
  uint8_t GetLevel() const;

  /// Sets the level of an item, i.e. its depth in the tree.
  void SetLevel(uint8_t level);

  /// Returns whether this node is part of any tree.
  [[nodiscard]] bool IsMapped() const;

  /// Returns the number of items in the subtree rooted by this item,
  /// including this one.
  size_t GetTreeSize();

  /// Returns the item at the root of the overall tree.
  AATreeItem* GetRoot();

  /// Returns the item in this item's subtree that is furthest to the left, or
  /// null if this item is a leaf node.
  AATreeItem* GetLeftmost();

  /// Returns the item in this item's subtree that is furthest to the right,
  /// or null if this item is a leaf node.
  AATreeItem* GetRightmost();

  /// Returns the rightmost item to the left of this item in its overall tree,
  /// or null if this item is the tree's leftmost item.
  AATreeItem* GetPredecessor();

  /// Returns the leftmost item to the right of this item in its overall tree,
  /// or null if this item is the tree's leftmost item.
  AATreeItem* GetSuccessor();

  /// Removes this item from its overall tree.
  ///
  /// This method creates a balanced subtree with the same items as this
  /// item's subtree, except for the item itself.
  ///
  /// Calling this method on an item that is not part of a tree has no effect.
  ///
  /// @returns  An item with a balanced subtree.
  AATreeItem* Unmap();

  /// Sets the left child of this item, and sets that child's parent to this
  /// item.
  void SetLeft(AATreeItem* left);

  /// Sets the right child of this item, and sets that child's parent to this
  /// item.
  void SetRight(AATreeItem* right);

  /// Sets either the left or right child of this node to `new_child` if it
  /// is currently `old_child`.
  void Replace(AATreeItem* old_child, AATreeItem* new_child);

  /// Performs a right rotation on this item's subtree, if necessary. This
  /// removes horizontal left links, where a horizontal link is between two
  /// items of the same level.
  ///
  /// @returns A subtree with left horizontal links removed.
  AATreeItem* Skew();

  /// Performs a left rotation on this item's subtree, if necessary. This
  /// removes consectuive horizontal right links, where a horizontal link is
  /// between two items of the same level.
  ///
  /// @returns A subtree with consecutive right horizontal links removed.
  AATreeItem* Split();

  /// Walks up the overall tree from this node, skewing and splitting as
  /// needed to produce a balanced AA tree.
  ///
  /// @returns An item representing the root of a balanced AA tree.
  AATreeItem* Rebalance();

  /// Removes the current item and all the items in its subtree from its
  /// overall tree.
  void Clear();

  /// Restores all the fields of this item except its key to their default
  /// values. Has no effect on any other item.
  void Reset();

  PackedPtr<AATreeItem> parent_;
  mutable PackedPtr<AATreeItem> left_;
  mutable PackedPtr<AATreeItem> right_;
};

}  // namespace pw::containers::internal
