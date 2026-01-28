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

#pragma once

#include "pw_bluetooth_proxy/channel_proxy.h"
#include "pw_bluetooth_proxy/internal/l2cap_channel.h"
#include "pw_bluetooth_proxy/l2cap_channel_common.h"
#include "pw_bluetooth_proxy/rfcomm/rfcomm_common.h"
#include "pw_bluetooth_proxy/rfcomm/rfcomm_config.h"
#include "pw_checksum/crc8.h"
#include "pw_containers/inline_queue.h"
#include "pw_containers/intrusive_map.h"
#include "pw_function/function.h"
#include "pw_sync/mutex.h"

namespace pw::bluetooth::proxy::rfcomm::internal {

// Represents an RFCOMM channel.
class RfcommChannelInternal
    : public IntrusiveMap<uint8_t, RfcommChannelInternal>::Item {
 public:
  RfcommChannelInternal(MultiBufAllocator& multibuf_allocator,
                        ChannelProxy& l2cap_channel_proxy,
                        ConnectionHandle connection_handle,
                        uint8_t channel_number,
                        bool mux_initiator,
                        const RfcommChannelConfig& rx_config,
                        const RfcommChannelConfig& tx_config,
                        const pw::checksum::Crc8& crc_calculator,
                        RfcommReceiveCallback&& receive_fn,
                        RfcommEventCallback&& event_fn);

  virtual ~RfcommChannelInternal() = default;

  // Closes the RFCOMM channel.
  virtual void Close(RfcommEvent event);

  // Writes a payload to the RFCOMM channel. If no credits are available, the
  // payload is queued.
  StatusWithMultiBuf Write(FlatConstMultiBuf&& payload);

  // Handles an RFCOMM PDU from the controller.
  // Returns false if the PDU was handled, true is for forwarded to the
  // Host.
  bool HandlePduFromController(uint8_t credits, ConstByteSpan pdu);

  uint8_t channel_number() const { return channel_number_; }
  uint8_t key() const { return channel_number_; }
  ConnectionHandle connection_handle() const { return connection_handle_; }

 private:
  // Add credits for sending data.
  void AddCredits(uint8_t credits);

  // Sends credits to the controller.
  Status SendCredits(uint8_t credits);

  // Tries to send a packet if credits are available.
  void TryToSendPacket() PW_EXCLUSIVE_LOCKS_REQUIRED(tx_mutex_);

  MultiBufAllocator& multibuf_allocator_;
  ChannelProxy& l2cap_channel_proxy_;
  const ConnectionHandle connection_handle_;
  const uint8_t channel_number_;
  const bool mux_initiator_;
  const RfcommChannelConfig rx_config_;
  const RfcommChannelConfig tx_config_;
  const pw::checksum::Crc8& crc_calculator_;
  const RfcommReceiveCallback receive_fn_;
  const RfcommEventCallback event_fn_;

  // The members below must be protected by their respective mutexes.
  sync::Mutex mutex_ PW_ACQUIRED_BEFORE(tx_mutex_);
  enum class State { kOpen, kClosed };
  State state_ PW_GUARDED_BY(mutex_) = State::kOpen;

  sync::Mutex tx_mutex_ PW_ACQUIRED_AFTER(mutex_) PW_ACQUIRED_BEFORE(rx_mutex_);
  uint8_t tx_credits_ PW_GUARDED_BY(tx_mutex_) = 0;
  // The offset into the current packet that will be sent next.
  size_t send_packet_offset_ PW_GUARDED_BY(tx_mutex_) = 0;
  // The pending credit frame to be sent to the controller.
  std::optional<FlatConstMultiBufInstance> pending_credit_tx_
      PW_GUARDED_BY(tx_mutex_);
  // The maximum number of packets that can be queued for transmit.
  static constexpr size_t kDefaultTxQueueSize = 10;
  InlineQueue<FlatConstMultiBufInstance, kDefaultTxQueueSize> tx_queue_
      PW_GUARDED_BY(tx_mutex_);

  sync::Mutex rx_mutex_ PW_ACQUIRED_AFTER(tx_mutex_);
  uint8_t rx_credits_ PW_GUARDED_BY(rx_mutex_) = 0;
};

}  // namespace pw::bluetooth::proxy::rfcomm::internal
