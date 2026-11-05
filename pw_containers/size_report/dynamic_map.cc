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

#include "pw_containers/size_report/dynamic_map.h"

#include <cstdint>
#include <utility>

#include "pw_allocator/libc_allocator.h"
#include "pw_bloat/bloat_this_binary.h"
#include "pw_containers/dynamic_map.h"
#include "pw_containers/size_report/size_report.h"

namespace pw::containers::size_report {

template <typename K, typename V, int&... kExplicitGuard, typename Iterator>
int MeasureDynamicMap(Iterator first, Iterator last, uint32_t mask) {
  static DynamicMap<K, V> dynamic_map(pw::allocator::GetLibCAllocator());
  return MeasureMap(dynamic_map, first, last, mask);
}

int Measure() {
  volatile uint32_t mask = bloat::kDefaultMask;
  auto& pairs1 = GetPairs<std::pair<K1, V1>>();
  int rc = MeasureDynamicMap<K1, V1>(pairs1.begin(), pairs1.end(), mask);

#ifdef PW_CONTAINERS_SIZE_REPORT_ALTERNATE_VALUE
  auto& pairs2 = GetPairs<std::pair<K1, V2>>();
  rc += MeasureDynamicMap<K1, V2>(pairs2.begin(), pairs2.end(), mask);
#endif

#ifdef PW_CONTAINERS_SIZE_REPORT_ALTERNATE_KEY_AND_VALUE
  auto& pairs3 = GetPairs<std::pair<K2, V2>>();
  rc += MeasureDynamicMap<K2, V2>(pairs3.begin(), pairs3.end(), mask);
#endif

  return rc;
}

}  // namespace pw::containers::size_report

int main() { return ::pw::containers::size_report::Measure(); }
