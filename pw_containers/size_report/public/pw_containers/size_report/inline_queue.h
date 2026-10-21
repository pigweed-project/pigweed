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
#include "pw_containers/inline_queue.h"             // IWYU pragma: export
#include "pw_containers/size_report/size_report.h"  // IWYU pragma: export

namespace pw::containers::size_report {

/// Invokes methods of the pw::InlineQueue type.
///
/// This method is used both to measure inline queues directly, as well as to
/// provide a baseline for measuring other types that use inline queues and want
/// to only measure their contributions to code size.
template <typename T, int&... kExplicitGuard, typename Iterator>
int MeasureInlineQueue(Iterator first, Iterator last, uint32_t mask) {
  mask = SetBaseline(mask);
  auto& inline_queue = GetContainer<InlineQueue<T, kNumItems>>();
  while (first != last) {
    inline_queue.push_overwrite(*first++);
  }
  mask = MeasureContainer(inline_queue, mask);
  PW_BLOAT_COND(inline_queue.full(), mask);

  T item = inline_queue.front();
  PW_BLOAT_EXPR(inline_queue.pop(), mask);
  PW_BLOAT_EXPR(inline_queue.push(item), mask);

  return inline_queue.size() == kNumItems ? 0 : 1;
}

}  // namespace pw::containers::size_report
