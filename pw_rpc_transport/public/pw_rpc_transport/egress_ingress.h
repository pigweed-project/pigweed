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
#pragma once

#include <mutex>

#include "pw_bytes/span.h"
#include "pw_rpc/channel.h"
#include "pw_rpc/packet_meta.h"
#include "pw_rpc_transport/hdlc_framing.h"
#include "pw_rpc_transport/rpc_transport.h"
#include "pw_rpc_transport/simple_framing.h"
#include "pw_status/status.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/mutex.h"

namespace pw::rpc {

// Ties RPC transport and RPC frame encoder together.
class BaseRpcEgress : public RpcEgressHandler, public ChannelOutput {
 protected:
  BaseRpcEgress(const char* name) : ChannelOutput(name) {}
};

template <typename Encoder>
class RpcEgress : public BaseRpcEgress {
 public:
  RpcEgress(std::string_view channel_name, RpcFrameSender& transport)
      : BaseRpcEgress(channel_name.data()), transport_(transport) {}

  // Implements both rpc::ChannelOutput and RpcEgressHandler. Encodes the
  // provided packet using the target transport's MTU as max frame size and
  // sends it over that transport.
  //
  // Sending a packet may result in multiple RpcTransport::Write calls which
  // must not be interleaved in order for the packet to be successfully
  // reassembled from the transport-level frames by the receiver. RpcEgress
  // is using a mutex to ensure this. Technically we could just rely on pw_rpc
  // global lock but that would unnecessarily couple transport logic to pw_rpc
  // internals.
  Status SendRpcPacket(ConstByteSpan rpc_packet) override {
    std::lock_guard lock(mutex_);
    return encoder_.Encode(rpc_packet,
                           transport_.MaximumTransmissionUnit(),
                           [this](RpcFrame& frame) {
                             // Encoders must call this callback inline so that
                             // we're still holding `mutex_` here. Unfortunately
                             // the lock annotations cannot be used on
                             // `transport_` to enforce this.
                             return transport_.Send(frame);
                           });
  }

  // Implements ChannelOutput.
  Status Send(ConstByteSpan buffer) override { return SendRpcPacket(buffer); }

 private:
  sync::Mutex mutex_;
  RpcFrameSender& transport_;
  Encoder encoder_ PW_GUARDED_BY(mutex_);
};

// Ties a channel id and the egress that packets on that channel should be sent
// to.
struct ChannelEgress {
  ChannelEgress(uint32_t id, RpcEgressHandler& egress_handler)
      : channel_id(id), egress(&egress_handler) {}

  const uint32_t channel_id;
  RpcEgressHandler* const egress = nullptr;
};

// Override and provide to RpcIngress to be notified of events.
class RpcIngressTracker {
 public:
  virtual ~RpcIngressTracker() = default;
  virtual void PacketProcessed([[maybe_unused]] ConstByteSpan packet) {}
  virtual void BadPacket() {}
  virtual void ChannelIdOverflow([[maybe_unused]] uint32_t channel_id,
                                 [[maybe_unused]] uint32_t max_channel_id) {}
  virtual void MissingEgressForChannel([[maybe_unused]] uint32_t channel_id) {}
  virtual void IngressSendFailure([[maybe_unused]] uint32_t channel_id,
                                  [[maybe_unused]] Status status) {}
};

// Handler for incoming RPC packets. RpcIngress is not thread-safe and must
// be accessed from a single thread (typically the RPC RX thread).
template <typename Decoder, size_t kMaxChannelId = 64>
class RpcIngress : public RpcIngressHandler {
 public:
  RpcIngress() = default;

  RpcIngress(span<ChannelEgress> channel_egresses,
             RpcIngressTracker* tracker = nullptr)
      : tracker_(tracker) {
    for (auto& channel : channel_egresses) {
      PW_ASSERT(channel.channel_id <= kMaxChannelId);
      channel_egresses_[channel.channel_id] = channel.egress;
    }
  }

  // Finds RPC packets in `buffer`, extracts pw_rpc channel ID from each
  // packet and sends the packet to the egress registered for that channel.
  Status ProcessIncomingData(ConstByteSpan buffer) override {
    return decoder_.Decode(buffer, [this](ConstByteSpan packet) {
      const auto packet_meta = rpc::PacketMeta::FromBuffer(packet);
      ++num_total_packets_;
      if (tracker_) {
        tracker_->PacketProcessed(packet);
      }
      if (!packet_meta.ok()) {
        if (tracker_) {
          tracker_->BadPacket();
        }
        return;
      }
      if (packet_meta->channel_id() > kMaxChannelId) {
        if (tracker_) {
          tracker_->ChannelIdOverflow(packet_meta->channel_id(), kMaxChannelId);
        }
        return;
      }
      auto* egress = channel_egresses_[packet_meta->channel_id()];
      if (egress == nullptr) {
        if (tracker_) {
          tracker_->MissingEgressForChannel(packet_meta->channel_id());
        }
        return;
      }
      const auto status = egress->SendRpcPacket(packet);
      if (!status.ok()) {
        if (tracker_) {
          tracker_->IngressSendFailure(packet_meta->channel_id(), status);
        }
      }
    });
  }

  uint32_t num_total_packets() const { return num_total_packets_; }

 private:
  uint32_t num_total_packets_ = 0;
  RpcIngressTracker* tracker_;
  std::array<RpcEgressHandler*, kMaxChannelId + 1> channel_egresses_{};
  Decoder decoder_;
};

template <size_t kMaxPacketSize>
using HdlcRpcEgress = RpcEgress<HdlcRpcPacketEncoder<kMaxPacketSize>>;

template <size_t kMaxPacketSize, size_t kMaxChannelId = 64>
using HdlcRpcIngress =
    RpcIngress<HdlcRpcPacketDecoder<kMaxPacketSize>, kMaxChannelId>;

template <size_t kMaxPacketSize>
using SimpleRpcEgress = RpcEgress<SimpleRpcPacketEncoder<kMaxPacketSize>>;

template <size_t kMaxPacketSize, size_t kMaxChannelId = 64>
using SimpleRpcIngress =
    RpcIngress<SimpleRpcPacketDecoder<kMaxPacketSize>, kMaxChannelId>;

}  // namespace pw::rpc
