// Copyright 2023 The Pigweed Authors
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

#include "pw_trace_tokenized/trace_service_pwpb.h"

namespace pw::trace {

TraceService::TraceService(TokenizedTracer& tokenized_tracer,
                           stream::Writer& trace_writer)
    : BaseTraceService(tokenized_tracer, trace_writer) {}

Status TraceService::Start(
    const proto::pwpb::StartRequest::Message& /*request*/,
    proto::pwpb::StartResponse::Message& /*response*/) {
  return BaseTraceService::Start();
}

Status TraceService::Stop(const proto::pwpb::StopRequest::Message& /*request*/,
                          proto::pwpb::StopResponse::Message& response) {
  if (auto status = BaseTraceService::Stop(); status != pw::OkStatus()) {
    return status;
  }

  response.file_id = transfer_id_;
  return pw::OkStatus();
}

Status TraceService::GetClockParameters(
    const proto::pwpb::ClockParametersRequest::Message& /*request*/,
    proto::pwpb::ClockParametersResponse::Message& response) {
  response.clock_parameters.tick_period_seconds_numerator = 1;
  response.clock_parameters.tick_period_seconds_denominator =
      static_cast<int32_t>(pw_trace_GetTraceTimeTicksPerSecond());

  // There is no platform API to get the epoch type of trace time.
  // Epoch type is also not part of generated trace json file and
  // has no real use to trace client programmably.
  response.clock_parameters.epoch_type =
      chrono::pwpb::EpochType::Enum::kUnknown;

  return pw::OkStatus();
}

}  // namespace pw::trace
