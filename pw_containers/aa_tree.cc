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

#include "pw_containers/internal/aa_tree.h"

#include <cstddef>
#include <utility>

#include "pw_containers/internal/aa_tree_item.h"

namespace pw::containers::internal {

void GenericAATree::SetRoot(AATreeItem* item) {
  if (item != nullptr) {
    item->parent_.set(nullptr);
  }
  root_ = item;
}

size_t GenericAATree::size() const {
  return empty() ? 0 : root_->GetTreeSize();
}

void GenericAATree::clear() {
  if (root_ != nullptr) {
    root_->Clear();
    SetRoot(nullptr);
  }
}

GenericAATree::iterator GenericAATree::erase_one(AATreeItem& item) {
  if (item.GetRoot() != root_) {
    return iterator(&root_);
  }
  AATreeItem* next = item.GetSuccessor();
  SetRoot(item.Unmap());
  return iterator(&root_, next);
}

GenericAATree::iterator GenericAATree::erase_range(AATreeItem& first,
                                                   AATreeItem& last) {
  iterator start(&root_, &first);
  iterator end(&root_, &last);
  return erase_range(start, end);
}

GenericAATree::iterator GenericAATree::erase_range(iterator first,
                                                   iterator last) {
  while (first != last) {
    first = erase_one(*first);
  }
  return first;
}

void GenericAATree::swap(GenericAATree& other) {
  std::swap(root_, other.root_);
}

}  // namespace pw::containers::internal
