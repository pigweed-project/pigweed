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
#include "pw_containers/intrusive_multimap.h"       // IWYU pragma: export
#include "pw_containers/size_report/size_report.h"  // IWYU pragma: export

namespace pw::containers::size_report {

/// A simple pair for intrusive multimaps that wraps a copyable key and value.
template <typename K, typename V>
struct MultiMapPair : public IntrusiveMultiMap<K, MultiMapPair<K, V>>::Pair {
  using Base = typename IntrusiveMultiMap<K, MultiMapPair<K, V>>::Pair;
  using KeyType = K;

  constexpr MultiMapPair(K key, V value) : Base(key), value_(value) {}

  V value_;
};

/// Invokes methods of the pw::IntrusiveMultiMap type.
///
/// This method is used both to measure intrusive multimaps directly, as well as
/// to provide a baseline for measuring other types that use intrusive multimaps
/// and want to only measure their contributions to code size.
template <typename KeyType,
          typename PairType,
          int&... kExplicitGuard,
          typename Iterator>
int MeasureIntrusiveMultiMap(Iterator first, Iterator last, uint32_t mask) {
  mask = SetBaseline(mask);
  auto& map1 = GetContainer<IntrusiveMultiMap<KeyType, PairType>>();
  map1.insert(first, last);
  mask = MeasureContainer(map1, mask);

  auto iter = map1.find(1U);
  PW_BLOAT_COND(iter != map1.end(), mask);

  auto iters = map1.equal_range(1U);
  PW_BLOAT_COND(iters.first != iters.second, mask);

  IntrusiveMultiMap<KeyType, PairType> map2;
  auto& item0 = *(map1.begin());
  PW_BLOAT_EXPR(map2.swap(map1), mask);
  PW_BLOAT_EXPR(map2.erase(item0), mask);
  PW_BLOAT_EXPR(map1.merge(map2), mask);
  PW_BLOAT_EXPR(map1.insert(item0), mask);

  return map1.count(item0.key()) != 0 ? 0 : 1;
}

}  // namespace pw::containers::size_report
