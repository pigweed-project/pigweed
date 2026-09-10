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
#include <mutex>

#include "pw_bytes/span.h"
#include "pw_rpc/channel.h"
#include "pw_rpc/server.h"
#include "pw_status/status.h"
#include "pw_sync/mutex.h"

namespace pw::rpc::examples {
namespace {

// Stub for driver transmission
pw::Status MyDriver_Transmit(const void* /*data*/, size_t /*size*/) {
  return pw::OkStatus();
}

// DOCSTAG: [pw_rpc-examples-channel-output-subclass]
class MyUartChannelOutput : public pw::rpc::ChannelOutput {
 public:
  constexpr MyUartChannelOutput(const char* name)
      : pw::rpc::ChannelOutput(name) {}

  // Returns the maximum size packet this output can transmit.
  size_t MaximumTransmissionUnit() override { return kMaxMtu; }

  // Sends an encoded pw_rpc packet over the transport.
  //
  // CRITICAL RULES:
  // 1. The buffer is ONLY valid for the duration of this call. Transmit
  //    synchronously or copy to a DMA/queue buffer before returning.
  // 2. NEVER call any pw_rpc Server/Client APIs inside Send() (causes
  //    deadlocks).
  // 3. Packet transmission cannot fail from the perspective of pw_rpc. If the
  //    underlying transport cannot send the packet, drop it.
  pw::Status Send(pw::span<const std::byte> buffer) override {
    MyDriver_Transmit(buffer.data(), buffer.size()).IgnoreError();
    return pw::OkStatus();
  }

 private:
  static constexpr size_t kMaxMtu = 512;
};
// DOCSTAG: [pw_rpc-examples-channel-output-subclass]

// DOCSTAG: [pw_rpc-examples-synchronized-channel-output]
class SynchronizedChannelOutput : public pw::rpc::ChannelOutput {
 public:
  SynchronizedChannelOutput(MyUartChannelOutput& output, const char* name)
      : pw::rpc::ChannelOutput(name), output_(output) {}

  pw::Status Send(pw::span<const std::byte> buffer) override {
    std::lock_guard guard(mutex_);
    return output_.Send(buffer);
  }

  size_t MaximumTransmissionUnit() override {
    return output_.MaximumTransmissionUnit();
  }

 private:
  pw::sync::Mutex mutex_;
  MyUartChannelOutput& output_;
};
// DOCSTAG: [pw_rpc-examples-synchronized-channel-output]

// DOCSTAG: [pw_rpc-examples-channel-instantiation]
enum class RpcChannelId : uint32_t {
  kHostUartChannel = 1,
  kPeerMcuSpiChannel = 2,
};

MyUartChannelOutput uart_output("UART_Output");
MyUartChannelOutput spi_output("SPI_Output");

// Define static channels array
pw::rpc::Channel channels[] = {
    pw::rpc::Channel::Create<RpcChannelId::kHostUartChannel>(&uart_output),
    pw::rpc::Channel::Create<RpcChannelId::kPeerMcuSpiChannel>(&spi_output),
};
// DOCSTAG: [pw_rpc-examples-channel-instantiation]

// DOCSTAG: [pw_rpc-examples-server-instantiation]
pw::rpc::Server server(channels);

[[maybe_unused]] pw::rpc::Server& GetServer() { return server; }
// DOCSTAG: [pw_rpc-examples-server-instantiation]

}  // namespace
}  // namespace pw::rpc::examples
