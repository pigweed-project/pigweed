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

#include "pw_containers/internal/aa_tree_item.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "pw_bytes/packed_ptr.h"

namespace pw::containers::internal {

uint8_t AATreeItem::GetLevel() const {
  return static_cast<uint8_t>(parent_.packed_value() |
                              (left_.packed_value() << 2) |
                              (right_.packed_value() << 4));
}

void AATreeItem::SetLevel(uint8_t level) {
  parent_.set_packed_value(level & 3);
  left_.set_packed_value((level >> 2) & 3);
  right_.set_packed_value((level >> 4) & 3);
}

bool AATreeItem::IsMapped() const {
  return GetLevel() != 0 || parent_.get() != nullptr ||
         left_.get() != nullptr || right_.get() != nullptr;
}

size_t AATreeItem::GetTreeSize() {
  return 1 + (left_.get() == nullptr ? 0 : left_->GetTreeSize()) +
         (right_.get() == nullptr ? 0 : right_->GetTreeSize());
}

AATreeItem* AATreeItem::GetRoot() {
  AATreeItem* node = this;
  while (node->parent_.get() != nullptr) {
    node = node->parent_.get();
  }
  return node;
}

AATreeItem* AATreeItem::GetLeftmost() {
  return left_.get() == nullptr ? this : left_->GetLeftmost();
}

AATreeItem* AATreeItem::GetRightmost() {
  return right_.get() == nullptr ? this : right_->GetRightmost();
}

AATreeItem* AATreeItem::GetPredecessor() {
  if (left_.get() != nullptr) {
    return left_->GetRightmost();
  }
  const AATreeItem* current = this;
  AATreeItem* ancestor = parent_.get();
  while (ancestor != nullptr && ancestor->left_.get() == current) {
    current = ancestor;
    ancestor = ancestor->parent_.get();
  }
  return ancestor;
}

AATreeItem* AATreeItem::GetSuccessor() {
  if (right_.get() != nullptr) {
    return right_->GetLeftmost();
  }
  const AATreeItem* current = this;
  AATreeItem* ancestor = parent_.get();
  while (ancestor != nullptr && ancestor->right_.get() == current) {
    current = ancestor;
    ancestor = ancestor->parent_.get();
  }
  return ancestor;
}

AATreeItem* AATreeItem::Unmap() {
  AATreeItem* node;
  if (left_.get() == nullptr && right_.get() == nullptr) {
    // Leaf node.
    node = nullptr;

  } else if (left_.get() == nullptr) {
    // Replace the node with the next one in value.
    node = GetSuccessor();
    right_->parent_.set(nullptr);
    node->SetRight(node->Unmap());

  } else {
    // Replace the node with the previous one in value.
    node = GetPredecessor();
    left_->parent_.set(nullptr);
    node->SetLeft(node->Unmap());
    node->SetRight(right_.get());
  }
  if (parent_.get() != nullptr) {
    parent_->Replace(this, node);
  } else if (node == nullptr) {
    // Removing the only node from the tree.
    Reset();
    return nullptr;
  }
  node = node == nullptr ? GetRoot() : node->Rebalance();
  Reset();
  return node;
}

void AATreeItem::SetLeft(AATreeItem* left) {
  if (left != nullptr) {
    left->parent_.set(this);
  }
  left_.set(left);
}

void AATreeItem::SetRight(AATreeItem* right) {
  if (right != nullptr) {
    right->parent_.set(this);
  }
  right_.set(right);
}

void AATreeItem::Replace(AATreeItem* old_child, AATreeItem* new_child) {
  if (left_.get() == old_child) {
    SetLeft(new_child);
  } else if (right_.get() == old_child) {
    SetRight(new_child);
  }
}

AATreeItem* AATreeItem::Skew() {
  if (left_.get() == nullptr || GetLevel() != left_->GetLevel()) {
    return this;
  }
  AATreeItem* skewed = left_.get();
  SetLeft(skewed->right_.get());
  skewed->parent_.set(parent_.get());
  skewed->SetRight(this);
  return skewed;
}

AATreeItem* AATreeItem::Split() {
  if (right_.get() == nullptr || right_->right_.get() == nullptr ||
      GetLevel() != right_->right_->GetLevel()) {
    return this;
  }
  AATreeItem* split = right_.get();
  SetRight(split->left_.get());
  split->parent_.set(parent_.get());
  split->SetLeft(this);
  split->SetLevel(split->GetLevel() + 1);
  return split;
}

AATreeItem* AATreeItem::Rebalance() {
  AATreeItem* node = this;
  while (true) {
    uint8_t left_level =
        node->left_.get() == nullptr ? 0 : node->left_->GetLevel();
    uint8_t right_level =
        node->right_.get() == nullptr ? 0 : node->right_->GetLevel();
    uint8_t new_level = std::min(left_level, right_level) + 1;
    if (new_level < node->GetLevel()) {
      node->SetLevel(new_level);
      if (new_level < right_level) {
        node->right_->SetLevel(new_level);
      }
    }
    AATreeItem* parent = node->parent_.get();
    AATreeItem* orig = node;
    node = node->Skew();
    if (node->right_.get() != nullptr) {
      node->SetRight(node->right_->Skew());
      if (node->right_->right_.get() != nullptr) {
        node->right_->SetRight(node->right_->right_->Skew());
      }
    }
    node = node->Split();
    if (node->right_.get() != nullptr) {
      node->SetRight(node->right_->Split());
    }
    if (parent == nullptr) {
      return node;
    }
    parent->Replace(orig, node);
    node = parent;
  }
}

void AATreeItem::Clear() {
  if (parent_.get() != nullptr) {
    parent_->Replace(this, nullptr);
  }
  if (left_.get() != nullptr) {
    left_->Clear();
  }
  if (right_.get() != nullptr) {
    right_->Clear();
  }
  Reset();
}

void AATreeItem::Reset() {
  parent_ = PackedPtr<AATreeItem>();
  left_ = PackedPtr<AATreeItem>();
  right_ = PackedPtr<AATreeItem>();
}

}  // namespace pw::containers::internal
