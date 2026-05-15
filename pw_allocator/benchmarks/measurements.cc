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

#include "pw_allocator/benchmarks/measurements.h"

namespace pw::allocator {
namespace internal {

// GenericMeasurement methods

GenericMeasurement::GenericMeasurement(metric::Token name) : metrics_(name) {
  metrics_.Add(nanoseconds_);
  metrics_.Add(fragmentation_);
  metrics_.Add(largest_);
  metrics_.Add(failures_);
}

void GenericMeasurement::Update(const BenchmarkSample& data) {
  count_++;

  float mean = nanoseconds_.value();
  mean += (static_cast<float>(data.nanoseconds) - mean) /
          static_cast<float>(count_);
  nanoseconds_.Set(mean);

  mean = fragmentation_.value();
  mean += (data.fragmentation - mean) / static_cast<float>(count_);
  fragmentation_.Set(mean);

  mean = largest_.value();
  mean +=
      (static_cast<float>(data.largest) - mean) / static_cast<float>(count_);
  largest_.Set(mean);

  if (data.failed) {
    failures_.Increment();
  }
}

}  // namespace internal

// Measurements methods

Measurements::Measurements(metric::Token name) : metrics_(name) {
  metrics_.Add(metrics_by_count_);
  metrics_.Add(metrics_by_fragmentation_);
  metrics_.Add(metrics_by_size_);
}

void Measurements::AddByCount(Measurement<size_t>& measurement) {
  metrics_by_count_.Add(measurement.metrics());
  by_count_.insert(measurement);
}

void Measurements::AddByFragmentation(Measurement<float>& measurement) {
  metrics_by_fragmentation_.Add(measurement.metrics());
  by_fragmentation_.insert(measurement);
}

void Measurements::AddBySize(Measurement<size_t>& measurement) {
  metrics_by_size_.Add(measurement.metrics());
  by_size_.insert(measurement);
}

void Measurements::Clear() {
  by_count_.clear();
  by_fragmentation_.clear();
  by_size_.clear();
}

Measurement<size_t>& Measurements::GetByCount(size_t count) {
  PW_ASSERT(!by_count_.empty());
  auto iter = by_count_.upper_bound(count);
  if (iter != by_count_.begin()) {
    --iter;
  }
  return *iter;
}

Measurement<float>& Measurements::GetByFragmentation(float fragmentation) {
  PW_ASSERT(!by_fragmentation_.empty());
  auto iter = by_fragmentation_.upper_bound(fragmentation);
  if (iter != by_fragmentation_.begin()) {
    --iter;
  }
  return *iter;
}

Measurement<size_t>& Measurements::GetBySize(size_t size) {
  PW_ASSERT(!by_size_.empty());
  auto iter = by_size_.upper_bound(size);
  if (iter != by_size_.begin()) {
    --iter;
  }
  return *iter;
}

// DefaultMeasurements methods

DefaultMeasurements::DefaultMeasurements(metric::Token name)
    : Measurements(name) {
  for (auto& measurement : by_count_) {
    AddByCount(measurement);
  }
  for (auto& measurement : by_fragmentation_) {
    AddByFragmentation(measurement);
  }
  for (auto& measurement : by_size_) {
    AddBySize(measurement);
  }
}

}  // namespace pw::allocator
