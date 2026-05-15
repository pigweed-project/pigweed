// Copyright 2024 The Pigweed Authors
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

#ifndef PW_ALLOCATOR_SIZE_REPORT_BASE
#include "pw_allocator/pmr_allocator.h"  // nogncheck
#endif

#include <vector>

#include "pw_allocator/best_fit.h"
#include "pw_allocator/size_report/size_report.h"
#include "pw_bloat/bloat_this_binary.h"

namespace pw::allocator::size_report {

int Measure() {
  volatile uint32_t mask = bloat::kDefaultMask;
  static BestFitAllocator<BlockType> base(GetBuffer());

#ifdef PW_ALLOCATOR_SIZE_REPORT_BASE
  std::vector<Bar> vec;
  auto* bar = base.New<Bar>(1u);
  vec.emplace_back(std::move(*bar));

#else  // PW_ALLOCATOR_SIZE_REPORT_BASE
  static PmrAllocator alloc(base);
  std::pmr::vector<Bar> vec(alloc);
  PW_BLOAT_EXPR(vec.emplace_back(1u), mask);

#endif  // PW_ALLOCATOR_SIZE_REPORT_BASE

  PW_BLOAT_EXPR(vec.clear(), mask);
  return vec.empty() ? 0 : 1;
}

}  // namespace pw::allocator::size_report

int main() { return pw::allocator::size_report::Measure(); }
