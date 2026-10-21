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

#include <cstdint>      // IWYU pragma: export
#include <type_traits>  // IWYU pragma: export

#include "pw_bloat/bloat_this_binary.h"             // IWYU pragma: export
#include "pw_containers/size_report/size_report.h"  // IWYU pragma: export

namespace pw::containers::size_report {

// Shared function for dynamic deque types (pw::DynamicDeque, std::deque).
template <typename Deque, int&... kExplicitGuard, typename Iterator>
int MeasureDeque(Deque& deque, Iterator first, Iterator last, uint32_t mask) {
  mask = SetBaseline(mask);
  deque.assign(first, last);
  mask = MeasureContainer(deque, mask);
  PW_BLOAT_COND(!deque.empty(), mask);

  auto front = deque.front();
  auto back = deque.back();
  PW_BLOAT_EXPR(deque.pop_front(), mask);
  PW_BLOAT_EXPR(deque.pop_back(), mask);
  PW_BLOAT_EXPR(deque.push_front(back), mask);
  PW_BLOAT_EXPR(deque.push_back(front), mask);
  PW_BLOAT_EXPR(deque.clear(), mask);

  PW_BLOAT_EXPR(deque.insert(deque.begin() + 1, 3, front), mask);
  PW_BLOAT_EXPR(deque.erase(deque.begin(), deque.begin() + 3), mask);

  const auto new_size = static_cast<typename Deque::size_type>(kNumItems + 2);
  PW_BLOAT_EXPR(deque.resize(new_size), mask);
  PW_BLOAT_EXPR(deque.shrink_to_fit(), mask);

  return deque.size() == new_size ? 0 : 1;
}

}  // namespace pw::containers::size_report
