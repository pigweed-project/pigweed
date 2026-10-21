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

#include <cstddef>  // IWYU pragma: export
#include <cstdint>  // IWYU pragma: export

#include "pw_bloat/bloat_this_binary.h"             // IWYU pragma: export
#include "pw_containers/size_report/size_report.h"  // IWYU pragma: export
#include "pw_containers/vector.h"                   // IWYU pragma: export

namespace pw::containers::size_report {

/// Invokes methods of the pw::Vector type.
///
/// This method is used both to measure vector directly, as well as to
/// provide a baseline for measuring other types that use vector and
/// want to only measure their contributions to code size.
template <typename T, size_t kSize, int&... kExplicitGuard, typename Iterator>
int MeasureVector(Iterator first, Iterator last, uint32_t mask) {
  mask = SetBaseline(mask);
  auto& vec = GetContainer<Vector<T, kSize>>();
  vec.assign(first, last);
  mask = MeasureContainer(vec, mask);

  PW_BLOAT_EXPR(vec.resize(kSize - 1), mask);
  PW_BLOAT_EXPR(vec.insert(vec.begin(), T()), mask);
  PW_BLOAT_EXPR(vec.erase(vec.begin()), mask);
  if (vec.full()) {
    return 1;
  }
  PW_BLOAT_EXPR(vec.clear(), mask);

  return vec.empty() ? 0 : 1;
}

}  // namespace pw::containers::size_report
