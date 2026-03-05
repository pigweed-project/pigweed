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

#include <cstdint>
#include <iterator>
#include <type_traits>

#include "pw_containers/internal/aa_tree_item.h"

namespace pw::containers::internal {

/// Iterator that has the ability to advance forwards or backwards over a
/// sequence of items.
///
/// This is roughly equivalent to std::bidirectional_iterator<T>, but for
/// intrusive maps.
template <typename T = AATreeItem>
class AATreeIterator {
 public:
  using difference_type = std::ptrdiff_t;
  using value_type = std::remove_cv_t<T>;
  using pointer = T*;
  using reference = T&;
  using iterator_category = std::bidirectional_iterator_tag;

  constexpr AATreeIterator() = default;
  constexpr AATreeIterator(const AATreeIterator&) = default;
  constexpr AATreeIterator& operator=(const AATreeIterator&) = default;

  template <typename Item>
  constexpr AATreeIterator(const AATreeIterator<Item>& other)
      : root_(other.root_), item_(other.item_) {}

  template <typename Item>
  constexpr AATreeIterator& operator=(const AATreeIterator<Item>& other) {
    root_ = other.root_;
    item_ = other.item_;
    return *this;
  }

  constexpr T& operator*() const { return *(downcast()); }
  constexpr T* operator->() const { return downcast(); }

  template <typename T2,
            typename = std::enable_if_t<
                std::is_same_v<std::remove_cv_t<T>, std::remove_cv_t<T2>>>>
  constexpr bool operator==(const AATreeIterator<T2>& rhs) const {
    return root_ == rhs.root_ && item_ == rhs.item_;
  }

  template <typename T2,
            typename = std::enable_if_t<
                std::is_same_v<std::remove_cv_t<T>, std::remove_cv_t<T2>>>>
  constexpr bool operator!=(const AATreeIterator<T2>& rhs) const {
    return !operator==(rhs);
  }

  constexpr AATreeIterator& operator++() {
    if (item_ != nullptr) {
      item_ = item_->GetSuccessor();
    } else {
      PW_DASSERT(root_ != nullptr);
      PW_DASSERT(*root_ != nullptr);
      item_ = (*root_)->GetLeftmost();
    }
    return *this;
  }

  constexpr AATreeIterator operator++(int) {
    AATreeIterator copy = *this;
    operator++();
    return copy;
  }

  constexpr AATreeIterator& operator--() {
    if (item_ != nullptr) {
      item_ = item_->GetPredecessor();
    } else {
      PW_DASSERT(root_ != nullptr);
      PW_DASSERT(*root_ != nullptr);
      item_ = (*root_)->GetRightmost();
    }
    return *this;
  }

  constexpr AATreeIterator operator--(int) {
    AATreeIterator copy = *this;
    operator--();
    return copy;
  }

 private:
  template <typename>
  friend class AATreeIterator;

  friend class GenericAATree;

  template <typename>
  friend class KeyedAATree;

  // Only the generic and derived AA trees can create iterators that actually
  // point to something. They provides a pointer to its pointer to the root of
  // tree. The root item may change, but the pointer to it remains the same for
  // the life of the tree.
  constexpr explicit AATreeIterator(AATreeItem** root) : root_(root) {}
  constexpr AATreeIterator(AATreeItem** root, AATreeItem* item)
      : root_(root), item_(item) {}

  T* downcast() const { return static_cast<T*>(item_); }

  AATreeItem** root_ = nullptr;
  mutable AATreeItem* item_ = nullptr;
};

}  // namespace pw::containers::internal
