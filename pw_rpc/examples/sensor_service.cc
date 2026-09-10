// Copyright 2026 The Pigweed Authors
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

#include "pw_rpc/examples/sensor_service.h"

#include <utility>

#include "pw_bytes/span.h"
#include "pw_rpc/raw/server_reader_writer.h"
#include "pw_rpc/server.h"
#include "pw_status/status.h"

namespace pw::rpc::examples {

// DOCSTAG: [pw_rpc-examples-sensor-service-impl]
pw::Status SensorServiceImpl::GetReading(
    const pw_rpc_examples_SensorRequest& /*request*/,
    pw_rpc_examples_SensorResponse& response) {
  response.temperature = 24.2f;
  response.humidity = 45.0f;
  response.error = false;
  return pw::OkStatus();
}

void SensorServiceImpl::GetReadingAsync(
    const pw_rpc_examples_SensorRequest& /*request*/,
    pw::rpc::NanopbUnaryResponder<pw_rpc_examples_SensorResponse>&
        new_responder) {
  // Move responder to member to complete asynchronously later
  async_responder_ = std::move(new_responder);
}

void SensorServiceImpl::StreamReadings(
    const pw_rpc_examples_StreamRequest& request,
    pw::rpc::NanopbServerWriter<pw_rpc_examples_SensorResponse>& writer) {
  for (uint32_t i = 0; i < request.sample_count; ++i) {
    pw_rpc_examples_SensorResponse response{
        .temperature = 20.0f + static_cast<float>(i),
        .humidity = 50.0f,
        .error = false,
    };
    if (!writer.Write(response).ok()) {
      break;  // Client disconnected or buffer full
    }
  }
  static_cast<void>(writer.Finish(pw::OkStatus()));
}

void SensorServiceImpl::Calibrate(
    pw::rpc::NanopbServerReaderWriter<pw_rpc_examples_CalibrationPoint,
                                      pw_rpc_examples_CalibrationStatus>&
        stream) {
  calib_stream_ = std::move(stream);

  calib_stream_.set_on_next(
      [this](const pw_rpc_examples_CalibrationPoint& point) {
        pw_rpc_examples_CalibrationStatus status{
            .calibrated = true,
            .offset = point.measured_val - point.reference_val,
        };
        static_cast<void>(calib_stream_.Write(status));
      });
}
// DOCSTAG: [pw_rpc-examples-sensor-service-impl]

// DOCSTAG: [pw_rpc-examples-sensor-raw-fallback]
class MyMixedService final
    : public pw::rpc::examples::pw_rpc::nanopb::SensorService::Service<
          MyMixedService> {
 public:
  // Standard Nanopb unary method:
  pw::Status GetReading(const pw_rpc_examples_SensorRequest& /*request*/,
                        pw_rpc_examples_SensorResponse& /*response*/) {
    return pw::OkStatus();
  }

  // Raw server streaming method fallback:
  void StreamReadings(pw::ConstByteSpan /*request_bytes*/,
                      pw::rpc::RawServerWriter& writer) {
    std::byte payload[32]{};
    static_cast<void>(writer.Write(payload));
    static_cast<void>(writer.Finish(pw::OkStatus()));
  }
};
// DOCSTAG: [pw_rpc-examples-sensor-raw-fallback]

// DOCSTAG: [pw_rpc-examples-sensor-register]
SensorServiceImpl sensor_service;

void RegisterAppServices(pw::rpc::Server& server) {
  server.RegisterService(sensor_service);
}
// DOCSTAG: [pw_rpc-examples-sensor-register]

}  // namespace pw::rpc::examples
