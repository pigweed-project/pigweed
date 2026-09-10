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

#include <cstddef>
#include <cstdint>
#include <utility>

#include "pw_bytes/span.h"
#include "pw_log/log.h"
#include "pw_rpc/client.h"
#include "pw_rpc/examples/sensor_service.raw_rpc.pb.h"
#include "pw_rpc/examples/sensor_service.rpc.pb.h"
#include "pw_rpc/raw/server_reader_writer.h"
#include "pw_rpc/server.h"
#include "pw_status/status.h"

namespace pw::rpc::examples {
namespace {

// DOCSTAG: [pw_rpc-raw-service-impl]
class RawSensorService final
    : public ::pw::rpc::examples::pw_rpc::raw::SensorService::Service<
          RawSensorService> {
 public:
  // 1. Unary RPC: accepts raw request bytes and completes with responder
  void GetReading(pw::ConstByteSpan request_bytes,
                  pw::rpc::RawUnaryResponder& responder) {
    PW_LOG_INFO("Received %u raw request bytes",
                static_cast<unsigned>(request_bytes.size()));
    std::byte response_buffer[64]{};
    static_cast<void>(responder.Finish(response_buffer, pw::OkStatus()));
  }

  // 2. Server Streaming RPC
  void StreamReadings(pw::ConstByteSpan request_bytes,
                      pw::rpc::RawServerWriter& writer) {
    PW_LOG_INFO("Streaming readings for request of size %u",
                static_cast<unsigned>(request_bytes.size()));
    std::byte chunk[32]{};
    static_cast<void>(writer.Write(chunk));
    static_cast<void>(writer.Finish(pw::OkStatus()));
  }

  // 3. Bidirectional Streaming RPC
  void Calibrate(pw::rpc::RawServerReaderWriter& stream) {
    stream_ = std::move(stream);

    stream_.set_on_next([this](pw::ConstByteSpan payload) {
      // Echo back raw calibration payload
      static_cast<void>(stream_.Write(payload));
    });
  }

 private:
  pw::rpc::RawServerReaderWriter stream_;
};
// DOCSTAG: [pw_rpc-raw-service-impl]

// DOCSTAG: [pw_rpc-raw-mixed-service]
class MixedSensorService final
    : public ::pw::rpc::examples::pw_rpc::nanopb::SensorService::Service<
          MixedSensorService> {
 public:
  // Standard Nanopb unary method:
  pw::Status GetReading(const pw_rpc_examples_SensorRequest& request,
                        pw_rpc_examples_SensorResponse& response) {
    PW_LOG_INFO("Reading sensor %u", static_cast<unsigned>(request.sensor_id));
    response.temperature = 22.5f;
    response.humidity = 45.0f;
    return pw::OkStatus();
  }

  // Raw server streaming method fallback:
  void StreamReadings([[maybe_unused]] pw::ConstByteSpan request_bytes,
                      pw::rpc::RawServerWriter& writer) {
    std::byte payload[32]{};
    static_cast<void>(writer.Write(payload));
    static_cast<void>(writer.Finish(pw::OkStatus()));
  }
};
// DOCSTAG: [pw_rpc-raw-mixed-service]

class FakeOutput : public pw::rpc::ChannelOutput {
 public:
  constexpr FakeOutput() : pw::rpc::ChannelOutput("Fake") {}
  pw::Status Send(pw::span<const std::byte>) override { return pw::OkStatus(); }
};

FakeOutput fake_output;
pw::rpc::Channel channels[] = {pw::rpc::Channel::Create<1>(&fake_output)};
pw::rpc::Client client(channels);

// DOCSTAG: [pw_rpc-raw-client-call]
using RawSensorClient = ::pw::rpc::examples::pw_rpc::raw::SensorService::Client;

[[maybe_unused]] void InvokeRawRpc() {
  RawSensorClient raw_client(client, 1);

  std::byte request_payload[16]{};
  auto call = raw_client.GetReading(
      request_payload, [](pw::ConstByteSpan response, pw::Status status) {
        if (status.ok()) {
          PW_LOG_INFO("Received %u raw bytes",
                      static_cast<unsigned>(response.size()));
        }
      });
}
// DOCSTAG: [pw_rpc-raw-client-call]

}  // namespace
}  // namespace pw::rpc::examples
