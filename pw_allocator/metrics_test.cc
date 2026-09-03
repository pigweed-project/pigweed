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

#include "pw_unit_test/framework.h"

namespace {

using ::pw::allocator::NoMetrics;
using ::pw::allocator::internal::AllMetrics;

TEST(MetricsTest, NoMetricsPresent) {
#define EXPECT_METRIC_MISSING(metric_name) \
  EXPECT_FALSE(NoMetrics::has_##metric_name());

  PW_ALLOCATOR_METRICS_FOREACH(EXPECT_METRIC_MISSING);

#undef EXPECT_METRIC_MISSING
}

TEST(MetricsTest, NoMetricsEnabled) {
#define EXPECT_METRIC_DISABLED(metric_name) \
  EXPECT_FALSE(NoMetrics::metric_name##_enabled());

  PW_ALLOCATOR_METRICS_FOREACH(EXPECT_METRIC_DISABLED);
  EXPECT_FALSE(pw::allocator::internal::AnyEnabled<NoMetrics>());

#undef EXPECT_METRIC_DISABLED
}

TEST(MetricsTest, TraitsAreConstexpr) {
#define FAIL_IF_PRESENT(metric_name)              \
  if constexpr (NoMetrics::has_##metric_name()) { \
    FAIL();                                       \
  }

  PW_ALLOCATOR_METRICS_FOREACH(FAIL_IF_PRESENT);

#undef FAIL_IF_PRESENT
}

TEST(MetricsTest, GetMissingMetricsReturnsNotFound) {
  NoMetrics metrics;
  pw::StatusWithSize result;

#define EXPECT_GET_MISSING_IS_NOT_FOUND(metric_name) \
  result = metrics.get_##metric_name();              \
  EXPECT_EQ(result.status(), pw::Status::NotFound());

  PW_ALLOCATOR_METRICS_FOREACH(EXPECT_GET_MISSING_IS_NOT_FOUND);

#undef EXPECT_GET_MISSING_IS_NOT_FOUND
}

TEST(MetricsTest, AllMetricsPresent) {
#define EXPECT_METRIC_PRESENT(metric_name) \
  EXPECT_TRUE(AllMetrics::has_##metric_name());

  PW_ALLOCATOR_METRICS_FOREACH(EXPECT_METRIC_PRESENT);

#undef EXPECT_METRIC_PRESENT
}

TEST(MetricsTest, AllMetricsEnabled) {
#define EXPECT_METRIC_ENABLED(metric_name) \
  EXPECT_TRUE(AllMetrics::metric_name##_enabled());

  PW_ALLOCATOR_METRICS_FOREACH(EXPECT_METRIC_ENABLED);
  EXPECT_TRUE(pw::allocator::internal::AnyEnabled<AllMetrics>());

#undef EXPECT_METRIC_ENABLED
}

TEST(MetricsTest, GetPresentMetricsReturnsOk) {
  AllMetrics metrics;
  pw::StatusWithSize result;

#define EXPECT_GET_PRESENT_IS_OK(metric_name) \
  result = metrics.get_##metric_name();       \
  EXPECT_EQ(result.status(), pw::OkStatus()); \
  EXPECT_EQ(result.size(), 0u);

  PW_ALLOCATOR_METRICS_FOREACH(EXPECT_GET_PRESENT_IS_OK);

#undef EXPECT_GET_PRESENT_IS_OK
}

TEST(MetricsTest, ReallocatingFlagSuppressesIncrements) {
  pw::allocator::internal::Metrics<AllMetrics> metrics(0);
  metrics.set_reallocating(true);

  metrics.IncrementAllocations();
  EXPECT_EQ(metrics.metrics().num_allocations.value(), 0u);

  metrics.IncrementDeallocations();
  EXPECT_EQ(metrics.metrics().num_deallocations.value(), 0u);

  metrics.IncrementResizes();
  EXPECT_EQ(metrics.metrics().num_resizes.value(), 0u);

  metrics.RecordFailure(100);
  EXPECT_EQ(metrics.metrics().num_failures.value(), 0u);
  EXPECT_EQ(metrics.metrics().unfulfilled_bytes.value(), 0u);

  metrics.set_reallocating(false);

  metrics.IncrementAllocations();
  EXPECT_EQ(metrics.metrics().num_allocations.value(), 1u);

  metrics.IncrementDeallocations();
  EXPECT_EQ(metrics.metrics().num_deallocations.value(), 1u);

  metrics.IncrementResizes();
  EXPECT_EQ(metrics.metrics().num_resizes.value(), 1u);

  metrics.RecordFailure(100);
  EXPECT_EQ(metrics.metrics().num_failures.value(), 1u);
  EXPECT_EQ(metrics.metrics().unfulfilled_bytes.value(), 100u);
}

}  // namespace
