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

// DOCSTAG: [pw_rpc-examples-full-integration]
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "pw_assert/check.h"
#include "pw_bytes/span.h"
#include "pw_hdlc/decoder.h"
#include "pw_hdlc/default_addresses.h"
#include "pw_hdlc/encoder.h"
#include "pw_log/log.h"
#include "pw_rpc/channel.h"
#include "pw_rpc/examples/echo_service.rpc.pb.h"
#include "pw_rpc/server.h"
#include "pw_sync/mutex.h"
#include "pw_thread/thread_core.h"

namespace {

// Hardware stubs for illustration:
void UartWriteByte(uint8_t /*byte*/) {}
size_t UartReadBytes(uint8_t* /*dest*/, size_t /*max_len*/) { return 0; }

// 1. Implement ChannelOutput for Egress (TX)
class HdlcUartChannelOutput : public pw::rpc::ChannelOutput {
 public:
  HdlcUartChannelOutput() : pw::rpc::ChannelOutput("HDLC_UART") {}

  size_t MaximumTransmissionUnit() override { return 512; }

  pw::Status Send(pw::span<const std::byte> buffer) override {
    std::lock_guard guard(tx_mutex_);
    for (std::byte b : buffer) {
      UartWriteByte(static_cast<uint8_t>(b));
    }
    return pw::OkStatus();
  }

 private:
  pw::sync::Mutex tx_mutex_;
};

HdlcUartChannelOutput uart_channel_output;

// 2. Declare Channels and Server
constexpr uint32_t kDefaultChannelId = 1;

pw::rpc::Channel channels[] = {
    pw::rpc::Channel::Create<kDefaultChannelId>(&uart_channel_output),
};

pw::rpc::Server server(channels);

// 3. Implement the Service
class EchoServiceImpl final
    : public pw::rpc::examples::pw_rpc::nanopb::EchoService::Service<
          EchoServiceImpl> {
 public:
  pw::Status Echo(const pw_rpc_examples_EchoMessage& request,
                  pw_rpc_examples_EchoMessage& response) {
    PW_LOG_INFO("Received Echo request: %d", static_cast<int>(request.msg_id));
    response = request;
    return pw::OkStatus();
  }
};

EchoServiceImpl echo_service;

// 4. Ingress (RX) Dispatch Thread
class RpcDispatchThread : public pw::thread::ThreadCore {
 public:
  void Run() override {
    PW_LOG_INFO("RPC Dispatch Thread active");

    std::array<std::byte, 512> decoder_buffer;
    pw::hdlc::Decoder decoder(decoder_buffer);

    std::array<uint8_t, 32> rx_raw;

    while (true) {
      size_t count = UartReadBytes(rx_raw.data(), rx_raw.size());
      for (size_t i = 0; i < count; ++i) {
        auto result = decoder.Process(static_cast<std::byte>(rx_raw[i]));
        if (result.ok()) {
          pw::hdlc::Frame& frame = result.value();
          if (frame.address() == pw::hdlc::kDefaultRpcAddress) {
            pw::Status status = server.ProcessPacket(frame.data());
            if (!status.ok()) {
              PW_LOG_WARN("Failed to process packet: %s", status.str());
            }
          }
        }
      }
      break;  // Prevent infinite loop in test harness
    }
  }
};

[[maybe_unused]] RpcDispatchThread rpc_dispatch_thread;

}  // namespace

// 5. System Initialization Entrypoint
[[maybe_unused]] void InitializeRpcSystem() {
  // Register services
  server.RegisterService(echo_service);
}
// DOCSTAG: [pw_rpc-examples-full-integration]
