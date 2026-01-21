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

#include "pw_allocator/metrics.h"

#include <cstdint>
#include <string_view>

#include "examples/named_u32.h"
#include "pw_allocator/testing.h"
#include "pw_allocator/tracking_allocator.h"
#include "pw_metric/metric.h"

namespace examples {

// DOCSTAG: [pw_allocator-examples-metrics-custom_metrics1]
struct CustomMetrics : public ::pw::allocator::NoMetrics {
  PW_ALLOCATOR_METRICS_ENABLE(allocated_bytes);
  PW_ALLOCATOR_METRICS_ENABLE(peak_allocated_bytes);
  PW_ALLOCATOR_METRICS_ENABLE(num_failures);
};
// DOCSTAG: [pw_allocator-examples-metrics-custom_metrics1]

void CollectCustomMetrics(pw::Allocator& allocator) {
  // DOCSTAG: [pw_allocator-examples-metrics-custom_metrics2]
  constexpr pw::metric::Token kToken = PW_METRIC_TOKEN("CustomMetrics");
  pw::allocator::TrackingAllocatorImpl<CustomMetrics> tracker(kToken,
                                                              allocator);
  // DOCSTAG: [pw_allocator-examples-metrics-custom_metrics2]

  // Keep one UniquePtr in scope, and let the other deallocate immediately.
  auto ptr = tracker.MakeUnique<NamedU32>("test", 111);
  ptr = tracker.MakeUnique<NamedU32>("test", 222);

  // DOCSTAG: [pw_allocator-examples-metrics-dump]
  tracker.metric_group().Dump();
  // DOCSTAG: [pw_allocator-examples-metrics-dump]
}

void CollectMultipleTrackers(pw::Allocator& allocator) {
  // DOCSTAG: [pw_allocator-examples-metrics-multiple_trackers]
  using MyTrackingAllocator =
      pw::allocator::TrackingAllocatorImpl<pw::allocator::internal::AllMetrics>;

  constexpr pw::metric::Token kToken0 = PW_METRIC_TOKEN("Combined");
  MyTrackingAllocator combined(kToken0, allocator);

  constexpr pw::metric::Token kToken1 = PW_METRIC_TOKEN("Tracker1");
  MyTrackingAllocator tracker1(kToken1, combined);

  constexpr pw::metric::Token kToken2 = PW_METRIC_TOKEN("Tracker2");
  MyTrackingAllocator tracker2(kToken2, combined);

  combined.metric_group().Add(tracker1.metric_group());
  combined.metric_group().Add(tracker2.metric_group());
  // DOCSTAG: [pw_allocator-examples-metrics-multiple_trackers]

  // Keep one UniquePtr in scope, and let others deallocate immediately.
  auto ptr = tracker1.MakeUnique<NamedU32>("test", 111);
  ptr = tracker1.MakeUnique<NamedU32>("test", 222);
  ptr = tracker2.MakeUnique<NamedU32>("test", 222);

  combined.metric_group().Dump();
}

}  // namespace examples

namespace pw::allocator {

using AllocatorForTest = ::pw::allocator::test::AllocatorForTest<256>;

class MetricsExampleTest : public ::testing::Test {
 protected:
  AllocatorForTest allocator_;
};

// TODO: b/328076428 - Use pwrev/193642.
TEST_F(MetricsExampleTest, CollectCustomMetrics) {
  // log_basic::test::LogCache<32> logs;
  examples::CollectCustomMetrics(allocator_);

  // std::array<std::atring_view, 1> kExpectedLines{
  //   "todo",
  // };
  // for (const &auto line : kExpectedLines) {
  //   ASSERT_TRUE(logs.HasNext());
  //   log_basic::test::LogMessage log = logs.Next();
  //   EXPECT_EQ(log.level, PW_LOG_LEVEL_INFO);
  //   EXPECT_STREQ(log.message.c_str(), line.data());
  // }
}

// TODO: b/328076428 - Use pwrev/193642.
TEST_F(MetricsExampleTest, CollectMultipleTrackers) {
  // log_basic::test::LogCache<32> logs;
  examples::CollectMultipleTrackers(allocator_);

  // std::array<std::atring_view, 1> kExpectedLines{
  //   "todo",
  // };
  // for (const &auto line : kExpectedLines) {
  //   ASSERT_TRUE(logs.HasNext());
  //   log_basic::test::LogMessage log = logs.Next();
  //   EXPECT_EQ(log.level, PW_LOG_LEVEL_INFO);
  //   EXPECT_STREQ(log.message.c_str(), line.data());
  // }
}

}  // namespace pw::allocator
