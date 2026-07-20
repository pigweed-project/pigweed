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

#include "pw_metric/metric_service_pwpb.h"

#include <limits>
#include <optional>
#include <vector>

#include "pw_bytes/span.h"
#include "pw_function/function.h"
#include "pw_metric/list.h"
#include "pw_metric/pwpb_metric_writer.h"
#include "pw_metric_proto/metric_service.pwpb.h"
#include "pw_protobuf/decoder.h"
#include "pw_protobuf/encoder.h"
#include "pw_protobuf/serialized_size.h"
#include "pw_rpc/pwpb/test_method_context.h"
#include "pw_rpc/raw/test_method_context.h"
#include "pw_rpc/test_helpers.h"
#include "pw_span/span.h"
#include "pw_stream/memory_stream.h"
#include "pw_unit_test/framework.h"
#include "pw_unit_test/status_macros.h"

namespace pw::metric {
namespace {

size_t CountEncodedMetrics(ConstByteSpan serialized_path) {
  protobuf::Decoder decoder(serialized_path);
  size_t num_metrics = 0;
  while (decoder.Next().ok()) {
    switch (decoder.FieldNumber()) {
      case static_cast<uint32_t>(
          proto::pwpb::MetricResponse::Fields::kMetrics): {
        num_metrics++;
      }
    }
  }
  return num_metrics;
}

size_t SumMetricInts(ConstByteSpan serialized_path) {
  protobuf::Decoder decoder(serialized_path);
  size_t metrics_sum = 0;
  while (decoder.Next().ok()) {
    switch (decoder.FieldNumber()) {
      case static_cast<uint32_t>(proto::pwpb::Metric::Fields::kAsInt): {
        uint32_t metric_value;
        PW_TEST_EXPECT_OK(decoder.ReadUint32(&metric_value));
        metrics_sum += metric_value;
        break;
      }
    }
  }
  return metrics_sum;
}

size_t GetMetricsSum(ConstByteSpan serialized_metric_buffer) {
  protobuf::Decoder decoder(serialized_metric_buffer);
  size_t metrics_sum = 0;
  while (decoder.Next().ok()) {
    switch (decoder.FieldNumber()) {
      case static_cast<uint32_t>(
          proto::pwpb::MetricResponse::Fields::kMetrics): {
        ConstByteSpan metric_buffer;
        PW_TEST_EXPECT_OK(decoder.ReadBytes(&metric_buffer));
        metrics_sum += SumMetricInts(metric_buffer);
      }
    }
  }
  return metrics_sum;
}

//
// Legacy Get() RPC Tests
//

TEST(MetricService, EmptyGroupAndNoMetrics) {
  // Empty root group.
  PW_METRIC_GROUP(root, "/");

  // Run the RPC and ensure it completes.

  PW_RAW_TEST_METHOD_CONTEXT(MetricService, Get)
  ctx{root.metrics(), root.children()};
  ctx.call({});
  EXPECT_TRUE(ctx.done());
  PW_TEST_EXPECT_OK(ctx.status());

  // No metrics should be in the response.
  EXPECT_EQ(0u, ctx.responses().size());
}

TEST(MetricService, OneGroupOneMetric) {
  // One root group with one metric.
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, a, "a", 3u);

  // Run the RPC and ensure it completes.

  PW_RAW_TEST_METHOD_CONTEXT(MetricService, Get)
  ctx{root.metrics(), root.children()};
  ctx.call({});
  EXPECT_TRUE(ctx.done());
  PW_TEST_EXPECT_OK(ctx.status());

  // One metric should be in the response.
  EXPECT_EQ(1u, ctx.responses().size());

  // Sum should be 3.
  EXPECT_EQ(3u, GetMetricsSum(ctx.responses()[0]));
}

TEST(MetricService, OneGroupFiveMetrics) {
  // One root group with five metrics.
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, a, "a", 1u);
  PW_METRIC(root, b, "b", 2u);  // Note: Max # per response is 3.
  PW_METRIC(root, c, "c", 3u);
  PW_METRIC(root, x, "x", 4u);
  PW_METRIC(root, y, "y", 5u);

  // Run the RPC and ensure it completes.

  PW_RAW_TEST_METHOD_CONTEXT(MetricService, Get)
  ctx{root.metrics(), root.children()};
  ctx.call({});
  EXPECT_TRUE(ctx.done());
  PW_TEST_EXPECT_OK(ctx.status());

  // Two metrics should be in the response.
  EXPECT_EQ(2u, ctx.responses().size());
  EXPECT_EQ(3u, CountEncodedMetrics(ctx.responses()[0]));
  EXPECT_EQ(2u, CountEncodedMetrics(ctx.responses()[1]));

  // The metrics are the numbers 1..5; sum them and compare.
  EXPECT_EQ(
      15u,
      GetMetricsSum(ctx.responses()[0]) + GetMetricsSum(ctx.responses()[1]));
}

TEST(MetricService, MixedMetrics) {
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, a, "a", 1u);
  PW_METRIC(root, b, "b", 2.0f);

  PW_RAW_TEST_METHOD_CONTEXT(MetricService, Get)
  ctx{root.metrics(), root.children()};
  ctx.call({});
  EXPECT_TRUE(ctx.done());
  PW_TEST_EXPECT_OK(ctx.status());

  EXPECT_EQ(1u, ctx.responses().size());
  EXPECT_EQ(2u, CountEncodedMetrics(ctx.responses()[0]));
  EXPECT_EQ(1u, GetMetricsSum(ctx.responses()[0]));
}

TEST(MetricService, NestedGroupFiveMetrics) {
  // Set up a nested group of metrics.
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, a, "a", 1u);
  PW_METRIC(root, b, "b", 2u);

  PW_METRIC_GROUP(inner, "inner");
  PW_METRIC(root, x, "x", 3u);  // Note: Max # per response is 3.
  PW_METRIC(inner, y, "y", 4u);
  PW_METRIC(inner, z, "z", 5u);

  root.Add(inner);

  // Run the RPC and ensure it completes.

  PW_RAW_TEST_METHOD_CONTEXT(MetricService, Get)
  ctx{root.metrics(), root.children()};
  ctx.call({});
  EXPECT_TRUE(ctx.done());
  PW_TEST_EXPECT_OK(ctx.status());

  // Two metrics should be in the response.
  EXPECT_EQ(2u, ctx.responses().size());
  EXPECT_EQ(3u, CountEncodedMetrics(ctx.responses()[0]));
  EXPECT_EQ(2u, CountEncodedMetrics(ctx.responses()[1]));

  EXPECT_EQ(
      15u,
      GetMetricsSum(ctx.responses()[0]) + GetMetricsSum(ctx.responses()[1]));
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
  PW_METRIC(inner_2, s, "s", 10u);  // Note: Max # per response is 3.
  PW_METRIC(inner_2, t, "t", 11u);
  PW_METRIC(inner_2, u, "u", 12u);

  root.Add(inner_1);
  root.Add(inner_2);

  // Run the RPC and ensure it completes.
  PW_RAW_TEST_METHOD_CONTEXT(MetricService, Get)
  ctx{root.metrics(), root.children()};
  ctx.call({});
  EXPECT_TRUE(ctx.done());
  PW_TEST_EXPECT_OK(ctx.status());

  // The response had to be split into four parts; check that they have the
  // appropriate sizes.
  EXPECT_EQ(4u, ctx.responses().size());
  EXPECT_EQ(3u, CountEncodedMetrics(ctx.responses()[0]));
  EXPECT_EQ(3u, CountEncodedMetrics(ctx.responses()[1]));
  EXPECT_EQ(3u, CountEncodedMetrics(ctx.responses()[2]));
  EXPECT_EQ(3u, CountEncodedMetrics(ctx.responses()[3]));

  EXPECT_EQ(78u,
            GetMetricsSum(ctx.responses()[0]) +
                GetMetricsSum(ctx.responses()[1]) +
                GetMetricsSum(ctx.responses()[2]) +
                GetMetricsSum(ctx.responses()[3]));
}

TEST(MetricService, MaxDepth4) {
  // MetricWalker internally uses: Vector<Token, /*capacity=*/4> path_;
  // pw.metric.proto.Metric.token_path max_count:4

  PW_METRIC_GROUP(global_group_lvl1, "level1");
  GroupList global_groups;    // Simulate pw::metric::global_groups
  MetricList global_metrics;  // Simulate pw::metric::global_metrics

  global_groups.push_front(global_group_lvl1);

  PW_METRIC_GROUP(global_group_lvl1, group_lvl2, "level2");
  PW_METRIC_GROUP(group_lvl2, group_lvl3, "level3");

  // Note: kMaxNumPackedEntries = 3
  PW_METRIC(group_lvl3, metric_a, "metric A", 1u);
  PW_METRIC(group_lvl3, metric_b, "metric B", 2u);
  PW_METRIC(group_lvl3, metric_c, "metric C", 3u);

  // Run the RPC and ensure it completes.
  PW_RAW_TEST_METHOD_CONTEXT(MetricService, Get)
  ctx{global_metrics, global_groups};
  ctx.call({});
  EXPECT_TRUE(ctx.done());
  PW_TEST_EXPECT_OK(ctx.status());

  // Verify the response
  EXPECT_EQ(1u, ctx.responses().size());
  EXPECT_EQ(3u, CountEncodedMetrics(ctx.responses()[0]));
  EXPECT_EQ(6u, GetMetricsSum(ctx.responses()[0]));
}

//
// Walk() RPC Tests
//

// Helper function to precisely calculate the encoded size of a metric as it
// would appear in a WalkResponse. This is critical for setting up deterministic
// pagination tests.
size_t GetEncodedMetricSize(const Metric& metric, const Vector<Token>& path) {
  // 1) Calculate the size of the *nested* Metric message's payload.
  size_t metric_payload_size = 0;

  // The 'token_path' field is written as REPEATED (not packed).
  // Size = (tag + fixed32_value) * num_tokens
  metric_payload_size +=
      path.size() *
      protobuf::SizeOfFieldFixed32(proto::pwpb::Metric::Fields::kTokenPath);

  if (metric.is_float()) {
    metric_payload_size +=
        protobuf::SizeOfFieldFloat(proto::pwpb::Metric::Fields::kAsFloat);
  } else if (metric.is_uint32()) {
    const auto& m = static_cast<const TypedMetric<uint32_t>&>(metric);
    metric_payload_size += protobuf::SizeOfFieldUint32(
        proto::pwpb::Metric::Fields::kAsInt, m.value());
  }

  // 2) Calculate the size of the *entire* nested Metric message, including
  // its tag and length prefix, as a field in the parent WalkResponse.
  return protobuf::SizeOfDelimitedField(
      proto::pwpb::WalkResponse::Fields::kMetrics,
      static_cast<uint32_t>(metric_payload_size));
}

size_t CountMetricsInWalkResponse(ConstByteSpan serialized_response) {
  protobuf::Decoder decoder(serialized_response);
  size_t num_metrics = 0;
  while (decoder.Next().ok()) {
    if (decoder.FieldNumber() ==
        static_cast<uint32_t>(proto::pwpb::WalkResponse::Fields::kMetrics)) {
      ++num_metrics;
    }
  }
  return num_metrics;
}

TEST(MetricService, Walk) {
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, a, "a", 1u);
  PW_METRIC(root, b, "b", 2u);
  PW_METRIC_GROUP(inner, "inner");
  PW_METRIC(inner, x, "x", 3u);
  root.Add(inner);

  PW_RAW_TEST_METHOD_CONTEXT(MetricService, Walk)
  ctx{root.metrics(), root.children()};

  // Manually encode the request.
  std::array<std::byte, 32> request_buffer;
  proto::pwpb::WalkRequest::MemoryEncoder request_encoder(request_buffer);
  PW_TEST_ASSERT_OK(request_encoder.Write({}));
  ctx.call(request_encoder);
  PW_TEST_EXPECT_OK(ctx.status());

  // Manually decode and iterate over the response.
  protobuf::Decoder decoder(ctx.response());
  size_t total_metrics = 0;
  bool done = false;
  bool has_cursor = false;

  while (decoder.Next().ok()) {
    switch (
        static_cast<proto::pwpb::WalkResponse::Fields>(decoder.FieldNumber())) {
      case proto::pwpb::WalkResponse::Fields::kMetrics:
        total_metrics++;
        break;
      case proto::pwpb::WalkResponse::Fields::kDone:
        PW_TEST_ASSERT_OK(decoder.ReadBool(&done));
        break;
      case proto::pwpb::WalkResponse::Fields::kCursor:
        has_cursor = true;
        break;
      default:
        break;
    }
  }

  EXPECT_EQ(3u, total_metrics);
  EXPECT_TRUE(done);
  EXPECT_FALSE(has_cursor);
}

TEST(MetricService, WalkWithPagination) {
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, m0, "m0", 0u);
  PW_METRIC(root, m1, "m1", 1u);
  PW_METRIC(root, m2, "m2", 2u);
  PW_METRIC(root, m3, "m3", 3u);
  PW_METRIC(root, m4, "m4", 4u);

  Vector<Token, 2> path;
  path.push_back(root.name());
  path.push_back(m0.name());  // Path is same for all metrics here.

  const size_t size_one_metric = GetEncodedMetricSize(m0, path);
  constexpr size_t kWalkResponseOverhead =
      protobuf::SizeOfFieldUint64(proto::pwpb::WalkResponse::Fields::kCursor);

  // The RPC framework reserves this many bytes for its own packet headers. This
  // was determined empirically through logging.
  constexpr size_t kRpcOverhead = 32;

  PW_RAW_TEST_METHOD_CONTEXT(MetricService, Walk)
  ctx{root.metrics(), root.children()};

  // Set the MTU to be large enough for exactly three metrics and the payload
  // overhead, plus the RPC overhead.
  const size_t payload_capacity = (3 * size_one_metric) + kWalkResponseOverhead;
  const size_t mtu = payload_capacity + kRpcOverhead;
  ctx.output().set_mtu(mtu);

  size_t total_metrics = 0;
  uint64_t cursor = 0;

  for (int i = 0; i < 5; ++i) {  // Loop to prevent infinite loops from bugs.
    std::array<std::byte, 32> request_buffer;
    proto::pwpb::WalkRequest::MemoryEncoder request_encoder(request_buffer);
    PW_TEST_ASSERT_OK(request_encoder.Write({.cursor = cursor}));
    ctx.call(request_encoder);
    PW_TEST_EXPECT_OK(ctx.status());

    total_metrics += CountMetricsInWalkResponse(ctx.response());

    protobuf::Decoder decoder(ctx.response());
    bool done = false;
    cursor = 0;

    while (decoder.Next().ok()) {
      switch (static_cast<proto::pwpb::WalkResponse::Fields>(
          decoder.FieldNumber())) {
        case proto::pwpb::WalkResponse::Fields::kMetrics:
          break;  // Already counted
        case proto::pwpb::WalkResponse::Fields::kDone:
          PW_TEST_ASSERT_OK(decoder.ReadBool(&done));
          break;
        case proto::pwpb::WalkResponse::Fields::kCursor:
          PW_TEST_ASSERT_OK(decoder.ReadUint64(&cursor));
          break;
        default:
          break;
      }
    }

    if (done) {
      EXPECT_EQ(cursor, 0u);
      break;
    }
    ctx.output().clear();
  }

  EXPECT_EQ(total_metrics, 5u);
}

TEST(MetricService, WalkWithInvalidCursor) {
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, a, "a", 1u);

  PW_RAW_TEST_METHOD_CONTEXT(MetricService, Walk)
  ctx{root.metrics(), root.children()};

  std::array<std::byte, 32> request_buffer;
  proto::pwpb::WalkRequest::MemoryEncoder request_encoder(request_buffer);
  PW_TEST_ASSERT_OK(request_encoder.Write({.cursor = 12345}));

  ctx.call(request_encoder);
  EXPECT_EQ(Status::NotFound(), ctx.status());
}

struct HasAMetric {
  PW_METRIC(m0, "m0", 0u);
};

TEST(MetricService, WalkWithStaleCursorAfterMutation) {
  PW_METRIC_GROUP(root, "/");
  std::optional<HasAMetric> m0_object(std::in_place);
  root.Add(m0_object->m0);
  PW_METRIC(root, m1, "m1", 1u);

  uint64_t response_cursor = 0;
  // Create a scope for the first RPC context.
  {
    PW_RAW_TEST_METHOD_CONTEXT(MetricService, Walk)
    ctx{root.metrics(), root.children()};

    // Set a small MTU to force pagination to occur after a single metric.
    constexpr size_t kRpcOverhead = 32;
    Vector<Token, 2> path;
    path.push_back(root.name());
    path.push_back(m1.name());
    const size_t size_m1 = GetEncodedMetricSize(m1, path);
    constexpr size_t kWalkResponseOverhead =
        protobuf::SizeOfFieldUint64(proto::pwpb::WalkResponse::Fields::kCursor);
    const size_t mtu = size_m1 + kWalkResponseOverhead + kRpcOverhead;
    ctx.output().set_mtu(mtu);

    // First page.
    std::array<std::byte, 32> request_buffer;
    proto::pwpb::WalkRequest::MemoryEncoder request_encoder(request_buffer);
    PW_TEST_ASSERT_OK(request_encoder.Write({}));
    ctx.call(request_encoder);
    PW_TEST_ASSERT_OK(ctx.status());

    protobuf::Decoder decoder(ctx.response());
    bool found_cursor = false;
    while (decoder.Next().ok()) {
      if (decoder.FieldNumber() ==
          static_cast<uint32_t>(proto::pwpb::WalkResponse::Fields::kCursor)) {
        PW_TEST_ASSERT_OK(decoder.ReadUint64(&response_cursor));
        found_cursor = true;
      }
    }
    ASSERT_TRUE(found_cursor);
  }

  // Due to push_front, the list order is [m1, m0]. The walker processes m1,
  // and the cursor for the next page points to m0.
  // Mutate the tree: remove the metric the cursor points to.
  m0_object.reset();

  // Second page: Use the now-stale cursor within a new context.
  {
    PW_RAW_TEST_METHOD_CONTEXT(MetricService, Walk)
    ctx{root.metrics(), root.children()};

    std::array<std::byte, 32> request_buffer;
    proto::pwpb::WalkRequest::MemoryEncoder request_encoder(request_buffer);
    PW_TEST_ASSERT_OK(request_encoder.Write({.cursor = response_cursor}));
    ctx.call(request_encoder);

    // This call must fail because the metric at the cursor address is gone.
    EXPECT_EQ(Status::NotFound(), ctx.status());
  }
}

TEST(MetricService, WalkPaginatesCorrectlyWhenPageIsFull) {
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, m0, "m0", 0u);
  PW_METRIC(root, m1, "m1", 1u);
  PW_METRIC(root, m2, "m2", 2u);

  Vector<Token, 2> path_m2;
  path_m2.push_back(root.name());
  path_m2.push_back(m2.name());

  Vector<Token, 2> path_m1;
  path_m1.push_back(root.name());
  path_m1.push_back(m1.name());

  const size_t size_m2 = GetEncodedMetricSize(m2, path_m2);
  const size_t size_m1 = GetEncodedMetricSize(m1, path_m1);

  PW_RAW_TEST_METHOD_CONTEXT(MetricService, Walk)
  ctx{root.metrics(), root.children()};

  // The RPC framework reserves this many bytes for its own packet headers.
  constexpr size_t kRpcOverhead = 32;
  constexpr size_t kWalkResponseOverhead =
      protobuf::SizeOfFieldUint64(proto::pwpb::WalkResponse::Fields::kCursor);

  // Set the MTU to be large enough for exactly two metrics and the payload
  // overhead, plus the RPC overhead. This forces pagination after two metrics.
  const size_t payload_capacity = size_m2 + size_m1 + kWalkResponseOverhead;
  const size_t mtu = payload_capacity + kRpcOverhead;
  ctx.output().set_mtu(mtu);

  // The first page should contain only the first two metrics processed (m2,
  // m1 because of intrusive list order).
  std::array<std::byte, 32> request_buffer;
  proto::pwpb::WalkRequest::MemoryEncoder request_encoder(request_buffer);
  PW_TEST_ASSERT_OK(request_encoder.Write({}));
  ctx.call(request_encoder);
  PW_TEST_ASSERT_OK(ctx.status());

  protobuf::Decoder decoder(ctx.response());
  size_t metric_count = 0;
  uint64_t cursor = 0;
  bool done = true;
  while (decoder.Next().ok()) {
    if (decoder.FieldNumber() ==
        static_cast<uint32_t>(proto::pwpb::WalkResponse::Fields::kMetrics)) {
      metric_count++;
    }
    if (decoder.FieldNumber() ==
        static_cast<uint32_t>(proto::pwpb::WalkResponse::Fields::kCursor)) {
      PW_TEST_ASSERT_OK(decoder.ReadUint64(&cursor));
    }
    if (decoder.FieldNumber() ==
        static_cast<uint32_t>(proto::pwpb::WalkResponse::Fields::kDone)) {
      PW_TEST_ASSERT_OK(decoder.ReadBool(&done));
    }
  }

  // Verify that only two metrics were included.
  EXPECT_EQ(metric_count, 2u);
  // Verify that the cursor points to the metric that didn't fit (m0).
  EXPECT_EQ(cursor, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&m0)));
  EXPECT_FALSE(done);
}

TEST(MetricService, WalkWithMaxDepth) {
  PW_METRIC_GROUP(root, "l0");
  PW_METRIC_GROUP(root, l1, "l1");
  PW_METRIC_GROUP(l1, l2, "l2");
  PW_METRIC(l2, a, "a", 1u);

  PW_RAW_TEST_METHOD_CONTEXT(MetricService, Walk)
  ctx{root.metrics(), root.children()};
  ctx.call({});
  PW_TEST_EXPECT_OK(ctx.status());
}

#if GTEST_HAS_DEATH_TEST
TEST(MetricService, WalkWithMaxDepthExceeded) {
  PW_METRIC_GROUP(root, "l0");
  PW_METRIC_GROUP(root, l1, "l1");
  PW_METRIC_GROUP(l1, l2, "l2");
  PW_METRIC_GROUP(l2, l3, "l3");
  PW_METRIC_GROUP(l3, l4, "l4");
  PW_METRIC(l4, a, "a", 1u);

  PW_RAW_TEST_METHOD_CONTEXT(MetricService, Walk)
  ctx{root.metrics(), root.children()};
  EXPECT_DEATH_IF_SUPPORTED(static_cast<void>(ctx.call({})), ".*");
}
#endif  // GTEST_HAS_DEATH_TEST

//
// PwpbMetricWriter Tests
//

TEST(PwpbMetricWriter, BasicWalk) {
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, a, "a", 123u);
  PW_METRIC(root, b, "b", 456.0f);
  PW_METRIC_GROUP(inner, "inner");
  PW_METRIC(inner, x, "x", 789u);
  root.Add(inner);

  std::array<std::byte, 256> encode_buffer;
  // Use WalkResponse as a mock parent message for testing the writer.
  proto::pwpb::WalkResponse::MemoryEncoder parent_encoder(encode_buffer);

  // Set limit to more than total metrics.
  size_t metric_limit = 5;

  PwpbMetricWriter writer(
      parent_encoder,
      static_cast<uint32_t>(proto::pwpb::WalkResponse::Fields::kMetrics),
      metric_limit);

  MetricWalker walker(writer);

  Status walk_status = walker.Walk(root);
  PW_TEST_ASSERT_OK(walk_status);

  // The walk finished, so the status is OK.
  EXPECT_EQ(metric_limit, 2u);
  EXPECT_EQ(CountMetricsInWalkResponse(
                pw::span(parent_encoder.data(), parent_encoder.size())),
            3u);
}

TEST(PwpbMetricWriter, StopsAtMetricLimit) {
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, a, "a", 123u);
  PW_METRIC(root, b, "b", 456.0f);
  PW_METRIC(root, x, "x", 789u);

  std::array<std::byte, 256> encode_buffer;
  proto::pwpb::WalkResponse::MemoryEncoder parent_encoder(encode_buffer);

  // Set limit to less than total metrics.
  size_t metric_limit = 2;

  PwpbMetricWriter writer(
      parent_encoder,
      static_cast<uint32_t>(proto::pwpb::WalkResponse::Fields::kMetrics),
      metric_limit);

  MetricWalker walker(writer);

  // The writer will return ResourceExhausted when the limit hits 0, which
  // stops the walker.
  Status walk_status = walker.Walk(root);
  ASSERT_EQ(Status::ResourceExhausted(), walk_status);

  // Verify the limit was reached and the correct number of metrics were
  // written.
  EXPECT_EQ(metric_limit, 0u);
  EXPECT_EQ(CountMetricsInWalkResponse(
                pw::span(parent_encoder.data(), parent_encoder.size())),
            2u);
}

TEST(PwpbMetricWriter, StopsAtBufferLimit) {
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, a, "a", 1u);
  PW_METRIC(root, b, "b", 2u);
  PW_METRIC(root, c, "c", 3u);

  Vector<Token, 2> path_c;
  path_c.push_back(root.name());
  path_c.push_back(c.name());

  Vector<Token, 2> path_b;
  path_b.push_back(root.name());
  path_b.push_back(b.name());

  Vector<Token, 2> path_a;
  path_a.push_back(root.name());
  path_a.push_back(a.name());

  // Calculate the on-wire size of each metric as a repeated field.
  const size_t size_of_c = GetEncodedMetricSize(c, path_c);
  const size_t size_of_b = GetEncodedMetricSize(b, path_b);
  const size_t size_of_a = GetEncodedMetricSize(a, path_a);

  // All metrics have 2 tokens and a 1-byte int, so they should be equal.
  ASSERT_EQ(size_of_c, size_of_b);
  ASSERT_EQ(size_of_b, size_of_a);

  // We must also reserve space for the largest non-metric field that the parent
  // (WalkResponse) might write after the walk.
  constexpr size_t kWalkResponseOverhead =
      protobuf::SizeOfFieldUint64(proto::pwpb::WalkResponse::Fields::kCursor);

  // Our buffer needs to fit:
  // - The WalkResponse's non-metric field overhead
  // - Exactly 2 metrics (c and b)
  const size_t kSmallBufferSize = kWalkResponseOverhead + size_of_c + size_of_b;

  ASSERT_LT(kSmallBufferSize,
            (kWalkResponseOverhead + size_of_c + size_of_b + size_of_a));

  std::vector<std::byte> encode_buffer(kSmallBufferSize);
  proto::pwpb::WalkResponse::MemoryEncoder parent_encoder(encode_buffer);

  // Set a high limit so that the buffer is the constraint.
  size_t metric_limit = 10;

  PwpbMetricWriter writer(
      parent_encoder,
      static_cast<uint32_t>(proto::pwpb::WalkResponse::Fields::kMetrics),
      metric_limit);

  MetricWalker walker(writer);

  Status walk_status = walker.Walk(root);
  ASSERT_EQ(Status::ResourceExhausted(), walk_status);

  // Verify that the metric limit was NOT the cause of the stop.
  // The walker will write 2 metrics (c and b) and stop before writing 'a'.
  EXPECT_EQ(metric_limit, 8u);
  size_t metrics_written = CountMetricsInWalkResponse(
      pw::span(parent_encoder.data(), parent_encoder.size()));
  EXPECT_GT(metrics_written, 0u);
  EXPECT_LT(metrics_written, 3u);
  EXPECT_EQ(metrics_written, 2u);
}

// Tests that the buffer limit is the constraint when the metric limit is
// set to "no limit" (i.e., SIZE_MAX).
TEST(PwpbMetricWriter, StopsAtBufferLimitWhenMetricLimitIsMax) {
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, a, "a", 1u);
  PW_METRIC(root, b, "b", 2u);
  PW_METRIC(root, c, "c", 3u);

  Vector<Token, 2> path_c;
  path_c.push_back(root.name());
  path_c.push_back(c.name());

  Vector<Token, 2> path_b;
  path_b.push_back(root.name());
  path_b.push_back(b.name());

  Vector<Token, 2> path_a;
  path_a.push_back(root.name());
  path_a.push_back(a.name());

  const size_t size_of_c = GetEncodedMetricSize(c, path_c);
  const size_t size_of_b = GetEncodedMetricSize(b, path_b);
  const size_t size_of_a = GetEncodedMetricSize(a, path_a);
  ASSERT_EQ(size_of_c, size_of_b);
  ASSERT_EQ(size_of_b, size_of_a);

  constexpr size_t kWalkResponseOverhead =
      protobuf::SizeOfFieldUint64(proto::pwpb::WalkResponse::Fields::kCursor);

  // Set buffer size to fit exactly 2 metrics and overhead.
  const size_t kSmallBufferSize = kWalkResponseOverhead + size_of_c + size_of_b;
  std::vector<std::byte> encode_buffer(kSmallBufferSize);
  proto::pwpb::WalkResponse::MemoryEncoder parent_encoder(encode_buffer);

  // Set a "no limit" metric limit.
  size_t metric_limit = std::numeric_limits<size_t>::max();

  PwpbMetricWriter writer(
      parent_encoder,
      static_cast<uint32_t>(proto::pwpb::WalkResponse::Fields::kMetrics),
      metric_limit);

  MetricWalker walker(writer);

  Status walk_status = walker.Walk(root);
  ASSERT_EQ(Status::ResourceExhausted(), walk_status);

  // Verify that the metric limit was NOT the cause of the stop.
  // The walker wrote 2 metrics (c and b) and decremented the limit.
  EXPECT_EQ(metric_limit, std::numeric_limits<size_t>::max() - 2);
  EXPECT_EQ(CountMetricsInWalkResponse(
                pw::span(parent_encoder.data(), parent_encoder.size())),
            2u);
}

// Tests that the walker correctly does nothing (and returns OK) when
// walking a metric tree that has no metrics.
TEST(PwpbMetricWriter, WalksEmptyRoot) {
  PW_METRIC_GROUP(root, "/");
  PW_METRIC_GROUP(inner, "empty_child");
  root.Add(inner);

  std::array<std::byte, 256> encode_buffer;
  proto::pwpb::WalkResponse::MemoryEncoder parent_encoder(encode_buffer);

  size_t metric_limit = 5;
  PwpbMetricWriter writer(
      parent_encoder,
      static_cast<uint32_t>(proto::pwpb::WalkResponse::Fields::kMetrics),
      metric_limit);
  MetricWalker walker(writer);

  Status walk_status = walker.Walk(root);
  PW_TEST_ASSERT_OK(walk_status);

  // No metrics were written, so limit is unchanged.
  EXPECT_EQ(metric_limit, 5u);
  EXPECT_EQ(CountMetricsInWalkResponse(
                pw::span(parent_encoder.data(), parent_encoder.size())),
            0u);
}

// Tests that if a single metric is larger than the buffer, the walk
// immediately stops with RESOURCE_EXHAUSTED and writes 0 metrics.
TEST(PwpbMetricWriter, StopsWhenSingleMetricIsTooLarge) {
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, a, "a_metric_with_a_name", 1u);

  // Calculate the true size of this metric.
  Vector<Token, 2> path_a;
  path_a.push_back(root.name());
  path_a.push_back(a.name());
  const size_t size_of_a = GetEncodedMetricSize(a, path_a);
  ASSERT_GT(size_of_a, 10u);

  // Create a buffer that is smaller than that single metric.
  const size_t kTooSmallBufferSize = size_of_a - 1;
  std::vector<std::byte> encode_buffer(kTooSmallBufferSize);
  proto::pwpb::WalkResponse::MemoryEncoder parent_encoder(encode_buffer);

  size_t metric_limit = 10;
  PwpbMetricWriter writer(
      parent_encoder,
      static_cast<uint32_t>(proto::pwpb::WalkResponse::Fields::kMetrics),
      metric_limit);
  MetricWalker walker(writer);

  // The first call to Write() should fail.
  Status walk_status = walker.Walk(root);
  ASSERT_EQ(Status::ResourceExhausted(), walk_status);

  // No metrics were written, limit is unchanged.
  EXPECT_EQ(metric_limit, 10u);
  EXPECT_EQ(CountMetricsInWalkResponse(
                pw::span(parent_encoder.data(), parent_encoder.size())),
            0u);
}

// Tests that the sizing logic is correct for mixed float and int value types.
TEST(PwpbMetricWriter, WalksWithMixedTypesAndExactBuffer) {
  PW_METRIC_GROUP(root, "/");
  // Walk order will be: float_metric, int_metric
  PW_METRIC(root, int_metric, "int_metric", 123u);
  PW_METRIC(root, float_metric, "float_metric", 456.0f);

  Vector<Token, 2> path_int;
  path_int.push_back(root.name());
  path_int.push_back(int_metric.name());
  const size_t size_of_int = GetEncodedMetricSize(int_metric, path_int);

  Vector<Token, 2> path_float;
  path_float.push_back(root.name());
  path_float.push_back(float_metric.name());
  const size_t size_of_float = GetEncodedMetricSize(float_metric, path_float);

  // This test relies on the float (fixed32) being larger than the int (varint).
  // (e.g., int_metric=123u takes 2 bytes; float takes 5 bytes).
  ASSERT_GT(size_of_float, size_of_int);

  // We must also reserve space for the largest non-metric field.
  constexpr size_t kWalkResponseOverhead =
      protobuf::SizeOfFieldUint64(proto::pwpb::WalkResponse::Fields::kCursor);

  // Create a buffer that fits exactly these two metrics and overhead.
  const size_t kExactBufferSize =
      size_of_float + size_of_int + kWalkResponseOverhead;

  std::vector<std::byte> encode_buffer(kExactBufferSize);
  proto::pwpb::WalkResponse::MemoryEncoder parent_encoder(encode_buffer);

  size_t metric_limit = 10;
  PwpbMetricWriter writer(
      parent_encoder,
      static_cast<uint32_t>(proto::pwpb::WalkResponse::Fields::kMetrics),
      metric_limit);
  MetricWalker walker(writer);

  Status walk_status = walker.Walk(root);
  PW_TEST_ASSERT_OK(walk_status);

  // 2 metrics were written.
  EXPECT_EQ(metric_limit, 8u);
  EXPECT_EQ(CountMetricsInWalkResponse(
                pw::span(parent_encoder.data(), parent_encoder.size())),
            2u);
}

//
// PwpbStreamingMetricWriter Tests
//

// A test stream::Writer that executes a callback before forwarding writes to
// an internal MemoryWriter. This is used to deterministically simulate a race
// condition.
class HookingWriter : public stream::NonSeekableWriter {
 public:
  HookingWriter(ByteSpan buffer, Function<void()>&& hook)
      : memory_writer_(buffer), hook_(std::move(hook)) {}

  ConstByteSpan WrittenData() const { return memory_writer_.WrittenData(); }

 private:
  Status DoWrite(ConstByteSpan data) override {
    if (first_write_ && hook_) {
      first_write_ = false;
      hook_();
    }
    // Forward the write to the internal MemoryWriter.
    return memory_writer_.Write(data);
  }

  stream::MemoryWriter memory_writer_;
  Function<void()> hook_;
  bool first_write_ = true;
};

TEST(PwpbStreamingMetricWriter, WriteIsAtomic) {
  PW_METRIC_GROUP(root, "/");
  constexpr uint32_t kInitialValue = 123u;
  constexpr uint32_t kUpdatedValue = 999u;
  PW_METRIC(root, atomic_metric, "atomic", kInitialValue);

  std::array<std::byte, 256> encode_buffer;
  HookingWriter writer_with_hook(encode_buffer, [&] {
    // This hook executes after the sizing pass of WriteNestedMessage but
    // before the writing pass has completed. We change the metric value here
    // to attempt to trigger the race condition.
    atomic_metric.Set(kUpdatedValue);
  });
  proto::pwpb::WalkResponse::StreamEncoder parent_encoder(writer_with_hook, {});

  PwpbStreamingMetricWriter writer(
      parent_encoder,
      static_cast<uint32_t>(proto::pwpb::WalkResponse::Fields::kMetrics));

  MetricWalker walker(writer);
  PW_TEST_ASSERT_OK(walker.Walk(root));
  PW_TEST_ASSERT_OK(parent_encoder.status());

  // Verify that the in-memory metric was updated by the hook, but the
  // original value was encoded.
  // 1) The metric's in-memory value should be the new value from the hook.
  EXPECT_EQ(atomic_metric.value(), kUpdatedValue);

  // 2) The encoded value is the original value, proving that `WriteContext`
  //    captured the value before it was modified by the hook.
  protobuf::Decoder decoder(writer_with_hook.WrittenData());
  bool metric_found = false;
  while (decoder.Next().ok()) {
    if (decoder.FieldNumber() ==
        static_cast<uint32_t>(proto::pwpb::WalkResponse::Fields::kMetrics)) {
      ConstByteSpan metric_bytes;
      PW_TEST_ASSERT_OK(decoder.ReadBytes(&metric_bytes));

      protobuf::Decoder metric_decoder(metric_bytes);
      PW_TEST_ASSERT_OK(metric_decoder.Next());  // Path
      PW_TEST_ASSERT_OK(metric_decoder.Next());  // Value

      uint32_t value;
      PW_TEST_ASSERT_OK(metric_decoder.ReadUint32(&value));
      EXPECT_EQ(value, kInitialValue);
      metric_found = true;
    }
  }
  EXPECT_TRUE(metric_found);
}

TEST(PwpbStreamingMetricWriter, BasicWalk) {
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, a, "a", 123u);
  PW_METRIC(root, b, "b", 456.0f);
  PW_METRIC_GROUP(inner, "inner");
  PW_METRIC(inner, x, "x", 789u);
  root.Add(inner);

  std::array<std::byte, 256> encode_buffer;
  stream::MemoryWriter memory_writer(encode_buffer);
  // Use WalkResponse as a mock parent message for testing the writer.
  // This must be a StreamEncoder, not a MemoryEncoder, to test the streaming
  // use case. The MemoryWriter is just for capturing the output.
  proto::pwpb::WalkResponse::StreamEncoder parent_encoder(memory_writer, {});

  PwpbStreamingMetricWriter writer(
      parent_encoder,
      static_cast<uint32_t>(proto::pwpb::WalkResponse::Fields::kMetrics));

  MetricWalker walker(writer);

  Status walk_status = walker.Walk(root);
  PW_TEST_ASSERT_OK(walk_status);
  PW_TEST_ASSERT_OK(parent_encoder.status());

  EXPECT_EQ(CountMetricsInWalkResponse(memory_writer.WrittenData()), 3u);
}

TEST(PwpbStreamingMetricWriter, StopsAtMetricLimit) {
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, a, "a", 123u);
  PW_METRIC(root, b, "b", 456.0f);
  PW_METRIC(root, c, "c", 789u);

  std::array<std::byte, 256> encode_buffer;
  stream::MemoryWriter memory_writer(encode_buffer);
  proto::pwpb::WalkResponse::StreamEncoder parent_encoder(memory_writer, {});

  size_t metric_limit = 2;
  PwpbStreamingMetricWriter writer(
      parent_encoder,
      static_cast<uint32_t>(proto::pwpb::WalkResponse::Fields::kMetrics),
      metric_limit);

  MetricWalker walker(writer);

  Status walk_status = walker.Walk(root);
  ASSERT_EQ(Status::ResourceExhausted(), walk_status);
  PW_TEST_ASSERT_OK(parent_encoder.status());

  EXPECT_EQ(CountMetricsInWalkResponse(memory_writer.WrittenData()), 2u);
}

TEST(PwpbStreamingMetricWriter, StopsWhenStreamIsFull) {
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, a, "a", 1u);
  PW_METRIC(root, b, "b", 2u);

  // Create a buffer that can only hold one metric.
  constexpr size_t kSmallBufferSize = 16;
  std::array<std::byte, kSmallBufferSize> encode_buffer;
  stream::MemoryWriter memory_writer(encode_buffer);
  proto::pwpb::WalkResponse::StreamEncoder parent_encoder(memory_writer, {});

  PwpbStreamingMetricWriter writer(
      parent_encoder,
      static_cast<uint32_t>(proto::pwpb::WalkResponse::Fields::kMetrics));

  MetricWalker walker(writer);

  Status walk_status = walker.Walk(root);

  // The walker is expected to stop and propagate the RESOURCE_EXHAUSTED status
  // from the writer when the underlying stream reports an error (e.g., is
  // full).
  ASSERT_EQ(Status::ResourceExhausted(), walk_status);
  ASSERT_EQ(Status::ResourceExhausted(), parent_encoder.status());

  EXPECT_EQ(CountMetricsInWalkResponse(memory_writer.WrittenData()), 1u);
}

TEST(PwpbStreamingMetricWriter, WalksEmptyMetricTree) {
  PW_METRIC_GROUP(root, "/");
  PW_METRIC_GROUP(inner, "empty_child");
  root.Add(inner);

  std::array<std::byte, 256> encode_buffer;
  stream::MemoryWriter memory_writer(encode_buffer);
  proto::pwpb::WalkResponse::StreamEncoder parent_encoder(memory_writer, {});

  PwpbStreamingMetricWriter writer(
      parent_encoder,
      static_cast<uint32_t>(proto::pwpb::WalkResponse::Fields::kMetrics));
  MetricWalker walker(writer);

  Status walk_status = walker.Walk(root);
  PW_TEST_ASSERT_OK(walk_status);
  PW_TEST_ASSERT_OK(parent_encoder.status());

  EXPECT_EQ(memory_writer.bytes_written(), 0u);
}

TEST(PwpbStreamingMetricWriter, StopsWithZeroMetricLimit) {
  PW_METRIC_GROUP(root, "/");
  PW_METRIC(root, a, "a", 123u);

  std::array<std::byte, 256> encode_buffer;
  stream::MemoryWriter memory_writer(encode_buffer);
  proto::pwpb::WalkResponse::StreamEncoder parent_encoder(memory_writer, {});

  size_t metric_limit = 0;
  PwpbStreamingMetricWriter writer(
      parent_encoder,
      static_cast<uint32_t>(proto::pwpb::WalkResponse::Fields::kMetrics),
      metric_limit);
  MetricWalker walker(writer);

  Status walk_status = walker.Walk(root);
  ASSERT_EQ(Status::ResourceExhausted(), walk_status);
  PW_TEST_ASSERT_OK(parent_encoder.status());

  EXPECT_EQ(memory_writer.bytes_written(), 0u);
}

}  // namespace
}  // namespace pw::metric
