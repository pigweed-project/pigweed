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
#include "pw_bluetooth_proxy/config.h"
#include "pw_bluetooth_proxy/l2cap_channel_manager_interface.h"
#include "pw_bluetooth_proxy/rfcomm/internal/rfcomm_channel_internal.h"
#include "pw_bluetooth_proxy/rfcomm/rfcomm_channel_manager_interface.h"
#include "pw_bluetooth_proxy/rfcomm/rfcomm_common.h"
#include "pw_bluetooth_proxy/rfcomm/rfcomm_config.h"
#include "pw_bluetooth_proxy/rfcomm/rfcomm_snapshot.h"
#include "pw_checksum/crc8.h"
#include "pw_containers/dynamic_map.h"
#include "pw_sync/mutex.h"

namespace pw::bluetooth::proxy::rfcomm {

// Manages RFCOMM channels over L2CAP channels. This manager supports multiple
// RFCOMM channels per ACL connection, but only one L2CAP channel per ACL
// connection.
class RfcommManager final : public RfcommChannelManagerInterface {
 public:
  /// Creates an `RfcommManager`.
  ///
  /// @param[in] l2cap_channel_manager Interface to L2CAP channel manager.
  /// @param[in] allocator Allocator used for connection and channel
  /// allocations.
  /// @param[in] state_update_callback Optional callback to receive incremental
  /// state updates for offload recovery persistence.
  RfcommManager(L2capChannelManagerInterface& l2cap_channel_manager,
                Allocator& allocator,
                RfcommStateUpdateCallback state_update_callback = nullptr);
  ~RfcommManager() override;

  // Deregisters all channels for the given connection and closes the
  // connection.
  void DeregisterAndCloseChannels(RfcommEvent event);

#if PW_BLUETOOTH_PROXY_CONFIG_ENABLE_RECOVERY
  /// Restores RFCOMM subsystem state from a previously saved snapshot.
  ///
  /// @note Must be called during initialization within the paused-traffic
  /// recovery window before packet traffic is processed, and after
  /// `RecoverFromSnapshot()` on ACL and L2CAP state.
  ///
  /// @note The caller must ensure that the @p snapshot object remains valid
  /// and in scope until `CompleteRecovery()` returns.
  ///
  /// @note During the recovery window, re-acquiring channels that were present
  /// in the restored snapshot suppresses initial creation notifications to
  /// avoid duplicate records in the container. Acquiring a channel that was not
  /// present in the snapshot will emit an initial state update.
  ///
  /// @param[in] snapshot The snapshot containing persisted RFCOMM channel
  /// state.
  ///
  /// @returns
  /// * @OK: State restored successfully.
  /// * @INVALID_ARGUMENT: Snapshot pointer is null.
  /// * @DATA_LOSS: Snapshot was marked incomplete or invalid.
  Status RecoverFromSnapshot(const RfcommSnapshot* snapshot)
      PW_LOCKS_EXCLUDED(connections_mutex_);

  /// Completes RFCOMM offload recovery and sweeps abandoned RFCOMM channels.
  ///
  /// Purges tracking of channels present in the restored snapshot that were not
  /// re-acquired by the host during the recovery window, sending channel
  /// removal notifications for each abandoned channel.
  ///
  /// @note Must be called at the end of the recovery window before packet
  /// traffic is processed, after all active RFCOMM channels have been
  /// re-acquired.
  void CompleteRecovery() PW_LOCKS_EXCLUDED(connections_mutex_);
#endif  // PW_BLUETOOTH_PROXY_CONFIG_ENABLE_RECOVERY

 private:
  // The map of RFCOMM channels for a connection, keyed by DLCI.
  using ChannelMap = DynamicMap<uint8_t, internal::RfcommChannelInternal>;

  struct ConnectionState {
    ConnectionState(ConnectionHandle handle,
                    uint16_t local_cid_arg,
                    uint16_t remote_cid_arg,
                    Allocator& allocator);
    ConnectionState(const ConnectionState&) = delete;
    ConnectionState& operator=(const ConnectionState&) = delete;

    UniquePtr<ChannelProxy> l2cap_channel_proxy;
    ConnectionHandle connection_handle;
    uint16_t local_cid;
    uint16_t remote_cid;

    // This map is protected by the `connections_mutex_`.
    ChannelMap channels;
  };

  // The map of connections.
  using ConnectionMap = DynamicMap<ConnectionHandle, ConnectionState>;

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
      multibuf::MultiBufAllocator& multibuf_allocator,
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
                             multibuf::MultiBuf&& payload) override;

  Status DoReleaseRfcommChannel(ConnectionHandle connection_handle,
                                uint8_t channel_number,
                                RfcommDirection direction) override;

  Status DoSendAdditionalRxCredits(ConnectionHandle connection_handle,
                                   uint8_t channel_number,
                                   RfcommDirection direction,
                                   uint8_t credits) override;

  // Handles an RFCOMM PDU received from the controller. If the PDU is not
  // handled, it is returned to the caller to be forwarded to the host.
  std::optional<multibuf::MultiBuf> HandlePduFromController(
      multibuf::MultiBuf&& pdu,
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

  // Closes all channels for the given connection state node and destroys it.
  void CloseConnectionState(
      UniquePtr<ConnectionMap::node_type>&& conn_state_node, RfcommEvent event);

  L2capChannelManagerInterface& l2cap_channel_manager_;
  Allocator& allocator_;

  // Protects `connections_` from concurrent access. This is also used to
  // synchronize the destruction of connection states.
  sync::Mutex connections_mutex_;
  ConnectionMap connections_ PW_GUARDED_BY(connections_mutex_);

#if PW_BLUETOOTH_PROXY_CONFIG_ENABLE_RECOVERY
  void NotifyChannelStateUpdate(const internal::RfcommChannelInternal& channel);
  void NotifyChannelRemoved(ConnectionHandle connection_handle,
                            uint8_t channel_number,
                            RfcommDirection direction);

  const RfcommSnapshot* restored_snapshot_ PW_GUARDED_BY(connections_mutex_){
      nullptr};

  // Registered state update callback for offload recovery persistence. This
  // must only be modified during initialization before packet traffic is
  // processed. This allows it to be safely invoked without acquiring any locks.
  RfcommStateUpdateCallback state_update_callback_;
#endif  // PW_BLUETOOTH_PROXY_CONFIG_ENABLE_RECOVERY
};

}  // namespace pw::bluetooth::proxy::rfcomm
