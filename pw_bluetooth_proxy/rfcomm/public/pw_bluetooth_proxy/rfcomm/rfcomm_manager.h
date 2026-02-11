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

#include <memory>

#include "pw_allocator/unique_ptr.h"
#include "pw_bluetooth/rfcomm_frames.emb.h"
#include "pw_bluetooth_proxy/channel_proxy.h"
#include "pw_bluetooth_proxy/l2cap_channel_manager_interface.h"
#include "pw_bluetooth_proxy/rfcomm/internal/rfcomm_channel_internal.h"
#include "pw_bluetooth_proxy/rfcomm/rfcomm_channel_manager_interface.h"
#include "pw_bluetooth_proxy/rfcomm/rfcomm_common.h"
#include "pw_bluetooth_proxy/rfcomm/rfcomm_config.h"
#include "pw_checksum/crc8.h"
#include "pw_containers/intrusive_map.h"
#include "pw_sync/mutex.h"

namespace pw::bluetooth::proxy::rfcomm {

// Manages RFCOMM channels over L2CAP channels. This manager supports multiple
// RFCOMM channels per ACL connection, but only one L2CAP channel per ACL
// connection.
class RfcommManager final : public RfcommChannelManagerInterface {
 public:
  RfcommManager(L2capChannelManagerInterface& l2cap_channel_manager,
                Allocator& allocator);
  ~RfcommManager() override;

  // Deregisters all channels for the given connection and closes the
  // connection.
  void DeregisterAndCloseChannels(RfcommEvent event);

 private:
  // The CRC-8 polynomial used for RFCOMM frame checksums.
  //   - Polynomial: 0x07 (x^8 + x^2 + x + 1)
  //   - Initial value: 0xFF
  //   - Reflect in: true
  //   - Reflect out: true
  //   - XOR out: 0xFF
  static constexpr pw::checksum::Crc8 kRfcommCrc =
      pw::checksum::Crc8(0x07, 0xFF, true, true, 0xff);

  // RfcommChannelManagerInterface overrides:
  Result<RfcommChannel> DoAcquireRfcommChannel(
      MultiBufAllocator& multibuf_allocator,
      ConnectionHandle connection_handle,
      uint8_t channel_number,
      RfcommDirection direction,
      bool mux_initiator,
      const RfcommChannelConfig& rx_config,
      const RfcommChannelConfig& tx_config,
      RfcommReceiveCallback&& receive_fn,
      RfcommEventCallback&& event_fn) override;

  StatusWithMultiBuf DoWrite(ConnectionHandle connection_handle,
                             uint8_t channel_number,
                             RfcommDirection direction,
                             FlatConstMultiBuf&& payload) override;

  Status DoReleaseRfcommChannel(ConnectionHandle connection_handle,
                                uint8_t channel_number,
                                RfcommDirection direction) override;

  struct ConnectionState
      : public IntrusiveMap<ConnectionHandle, ConnectionState>::Item {
    ConnectionState(ConnectionHandle handle,
                    uint16_t local_cid_arg,
                    uint16_t remote_cid_arg);
    ConnectionState(const ConnectionState&) = delete;
    ConnectionState& operator=(const ConnectionState&) = delete;

    ConnectionHandle key() const { return connection_handle; }

    UniquePtr<ChannelProxy> l2cap_channel_proxy;
    ConnectionHandle connection_handle;
    uint16_t local_cid;
    uint16_t remote_cid;

    // This map is protected by the `connections_mutex_`.
    IntrusiveMap<uint8_t, rfcomm::internal::RfcommChannelInternal> channels;
  };

  // Handles an RFCOMM PDU received from the controller. If the PDU is not
  // handled, it is returned to the caller to be forwarded to the host.
  std::optional<FlatConstMultiBufInstance> HandlePduFromController(
      FlatMultiBuf&& pdu,
      ConnectionHandle connection_handle,
      uint16_t local_cid,
      uint16_t remote_cid);

  // Handles an L2CAP event for a connection. This is called by the L2CAP
  // channel proxy.
  void HandleL2capEvent(L2capChannelEvent event,
                        ConnectionHandle connection_handle);

  // Parses an RFCOMM PDU into its components.
  static Result<emboss::RfcommFrameView> ParseRfcommFrame(ConstByteSpan pdu);

  // Closes all channels for the given connection.
  void CloseAllChannelsForConnection(ConnectionHandle connection_handle,
                                     RfcommEvent event);

  // Closes all channels for the given connection state and deletes the
  // connection state.
  void CloseConnectionState(ConnectionState* conn_state, RfcommEvent event);

  L2capChannelManagerInterface& l2cap_channel_manager_;
  Allocator& allocator_;

  // Protects `connections_` from concurrent access. This is also used to
  // synchronize the destruction of connection states.
  sync::Mutex connections_mutex_;
  IntrusiveMap<ConnectionHandle, ConnectionState> connections_
      PW_GUARDED_BY(connections_mutex_);
};

}  // namespace pw::bluetooth::proxy::rfcomm
