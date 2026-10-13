// Copyright 2024 The Pigweed Authors
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

#include <cstdint>
#include <mutex>
#include <optional>

#include "pw_allocator/allocator.h"
#include "pw_async2/dispatcher.h"
#include "pw_channel/channel.h"
#include "pw_channel/forwarding_channel.h"
#include "pw_containers/inline_var_len_entry_queue.h"
#include "pw_hdlc/router.h"
#include "pw_multibuf/multibuf.h"
#include "pw_multibuf/simple_allocator.h"
#include "pw_rpc/server.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/mutex.h"
#include "pw_sync/thread_notification.h"
#include "pw_system/config.h"
#include "pw_thread/thread.h"

namespace pw::system::internal {

// pw::rpc::ChannelOutput with a queue for outgoing RPC packets.
class RpcChannelOutputQueue final : public rpc::ChannelOutput {
 public:
  RpcChannelOutputQueue()
      : rpc::ChannelOutput("RPC output queue"), dropped_packets_(0) {}

  // Read packets from the outbound queue.
  async2::Poll<InlineVarLenEntryQueue<>::const_value_type> PendOutgoingDatagram(
      async2::Context& cx);

  // Pops the packet read from a Pend call.
  void Pop() {
    std::lock_guard lock(mutex_);
    queue_.pop();
  }

  // Number of outgoing packets that have been dropped.
  uint32_t dropped_packets() const {
    std::lock_guard lock(mutex_);
    return dropped_packets_;
  }

  size_t MaximumTransmissionUnit() override {
    return PW_SYSTEM_MAX_TRANSMISSION_UNIT;
  }

 private:
  Status Send(ConstByteSpan datagram) override;

  mutable sync::Mutex mutex_;
  InlineVarLenEntryQueue<PW_SYSTEM_MAX_TRANSMISSION_UNIT> queue_
      PW_GUARDED_BY(mutex_);
  async2::Waker packet_ready_ PW_GUARDED_BY(mutex_);
  uint32_t dropped_packets_ PW_GUARDED_BY(mutex_);
};

// Thread that receives inbound RPC packets and calls
// pw::rpc::Server::ProcessPacket() with them.
class RpcServerThread final {
 public:
  RpcServerThread(Allocator& allocator, rpc::Server& server);

  async2::Poll<InlineVarLenEntryQueue<>::const_value_type> PendOutgoingDatagram(
      async2::Context& cx) {
    return rpc_packet_queue_.PendOutgoingDatagram(cx);
  }

  void PopOutboundPacket() { return rpc_packet_queue_.Pop(); }

  // This approach only works with a single producer.
  async2::Poll<> PendReadyForPacket(async2::Context& cx) {
    std::lock_guard lock(mutex_);
    if (ready_for_packet_) {
      return async2::Ready();
    }
    PW_ASYNC_STORE_WAKER(
        cx, ready_to_receive_packet_, "RpcServerThread waiting for RPC packet");
    return async2::Pending();
  }

  void PushPacket(multibuf::MultiBuf&& packet);

  void RunOnce();

 private:
  Allocator& allocator_;
  sync::Mutex mutex_;
  bool ready_for_packet_ PW_GUARDED_BY(mutex_) = true;
  async2::Waker ready_to_receive_packet_ PW_GUARDED_BY(mutex_);
  multibuf::MultiBuf packet_multibuf_;
  sync::ThreadNotification new_packet_available_;
  RpcChannelOutputQueue rpc_packet_queue_;
  rpc::Server& rpc_server_;
};

class PacketIO {
 public:
  PacketIO(channel::ByteReaderWriter& io_channel,
           ByteSpan buffer,
           Allocator& allocator,
           rpc::Server& rpc_server);

  void Start(async2::Dispatcher& dispatcher,
             const thread::Options& thread_options);

 private:
  class PacketReader : public async2::Task {
   public:
    constexpr PacketReader(PacketIO& io) : io_(io) {}

   private:
    async2::Poll<> DoPend(async2::Context& cx) override;

    PacketIO& io_;
  };

  class PacketWriter : public async2::Task {
   public:
    constexpr PacketWriter(PacketIO& io)
        : io_(io), outbound_packet_(async2::Pending()) {}

   private:
    async2::Poll<> DoPend(async2::Context& cx) override;

    PacketIO& io_;
    async2::Poll<InlineVarLenEntryQueue<>::const_value_type> outbound_packet_;
  };

  channel::DatagramReaderWriter& channel() { return channels_.first(); }

  std::byte mb_allocator_buffer_1_[PW_SYSTEM_MAX_TRANSMISSION_UNIT];
  std::byte mb_allocator_buffer_2_[PW_SYSTEM_MAX_TRANSMISSION_UNIT];
  Allocator& allocator_;
  multibuf::SimpleAllocator mb_allocator_1_;
  multibuf::SimpleAllocator mb_allocator_2_;
  channel::ForwardingDatagramChannelPair channels_;
  hdlc::Router router_;
  RpcServerThread rpc_server_thread_;

  PacketReader packet_reader_;
  PacketWriter packet_writer_;
};

}  // namespace pw::system::internal
