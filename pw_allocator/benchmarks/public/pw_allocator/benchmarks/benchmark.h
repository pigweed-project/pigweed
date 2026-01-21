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
#pragma once

#include <cstddef>
#include <optional>

#include "pw_allocator/benchmarks/measurements.h"
#include "pw_allocator/fragmentation.h"
#include "pw_allocator/layout.h"
#include "pw_allocator/test_harness.h"
#include "pw_chrono/system_clock.h"
#include "pw_metric/metric.h"

namespace pw::allocator {
namespace internal {

/// Base class for benchmarking block allocators.
///
/// This class extends the test harness to sample data relevant to benchmarking
/// the performance of a blockallocator before and after each request. It is not
/// templated and avoids any types related to the specific block allocator.
///
/// Callers should not use this class directly, and instead using
/// `BlockAllocatorBenchmark`.
class GenericBlockAllocatorBenchmark : public pw::allocator::test::TestHarness {
 public:
  metric::Group& metrics() { return measurements_.metrics(); }
  Measurements& measurements() { return measurements_; }

 protected:
  constexpr explicit GenericBlockAllocatorBenchmark(Measurements& measurements)
      : measurements_(measurements) {}

 private:
  /// @copydoc test::TestHarness::BeforeAllocate
  void BeforeAllocate(const Layout& layout) override;

  /// @copydoc test::TestHarness::BeforeAllocate
  void AfterAllocate(const void* ptr) override;

  /// @copydoc test::TestHarness::BeforeAllocate
  void BeforeDeallocate(const void* ptr) override;

  /// @copydoc test::TestHarness::BeforeAllocate
  void AfterDeallocate() override;

  /// @copydoc test::TestHarness::BeforeAllocate
  void BeforeReallocate(const Layout& layout) override;

  /// @copydoc test::TestHarness::BeforeAllocate
  void AfterReallocate(const void* new_ptr) override;

  /// Preparse to benchmark an allocator request.
  void DoBefore();

  /// Finishes benchmarking an allocator request.
  void DoAfter();

  /// Updates the `measurements` with data from an allocator request.
  void Update();

  /// Returns the inner size of block from is usable space.
  virtual size_t GetBlockInnerSize(const void* ptr) const = 0;

  /// Iterates over an allocators blocks and collects benchmark data.
  virtual void IterateOverBlocks(BenchmarkSample& data) const = 0;

  /// Measures the current fragmentation of an allocator.
  virtual Fragmentation GetBlockFragmentation() const = 0;

  std::optional<chrono::SystemClock::time_point> start_;
  size_t num_allocations_ = 0;
  size_t size_ = 0;
  BenchmarkSample data_;
  Measurements& measurements_;
};

}  // namespace internal

/// test harness used for benchmarking block allocators.
///
/// This class records measurements aggregated from benchmarking samples of a
/// sequence of block allocator requests. The `Measurements` objects must
/// outlive the benchmark test harness.
///
/// @tparam   AllocatorType  Type of the block allocator being benchmarked.
template <typename AllocatorType>
class BlockAllocatorBenchmark
    : public internal::GenericBlockAllocatorBenchmark {
 public:
  BlockAllocatorBenchmark(Measurements& measurements, AllocatorType& allocator)
      : internal::GenericBlockAllocatorBenchmark(measurements),
        allocator_(allocator) {
    set_allocator(&allocator);
  }

 private:
  using BlockType = typename AllocatorType::BlockType;

  /// @copydoc GenericBlockAllocatorBenchmark::GetBlockInnerSize
  size_t GetBlockInnerSize(const void* ptr) const override;

  /// @copydoc GenericBlockAllocatorBenchmark::IterateOverBlocks
  void IterateOverBlocks(internal::BenchmarkSample& data) const override;

  /// @copydoc GenericBlockAllocatorBenchmark::GetBlockFragmentation
  Fragmentation GetBlockFragmentation() const override;

  AllocatorType& allocator_;
};

/// Block allocator benchmark that use a default set of measurements
///
/// This class simplifies the set up of a block allocator benchmark by defining
/// a default set of metrics and linking all the relevant metrics together.
template <typename AllocatorType>
class DefaultBlockAllocatorBenchmark
    : public BlockAllocatorBenchmark<AllocatorType> {
 public:
  DefaultBlockAllocatorBenchmark(metric::Token name, AllocatorType& allocator)
      : BlockAllocatorBenchmark<AllocatorType>(measurements_, allocator),
        measurements_(name) {}

 private:
  DefaultMeasurements measurements_;
};

// Template method implementations

template <typename AllocatorType>
size_t BlockAllocatorBenchmark<AllocatorType>::GetBlockInnerSize(
    const void* ptr) const {
  const auto* block = BlockType::FromUsableSpace(ptr);
  return block->InnerSize();
}

template <typename AllocatorType>
void BlockAllocatorBenchmark<AllocatorType>::IterateOverBlocks(
    internal::BenchmarkSample& data) const {
  data.largest = 0;
  for (const auto* block : allocator_.blocks()) {
    if (block->IsFree()) {
      data.largest = std::max(data.largest, block->InnerSize());
    }
  }
}

template <typename AllocatorType>
Fragmentation BlockAllocatorBenchmark<AllocatorType>::GetBlockFragmentation()
    const {
  return allocator_.MeasureFragmentation();
}

}  // namespace pw::allocator
