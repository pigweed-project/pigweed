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

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <optional>

#include "pw_containers/vector.h"
#include "pw_log/log.h"
#include "pw_metric/config.h"
#include "pw_metric/list.h"
#include "pw_metric/metric.h"
#include "pw_metric/metric_walker.h"
#include "pw_metric/pwpb_metric_writer.h"
#include "pw_metric_proto/metric_service.pwpb.h"
#include "pw_protobuf/serialized_size.h"
#include "pw_result/result.h"
#include "pw_rpc/raw/server_reader_writer.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"
#include "pw_status/try.h"
#include "pw_stream/memory_stream.h"

namespace pw::metric {

// TODO(amontanez): Make this follow the metric_service.options configuration.
constexpr size_t kMaxNumPackedEntries = 3;

namespace {

class PwpbServiceStreamingMetricWriter : public virtual MetricWriter {
 public:
  PwpbServiceStreamingMetricWriter(span<std::byte> response,
                                   rpc::RawServerWriter& response_writer)
      : response_(response),
        response_writer_(response_writer),
        encoder_(response) {}

  // TODO(keir): Figure out a pw_rpc mechanism to fill a streaming packet based
  // on transport MTU, rather than having this as a static knob. For example,
  // some transports may be able to fit 30 metrics; others, only 5.
  Status Write(const UntypedMetric& metric,
               const Vector<Token>& path) override {
    {  // Scope to control proto_encoder lifetime.

      // Grab the next available Metric slot to write to in the response.
      proto::pwpb::Metric::StreamEncoder proto_encoder =
          encoder_.GetMetricsEncoder();
      PW_TRY(proto_encoder.WriteTokenPath(path));
      // Encode the metric value.
      switch (metric.type()) {
        case UntypedMetric::kTypeFloat: {
          const auto& m = static_cast<const TypedMetric<float>&>(metric);
          PW_TRY(proto_encoder.WriteAsFloat(m.value()));
          break;
        }
        case UntypedMetric::kTypeUint32: {
          const auto& m = static_cast<const TypedMetric<uint32_t>&>(metric);
          PW_TRY(proto_encoder.WriteAsInt(m.value()));
          break;
        }
#if PW_METRIC_CONFIG_ENABLE_64BIT
        case UntypedMetric::kTypeUint64: {
          const auto& m = static_cast<const TypedMetric<uint64_t>&>(metric);
          PW_TRY(proto_encoder.WriteAsUint64(m.value()));
          break;
        }
        case UntypedMetric::kTypeInt64: {
          const auto& m = static_cast<const TypedMetric<int64_t>&>(metric);
          PW_TRY(proto_encoder.WriteAsInt64(m.value()));
          break;
        }
#endif  // PW_METRIC_CONFIG_ENABLE_64BIT
      }
    }
    ++metrics_count_;

    if (metrics_count_ == kMaxNumPackedEntries) {
      return Flush();
    }
    return OkStatus();
  }

  Status Flush() {
    Status status;
    if (metrics_count_) {
      status = response_writer_.Write(encoder_);
      // Clear the encoder by reconstructing it in place.
      encoder_.~MemoryEncoder();
      new (&encoder_) proto::pwpb::MetricResponse::MemoryEncoder(response_);
      metrics_count_ = 0;
    }
    return status;
  }

 private:
  span<std::byte> response_;
  // This RPC stream writer handle must be valid for the metric writer
  // lifetime.
  rpc::RawServerWriter& response_writer_;
  proto::pwpb::MetricResponse::MemoryEncoder encoder_;
  size_t metrics_count_ = 0;
};

// The maximum possible overhead for fields in the WalkResponse that are not
// metrics (e.g. cursor or done). This ensures that the pagination logic
// reserves enough space for these fields after the last metric is written.
// The cursor (uint64) is the largest possible non-metric field.
constexpr size_t kWalkResponseOverhead =
    protobuf::SizeOfFieldUint64(proto::pwpb::WalkResponse::Fields::kCursor);

// A UnaryMetricWriter that uses a pw::protobuf::MemoryEncoder to serialize
// metrics for the paginated Walk RPC.
class PwpbUnaryMetricWriter final : public UnaryMetricWriter {
 public:
  explicit PwpbUnaryMetricWriter(
      proto::pwpb::WalkResponse::MemoryEncoder& encoder, size_t capacity)
      : encoder_(encoder), capacity_(capacity) {}

  // Writes a metric and its path to the underlying protobuf encoder.
  // This method calculates the required size for the metric and returns
  // RESOURCE_EXHAUSTED if the metric will not fit in the remaining buffer
  // space, which drives the server-side pagination.
  Status Write(const UntypedMetric& metric,
               const Vector<Token>& path) override {
    // A packed repeated fixed32 field (like token_path) is encoded on the
    // wire identically to a bytes field. First, calculate the size of the
    // payload.
    const size_t token_path_payload_size = path.size() * sizeof(uint32_t);

    // Calculate the total size of the token_path field within the Metric
    // message, including its tag and length prefix.
    size_t metric_payload_size = protobuf::SizeOfDelimitedField(
        proto::pwpb::Metric::Fields::kTokenPath,
        static_cast<uint32_t>(token_path_payload_size));

    // Read the atomic metric value once to ensure the same value is used for
    // both the sizing and writing passes. This prevents a race condition
    // where the metric's value could change between the two passes.
    const auto value = internal::ReadMetricValue(metric);
    switch (metric.type()) {
      case UntypedMetric::kTypeFloat:
        metric_payload_size +=
            protobuf::SizeOfFieldFloat(proto::pwpb::Metric::Fields::kAsFloat);
        break;
      case UntypedMetric::kTypeUint32:
        metric_payload_size += protobuf::SizeOfFieldUint32(
            proto::pwpb::Metric::Fields::kAsInt, value.u32);
        break;
#if PW_METRIC_CONFIG_ENABLE_64BIT
      case UntypedMetric::kTypeUint64:
        metric_payload_size += protobuf::SizeOfFieldUint64(
            proto::pwpb::Metric::Fields::kAsUint64, value.u64);
        break;
      case UntypedMetric::kTypeInt64:
        metric_payload_size += protobuf::SizeOfFieldInt64(
            proto::pwpb::Metric::Fields::kAsInt64, value.i64);
        break;
#endif  // PW_METRIC_CONFIG_ENABLE_64BIT
    }

    // Calculate the size of the entire Metric message when encoded as a field
    // within the WalkResponse.
    const size_t required_size_for_field = protobuf::SizeOfDelimitedField(
        proto::pwpb::WalkResponse::Fields::kMetrics,
        static_cast<uint32_t>(metric_payload_size));

    // Check if the metric AND the final response fields (cursor/done) will
    // fit in the buffer. If not, return RESOURCE_EXHAUSTED to signal the
    // ResumableMetricWalker to pause and return a cursor.
    if ((encoder_.size() + required_size_for_field + kWalkResponseOverhead) >
        capacity_) {
      return Status::ResourceExhausted();
    }

    Status write_status;
    {  // Scope to control proto_encoder lifetime.
      proto::pwpb::Metric::StreamEncoder metric_encoder =
          encoder_.GetMetricsEncoder();
      PW_TRY(metric_encoder.WriteTokenPath(path));
      switch (metric.type()) {
        case UntypedMetric::kTypeFloat:
          PW_TRY(metric_encoder.WriteAsFloat(value.f));
          break;
        case UntypedMetric::kTypeUint32:
          PW_TRY(metric_encoder.WriteAsInt(value.u32));
          break;
#if PW_METRIC_CONFIG_ENABLE_64BIT
        case UntypedMetric::kTypeUint64:
          PW_TRY(metric_encoder.WriteAsUint64(value.u64));
          break;
        case UntypedMetric::kTypeInt64:
          PW_TRY(metric_encoder.WriteAsInt64(value.i64));
          break;
#endif  // PW_METRIC_CONFIG_ENABLE_64BIT
      }
      write_status = metric_encoder.status();
    }  // Destructor for metric_encoder commits the write to the parent encoder.

    return write_status;
  }

 private:
  proto::pwpb::WalkResponse::MemoryEncoder& encoder_;
  size_t capacity_;
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

void MetricService::Get(ConstByteSpan /*request*/,
                        rpc::RawServerWriter& raw_response) {
  // For now, ignore the request and just stream all the metrics back.

  // The `string_path` field of Metric is not supported. The maximum size
  // without values includes the maximum token path. Additionally, include the
  // maximum size of the `as_int` field.
  constexpr size_t kSizeOfOneMetric =
      proto::pwpb::MetricResponse::kMaxEncodedSizeBytesWithoutValues +
      proto::pwpb::Metric::kMaxEncodedSizeBytesWithoutValues +
      protobuf::SizeOfFieldUint32(proto::pwpb::Metric::Fields::kAsInt);

  // TODO(amontanez): Make this follow the metric_service.options configuration.
  constexpr size_t kEncodeBufferSize = kMaxNumPackedEntries * kSizeOfOneMetric;

  std::array<std::byte, kEncodeBufferSize> encode_buffer;

  PwpbServiceStreamingMetricWriter writer(encode_buffer, raw_response);
  MetricWalker walker(writer);

  // This will stream all the metrics in the span of this Get() method call.
  // This will have the effect of blocking the RPC thread until all the metrics
  // are sent. That is likely to cause problems if there are many metrics, or
  // if other RPCs are higher priority and should complete first.
  //
  // In the future, this should be replaced with an optional async solution
  // that puts the application in control of when the response batches are sent.

  // Propagate status through walker.
  Status status;
  status.Update(walker.Walk(metrics_));
  status.Update(walker.Walk(groups_));
  status.Update(writer.Flush());
  raw_response.Finish(status).IgnoreError();
}

// Implements the paginated, unary Walk RPC using a FinishCallback to enable
// server-driven page sizing.
void MetricService::Walk(ConstByteSpan serialized_request,
                         rpc::RawUnaryResponder& responder) {
  proto::pwpb::WalkRequest::Message request;
  stream::MemoryReader reader(serialized_request);
  proto::pwpb::WalkRequest::StreamDecoder decoder(reader);
  if (const Status status = decoder.Read(request); !status.ok()) {
    responder.Finish({}, status).IgnoreError();
    return;
  }

  // Pre-flight check: If a non-zero cursor is provided, ensure it's valid
  // before calling FinishCallback. This correctly propagates NOT_FOUND as the
  // final RPC status, which is not possible from within the callback.
  if (request.cursor.has_value() && request.cursor.value() != 0) {
    if (!FindMetricByAddress(metrics_, groups_, request.cursor.value())) {
      responder.Finish({}, Status::NotFound()).IgnoreError();
      return;
    }
  }

  // This struct captures the context needed by the FinishCallback lambda. This
  // avoids a large lambda capture that can cause issues on some compilers when
  // targeting resource-constrained devices.
  struct WalkContext {
    MetricService& service;
    std::optional<uint64_t> cursor;
    size_t capacity;
  };

  WalkContext context{.service = *this,
                      .cursor = request.cursor,
                      .capacity = responder.MaxWriteSizeBytes()};

  // Use the callback-based FinishCallback overload to encode directly into the
  // framework's transport buffer. The buffer provided to the callback is
  // sized according to the channel's MTU, which allows for server-driven
  // page-sizing.
  const Status status = responder.FinishCallback({[&context](ByteSpan buffer) {
    proto::pwpb::WalkResponse::MemoryEncoder encoder(buffer);
    // The capacity passed to the writer is the one reported by the
    // responder before the callback. This is our ground truth for the
    // payload size.
    PwpbUnaryMetricWriter writer(encoder, context.capacity);
    ResumableMetricWalker walker(writer);

    Result<uint64_t> result = walker.Walk(
        context.service.metrics_, context.service.groups_, context.cursor);

    if (result.ok()) {
      // The walk completed successfully.
      encoder.WriteDone(true).IgnoreError();
    } else if (result.status().IsResourceExhausted()) {
      // A page was filled. The walker's `next_cursor_` is the address of
      // the next metric.
      encoder.WriteCursor(walker.next_cursor()).IgnoreError();
      // Explicitly set done to false for clarity on paginated responses.
      encoder.WriteDone(false).IgnoreError();
    } else if (result.status().IsNotFound()) {
      // This indicates a logic error. The pre-flight check in Walk()
      // should have caught an invalid cursor.
      PW_LOG_ERROR("Walker returned NOT_FOUND after pre-flight passed!");
      return StatusWithSize(Status::Internal(), 0);
    } else {
      // For any other error, we don't write any final fields and just
      // return the status.
      return StatusWithSize(result.status(), 0);
    }

    return StatusWithSize(encoder.status(), encoder.size());
  }});

  if (!status.ok()) {
    PW_LOG_ERROR("MetricService::Walk failed to send response: %s",
                 status.str());
  }
}

}  // namespace pw::metric
