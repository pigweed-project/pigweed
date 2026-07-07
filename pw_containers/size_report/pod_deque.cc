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

struct NonPod1 {
  int value_;
  char id_;
  NonPod1() = default;
  NonPod1(int val) : value_(val), id_(static_cast<char>(val)) {}
  NonPod1(int value, char id) : value_(value), id_(id) {}
  ~NonPod1() {}  // Non-trivially destructible
};

struct Pod1 {
  int value_;
  char id_;
};

struct NonPod2 {
  int value_;
  char id_;
  NonPod2() = default;
  NonPod2(int val) : value_(val), id_(static_cast<char>(val)) {}
  NonPod2(int value, char id) : value_(value), id_(id) {}
  ~NonPod2() {}
};

struct Pod2 {
  int value_;
  char id_;
};

template <typename T, int&... kExplicitGuard, typename Iterator>
int MeasurePodDeque(Iterator first, Iterator last, uint32_t mask) {
  auto& pod_deque = GetContainer<FixedDeque<T, kNumItems>>();
  return MeasureDeque(pod_deque, first, last, mask);
}

// Braced Initialization to accommodate POD structs
template <typename T>
std::array<T, kNumItems>& GetItemsBI() {
  static std::array<T, kNumItems> items = {T{0, 'a'},
                                           T{1, 'b'},
                                           T{2, 'c'},
                                           T{3, 'd'},
                                           T{4, 'e'},
                                           T{5, 'f'},
                                           T{6, 'g'},
                                           T{7, 'h'},
                                           T{8, 'i'},
                                           T{9, 'j'}};
  return items;
}

int Measure() {
  volatile uint32_t mask = bloat::kDefaultMask;
#ifdef PW_CONTAINERS_SIZE_REPORT_POD_TYPE
  using TestType1 [[maybe_unused]] = Pod1;
  using TestType2 [[maybe_unused]] = Pod2;
#else
  using TestType1 [[maybe_unused]] = NonPod1;
  using TestType2 [[maybe_unused]] = NonPod2;
#endif

  auto& items1 = GetItemsBI<TestType1>();
  int rc = MeasurePodDeque<TestType1>(items1.begin(), items1.end(), mask);

#ifdef PW_CONTAINERS_SIZE_REPORT_ALTERNATE_VALUE
  auto& items2 = GetItemsBI<TestType2>();
  rc += MeasurePodDeque<TestType2>(items2.begin(), items2.end(), mask);
#endif

  return rc;
}

}  // namespace pw::containers::size_report

int main() { return ::pw::containers::size_report::Measure(); }
