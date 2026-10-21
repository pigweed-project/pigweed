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

#include <cstdint>      // IWYU pragma: export
#include <type_traits>  // IWYU pragma: export

#include "pw_bloat/bloat_this_binary.h"             // IWYU pragma: export
#include "pw_containers/size_report/size_report.h"  // IWYU pragma: export

namespace pw::containers::size_report {

// Shared function for dynamic map types (pw::DynamicMap, std::map).
template <typename Map, int&... kExplicitGuard, typename Iterator>
int MeasureMap(Map& map, Iterator first, Iterator last, uint32_t mask) {
  mask = SetBaseline(mask);
  map.insert(first, last);
  mask = MeasureContainer(map, mask);
  PW_BLOAT_COND(!map.empty(), mask);

  const typename Map::key_type key = 1;

  auto it = map.find(key);
  PW_BLOAT_COND(it != map.end(), mask);

  auto iters = map.equal_range(key);
  PW_BLOAT_COND(iters.first != iters.second, mask);

  PW_BLOAT_EXPR(map.insert(*first), mask);
  PW_BLOAT_EXPR(map.emplace(first->first, first->second), mask);
  PW_BLOAT_EXPR(map.erase(key), mask);
  PW_BLOAT_EXPR(map.clear(), mask);

  return map.count(key) == 0 ? 0 : 1;
}

}  // namespace pw::containers::size_report
