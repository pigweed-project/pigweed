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

#include <limits>

#include "pw_metric/metric.h"
#include "pw_metric/metric_walker.h"
#include "pw_metric_proto/metric_service.pwpb.h"
#include "pw_protobuf/encoder.h"
#include "pw_protobuf/serialized_size.h"
#include "pw_span/span.h"
#include "pw_status/status.h"

namespace pw::metric {

// Writes all metrics from a MetricWalker into a pwpb stream encoder.
//
// This utility class implements the pw::metric::MetricWriter
// interface to bridge the MetricWalker to any pwpb stream encoder that has a
// repeated pw.metric.proto.Metric field. This is useful for generically
// dumping all metrics into any container proto (like a snapshot, telemetry
// packet, etc.) without coupling the walker to the container's schema.
//
// This class handles all sizing logic and gracefully stops the walk by
// returning ResourceExhausted if either the provided buffer runs out of space
// or an application-defined metric count limit is reached.
class PwpbMetricWriter : public MetricWriter {
 public:
  // Constructs a new pwpb metric writer.
  //
  // @param parent_encoder A pwpb stream encoder for the parent message
  //    (e.g., a WalkResponse or a custom snapshot proto).
  // @param field_number The field number (tag) of the
  //    `repeated pw.metric.proto.Metric` field in the parent proto.
  // @param metric_limit A reference to an external counter. The walk will stop
  //    when this counter reaches 0. The counter is decremented by this class
  //    for each metric written.
  //    To specify no limit, pass a size_t initialized to
  //    `std::numeric_limits<size_t>::max()`.
  PwpbMetricWriter(protobuf::StreamEncoder& parent_encoder,
                   uint32_t field_number,
                   size_t& metric_limit)
      : parent_encoder_(parent_encoder),
        field_number_(field_number),
        metric_limit_(metric_limit) {}

  pw::Status Write(const Metric& metric, const Vector<Token>& path) override {
    if (metric_limit_ == 0) {
      return pw::Status::ResourceExhausted();
    }

    // Size and Fit Verification
    // 1) Calculate the size of the nested Metric message's payload.
    // This logic must match the write logic below.
    size_t metric_payload_size = 0;
    for (size_t i = 0; i < path.size(); ++i) {
      metric_payload_size +=
          protobuf::SizeOfFieldFixed32(proto::pwpb::Metric::Fields::kTokenPath);
    }
    if (metric.is_float()) {
      metric_payload_size +=
          protobuf::SizeOfFieldFloat(proto::pwpb::Metric::Fields::kAsFloat);
    } else {
      metric_payload_size += protobuf::SizeOfFieldUint32(
          proto::pwpb::Metric::Fields::kAsInt, metric.as_int());
    }

    // 2) Calculate the total on-wire size this metric will consume in the
    // parent encoder, including its own tag and length delimiter.
    const size_t required_size_for_field = protobuf::SizeOfDelimitedField(
        field_number_, static_cast<uint32_t>(metric_payload_size));

    // 3) Check if the parent encoder can fit this new field.
    if (parent_encoder_.ConservativeWriteLimit() < required_size_for_field) {
      return pw::Status::ResourceExhausted();
    }

    // Write Process
    // Create a new nested encoder for this specific metric.
    // Its destructor will commit the write to the parent encoder.
    proto::pwpb::Metric::StreamEncoder metric_encoder =
        parent_encoder_.GetNestedEncoder(field_number_);

    // The pwpb stream encoder latches the first error.
    metric_encoder.WriteTokenPath(path).IgnoreError();
    if (metric.is_float()) {
      metric_encoder.WriteAsFloat(metric.as_float()).IgnoreError();
    } else {
      metric_encoder.WriteAsInt(metric.as_int()).IgnoreError();
    }

    --metric_limit_;

    // Return the latched status of the nested encoder. If any write failed
    // this will return that error status and the destructor will not commit to
    // the parent.
    return metric_encoder.status();
  }

 private:
  protobuf::StreamEncoder& parent_encoder_;
  const uint32_t field_number_;
  size_t& metric_limit_;
};

// Writes all metrics from a MetricWalker into a pwpb stream encoder using the
// two-pass encoding method.
//
// This writer is specialized for streaming use cases where intermediate copies
// should be avoided and the total size of the output is not known ahead of
// time. It does NOT perform any size checks, relying on the underlying stream
// to handle flow control or report errors.
//
// This contrasts with PwpbMetricWriter, which is designed for size-aware,
// bounded `pw::protobuf::MemoryEncoder` instances.
class PwpbStreamingMetricWriter : public MetricWriter {
 public:
  // Constructs a new pwpb streaming metric writer.
  //
  // @param parent_encoder A pwpb stream encoder for the parent message.
  // @param field_number The field number (tag) of the
  //    `repeated pw.metric.proto.Metric` field in the parent proto.
  // @param metric_limit An optional limit on the number of metrics to
  //    write. The walk will stop when this many metrics have been written.
  //    Defaults to `std::numeric_limits<size_t>::max()` (no limit).
  PwpbStreamingMetricWriter(
      protobuf::StreamEncoder& parent_encoder,
      uint32_t field_number,
      size_t metric_limit = std::numeric_limits<size_t>::max())
      : parent_encoder_(parent_encoder),
        field_number_(field_number),
        metric_limit_(metric_limit) {}

  pw::Status Write(const Metric& metric, const Vector<Token>& path) override {
    if (metric_limit_ == 0) {
      return pw::Status::ResourceExhausted();
    }

    // Read the atomic metric value once to ensure the same value is used for
    // both the sizing and writing passes of `WriteNestedMessage`. This prevents
    // a race condition where the metric's value could change between the two
    // passes.
    const bool is_float = metric.is_float();
    const float float_value = is_float ? metric.as_float() : 0.0f;
    const uint32_t int_value = is_float ? 0 : metric.as_int();
    const pw::span<const Token> path_span = path;

    // Use the two-pass WriteNestedMessage API to write the metric directly to
    // the stream without an intermediate buffer. The lambda will be called
    // twice: once to measure the size, and a second time to write.
    pw::Status status = parent_encoder_.WriteNestedMessage(
        field_number_,
        [path_span, is_float, float_value, int_value](
            pw::protobuf::StreamEncoder& base_encoder) {
          auto& metric_encoder = pw::protobuf::StreamEncoderCast<
              pw::metric::proto::pwpb::Metric::StreamEncoder>(base_encoder);

          metric_encoder.WriteTokenPath(path_span).IgnoreError();
          if (is_float) {
            metric_encoder.WriteAsFloat(float_value).IgnoreError();
          } else {
            metric_encoder.WriteAsInt(int_value).IgnoreError();
          }
          return metric_encoder.status();
        });

    if (status.ok()) {
      --metric_limit_;
    }

    return status;
  }

 private:
  protobuf::StreamEncoder& parent_encoder_;
  const uint32_t field_number_;
  size_t metric_limit_;
};

}  // namespace pw::metric
