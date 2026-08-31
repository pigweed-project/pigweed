// Copyright 2025 The Pigweed Authors
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

#include <variant>

#include "pw_allocator/unique_ptr.h"
#include "pw_bluetooth_proxy/basic_mode_channel_config.h"
#include "pw_bluetooth_proxy/channel_proxy.h"
#include "pw_bluetooth_proxy/connection_handle.h"
#include "pw_bluetooth_proxy/internal/logical_transport.h"
#include "pw_bluetooth_proxy/l2cap_channel_common.h"
#include "pw_bluetooth_proxy/l2cap_coc_config.h"
#include "pw_bytes/span.h"
#include "pw_function/function.h"

namespace pw::bluetooth::proxy {

class L2capChannelManagerInterface {
 public:
  virtual ~L2capChannelManagerInterface() = default;

  // Return false to intercept/take the packet, or true to forward the packet.
  // This is an optimization to avoid allocating and copying on every H4 packet.
  // TODO: https://pwbug.dev/411168474 - Use multibuf for H4 packets and delete
  // this.
  using SpanReceiveFunction = Function<bool(ConstByteSpan,
                                            ConnectionHandle connection_handle,
                                            uint16_t local_channel_id,
                                            uint16_t remote_channel_id)>;

  using OptionalBufferReceiveFunction =
      Function<std::optional<multibuf::MultiBuf>(
          multibuf::MultiBuf&& payload,
          ConnectionHandle connection_handle,
          uint16_t local_channel_id,
          uint16_t remote_channel_id)>;

  using BufferReceiveFunction =
      std::variant<OptionalBufferReceiveFunction, SpanReceiveFunction>;

  /// Returns an L2CAP channel operating in basic mode that supports writing to
  /// and reading from a remote peer.
  ///
  /// @param[in] config                     Configuration parameters for the
  ///                                       channel.
  ///
  /// @param[in] payload_from_controller_fn Read callback to be invoked on Rx
  ///                                       SDUs. If a multibuf is returned by
  ///                                       the callback, it is copied into the
  ///                                       payload to be forwarded to the host.
  ///                                       Optional null return indicates
  ///                                       packet was handled and no forwarding
  ///                                       is required.
  ///
  /// @param[in] payload_from_host_fn       Read callback to be invoked on Tx
  ///                                       SDUs. If a multibuf is returned by
  ///                                       the callback, it is copied into the
  ///                                       payload to be forwarded to the
  ///                                       controller. Optional null return
  ///                                       indicates packet was handled and no
  ///                                       forwarding is required.
  ///
  /// @param[in] event_fn                   Handle asynchronous events such as
  ///                                       errors and flow control events
  ///                                       encountered by the channel. See
  ///                                       `l2cap_channel_common.h`.
  ///                                       Must outlive the channel and remain
  ///                                       valid until the channel destructor
  ///                                       returns.
  /// @returns @Result{the channel}
  /// * @INVALID_ARGUMENT: Arguments are invalid. Check the logs.
  /// * @UNAVAILABLE: A channel could not be created because no memory was
  ///   available to accommodate an additional ACL connection.
  /// * @CANCELLED: Offload channel recovery failed because client data was
  ///   lost according to the snapshot and `allow_data_loss` is false.
  Result<UniquePtr<ChannelProxy>> InterceptBasicModeChannel(
      BasicModeChannelConfig config,
      BufferReceiveFunction&& payload_from_controller_fn,
      BufferReceiveFunction&& payload_from_host_fn,
      ChannelEventCallback&& event_fn) {
    return DoInterceptBasicModeChannel(config,
                                       std::move(payload_from_controller_fn),
                                       std::move(payload_from_host_fn),
                                       std::move(event_fn));
  }

  /// Returns an L2CAP credit-based flow control channel that supports writing
  /// to and reading from a remote peer.
  ///
  /// @param[in] connection_handle      The connection handle of the remote
  ///                                   peer.
  ///
  /// @param[in] rx_config              Parameters applying to reading packets.
  ///                                   See @ref ConnectionOrientedChannelConfig
  ///                                   for details.
  ///
  /// @param[in] tx_config              Parameters applying to writing packets.
  ///                                   See @ref ConnectionOrientedChannelConfig
  ///                                   for details.
  ///
  /// @param[in] receive_fn             Read callback to be invoked on Rx SDUs.
  ///
  /// @param[in] event_fn               Handle asynchronous events such as
  ///                                   errors and flow control events
  ///                                   encountered by the channel. See
  ///                                   `l2cap_channel_common.h`.
  ///                                   Must outlive the channel and remain
  ///                                   valid until the channel destructor
  ///                                   returns.
  ///
  /// @returns @Result{the channel}
  /// * @INVALID_ARGUMENT: Arguments are invalid. Check the logs.
  /// * @UNAVAILABLE: A channel could not be created because no memory was
  ///   available to accommodate an additional ACL connection.
  /// * @CANCELLED: Offload channel recovery failed because an SDU or ACL frame
  ///   transfer was in progress when the proxy snapshot was captured.
  Result<UniquePtr<ChannelProxy>> InterceptCreditBasedFlowControlChannel(
      ConnectionHandle connection_handle,
      ConnectionOrientedChannelConfig rx_config,
      ConnectionOrientedChannelConfig tx_config,
      MultiBufReceiveFunction&& receive_fn,
      ChannelEventCallback&& event_fn) {
    return DoInterceptCreditBasedFlowControlChannel(connection_handle,
                                                    rx_config,
                                                    tx_config,
                                                    std::move(receive_fn),
                                                    std::move(event_fn));
  }

 private:
  // TODO: https://pwbug.dev/553990261 - Remove transitional 7-parameter
  // overload once downstream implementations are updated.
  virtual Result<UniquePtr<ChannelProxy>> DoInterceptBasicModeChannel(
      ConnectionHandle /*connection_handle*/,
      uint16_t /*local_channel_id*/,
      uint16_t /*remote_channel_id*/,
      AclTransportType /*transport*/,
      BufferReceiveFunction&& /*payload_from_controller_fn*/,
      BufferReceiveFunction&& /*payload_from_host_fn*/,
      ChannelEventCallback&& /*event_fn*/) {
    return Status::Unimplemented();
  }

  // TODO: https://pwbug.dev/553990261 - Return to pure virtual once downstream
  // implementations are updated.
  virtual Result<UniquePtr<ChannelProxy>> DoInterceptBasicModeChannel(
      BasicModeChannelConfig config,
      BufferReceiveFunction&& payload_from_controller_fn,
      BufferReceiveFunction&& payload_from_host_fn,
      ChannelEventCallback&& event_fn) {
    return DoInterceptBasicModeChannel(config.connection_handle,
                                       config.local_channel_id,
                                       config.remote_channel_id,
                                       config.transport,
                                       std::move(payload_from_controller_fn),
                                       std::move(payload_from_host_fn),
                                       std::move(event_fn));
  }

  virtual Result<UniquePtr<ChannelProxy>>
  DoInterceptCreditBasedFlowControlChannel(
      ConnectionHandle connection_handle,
      ConnectionOrientedChannelConfig rx_config,
      ConnectionOrientedChannelConfig tx_config,
      Function<void(multibuf::MultiBuf&& payload)>&& receive_fn,
      ChannelEventCallback&& event_fn) = 0;
};

}  // namespace pw::bluetooth::proxy
