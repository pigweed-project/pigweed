// Copyright 2025 The Pigweed Authors
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

#include <cstddef>      // IWYU pragma: export
#include <cstdint>      // IWYU pragma: export
#include <type_traits>  // IWYU pragma: export

#include "pw_bloat/bloat_this_binary.h"             // IWYU pragma: export
#include "pw_containers/internal/intrusive_item.h"  // IWYU pragma: export
#include "pw_containers/intrusive_list.h"           // IWYU pragma: export
#include "pw_containers/size_report/size_report.h"  // IWYU pragma: export

namespace pw::containers::size_report {

/// A simple item for intrusive lists that wraps a moveable value.
template <typename T>
struct ListItem : public future::IntrusiveList<ListItem<T>>::Item {
  using Base = typename future::IntrusiveList<ListItem<T>>::Item;

  constexpr explicit ListItem(T value) : value_(value) {}

  ListItem(ListItem&& other) { *this = std::move(other); }

  ListItem& operator=(ListItem&& other) {
    value_ = std::move(other.value_);
    return *this;
  }

  bool operator<(const ListItem& rhs) const { return value_ < rhs.value_; }

  bool operator==(const ListItem& rhs) const { return value_ == rhs.value_; }

  T value_ = 0;
};

/// Invokes methods of the pw::containers::future::IntrusiveList type.
///
/// This method is used both to measure intrusive lists directly, as well as
/// to provide a baseline for measuring other types that use intrusive lists and
/// want to only measure their contributions to code size.
template <typename ItemType, int&... kExplicitGuard, typename Iterator>
int MeasureIntrusiveList(Iterator first, Iterator last, uint32_t mask) {
  mask = SetBaseline(mask);
  auto& list1 = GetContainer<future::IntrusiveList<ItemType>>();
  future::IntrusiveList<ItemType> list2;
  auto iter1 = first;
  for (size_t i = 0; i < 3; ++i) {
    if (iter1 != last) {
      ++iter1;
    }
  }
  list1.assign(first, iter1);
  list2.assign(iter1, last);
  mask = MeasureContainer(list1, mask);

  auto& item1 = list1.front();
  PW_BLOAT_EXPR(list1.pop_front(), mask);
  PW_BLOAT_EXPR(list2.push_front(item1), mask);
  if constexpr (std::is_move_assignable_v<ItemType>) {
    PW_BLOAT_EXPR(list1.swap(list2), mask);
    PW_BLOAT_EXPR(list1.reverse(), mask);
  }
  if constexpr (internal::is_weakly_orderable_v<ItemType>) {
    PW_BLOAT_EXPR(list1.sort(), mask);
    PW_BLOAT_EXPR(list1.merge(list2), mask);
    PW_BLOAT_COND(list1.unique() != 0, mask);
  }
  PW_BLOAT_EXPR(list2.clear(), mask);
  PW_BLOAT_EXPR(list1.remove(item1), mask);

  auto& item2 = list1.front();
  auto iter2 = list1.erase(item2);
  ++iter2;
  PW_BLOAT_EXPR(list1.insert(iter2, item2), mask);
  PW_BLOAT_EXPR(list1.splice(list1.end(), list2), mask);

  return list1.empty() ? 1 : 0;
}

}  // namespace pw::containers::size_report
