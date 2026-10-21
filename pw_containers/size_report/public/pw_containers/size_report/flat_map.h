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
#include "pw_containers/flat_map.h"                 // IWYU pragma: export
#include "pw_containers/size_report/size_report.h"  // IWYU pragma: export

namespace pw::containers::size_report {

template <typename K, typename V>
using FlatMapPair = ::pw::containers::Pair<K, V>;

/// Invokes methods of the pw::FlatMap type.
///
/// This method is used both to measure flat maps directly, as well as to
/// provide a baseline for measuring other types that use flat maps and want to
/// only measure their contributions to code size.
template <typename K, typename V>
int MeasureFlatMap(uint32_t mask) {
  using PairType = FlatMapPair<K, V>;
  mask = SetBaseline(mask);
  auto& pairs = GetPairs<PairType>();
  auto& flat_map = GetContainer<FlatMap<K, V, kNumItems>>(pairs);
  mask = MeasureContainer(flat_map, mask);

  auto iter = flat_map.find(1U);
  PW_BLOAT_COND(iter != flat_map.end(), mask);

  auto iters = flat_map.equal_range(1U);
  PW_BLOAT_COND(iters.first != iters.second, mask);

  return flat_map.contains(1U) ? 0 : 1;
}

}  // namespace pw::containers::size_report
