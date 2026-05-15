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

#include "pw_allocator/benchmarks/benchmark.h"

#include "pw_allocator/fragmentation.h"
#include "pw_assert/check.h"

namespace pw::allocator::internal {

// GenericBlockAllocatorBenchmark methods

void GenericBlockAllocatorBenchmark::BeforeAllocate(const Layout& layout) {
  size_ = layout.size();
  DoBefore();
}

void GenericBlockAllocatorBenchmark::AfterAllocate(const void* ptr) {
  DoAfter();
  if (ptr == nullptr) {
    data_.failed = true;
  } else {
    ++num_allocations_;
  }
  Update();
}

void GenericBlockAllocatorBenchmark::BeforeDeallocate(const void* ptr) {
  size_ = GetBlockInnerSize(ptr);
  DoBefore();
}

void GenericBlockAllocatorBenchmark::AfterDeallocate() {
  DoAfter();
  data_.failed = false;
  PW_CHECK(num_allocations_ != 0);
  --num_allocations_;
  Update();
}

void GenericBlockAllocatorBenchmark::BeforeReallocate(const Layout& layout) {
  size_ = layout.size();
  DoBefore();
}

void GenericBlockAllocatorBenchmark::AfterReallocate(const void* new_ptr) {
  DoAfter();
  data_.failed = new_ptr == nullptr;
  Update();
}

void GenericBlockAllocatorBenchmark::DoBefore() {
  start_ = chrono::SystemClock::now();
}

void GenericBlockAllocatorBenchmark::DoAfter() {
  auto finish = chrono::SystemClock::now();
  PW_ASSERT(start_.has_value());
  auto elapsed = finish - start_.value();
  data_.nanoseconds = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());

  IterateOverBlocks(data_);
  Fragmentation fragmentation = GetBlockFragmentation();
  data_.fragmentation = CalculateFragmentation(fragmentation);
}

void GenericBlockAllocatorBenchmark::Update() {
  measurements_.GetByCount(num_allocations_).Update(data_);
  measurements_.GetByFragmentation(data_.fragmentation).Update(data_);
  measurements_.GetBySize(size_).Update(data_);
}

}  // namespace pw::allocator::internal
