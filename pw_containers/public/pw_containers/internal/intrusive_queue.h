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
#pragma once

#include <cstddef>

#include "pw_containers/internal/intrusive_list.h"
#include "pw_containers/internal/intrusive_list_item.h"

namespace pw::containers::internal {

class GenericIntrusiveQueue;

class IntrusiveQueueItem : private IntrusiveForwardListItem {
 protected:
  constexpr IntrusiveQueueItem() = default;

 private:
  friend class GenericIntrusiveQueue;
  friend GenericIntrusiveList<IntrusiveForwardListItem>;
  template <typename, typename>
  friend class Incrementable;
};

class GenericIntrusiveQueue {
 public:
  using Item = IntrusiveQueueItem;
  using size_type = std::size_t;

  constexpr GenericIntrusiveQueue() : list_(), tail_(list_.before_begin()) {}

  GenericIntrusiveQueue(const GenericIntrusiveQueue&) = delete;
  GenericIntrusiveQueue& operator=(const GenericIntrusiveQueue&) = delete;

  GenericIntrusiveQueue(GenericIntrusiveQueue&& other)
      : list_(), tail_(list_.before_begin()) {
    swap(other);
  }

  GenericIntrusiveQueue& operator=(GenericIntrusiveQueue&& other) {
    clear();
    swap(other);
    return *this;
  }

  // Iterators
  Item* before_begin() noexcept {
    return static_cast<Item*>(list_.before_begin());
  }
  const Item* before_begin() const noexcept {
    return static_cast<const Item*>(list_.before_begin());
  }

  Item* begin() noexcept { return static_cast<Item*>(list_.begin()); }
  const Item* begin() const noexcept {
    return static_cast<const Item*>(list_.begin());
  }

  Item* end() noexcept { return static_cast<Item*>(list_.end()); }
  const Item* end() const noexcept {
    return static_cast<const Item*>(list_.end());
  }

  // Capacity
  bool empty() const noexcept { return list_.empty(); }

  // Modifiers

  void clear() {
    list_.clear();
    tail_ = list_.before_begin();
  }

  void push_front(Item& item) { insert_after(before_begin(), item); }

  void pop_front() { erase_after(before_begin()); }

  void push_back(Item& item) { tail_ = list_.insert_after(tail_, item); }

  Item* insert_after(Item* pos, Item& item);

  template <typename Iterator>
  Item* insert_after(Item* const pos, Iterator first, Iterator last) {
    const bool is_tail = (pos == tail_);
    IntrusiveForwardListItem* const prev = list_.insert_after(pos, first, last);
    if (is_tail) {
      tail_ = prev;
    }
    return static_cast<Item*>(prev);
  }

  Item* erase_after(Item* pos);
  Item* erase_after(Item* first, Item* last);

  bool remove(const Item& item_to_remove);

  template <typename DerivedItem, typename UnaryPredicate>
  size_type remove_if(UnaryPredicate pred) {
    size_type removed = 0;
    Item* prev = before_begin();
    while (true) {
      if (prev->next_ == list_.end()) {
        break;
      }
      if (pred(static_cast<DerivedItem&>(*prev->next_))) {
        erase_after(prev);
        removed += 1;
      } else {
        prev = static_cast<Item*>(prev->next_);
      }
    }
    return removed;
  }

  void swap(GenericIntrusiveQueue& other) noexcept;

  template <typename Iterator>
  void assign(Iterator first, Iterator last) {
    clear();
    insert_after(before_begin(), first, last);
  }

  Item* tail() const { return static_cast<Item*>(tail_); }

 private:
  GenericIntrusiveList<IntrusiveForwardListItem> list_;
  IntrusiveForwardListItem* tail_;
};

}  // namespace pw::containers::internal
