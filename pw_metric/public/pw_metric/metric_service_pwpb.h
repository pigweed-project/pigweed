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

#include "pw_bytes/span.h"
#include "pw_metric/list.h"
#include "pw_metric/metric.h"
#include "pw_metric_proto/metric_service.pwpb.h"
#include "pw_metric_proto/metric_service.raw_rpc.pb.h"
#include "pw_rpc/raw/server_reader_writer.h"

namespace pw::metric {

// The MetricService provides RPC-based mechanisms for fetching metrics from a
// device. It supports two distinct methods for metric retrieval:
//
// 1. Walk (Unary RPC): This is the recommended method for fetching metrics. Its
//    unary, client-driven pattern is suitable for asynchronous transports where
//    the server cannot guarantee transport readiness. The paginated nature also
//    makes it ideal for large metric sets that may exceed the transport's MTU.
//
// 2. Get (Server-Streaming RPC): This is the legacy streaming method. It is
//    less robust than Walk and may not be suitable for all transports. It is
//    preserved for backward compatibility.
//
// The service is initialized with the global lists of metrics and groups that
// are defined on the device.
//
// In the future, filtering will be supported.
class MetricService final
    : public proto::pw_rpc::raw::MetricService::Service<MetricService> {
 public:
  constexpr MetricService(const MetricList& metrics, const GroupList& groups)
      : metrics_(metrics), groups_(groups) {}

  // Returns metrics or groups matching the requested paths. This is the legacy
  // streaming RPC and is less robust than Walk().
  //
  // An important limitation of this implementation is that it's blocking, and
  // sends all metrics at once (though batched).
  void Get(ConstByteSpan request, rpc::RawServerWriter& response);

  // Walks the metric tree and returns a single page of metrics. This is the
  // recommended method for fetching metrics, especially over asynchronous
  // transports or for large metric sets.
  //
  // Returns an `OUT_OF_RANGE` status if the requested `cursor` is invalid
  // (i.e. it is non-zero and past the end of the metric list).
  void Walk(ConstByteSpan request, rpc::RawUnaryResponder& responder);

  const MetricList& metrics() const { return metrics_; }
  const GroupList& groups() const { return groups_; }

 private:
  const MetricList& metrics_;
  const GroupList& groups_;
};

}  // namespace pw::metric
