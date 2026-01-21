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

#include <array>
#include <cstddef>
#include <cstdint>

#include "pw_allocator/benchmarks/benchmark.h"
#include "pw_allocator/benchmarks/config.h"
#include "pw_allocator/first_fit.h"
#include "pw_metric/metric.h"

namespace pw::allocator {

constexpr metric::Token kFirstFitBenchmark =
    PW_METRIC_TOKEN("first fit benchmark");

std::array<std::byte, benchmarks::kCapacity> buffer;

void DoFirstFitBenchmark() {
  FirstFitAllocator allocator(buffer);
  DefaultBlockAllocatorBenchmark benchmark(kFirstFitBenchmark, allocator);
  benchmark.set_prng_seed(1);
  benchmark.set_available(benchmarks::kCapacity);
  benchmark.GenerateRequests(benchmarks::kMaxSize, benchmarks::kNumRequests);
  benchmark.metrics().Dump();
}

}  // namespace pw::allocator

int main() {
  pw::allocator::DoFirstFitBenchmark();
  return 0;
}
