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

#include "pw_bloat/bloat_this_binary.h"
#include "pw_containers/deque.h"
#include "pw_containers/size_report/deque.h"

namespace pw::containers::size_report {
namespace {

template <typename T, int&... kExplicitGuard, typename Iterator>
int MeasureStatic(Iterator first, Iterator last, uint32_t mask) {
  auto& fixed_deque = GetContainer<FixedDeque<T, kNumItems>>();
  if constexpr (containers::internal::kPodType<T>) {
    return MeasureDeque(fixed_deque, first, last, mask);
  } else {
    return MeasureDeque<Deque<T>>(fixed_deque, first, last, mask);
  }
}

template <typename T, int&... kExplicitGuard, typename Iterator>
int MeasureDynamic(Iterator first, Iterator last, uint32_t mask) {
  static FixedDeque<T> fixed_deque =
      FixedDeque<T>::Allocate(pw::allocator::GetLibCAllocator(), kNumItems);
  return MeasureDeque<Deque<T>>(fixed_deque, first, last, mask);
}

}  // namespace

int Measure() {
  volatile uint32_t mask = bloat::kDefaultMask;
  auto& items = GetItems<V1>();
  return MeasureStatic<V1>(items.begin(), items.end(), mask) +
         MeasureDynamic<V1>(items.begin(), items.end(), mask);
}

}  // namespace pw::containers::size_report

int main() { return ::pw::containers::size_report::Measure(); }
