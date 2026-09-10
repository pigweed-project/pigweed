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

#include "pw_bytes/span.h"
#include "pw_log/log.h"
#include "pw_rpc/client.h"
#include "pw_rpc/client_server.h"
#include "pw_rpc/examples/sensor_service.rpc.pb.h"
#include "pw_rpc/synchronous_call.h"
#include "pw_status/status.h"

namespace pw::rpc::examples {
namespace {

class FakeOutput : public pw::rpc::ChannelOutput {
 public:
  constexpr FakeOutput() : pw::rpc::ChannelOutput("Fake") {}
  pw::Status Send(pw::span<const std::byte>) override { return pw::OkStatus(); }
};

FakeOutput mcu2_output;
FakeOutput uart_output;

// DOCSTAG: [pw_rpc-examples-client-instantiation]
pw::rpc::Channel client_channels[] = {
    pw::rpc::Channel::Create<1>(&mcu2_output),
};

pw::rpc::Client rpc_client(client_channels);

// Route incoming packets from MCU2 to the client:
[[maybe_unused]] void OnMcu2PacketReceived(pw::ConstByteSpan packet) {
  static_cast<void>(rpc_client.ProcessPacket(packet));
}
// DOCSTAG: [pw_rpc-examples-client-instantiation]

// DOCSTAG: [pw_rpc-examples-client-server]
pw::rpc::Channel shared_channels[] = {
    pw::rpc::Channel::Create<1>(&uart_output),
};

// Instantiates both client and server sharing the channels
pw::rpc::ClientServer client_server(shared_channels);

[[maybe_unused]] void OnPacketReceived(pw::ConstByteSpan packet) {
  // Automatically routes request packets to the server, and response packets to
  // the client:
  static_cast<void>(client_server.ProcessPacket(packet));
}
// DOCSTAG: [pw_rpc-examples-client-server]

// DOCSTAG: [pw_rpc-examples-async-client-call]
using SensorClient = pw::rpc::examples::pw_rpc::nanopb::SensorService::Client;
constexpr uint32_t kPeerChannelId = 1;

// Retain call object to keep call active
pw::rpc::NanopbUnaryReceiver<pw_rpc_examples_SensorResponse> active_call;

void OnSensorResponse(const pw_rpc_examples_SensorResponse& resp,
                      pw::Status status) {
  if (status.ok()) {
    PW_LOG_INFO("Temperature: %f", resp.temperature);
  }
}

[[maybe_unused]] void RequestSensorReading() {
  SensorClient client(rpc_client, kPeerChannelId);
  pw_rpc_examples_SensorRequest req{.sensor_id = 1};

  active_call = client.GetReading(req, OnSensorResponse);
}
// DOCSTAG: [pw_rpc-examples-async-client-call]

// DOCSTAG: [pw_rpc-examples-sync-client-call]
[[maybe_unused]] pw::Status FetchSensorSync() {
  pw_rpc_examples_SensorRequest req{.sensor_id = 1};

  // Blocks calling thread until response arrives or timeout occurs
  pw::rpc::SynchronousCallResult<pw_rpc_examples_SensorResponse> result =
      pw::rpc::SynchronousCall<
          pw::rpc::examples::pw_rpc::nanopb::SensorService::GetReading>(
          rpc_client, kPeerChannelId, req);

  if (!result.ok()) {
    PW_LOG_ERROR("RPC failed: %s", result.status().str());
    return result.status();
  }

  PW_LOG_INFO("Temperature: %f", result.response().temperature);
  return pw::OkStatus();
}
// DOCSTAG: [pw_rpc-examples-sync-client-call]

}  // namespace
}  // namespace pw::rpc::examples
