// Copyright 2022 The Pigweed Authors
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

#include <cinttypes>
#include <mutex>

#include "pw_function/function.h"
#include "pw_rpc/channel.h"
#include "pw_rpc/client_server.h"
#include "pw_rpc/internal/fake_channel_output.h"
#include "pw_rpc/internal/lock.h"
#include "pw_rpc/packet_meta.h"
#include "pw_span/span.h"
#include "pw_status/status.h"

namespace pw::rpc {
using TestPacketProcessor =
    pw::Function<pw::Status(ClientServer&, pw::ConstByteSpan)>;

namespace internal {

// Expands on a Fake Channel Output implementation to allow for forwarding of
// packets.
template <typename FakeChannelOutputImpl,
          size_t kOutputSize,
          size_t kMaxPackets,
          size_t kPayloadsBufferSizeBytes>
class ForwardingChannelOutput : public ChannelOutput {
 public:
  size_t MaximumTransmissionUnit() override {
    return output_.MaximumTransmissionUnit();
  }

  Status Send(span<const std::byte> buffer) override {
    return output_.Send(buffer);
  }

  // Returns true if new packets were available to forward
  bool ForwardNextPacket(ClientServer& client_server) {
    std::array<std::byte, kOutputSize> packet_buffer;
    Result<ConstByteSpan> result = EncodeNextUnsentPacket(packet_buffer);
    if (!result.ok()) {
      return false;
    }
    ++sent_packets_;

    Result<PacketMeta> meta = pw::rpc::PacketMeta::FromBuffer(*result);
    PW_ASSERT(meta.ok());

    pw::Status process_result = pw::Status::Internal();
    if (meta->destination_is_server() && server_packet_processor_) {
      process_result = server_packet_processor_(client_server, *result);
    } else if (meta->destination_is_client() && client_packet_processor_) {
      process_result = client_packet_processor_(client_server, *result);
    } else {
      process_result = client_server.ProcessPacket(*result);
    }
    PW_ASSERT(process_result.ok());
    return true;
  }

  // Clears and resets the FakeChannelOutputImpl.
  void clear() {
    output_.clear();
    sent_packets_ = 0;
  }

 protected:
  explicit ForwardingChannelOutput(
      TestPacketProcessor&& server_packet_processor = nullptr,
      TestPacketProcessor&& client_packet_processor = nullptr)
      : ChannelOutput("testing::FakeChannelOutput"),
        server_packet_processor_(std::move(server_packet_processor)),
        client_packet_processor_(std::move(client_packet_processor)) {}

  FakeChannelOutputImpl output_;

  // Functions are virtual to allow for their override in threaded version, so
  // threading protection can be added.
  virtual size_t PacketCount() const { return output_.total_packets(); }

  virtual Result<ConstByteSpan> EncodeNextUnsentPacket(
      std::array<std::byte, kPayloadsBufferSizeBytes>& packet_buffer) {
    std::lock_guard lock(output_.mutex_);
    const auto& packets = output_.packets();
    if (packets.size() <= sent_packets_) {
      return Status::NotFound();
    }
    return packets[sent_packets_].Encode(packet_buffer);
  }

  uint16_t sent_packets_ = 0;

  const TestPacketProcessor server_packet_processor_;
  const TestPacketProcessor client_packet_processor_;
};

// Provides a testing context with a real client and server. This class is for
// internal use only and should not be used directly. Instead, use
// `NanopbClientServerTestContext` in pw_rpc/nanopb/client_server_testing.h or
// `PwpbClientServerTestContext` in pw_rpc/pwpb/client_server_testing.h.
template <typename ForwardingChannelOutputImpl,
          size_t kOutputSize = 128,
          size_t kMaxPackets = 16,
          size_t kPayloadsBufferSizeBytes = 128>
class ClientServerTestContext {
 public:
  const pw::rpc::Channel& channel() { return channel_; }
  Client& client() { return client_server_.client(); }
  Server& server() { return client_server_.server(); }

  // Should be called after each rpc call to synchronously forward all queued
  // messages. Otherwise this function can be ignored.
  void ForwardNewPackets() {
    while (channel_output_.ForwardNextPacket(client_server_)) {
    }
  }

  // Clears and resets the ForwardingChannelOutputImpl.
  void clear() { channel_output_.clear(); }

 protected:
  explicit ClientServerTestContext(
      TestPacketProcessor&& server_packet_processor = nullptr,
      TestPacketProcessor&& client_packet_processor = nullptr)
      : channel_output_(std::move(server_packet_processor),
                        std::move(client_packet_processor)),
        channel_(Channel::Create<1>(&channel_output_)),
        client_server_({&channel_, 1}) {}

  ~ClientServerTestContext() = default;

  ForwardingChannelOutputImpl channel_output_;

 private:
  pw::rpc::Channel channel_;
  ClientServer client_server_;
};

}  // namespace internal
}  // namespace pw::rpc
