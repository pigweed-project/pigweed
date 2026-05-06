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
#pragma once

#include <cstring>

#include "pw_metric/list.h"
#include "pw_metric/metric.h"
#include "pw_metric_proto/metric_service.rpc.pb.h"
#include "pw_span/span.h"
#include "pw_status/status.h"

namespace pw::metric {

// Implementation of the pw.metric.MetricService RPC service.
//
// This service provides two RPC methods to fetch metrics from a device:
//
//   - Walk: A unary, client-driven RPC suitable for asynchronous transports
//     where the server cannot guarantee transport readiness. Its paginated
//     nature makes it ideal for large metric sets that may exceed the
//     transport's MTU. This is the preferred method for fetching metrics.
//
//   - Get: A legacy server-streaming RPC. This method is blocking and sends
//     all metrics in a single stream, which may not be suitable for all
//     transports.
//
// Both methods recursively traverse all metric groups. Future versions may
// add support for filtering.
class MetricService final
    : public proto::pw_rpc::nanopb::MetricService::Service<MetricService> {
 public:
  constexpr MetricService(const MetricList& metrics, const GroupList& groups)
      : metrics_(metrics), groups_(groups) {}

  // The legacy streaming RPC for fetching metrics. The paginated Walk() RPC is
  // preferred.
  void Get(const pw_metric_proto_MetricRequest& request,
           ServerWriter<pw_metric_proto_MetricResponse>& response);

  // The paginated unary RPC for fetching metrics. This method is recommended
  // for asynchronous transports and for fetching large metric sets.
  Status Walk(const pw_metric_proto_WalkRequest& request,
              pw_metric_proto_WalkResponse& response);

  const MetricList& metrics() const { return metrics_; }
  const GroupList& groups() const { return groups_; }

 private:
  const MetricList& metrics_;
  const GroupList& groups_;
};

}  // namespace pw::metric
