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

#include <cstdint>

#include "pw_bloat/bloat_this_binary.h"
#include "pw_containers/size_report/intrusive_map.h"
#include "pw_containers/size_report/intrusive_multimap.h"
#include "pw_containers/size_report/size_report.h"

namespace pw::containers::size_report {

int Measure() {
  volatile uint32_t mask = bloat::kDefaultMask;
  auto& pairs1 = GetPairs<MapPair<K1, V1>>();
  auto& pairs2 = GetPairs<MultiMapPair<K1, V1>>();
  return MeasureIntrusiveMap<K1, MapPair<K1, V1>>(
             pairs1.begin(), pairs1.end(), mask) +
         MeasureIntrusiveMultiMap<K1, MultiMapPair<K1, V1>>(
             pairs2.begin(), pairs2.end(), mask);
}

}  // namespace pw::containers::size_report

int main() { return ::pw::containers::size_report::Measure(); }
