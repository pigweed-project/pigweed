// Copyright 2025 The Pigweed Authors
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

#include "pw_metric/metric_service_nanopb.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include "pw_metric/config.h"
#include "pw_metric/metric.h"
#include "pw_metric/metric_walker.h"
#include "pw_metric/nanopb_metric_writer.h"
#include "pw_metric_proto/metric_service.pb.h"
#include "pw_rpc/nanopb/test_method_context.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

namespace pw::metric {
namespace {

#define GET_METHOD_CONTEXT \
  PW_NANOPB_TEST_METHOD_CONTEXT(MetricService, Get, 4, 256)

#define WALK_METHOD_CONTEXT PW_NANOPB_TEST_METHOD_CONTEXT(MetricService, Walk)

TEST(MetricService, EmptyGroupAndNoMetrics) {
  // Empty root group.
  PW_METRIC_GROUP(root, "/");

  // Run the RPC and ensure it completes.
  GET_METHOD_CONTEXT context(root.metrics(), root.children());
  context.call({});
  EXPECT_TRUE(context.done());
  EXPECT_EQ(OkStatus(), context.status());

  // No metrics should be in the response.
  EXPECT_EQ(0u, context.responses().size());
}

TEST(MetricService, FlatMetricsNoGroupsOneResponseOnly) {
  // Set up a one-group suite of metrics.
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, a, "a", 1.0);
  PW_METRIC(root, b, "b", 1.0);
  PW_METRIC(root, c, "c", 1.0);
  PW_METRIC(root, d, "d", 1.0);
  PW_METRIC(root, e, "e", 1.0);

  // Run the RPC and ensure it completes.
  GET_METHOD_CONTEXT context(root.metrics(), root.children());
  context.call({});
  EXPECT_TRUE(context.done());
  EXPECT_EQ(OkStatus(), context.status());

  // All of the responses should have fit in one proto.
  EXPECT_EQ(1u, context.responses().size());
  EXPECT_EQ(5, context.responses()[0].metrics_count);
}

#if PW_METRIC_CONFIG_ENABLE_64BIT
TEST(MetricService, OneGroupOneUint64Metric) {
  PW_METRIC_GROUP(root, "/");
  PW_METRIC_TYPED(root, a, "a", uint64_t, 3ULL);

  GET_METHOD_CONTEXT context(root.metrics(), root.children());
  context.call({});
  EXPECT_TRUE(context.done());
  EXPECT_EQ(OkStatus(), context.status());

  EXPECT_EQ(1u, context.responses().size());
  EXPECT_EQ(1, context.responses()[0].metrics_count);
  EXPECT_EQ(3ULL, context.responses()[0].metrics[0].value.as_uint64);
  EXPECT_EQ(pw_metric_proto_Metric_as_uint64_tag,
            context.responses()[0].metrics[0].which_value);
}

TEST(MetricService, OneGroupOneInt64Metric) {
  PW_METRIC_GROUP(root, "/");
  PW_METRIC_TYPED(root, a, "a", int64_t, -3LL);

  GET_METHOD_CONTEXT context(root.metrics(), root.children());
  context.call({});
  EXPECT_TRUE(context.done());
  EXPECT_EQ(OkStatus(), context.status());

  EXPECT_EQ(1u, context.responses().size());
  EXPECT_EQ(1, context.responses()[0].metrics_count);
  EXPECT_EQ(-3LL, context.responses()[0].metrics[0].value.as_int64);
  EXPECT_EQ(pw_metric_proto_Metric_as_int64_tag,
            context.responses()[0].metrics[0].which_value);
}

TEST(MetricService, MixedMetricsWith64Bit) {
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, a, "a", 1u);
  PW_METRIC(root, b, "b", 2.0f);
  PW_METRIC_TYPED(root, c, "c", uint64_t, 3ULL);
  PW_METRIC_TYPED(root, d, "d", int64_t, -3LL);

  GET_METHOD_CONTEXT context(root.metrics(), root.children());
  context.call({});
  EXPECT_TRUE(context.done());
  EXPECT_EQ(OkStatus(), context.status());

  EXPECT_EQ(1u, context.responses().size());
  EXPECT_EQ(4, context.responses()[0].metrics_count);

  int64_t metric_sum = 0;
  for (int i = 0; i < context.responses()[0].metrics_count; ++i) {
    const auto& m = context.responses()[0].metrics[i];
    if (m.which_value == pw_metric_proto_Metric_as_int_tag) {
      metric_sum += m.value.as_int;
    } else if (m.which_value == pw_metric_proto_Metric_as_uint64_tag) {
      metric_sum += m.value.as_uint64;
    } else if (m.which_value == pw_metric_proto_Metric_as_int64_tag) {
      metric_sum += m.value.as_int64;
    }
  }
  EXPECT_EQ(1LL, metric_sum);
}
#endif  // PW_METRIC_CONFIG_ENABLE_64BIT

TEST(MetricService, MixedMetrics) {
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, a, "a", 1u);
  PW_METRIC(root, b, "b", 2.0f);

  GET_METHOD_CONTEXT context(root.metrics(), root.children());
  context.call({});
  EXPECT_TRUE(context.done());
  EXPECT_EQ(OkStatus(), context.status());

  EXPECT_EQ(1u, context.responses().size());
  EXPECT_EQ(2, context.responses()[0].metrics_count);

  uint32_t metric_sum = 0;
  for (int i = 0; i < context.responses()[0].metrics_count; ++i) {
    const auto& m = context.responses()[0].metrics[i];
    if (m.which_value == pw_metric_proto_Metric_as_int_tag) {
      metric_sum += m.value.as_int;
    }
  }
  EXPECT_EQ(1u, metric_sum);
}

TEST(MetricService, NestedGroupsButOnlyOneBatch) {
  // Set up a nested group of metrics that will fit in the default batch (10).
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, a, "a", 1.0);
  PW_METRIC(root, b, "b", 1.0);
  PW_METRIC(root, c, "c", 1.0);

  PW_METRIC_GROUP(inner, "inner");
  PW_METRIC(inner, x, "x", 1.0);
  PW_METRIC(inner, y, "y", 1.0);
  PW_METRIC(inner, z, "z", 1.0);

  root.Add(inner);

  // Run the RPC and ensure it completes.
  GET_METHOD_CONTEXT context(root.metrics(), root.children());
  context.call({});
  EXPECT_TRUE(context.done());
  EXPECT_EQ(OkStatus(), context.status());

  // All of the responses should fit in one proto.
  EXPECT_EQ(1u, context.responses().size());
  EXPECT_EQ(6, context.responses()[0].metrics_count);
}

TEST(MetricService, NestedGroupsWithBatches) {
  // Set up a nested group of metrics that will not fit in a single batch.
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, a, "a", 1u);
  PW_METRIC(root, d, "d", 2u);
  PW_METRIC(root, f, "f", 3u);

  PW_METRIC_GROUP(inner_1, "inner1");
  PW_METRIC(inner_1, x, "x", 4u);
  PW_METRIC(inner_1, y, "y", 5u);
  PW_METRIC(inner_1, z, "z", 6u);

  PW_METRIC_GROUP(inner_2, "inner2");
  PW_METRIC(inner_2, p, "p", 7u);
  PW_METRIC(inner_2, q, "q", 8u);
  PW_METRIC(inner_2, r, "r", 9u);
  PW_METRIC(inner_2, s, "s", 10u);  // Note: Max # per response is 10.
  PW_METRIC(inner_2, t, "s", 11u);
  PW_METRIC(inner_2, u, "s", 12u);

  root.Add(inner_1);
  root.Add(inner_2);

  // Run the RPC and ensure it completes.
  GET_METHOD_CONTEXT context(root.metrics(), root.children());
  context.call({});
  EXPECT_TRUE(context.done());
  EXPECT_EQ(OkStatus(), context.status());

  // The response had to be split into two parts; check that they have the
  // appropriate sizes.
  EXPECT_EQ(2u, context.responses().size());
  EXPECT_EQ(10, context.responses()[0].metrics_count);
  EXPECT_EQ(2, context.responses()[1].metrics_count);

  // The metrics are the numbers 1..12; sum them and compare.
  uint32_t metric_sum = 0;
  for (const auto& response : context.responses()) {
    for (unsigned i = 0; i < response.metrics_count; ++i) {
      metric_sum += response.metrics[i].value.as_int;
    }
  }
  EXPECT_EQ(78u, metric_sum);

  // TODO(keir): Properly check all the fields.
}

bool TokenPathsMatch(uint32_t expected_token_path[5],
                     const pw_metric_proto_Metric& metric) {
  // Calculate length of expected token & compare.
  int expected_length = 0;
  while (expected_token_path[expected_length]) {
    expected_length++;
  }
  if (expected_length != metric.token_path_count) {
    return false;
  }

  // Lengths match; so search the tokens themselves.
  for (int i = 0; i < expected_length; ++i) {
    if (expected_token_path[i] != metric.token_path[i]) {
      return false;
    }
  }
  return true;
}

TEST(MetricService, TokenPaths) {
  // Set up a nested group of metrics that will not fit in a single batch.
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, a, "a", 1u);

  PW_METRIC_GROUP(inner_1, "inner1");
  PW_METRIC(inner_1, x, "x", 4u);
  PW_METRIC(inner_1, z, "z", 6u);

  PW_METRIC_GROUP(inner_2, "inner2");
  PW_METRIC(inner_2, p, "p", 7u);
  PW_METRIC(inner_2, u, "s", 12u);

  root.Add(inner_1);
  root.Add(inner_2);

  // Run the RPC and ensure it completes.
  GET_METHOD_CONTEXT context(root.metrics(), root.children());
  context.call({});
  EXPECT_TRUE(context.done());
  EXPECT_EQ(OkStatus(), context.status());

  // The metrics should fit in one batch.
  EXPECT_EQ(1u, context.responses().size());
  EXPECT_EQ(5, context.responses()[0].metrics_count);

  // Declare the token paths we expect to find.
  // Note: This depends on the token variables from the PW_METRIC*() macros.
  uint32_t expected_token_paths[5][5] = {
      {a_token, 0u},
      {inner_1_token, x_token, 0u},
      {inner_1_token, z_token, 0u},
      {inner_2_token, p_token, 0u},
      {inner_2_token, u_token, 0u},
  };

  // For each expected token, search through all returned metrics to find it.
  // The search is necessary since there is no guarantee of metric ordering.
  for (auto& expected_token_path : expected_token_paths) {
    int found_matches = 0;
    // Note: There should only be 1 response.
    for (const auto& response : context.responses()) {
      for (unsigned m = 0; m < response.metrics_count; ++m) {
        if (TokenPathsMatch(expected_token_path, response.metrics[m])) {
          found_matches++;
        }
      }
    }
    EXPECT_EQ(found_matches, 1);
  }
}

//
// Walk() RPC Tests
//

TEST(MetricService, Walk) {
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, a, "a", 1u);
  PW_METRIC(root, b, "b", 2u);
  PW_METRIC_GROUP(inner, "inner");
  PW_METRIC(inner, x, "x", 3u);
  root.Add(inner);

  WALK_METHOD_CONTEXT ctx{root.metrics(), root.children()};
  EXPECT_EQ(OkStatus(), ctx.call({}));
  const pw_metric_proto_WalkResponse& response = ctx.response();

  EXPECT_EQ(3u, response.metrics_count);
  EXPECT_TRUE(response.done);
  EXPECT_FALSE(response.has_cursor);
}

TEST(MetricService, WalkWithPagination) {
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, m0, "m0", 0u);
  PW_METRIC(root, m1, "m1", 1u);
  PW_METRIC(root, m2, "m2", 2u);
  PW_METRIC(root, m3, "m3", 3u);
  PW_METRIC(root, m4, "m4", 4u);
  PW_METRIC(root, m5, "m5", 5u);
  PW_METRIC(root, m6, "m6", 6u);
  PW_METRIC(root, m7, "m7", 7u);
  PW_METRIC(root, m8, "m8", 8u);
  PW_METRIC(root, m9, "m9", 9u);
  PW_METRIC(root, m10, "m10", 10u);

  WALK_METHOD_CONTEXT ctx{root.metrics(), root.children()};

  // First page. The nanopb response can hold a max of 10 metrics.
  // Because metrics are pushed to the front of the intrusive list, the walk
  // will process them in reverse order of declaration (m10, m9, ..., m1).
  EXPECT_EQ(OkStatus(), ctx.call({}));
  const pw_metric_proto_WalkResponse& response1 = ctx.response();
  EXPECT_EQ(10u, response1.metrics_count);
  EXPECT_FALSE(response1.done);
  EXPECT_TRUE(response1.has_cursor);

  // The cursor should be the address of the next metric to process (m0).
  EXPECT_EQ(response1.cursor,
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&m0)));

  // Second page.
  pw_metric_proto_WalkRequest request = pw_metric_proto_WalkRequest_init_zero;
  request.has_cursor = true;
  request.cursor = response1.cursor;
  EXPECT_EQ(OkStatus(), ctx.call(request));
  const pw_metric_proto_WalkResponse& response2 = ctx.response();
  EXPECT_EQ(1u, response2.metrics_count);
  EXPECT_TRUE(response2.done);
  EXPECT_FALSE(response2.has_cursor);
}

TEST(MetricService, WalkWithInvalidCursor) {
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, a, "a", 1u);

  WALK_METHOD_CONTEXT ctx{root.metrics(), root.children()};

  pw_metric_proto_WalkRequest request = pw_metric_proto_WalkRequest_init_zero;
  request.has_cursor = true;
  request.cursor = 12345;  // An invalid, non-zero address.

  EXPECT_EQ(Status::NotFound(), ctx.call(request));
}

struct HasAMetric {
  PW_METRIC(m0, "m0", 0u);
};

// This test validates the core reason for using an address-based cursor.
// It ensures that if the metric tree is mutated between paginated calls,
// a stale cursor pointing to a now-deleted metric fails gracefully.
TEST(MetricService, WalkWithStaleCursorAfterMutation) {
  PW_METRIC_GROUP(root, "/");

  std::optional<HasAMetric> m0_object(std::in_place);
  root.Add(m0_object->m0);

  PW_METRIC(root, m1, "m1", 1u);
  PW_METRIC(root, m2, "m2", 2u);
  PW_METRIC(root, m3, "m3", 3u);
  PW_METRIC(root, m4, "m4", 4u);
  PW_METRIC(root, m5, "m5", 5u);
  PW_METRIC(root, m6, "m6", 6u);
  PW_METRIC(root, m7, "m7", 7u);
  PW_METRIC(root, m8, "m8", 8u);
  PW_METRIC(root, m9, "m9", 9u);
  PW_METRIC(root, m10, "m10", 10u);

  WALK_METHOD_CONTEXT ctx{root.metrics(), root.children()};

  // First page: The response can hold a max of 10 metrics.
  EXPECT_EQ(OkStatus(), ctx.call({}));
  const pw_metric_proto_WalkResponse& response1 = ctx.response();
  EXPECT_EQ(10u, response1.metrics_count);
  EXPECT_FALSE(response1.done);
  EXPECT_TRUE(response1.has_cursor);

  // The cursor will point to the next metric to be processed (m0).
  EXPECT_EQ(response1.cursor,
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&m0_object->m0)));

  // Mutate the tree: remove the metric the cursor points to.
  m0_object.reset();

  // Second page: Use the now-stale cursor.
  pw_metric_proto_WalkRequest request = pw_metric_proto_WalkRequest_init_zero;
  request.has_cursor = true;
  request.cursor = response1.cursor;

  // This call must fail because the metric at the cursor address is gone.
  EXPECT_EQ(Status::NotFound(), ctx.call(request));
}

TEST(MetricService, WalkPaginatesCorrectlyWhenPageIsFull) {
  PW_METRIC_GROUP(root, "/");
  // The nanopb response can hold 10 metrics. Create 11 to force pagination.
  PW_METRIC(root, m0, "m0", 0u);
  PW_METRIC(root, m1, "m1", 1u);
  PW_METRIC(root, m2, "m2", 2u);
  PW_METRIC(root, m3, "m3", 3u);
  PW_METRIC(root, m4, "m4", 4u);
  PW_METRIC(root, m5, "m5", 5u);
  PW_METRIC(root, m6, "m6", 6u);
  PW_METRIC(root, m7, "m7", 7u);
  PW_METRIC(root, m8, "m8", 8u);
  PW_METRIC(root, m9, "m9", 9u);
  PW_METRIC(root, m10, "m10", 10u);

  WALK_METHOD_CONTEXT ctx{root.metrics(), root.children()};

  // First page should contain exactly 10 metrics.
  EXPECT_EQ(OkStatus(), ctx.call({}));
  const pw_metric_proto_WalkResponse& response1 = ctx.response();
  EXPECT_EQ(10u, response1.metrics_count);
  EXPECT_FALSE(response1.done);
  EXPECT_TRUE(response1.has_cursor);
  EXPECT_EQ(response1.cursor,
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&m0)));
}

#if GTEST_HAS_DEATH_TEST
TEST(MetricService, WalkWithMaxDepthExceeded) {
  PW_METRIC_GROUP(root, "l0");
  PW_METRIC_GROUP(root, l1, "l1");
  PW_METRIC_GROUP(l1, l2, "l2");
  PW_METRIC_GROUP(l2, l3, "l3");
  PW_METRIC_GROUP(l3, l4, "l4");
  PW_METRIC(l4, a, "a", 1u);

  WALK_METHOD_CONTEXT ctx{root.metrics(), root.children()};
  EXPECT_DEATH_IF_SUPPORTED(static_cast<void>(ctx.call({})), ".*");
}
#endif  // GTEST_HAS_DEATH_TEST

//
// NanopbMetricWriter Tests
//

TEST(NanopbMetricWriter, BasicWalk) {
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, a, "a", 123u);
  PW_METRIC(root, b, "b", 456.0f);
  PW_METRIC_GROUP(inner, "inner");
  PW_METRIC(inner, x, "x", 789u);
  root.Add(inner);

  // Use WalkResponse as a test harness for the parent struct.
  pw_metric_proto_WalkResponse response =
      pw_metric_proto_WalkResponse_init_zero;
  size_t metric_limit = 5;  // More than the total number of metrics.

  NanopbMetricWriter writer(
      response.metrics, response.metrics_count, metric_limit);
  MetricWalker walker(writer);

  Status walk_status = walker.Walk(root);
  ASSERT_EQ(OkStatus(), walk_status);

  // The walk finished, so the status is OK.
  EXPECT_EQ(metric_limit, 2u);
  EXPECT_EQ(response.metrics_count, 3u);
}

TEST(NanopbMetricWriter, StopsAtMetricLimit) {
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, a, "a", 123u);
  PW_METRIC(root, b, "b", 456.0f);
  PW_METRIC(root, x, "x", 789u);

  pw_metric_proto_WalkResponse response =
      pw_metric_proto_WalkResponse_init_zero;
  size_t metric_limit = 2;

  NanopbMetricWriter writer(
      response.metrics, response.metrics_count, metric_limit);
  MetricWalker walker(writer);

  // The writer will return ResourceExhausted when the limit hits 0.
  Status walk_status = walker.Walk(root);
  ASSERT_EQ(Status::ResourceExhausted(), walk_status);

  // Verify the limit was reached and the correct number of metrics were
  // written.
  EXPECT_EQ(metric_limit, 0u);
  EXPECT_EQ(response.metrics_count, 2u);
}

TEST(NanopbMetricWriter, StopsAtBufferLimit) {
  PW_METRIC_GROUP(root, "/");
  // The nanopb WalkResponse struct can hold 10 metrics.
  // We add 11, so the walk should stop.
  // Walk order is m10, m9, ... m1, m0.
  PW_METRIC(root, m0, "m0", 0u);
  PW_METRIC(root, m1, "m1", 1u);
  PW_METRIC(root, m2, "m2", 2u);
  PW_METRIC(root, m3, "m3", 3u);
  PW_METRIC(root, m4, "m4", 4u);
  PW_METRIC(root, m5, "m5", 5u);
  PW_METRIC(root, m6, "m6", 6u);
  PW_METRIC(root, m7, "m7", 7u);
  PW_METRIC(root, m8, "m8", 8u);
  PW_METRIC(root, m9, "m9", 9u);
  PW_METRIC(root, m10, "m10", 10u);

  pw_metric_proto_WalkResponse response =
      pw_metric_proto_WalkResponse_init_zero;
  // Limit is high, buffer is the constraint.
  size_t metric_limit = 20;

  NanopbMetricWriter writer(
      response.metrics, response.metrics_count, metric_limit);
  MetricWalker walker(writer);

  // The writer will return ResourceExhausted when it tries to write the 11th
  // metric (m0) into the 10-slot array.
  Status walk_status = walker.Walk(root);
  ASSERT_EQ(Status::ResourceExhausted(), walk_status);

  // Verify that the metric limit was NOT the cause of the stop.
  // The walker wrote 10 metrics.
  EXPECT_EQ(metric_limit, 10u);
  EXPECT_EQ(response.metrics_count, 10u);
}

// Tests that the buffer limit is the constraint when the metric limit is
// set to "no limit" (i.e., SIZE_MAX).
TEST(NanopbMetricWriter, StopsAtBufferLimitWhenMetricLimitIsMax) {
  PW_METRIC_GROUP(root, "/");
  // The nanopb WalkResponse struct can hold 10 metrics.
  // We add 11, so the walk should stop.
  // Walk order is m10, m9, ... m1, m0.
  PW_METRIC(root, m0, "m0", 0u);
  PW_METRIC(root, m1, "m1", 1u);
  PW_METRIC(root, m2, "m2", 2u);
  PW_METRIC(root, m3, "m3", 3u);
  PW_METRIC(root, m4, "m4", 4u);
  PW_METRIC(root, m5, "m5", 5u);
  PW_METRIC(root, m6, "m6", 6u);
  PW_METRIC(root, m7, "m7", 7u);
  PW_METRIC(root, m8, "m8", 8u);
  PW_METRIC(root, m9, "m9", 9u);
  PW_METRIC(root, m10, "m10", 10u);

  pw_metric_proto_WalkResponse response =
      pw_metric_proto_WalkResponse_init_zero;
  // Set a "no limit" metric limit.
  size_t metric_limit = std::numeric_limits<size_t>::max();

  NanopbMetricWriter writer(
      response.metrics, response.metrics_count, metric_limit);
  MetricWalker walker(writer);

  // The writer will return ResourceExhausted when it tries to write the 11th
  // metric (m0) into the 10-slot array.
  Status walk_status = walker.Walk(root);
  ASSERT_EQ(Status::ResourceExhausted(), walk_status);

  // Verify that the metric limit was NOT the cause of the stop.
  // The walker wrote 10 metrics and decremented the limit.
  EXPECT_EQ(metric_limit, std::numeric_limits<size_t>::max() - 10);
  EXPECT_EQ(response.metrics_count, 10u);
}

// Tests that the walker correctly does nothing (and returns OK) when
// walking a metric tree that has no metrics.
TEST(NanopbMetricWriter, WalksEmptyRoot) {
  PW_METRIC_GROUP(root, "/");
  PW_METRIC_GROUP(inner, "empty_child");
  root.Add(inner);

  pw_metric_proto_WalkResponse response =
      pw_metric_proto_WalkResponse_init_zero;
  size_t metric_limit = 5;

  NanopbMetricWriter writer(
      response.metrics, response.metrics_count, metric_limit);
  MetricWalker walker(writer);

  Status walk_status = walker.Walk(root);
  ASSERT_EQ(OkStatus(), walk_status);

  EXPECT_EQ(metric_limit, 5u);
  EXPECT_EQ(response.metrics_count, 0u);
}

}  // namespace
}  // namespace pw::metric
