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

#include <cstdint>  // IWYU pragma: export

#include "pw_bloat/bloat_this_binary.h"             // IWYU pragma: export
#include "pw_containers/intrusive_set.h"            // IWYU pragma: export
#include "pw_containers/size_report/size_report.h"  // IWYU pragma: export

namespace pw::containers::size_report {

/// A simple item for intrusive sets that wraps a copyable value.
template <typename T>
struct SetItem : public IntrusiveSet<SetItem<T>>::Item {
  constexpr explicit SetItem(T value) : value_(value) {}
  bool operator<(const SetItem& rhs) const { return value_ < rhs.value_; }
  bool operator==(const SetItem& rhs) const { return value_ == rhs.value_; }
  T value_;
};

/// Invokes methods of the pw::IntrusiveSet type.
///
/// This method is used both to measure intrusive sets directly, as well as to
/// provide a baseline for measuring other types that use intrusive sets and
/// want to only measure their contributions to code size.
template <typename ItemType, int&... kExplicitGuard, typename Iterator>
int MeasureIntrusiveSet(Iterator first, Iterator last, uint32_t mask) {
  mask = SetBaseline(mask);
  auto& set1 = GetContainer<IntrusiveSet<ItemType>>();
  set1.insert(first, last);
  mask = MeasureContainer(set1, mask);

  IntrusiveSet<ItemType> set2;
  auto& item0 = *(set1.begin());
  PW_BLOAT_EXPR(set2.swap(set1), mask);
  PW_BLOAT_EXPR(set2.erase(item0), mask);
  PW_BLOAT_EXPR(set1.merge(set2), mask);
  PW_BLOAT_EXPR(set1.insert(item0), mask);

  return set1.count(item0) != 0 ? 0 : 1;
}

}  // namespace pw::containers::size_report
