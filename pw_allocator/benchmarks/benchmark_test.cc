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

#include <cstddef>

#include "pw_allocator/benchmarks/measurements.h"
#include "pw_allocator/fragmentation.h"
#include "pw_allocator/test_harness.h"
#include "pw_allocator/testing.h"
#include "pw_random/xor_shift.h"
#include "pw_unit_test/framework.h"

namespace {

constexpr size_t kCapacity = 65536;
constexpr size_t kMaxSize = 64;

using AllocatorForTest = ::pw::allocator::test::AllocatorForTest<kCapacity>;
using Benchmark =
    ::pw::allocator::DefaultBlockAllocatorBenchmark<AllocatorForTest>;
using ::pw::allocator::CalculateFragmentation;
using ::pw::allocator::Measurement;
using ::pw::allocator::Measurements;
using ::pw::allocator::test::AllocationRequest;
using ::pw::allocator::test::kToken;
using ::pw::allocator::test::Request;

template <typename GetByKey>
bool IsChanged(Benchmark& benchmark, GetByKey get_by_key) {
  return get_by_key(benchmark.measurements()).count() != 0;
}

bool ByCountChanged(Benchmark& benchmark, size_t count) {
  return IsChanged(benchmark, [count](Measurements& m) -> Measurement<size_t>& {
    return m.GetByCount(count);
  });
}

TEST(BenchmarkTest, ByCount) {
  AllocatorForTest allocator;
  Benchmark benchmark(kToken, allocator);
  benchmark.set_prng_seed(1);
  benchmark.set_available(kCapacity);

  EXPECT_FALSE(ByCountChanged(benchmark, 0));

  benchmark.GenerateRequest(kMaxSize);
  EXPECT_TRUE(ByCountChanged(benchmark, 0));

  while (benchmark.num_allocations() < 9) {
    benchmark.GenerateRequest(kMaxSize);
  }
  EXPECT_FALSE(ByCountChanged(benchmark, 10));

  while (benchmark.num_allocations() < 10) {
    benchmark.GenerateRequest(kMaxSize);
  }
  EXPECT_TRUE(ByCountChanged(benchmark, 10));

  while (benchmark.num_allocations() < 99) {
    benchmark.GenerateRequest(kMaxSize);
  }
  EXPECT_FALSE(ByCountChanged(benchmark, 100));

  while (benchmark.num_allocations() < 100) {
    benchmark.GenerateRequest(kMaxSize);
  }
  EXPECT_TRUE(ByCountChanged(benchmark, 100));

  while (benchmark.num_allocations() < 999) {
    benchmark.GenerateRequest(kMaxSize);
  }
  EXPECT_FALSE(ByCountChanged(benchmark, 1000));

  while (benchmark.num_allocations() < 1000) {
    benchmark.GenerateRequest(kMaxSize);
  }
  EXPECT_TRUE(ByCountChanged(benchmark, 1000));
}

size_t ByFragmentationChanged(Benchmark& benchmark, float fragmentation) {
  return IsChanged(benchmark,
                   [fragmentation](Measurements& m) -> Measurement<float>& {
                     return m.GetByFragmentation(fragmentation);
                   });
}

TEST(BenchmarkTest, ByFragmentation) {
  AllocatorForTest allocator;
  Benchmark benchmark(kToken, allocator);
  benchmark.set_prng_seed(1);
  benchmark.set_available(kCapacity);

  EXPECT_FALSE(ByFragmentationChanged(benchmark, 0.2f));

  for (float threshold = 0.2f; threshold < 1.0f; threshold += 0.2f) {
    while (true) {
      auto fragmentation = allocator.MeasureFragmentation();
      ASSERT_TRUE(fragmentation.has_value());
      if (CalculateFragmentation(*fragmentation) >= threshold) {
        break;
      }
      benchmark.GenerateRequest(kMaxSize);
    }
    EXPECT_TRUE(ByFragmentationChanged(benchmark, threshold));
  }
}

bool BySizeChanged(Benchmark& benchmark, size_t size) {
  return IsChanged(benchmark, [size](Measurements& m) -> Measurement<size_t>& {
    return m.GetBySize(size);
  });
}

TEST(BenchmarkTest, BySize) {
  AllocatorForTest allocator;
  Benchmark benchmark(kToken, allocator);
  benchmark.set_prng_seed(1);
  benchmark.set_available(kCapacity);
  AllocationRequest request;

  EXPECT_FALSE(BySizeChanged(benchmark, 4096));
  request.size = 8192;
  EXPECT_TRUE(benchmark.HandleRequest(request));
  EXPECT_TRUE(BySizeChanged(benchmark, 4096));
  EXPECT_FALSE(BySizeChanged(benchmark, 4095));

  EXPECT_FALSE(BySizeChanged(benchmark, 1024));
  request.size = 4095;
  EXPECT_TRUE(benchmark.HandleRequest(request));
  EXPECT_TRUE(BySizeChanged(benchmark, 1024));
  EXPECT_FALSE(BySizeChanged(benchmark, 1023));

  EXPECT_FALSE(BySizeChanged(benchmark, 256));
  request.size = 256;
  EXPECT_TRUE(benchmark.HandleRequest(request));
  EXPECT_TRUE(BySizeChanged(benchmark, 256));
  EXPECT_FALSE(BySizeChanged(benchmark, 255));

  EXPECT_FALSE(BySizeChanged(benchmark, 64));
  request.size = 96;
  EXPECT_TRUE(benchmark.HandleRequest(request));
  EXPECT_TRUE(BySizeChanged(benchmark, 64));
  EXPECT_FALSE(BySizeChanged(benchmark, 63));

  EXPECT_FALSE(BySizeChanged(benchmark, 16));
  request.size = 63;
  EXPECT_TRUE(benchmark.HandleRequest(request));
  EXPECT_TRUE(BySizeChanged(benchmark, 16));
  EXPECT_FALSE(BySizeChanged(benchmark, 15));
}

}  // namespace
