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

#include <algorithm>
#include <cstring>
#include <optional>

#include "pw_assert/check.h"
#include "pw_containers/vector.h"
#include "pw_metric/config.h"
#include "pw_metric/metric.h"
#include "pw_metric/metric_walker.h"
#include "pw_span/span.h"

namespace pw::metric {
namespace {

// Writes a pw::metric::Metric object to a nanopb-generated Metric struct.
// This is a shared helper used by both the streaming and unary writers.
template <typename ResponseStruct>
void WriteMetricToResponse(const UntypedMetric& metric,
                           const Vector<Token>& path,
                           ResponseStruct& response) {
  // Grab the next available Metric slot to write to in the response.
  pw_metric_proto_Metric& proto_metric =
      response.metrics[response.metrics_count];

  // Copy the path.
  span<Token> proto_path(proto_metric.token_path);
  PW_CHECK_INT_LE(path.size(), proto_path.size());
  std::copy(path.begin(), path.end(), proto_path.begin());
  proto_metric.token_path_count = static_cast<pb_size_t>(path.size());

  // Copy the metric value.
  switch (metric.type()) {
    case UntypedMetric::kTypeFloat: {
      const auto& m = static_cast<const TypedMetric<float>&>(metric);
      proto_metric.value.as_float = m.value();
      proto_metric.which_value = pw_metric_proto_Metric_as_float_tag;
      break;
    }
    case UntypedMetric::kTypeUint32: {
      const auto& m = static_cast<const TypedMetric<uint32_t>&>(metric);
      proto_metric.value.as_int = m.value();
      proto_metric.which_value = pw_metric_proto_Metric_as_int_tag;
      break;
    }
#if PW_METRIC_CONFIG_ENABLE_64BIT
    case UntypedMetric::kTypeUint64: {
      const auto& m = static_cast<const TypedMetric<uint64_t>&>(metric);
      proto_metric.value.as_uint64 = m.value();
      proto_metric.which_value = pw_metric_proto_Metric_as_uint64_tag;
      break;
    }
    case UntypedMetric::kTypeInt64: {
      const auto& m = static_cast<const TypedMetric<int64_t>&>(metric);
      proto_metric.value.as_int64 = m.value();
      proto_metric.which_value = pw_metric_proto_Metric_as_int64_tag;
      break;
    }
#endif  // PW_METRIC_CONFIG_ENABLE_64BIT
  }

  // Move write head to the next slot.
  ++response.metrics_count;
}

// A MetricWriter for the legacy, streaming Get RPC. It writes metrics to a
// nanopb struct and flushes the batch when it's full.
class NanopbStreamingMetricWriter : public virtual MetricWriter {
 public:
  NanopbStreamingMetricWriter(
      MetricService::ServerWriter<pw_metric_proto_MetricResponse>&
          response_writer)
      : response_(pw_metric_proto_MetricResponse_init_zero),
        response_writer_(response_writer) {}

  // TODO(keir): Figure out a pw_rpc mechanism to fill a streaming packet based
  // on transport MTU, rather than having this as a static knob. For example,
  // some transports may be able to fit 30 metrics; others, only 5.
  Status Write(const UntypedMetric& metric,
               const Vector<Token>& path) override {
    // Nanopb doesn't offer an easy way to do bounds checking, so use span's
    // type deduction magic to figure out the max size.
    span<pw_metric_proto_Metric> metrics(response_.metrics);
    PW_CHECK_INT_LT(response_.metrics_count, metrics.size());

    WriteMetricToResponse(metric, path, response_);

    // If the metric response object is full, send the response and reset.
    // TODO(keir): Support runtime batch sizes < max proto size.
    if (response_.metrics_count == metrics.size()) {
      Flush();
    }

    return OkStatus();
  }

  void Flush() {
    if (response_.metrics_count) {
      response_writer_.Write(response_)
          .IgnoreError();  // TODO: b/242598609 - Handle Status properly
      response_ = pw_metric_proto_MetricResponse_init_zero;
    }
  }

 private:
  pw_metric_proto_MetricResponse response_;
  // This RPC stream writer handle must be valid for the metric writer lifetime.
  MetricService::ServerWriter<pw_metric_proto_MetricResponse>& response_writer_;
};

// A UnaryMetricWriter that populates a nanopb WalkResponse struct. This writer
// is used by the ResumableMetricWalker to fill a page of metrics.
class NanopbUnaryMetricWriter : public UnaryMetricWriter {
 public:
  explicit NanopbUnaryMetricWriter(pw_metric_proto_WalkResponse& response)
      : response_(response) {}

  // Writes a metric to the next available slot in the response's metrics
  // array. If the array is full, this method returns RESOURCE_EXHAUSTED to
  // signal the walker to stop and paginate.
  Status Write(const UntypedMetric& metric,
               const Vector<Token>& path) override {
    span<pw_metric_proto_Metric> metrics(response_.metrics);
    if (response_.metrics_count >= metrics.size()) {
      return Status::ResourceExhausted();
    }

    WriteMetricToResponse(metric, path, response_);
    return OkStatus();
  }

 private:
  pw_metric_proto_WalkResponse& response_;
};

// Helper to recursively search the metric tree for a metric at a given memory
// address. This is used for pre-flight cursor validation.
bool FindMetricByAddress(const MetricList& metrics,
                         const GroupList& groups,
                         uint64_t address) {
  bool found = false;
  metrics.for_each([&](const auto& metric) {
    if (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&metric)) ==
        address) {
      found = true;
    }
  });
  if (found) {
    return true;
  }

  groups.for_each([&](const auto& group) {
    if (FindMetricByAddress(group.metrics(), group.children(), address)) {
      found = true;
    }
  });
  return found;
}

}  // namespace

void MetricService::Get(
    const pw_metric_proto_MetricRequest& /* request */,
    ServerWriter<pw_metric_proto_MetricResponse>& response) {
  // For now, ignore the request and just stream all the metrics back.
  NanopbStreamingMetricWriter writer(response);
  MetricWalker walker(writer);

  // This will stream all the metrics in the span of this Get() method call.
  // This will have the effect of blocking the RPC thread until all the metrics
  // are sent. That is likely to cause problems if there are many metrics, or
  // if other RPCs are higher priority and should complete first.
  //
  // In the future, this should be replaced with an optional async solution
  // that puts the application in control of when the response batches are sent.
  walker.Walk(metrics_).IgnoreError();
  walker.Walk(groups_).IgnoreError();
  writer.Flush();
}

// This method populates the response struct that is provided by the pw_rpc
// framework.
Status MetricService::Walk(const pw_metric_proto_WalkRequest& request,
                           pw_metric_proto_WalkResponse& response) {
  // Pre-flight check for cursor validity.
  if (request.has_cursor && request.cursor != 0) {
    if (!FindMetricByAddress(metrics_, groups_, request.cursor)) {
      return Status::NotFound();
    }
  }

  response = pw_metric_proto_WalkResponse_init_zero;
  NanopbUnaryMetricWriter writer(response);
  ResumableMetricWalker walker(writer);

  Result<uint64_t> result = walker.Walk(
      metrics_,
      groups_,
      request.has_cursor ? std::optional(request.cursor) : std::nullopt);

  if (result.status().IsResourceExhausted()) {
    // Pagination case: The page is full.
    response.has_cursor = true;
    response.cursor = walker.next_cursor();
    response.done = false;
    return OkStatus();  // Successful RPC, just paginated.
  }

  if (!result.ok()) {
    return result.status();  // Propagate other errors.
  }

  // The walk completed successfully.
  response.done = true;
  response.has_cursor = false;

  return OkStatus();
}

}  // namespace pw::metric
