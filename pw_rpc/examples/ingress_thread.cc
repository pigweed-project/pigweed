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

#include <array>
#include <cstddef>
#include <cstdint>

#include "pw_hdlc/decoder.h"
#include "pw_hdlc/default_addresses.h"
#include "pw_log/log.h"
#include "pw_rpc/server.h"
#include "pw_thread/thread_core.h"

namespace pw::rpc::examples {
namespace {

// Mock external APIs
pw::rpc::Server& GetServer() {
  static pw::rpc::Channel ch;
  static pw::rpc::Server server(pw::span(&ch, 0));
  return server;
}

size_t MyUartDriver_ReadBlocking(std::byte* /*dest*/, size_t /*max_len*/) {
  return 0;
}

size_t MyUartDriver_ReadNonBlocking(std::byte* /*dest*/, size_t /*max_len*/) {
  return 0;
}

// DOCSTAG: [pw_rpc-examples-ingress-thread]
class RpcIngressThread : public pw::thread::ThreadCore {
 public:
  void Run() override {
    PW_LOG_INFO("Starting RPC Ingress Thread...");

    std::array<std::byte, 512> decoder_buffer;
    pw::hdlc::Decoder decoder(decoder_buffer);

    std::array<std::byte, 64> rx_chunk;

    while (true) {
      // 1. Read raw bytes from physical transport (blocking read)
      size_t bytes_read =
          MyUartDriver_ReadBlocking(rx_chunk.data(), rx_chunk.size());

      // 2. Feed bytes into framing decoder
      for (size_t i = 0; i < bytes_read; ++i) {
        auto result = decoder.Process(rx_chunk[i]);
        if (result.ok()) {
          pw::hdlc::Frame& frame = result.value();

          // 3. Filter by address if multiple protocols share the link
          if (frame.address() == pw::hdlc::kDefaultRpcAddress) {
            // 4. Pass the unframed RPC packet to the server
            pw::Status status = GetServer().ProcessPacket(frame.data());
            if (!status.ok()) {
              PW_LOG_WARN("RPC ProcessPacket failed: %s", status.str());
            }
          }
        }
      }
      break;  // Prevent infinite loop in non-threaded tests
    }
  }
};
// DOCSTAG: [pw_rpc-examples-ingress-thread]

// DOCSTAG: [pw_rpc-examples-ingress-polling]
std::array<std::byte, 512> poll_decoder_buffer;
pw::hdlc::Decoder poll_decoder(poll_decoder_buffer);

// Non-blocking poll function called periodically from main superloop
[[maybe_unused]] void PollRpcIngress() {
  std::array<std::byte, 32> rx_chunk;

  // Non-blocking read: returns 0 immediately if no bytes available
  size_t bytes_read =
      MyUartDriver_ReadNonBlocking(rx_chunk.data(), rx_chunk.size());

  for (size_t i = 0; i < bytes_read; ++i) {
    auto result = poll_decoder.Process(rx_chunk[i]);
    if (result.ok()) {
      pw::hdlc::Frame& frame = result.value();
      if (frame.address() == pw::hdlc::kDefaultRpcAddress) {
        pw::Status status = GetServer().ProcessPacket(frame.data());
        if (!status.ok()) {
          PW_LOG_WARN("RPC ProcessPacket error: %s", status.str());
        }
      }
    }
  }
}
// DOCSTAG: [pw_rpc-examples-ingress-polling]

}  // namespace
}  // namespace pw::rpc::examples
