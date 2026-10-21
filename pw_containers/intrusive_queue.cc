// Copyright 2026 The Pigweed Authors
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

#include "pw_containers/internal/intrusive_queue.h"

#include <utility>

#include "pw_containers/internal/intrusive_list_item.h"

namespace pw::containers::internal {

auto GenericIntrusiveQueue::insert_after(Item* pos, Item& item) -> Item* {
  const bool is_tail = (pos == tail_);
  IntrusiveForwardListItem* inserted = list_.insert_after(pos, item);
  if (is_tail) {
    tail_ = inserted;
  }
  return static_cast<Item*>(inserted);
}

auto GenericIntrusiveQueue::erase_after(Item* pos) -> Item* {
  if (pos->next_ == tail_) {
    tail_ = pos;
  }
  return static_cast<Item*>(list_.erase_after(pos));
}

auto GenericIntrusiveQueue::erase_after(Item* first, Item* last) -> Item* {
  if (last == list_.end() && first->next_ != last) {
    tail_ = first;
  }
  return static_cast<Item*>(list_.erase_after(first, last));
}

bool GenericIntrusiveQueue::remove(const Item& item_to_remove) {
  IntrusiveForwardListItem* prev = list_.find_previous(item_to_remove);
  if (prev == nullptr) {
    return false;
  }
  erase_after(static_cast<Item*>(prev));
  return true;
}

void GenericIntrusiveQueue::swap(GenericIntrusiveQueue& other) noexcept {
  // This is an O(1) swap utilizing the known `tail_` elements. Standard
  // `list_.swap` is O(n) for circular singly-linked lists because the pointer
  // from the last item to the head must be updated.
  IntrusiveForwardListItem* const before = list_.before_begin();
  IntrusiveForwardListItem* const other_before = other.list_.before_begin();

  if (empty()) {
    if (other.empty()) {
      return;
    }
    // If this is empty, just move the nodes from other to this.
    before->next_ = other_before->next_;
    other.tail_->next_ = before;
    tail_ = other.tail_;

    other_before->next_ = other_before;
    other.tail_ = other_before;
  } else if (other.empty()) {
    other.swap(*this);
  } else {
    std::swap(before->next_, other_before->next_);
    tail_->next_ = other_before;
    other.tail_->next_ = before;
    std::swap(tail_, other.tail_);
  }
}

}  // namespace pw::containers::internal
