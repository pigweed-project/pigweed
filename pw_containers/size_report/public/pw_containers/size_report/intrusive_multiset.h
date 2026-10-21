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
#include "pw_containers/intrusive_multiset.h"       // IWYU pragma: export
#include "pw_containers/size_report/size_report.h"  // IWYU pragma: export

namespace pw::containers::size_report {

/// A simple item for intrusive multisets that wraps a copyable value.
template <typename T>
struct MultiSetItem : public IntrusiveMultiSet<MultiSetItem<T>>::Item {
  constexpr explicit MultiSetItem(T value) : value_(value) {}
  bool operator<(const MultiSetItem& rhs) const { return value_ < rhs.value_; }
  bool operator==(const MultiSetItem& rhs) const {
    return value_ == rhs.value_;
  }
  T value_;
};

/// Invokes methods of the pw::IntrusiveMultiSet type.
///
/// This method is used both to measure intrusive multisets directly, as well as
/// to provide a baseline for measuring other types that use intrusive multisets
/// and want to only measure their contributions to code size.
template <typename ItemType, int&... kExplicitGuard, typename Iterator>
int MeasureIntrusiveMultiSet(Iterator first, Iterator last, uint32_t mask) {
  mask = SetBaseline(mask);
  auto& set1 = GetContainer<IntrusiveMultiSet<ItemType>>();
  set1.insert(first, last);
  mask = MeasureContainer(set1, mask);

  IntrusiveMultiSet<ItemType> set2;
  auto& item0 = *(set1.begin());
  PW_BLOAT_EXPR(set2.swap(set1), mask);
  PW_BLOAT_EXPR(set2.erase(item0), mask);
  PW_BLOAT_EXPR(set1.merge(set2), mask);
  PW_BLOAT_EXPR(set1.insert(item0), mask);

  return set1.count(item0) != 0 ? 0 : 1;
}

}  // namespace pw::containers::size_report
