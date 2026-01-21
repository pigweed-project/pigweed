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
#include <cstdint>

#include "pw_containers/intrusive_map.h"
#include "pw_metric/metric.h"

namespace pw::allocator {
namespace internal {

// Forward declaration for friending.
class GenericBlockAllocatorBenchmark;

/// Collection data relating to an allocating request.
struct BenchmarkSample {
  /// How many nanoseconds the request took.
  uint64_t nanoseconds = 0;

  /// Current fragmentation reported by the block allocaor.
  float fragmentation = 0.f;

  /// Current single largest allocation that could succeed.
  size_t largest = 0;

  /// Result of the last allocator request.
  bool failed = false;
};

/// Base class for an accumulation of samples into a single measurement.
///
/// As samples are collected, they are aggregated into a set of bins described
/// a specific domain and a range in that domain, e.g. the set of all
/// samples for requests of at least 512 bytes but less than 1024.
///
/// This class describes the common behavior of those bins without referencing
/// specific domain. Callers should not use this class directly, and use
/// `Measurement` instead.measurement
class GenericMeasurement {
 public:
  GenericMeasurement(metric::Token name);

  metric::Group& metrics() { return metrics_; }
  size_t count() const { return count_; }

  float nanoseconds() const { return nanoseconds_.value(); }
  float fragmentation() const { return fragmentation_.value(); }
  float largest() const { return largest_.value(); }
  uint32_t failures() const { return failures_.value(); }

  void Update(const BenchmarkSample& data);

 private:
  metric::Group metrics_;

  PW_METRIC(nanoseconds_, "mean response time (ns)", 0.f);
  PW_METRIC(fragmentation_, "mean fragmentation metric", 0.f);
  PW_METRIC(largest_, "mean max available (bytes)", 0.f);
  PW_METRIC(failures_, "number of calls that failed", 0u);

  size_t count_ = 0;
};

}  // namespace internal

/// An accumulation of samples into a single measurement.
///
/// This class extends `GenericMeasurement` with a key that describes what
/// domain is being used to partition samples. It is intrusively mappable using
/// that key, allowing other objects such as `Measurements` to maintain sorted
/// containers of this type.
template <typename Key>
class Measurement : public internal::GenericMeasurement,
                    public IntrusiveMap<Key, Measurement<Key>>::Item {
 public:
  Measurement(metric::Token name, Key lower_limit)
      : internal::GenericMeasurement(name), lower_limit_(lower_limit) {}
  const Key& key() const { return lower_limit_; }

 private:
  Key lower_limit_;
};

/// A collection of sorted containers of `Measurement`s.
///
/// This collection includes sorting `Measurement`s by
/// * The number of allocator requests that have been performed.
/// * The level of fragmentation as measured by the block allocator.
/// * The size of the most recent allocator request.
class Measurements {
 public:
  explicit Measurements(metric::Token name);

  metric::Group& metrics() { return metrics_; }
  Measurement<size_t>& GetByCount(size_t count);
  Measurement<float>& GetByFragmentation(float fragmentation);
  Measurement<size_t>& GetBySize(size_t size);

 protected:
  void AddByCount(Measurement<size_t>& measurement);
  void AddByFragmentation(Measurement<float>& measurement);
  void AddBySize(Measurement<size_t>& measurement);

  /// Removes measurements from the sorted
  void Clear();

 private:
  // Allow the benchmark harness to retrieve measurements.
  friend class internal::GenericBlockAllocatorBenchmark;

  metric::Group metrics_;

  PW_METRIC_GROUP(metrics_by_count_, "by allocation count");
  IntrusiveMap<size_t, Measurement<size_t>> by_count_;

  PW_METRIC_GROUP(metrics_by_fragmentation_, "by fragmentation");
  IntrusiveMap<float, Measurement<float>> by_fragmentation_;

  PW_METRIC_GROUP(metrics_by_size_, "by allocation size");
  IntrusiveMap<size_t, Measurement<size_t>> by_size_;
};

/// A default set of measurements for benchmarking allocators.
///
/// This organizes measurements in to logarithmically increasing ranges of
/// alloations counts and sizes, as well as fragmentation quintiles.
class DefaultMeasurements final : public Measurements {
 public:
  DefaultMeasurements(metric::Token name);
  ~DefaultMeasurements() { Measurements::Clear(); }

 private:
  static constexpr size_t kNumByCount = 5;
  std::array<Measurement<size_t>, kNumByCount> by_count_{{
      {PW_METRIC_TOKEN_EXPR("allocation count in [0, 10)"), 0},
      {PW_METRIC_TOKEN_EXPR("allocation count in [10, 100)"), 10},
      {PW_METRIC_TOKEN_EXPR("allocation count in [100, 1,000)"), 100},
      {PW_METRIC_TOKEN_EXPR("allocation count in [1,000, 10,000)"), 1000},
      {PW_METRIC_TOKEN_EXPR("allocation count in [10,000, inf)"), 10000},
  }};

  static constexpr size_t kNumByFragmentation = 5;
  std::array<Measurement<float>, kNumByFragmentation> by_fragmentation_ = {{
      {PW_METRIC_TOKEN_EXPR("fragmentation in [0.0, 0.2)"), 0.0f},
      {PW_METRIC_TOKEN_EXPR("fragmentation in [0.2, 0.4)"), 0.2f},
      {PW_METRIC_TOKEN_EXPR("fragmentation in [0.4, 0.6)"), 0.4f},
      {PW_METRIC_TOKEN_EXPR("fragmentation in [0.6, 0.8)"), 0.6f},
      {PW_METRIC_TOKEN_EXPR("fragmentation in [0.8, 1.0]"), 0.8f},
  }};

  static constexpr size_t kNumBySize = 6;
  std::array<Measurement<size_t>, kNumBySize> by_size_ = {{
      {PW_METRIC_TOKEN_EXPR("usable size in [0, 16)"), 0},
      {PW_METRIC_TOKEN_EXPR("usable size in [16, 64)"), 16},
      {PW_METRIC_TOKEN_EXPR("usable size in [64, 256)"), 64},
      {PW_METRIC_TOKEN_EXPR("usable size in [256, 1024)"), 256},
      {PW_METRIC_TOKEN_EXPR("usable size in [1024, 4096)"), 1024},
      {PW_METRIC_TOKEN_EXPR("usable size in [4096, inf)"), 4096},
  }};
};

}  // namespace pw::allocator
