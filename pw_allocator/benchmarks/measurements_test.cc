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

#include "pw_metric/metric.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::allocator::Measurement;
using pw::allocator::Measurements;
using pw::allocator::internal::BenchmarkSample;

constexpr pw::metric::Token kName = PW_METRIC_TOKEN("test");

TEST(MeasurementTest, Construct_Default) {
  Measurement<size_t> measurement(kName, 0);

  EXPECT_EQ(measurement.nanoseconds(), 0.f);
  EXPECT_FLOAT_EQ(measurement.fragmentation(), 0.f);
  EXPECT_FLOAT_EQ(measurement.largest(), 0.f);
  EXPECT_EQ(measurement.failures(), 0u);
}

TEST(MeasurementTest, Update_Once) {
  BenchmarkSample data = {
      .nanoseconds = 1000,
      .fragmentation = 0.1f,
      .largest = 4096,
      .failed = false,
  };

  Measurement<size_t> measurement(kName, 0);
  measurement.Update(data);

  EXPECT_FLOAT_EQ(measurement.nanoseconds(), 1000.f);
  EXPECT_FLOAT_EQ(measurement.fragmentation(), 0.1f);
  EXPECT_FLOAT_EQ(measurement.largest(), 4096.f);
  EXPECT_EQ(measurement.failures(), 0u);
}

TEST(MeasurementTest, Update_TwiceSame) {
  BenchmarkSample data = {
      .nanoseconds = 1000,
      .fragmentation = 0.1f,
      .largest = 4096,
      .failed = true,
  };

  Measurement<size_t> measurement(kName, 0);
  measurement.Update(data);
  measurement.Update(data);

  EXPECT_FLOAT_EQ(measurement.nanoseconds(), 1000.f);
  EXPECT_FLOAT_EQ(measurement.fragmentation(), 0.1f);
  EXPECT_FLOAT_EQ(measurement.largest(), 4096.f);
  EXPECT_EQ(measurement.failures(), 2u);
}

TEST(MeasurementTest, Update_TwiceDifferent) {
  BenchmarkSample data = {
      .nanoseconds = 1000,
      .fragmentation = 0.1f,
      .largest = 4096,
      .failed = true,
  };

  Measurement<size_t> measurement(kName, 0);
  measurement.Update(data);

  data.nanoseconds = 2000;
  data.fragmentation = 0.04f;
  data.largest = 2048;
  data.failed = false;
  measurement.Update(data);

  EXPECT_FLOAT_EQ(measurement.nanoseconds(), 1500.f);
  EXPECT_FLOAT_EQ(measurement.fragmentation(), 0.07f);
  EXPECT_FLOAT_EQ(measurement.largest(), 3072.f);
  EXPECT_EQ(measurement.failures(), 1u);
}

TEST(MeasurementTest, Update_ManyVarious) {
  BenchmarkSample data;
  data.largest = 8192;
  Measurement<size_t> measurement(kName, 0);
  for (size_t i = 0; i < 10; ++i) {
    data.nanoseconds += 100.f;
    data.fragmentation += 0.02f;
    data.largest -= 512;
    data.failed = !data.failed;
    measurement.Update(data);
  }

  // sum([1..10]) is 55, for averages that are 5.5 times each increment.
  EXPECT_FLOAT_EQ(measurement.nanoseconds(), 5.5f * 100);
  EXPECT_FLOAT_EQ(measurement.fragmentation(), 5.5f * 0.02f);
  EXPECT_FLOAT_EQ(measurement.largest(), 8192 - (5.5f * 512));
  EXPECT_EQ(measurement.failures(), 5u);
}

class TestMeasurements : public Measurements {
 public:
  TestMeasurements() : Measurements(kName) {}
  ~TestMeasurements() { Measurements::Clear(); }
  using Measurements::AddByCount;
  using Measurements::AddByFragmentation;
  using Measurements::AddBySize;
  using Measurements::GetByCount;
  using Measurements::GetByFragmentation;
  using Measurements::GetBySize;
};

TEST(MeasurementsTest, ByCount) {
  Measurement<size_t> at_least_0(kName, 0);
  Measurement<size_t> at_least_10(kName, 10);
  Measurement<size_t> at_least_100(kName, 100);

  TestMeasurements by_count;
  by_count.AddByCount(at_least_0);
  by_count.AddByCount(at_least_10);
  by_count.AddByCount(at_least_100);

  EXPECT_EQ(&(by_count.GetByCount(0)), &at_least_0);
  EXPECT_EQ(&(by_count.GetByCount(9)), &at_least_0);
  EXPECT_EQ(&(by_count.GetByCount(10)), &at_least_10);
  EXPECT_EQ(&(by_count.GetByCount(99)), &at_least_10);
  EXPECT_EQ(&(by_count.GetByCount(100)), &at_least_100);
  EXPECT_EQ(&(by_count.GetByCount(size_t(-1))), &at_least_100);
}

TEST(MeasurementsTest, ByFragmentation) {
  Measurement<float> bottom_third(kName, 0.0f);
  Measurement<float> middle_third(kName, 0.33f);
  Measurement<float> top_third(kName, 0.66f);

  TestMeasurements by_fragmentation;
  by_fragmentation.AddByFragmentation(bottom_third);
  by_fragmentation.AddByFragmentation(middle_third);
  by_fragmentation.AddByFragmentation(top_third);

  EXPECT_EQ(&(by_fragmentation.GetByFragmentation(0)), &bottom_third);
  EXPECT_EQ(&(by_fragmentation.GetByFragmentation(0.3299)), &bottom_third);
  EXPECT_EQ(&(by_fragmentation.GetByFragmentation(0.33f)), &middle_third);
  EXPECT_EQ(&(by_fragmentation.GetByFragmentation(0.6599f)), &middle_third);
  EXPECT_EQ(&(by_fragmentation.GetByFragmentation(0.66f)), &top_third);
  EXPECT_EQ(&(by_fragmentation.GetByFragmentation(1.0f)), &top_third);
}

TEST(MeasurementsTest, BySize) {
  Measurement<size_t> at_least_0(kName, 0);
  Measurement<size_t> at_least_16(kName, 0x10);
  Measurement<size_t> at_least_256(kName, 0x100);

  TestMeasurements by_size;
  by_size.AddBySize(at_least_0);
  by_size.AddBySize(at_least_16);
  by_size.AddBySize(at_least_256);

  EXPECT_EQ(&(by_size.GetBySize(0)), &at_least_0);
  EXPECT_EQ(&(by_size.GetBySize(0xf)), &at_least_0);
  EXPECT_EQ(&(by_size.GetBySize(0x10)), &at_least_16);
  EXPECT_EQ(&(by_size.GetBySize(0xff)), &at_least_16);
  EXPECT_EQ(&(by_size.GetBySize(0x100)), &at_least_256);
  EXPECT_EQ(&(by_size.GetBySize(size_t(-1))), &at_least_256);
}

}  // namespace
