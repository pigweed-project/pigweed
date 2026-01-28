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

#include "pw_bluetooth_proxy/l2cap_channel_common.h"
#include "pw_bluetooth_proxy/rfcomm/rfcomm_common.h"
#include "pw_bluetooth_proxy/rfcomm/rfcomm_config.h"
#include "pw_result/result.h"

namespace pw::bluetooth::proxy::rfcomm {

class RfcommChannelManagerInterface;

/// A smart pointer-like object that manages the lifetime of an RFCOMM channel.
/// When this object goes out of scope, the underlying channel is automatically
/// released. This object cannot be copied, only moved.
class RfcommChannel {
 public:
  RfcommChannel() = default;

  /// RfcommChannel is not copyable.
  RfcommChannel(const RfcommChannel&) = delete;
  RfcommChannel& operator=(const RfcommChannel&) = delete;

  /// RfcommChannel is movable.
  RfcommChannel(RfcommChannel&& other) noexcept;
  RfcommChannel& operator=(RfcommChannel&& other) noexcept;

  ~RfcommChannel();

  explicit operator bool() const { return manager_ != nullptr; }

  /// @brief Writes a payload to the RFCOMM channel. If no credits are
  /// available, the payload is queued.
  ///
  /// @return A `StatusWithMultiBuf` containing the status of the write
  ///     operation.
  /// @retval `OK` if the payload was successfully queued.
  /// @retval `NOT_FOUND` if the channel was not found or closed.
  /// @retval `UNAVAILABLE` if the transmit queue is full. In this case, the
  ///     unwritten payload is returned to the caller.
  StatusWithMultiBuf Write(FlatConstMultiBuf&& payload);

  /// Allow comparison for testing.
  bool operator==(const RfcommChannel& other) const {
    return connection_handle_ == other.connection_handle_ &&
           channel_number_ == other.channel_number_ &&
           manager_ == other.manager_;
  }

  bool operator!=(const RfcommChannel& other) const {
    return !(*this == other);
  }

 private:
  friend class RfcommManager;

  /// Private constructor to ensure only RfcommChannelManager can create this.
  RfcommChannel(ConnectionHandle connection_handle,
                uint8_t channel_number,
                RfcommChannelManagerInterface* manager);

  /// Releases the RFCOMM channel.
  void Reset();

  ConnectionHandle connection_handle_ = static_cast<ConnectionHandle>(0);
  uint8_t channel_number_ = 0;
  RfcommChannelManagerInterface* manager_ = nullptr;
};

/// This interface defines a manager for RFCOMM channels, enabling clients to
/// acquire and manage RFCOMM channels for communication.
class RfcommChannelManagerInterface {
 public:
  virtual ~RfcommChannelManagerInterface() = default;

  /// @brief Acquires an RFCOMM channel for a given connection and channel
  /// number. The returned channel is owned by the caller and will be
  /// automatically released when it goes out of scope.
  ///
  /// @param multibuf_allocator The allocator for multibuf buffers.
  /// @param connection_handle The handle for the ACL connection.
  /// @param channel_number The server channel number for the channel.
  /// @param mux_initiator Whether this device is the initiator of the RFCOMM
  ///     multiplexer.
  /// @param rx_config Configuration for the receive direction of the
  ///     channel.
  /// @param tx_config Configuration for the transmit direction of the
  ///     channel.
  /// @param receive_fn A callback that is invoked when new data is
  ///     received on the channel.
  /// @param event_fn A callback that is invoked for channel events, such as
  ///     channel closure.
  ///
  /// @note The callbacks (`receive_fn` and `event_fn`) are invoked on an
  ///     internal dispatcher thread. Calling `AcquireRfcommChannel()` from
  ///     within these callbacks may lead to deadlocks and should be avoided.
  ///     It is also not safe to call `Write()` or `ReleaseRfcommChannel()` from
  ///     within the `receive_fn` callback.
  ///
  /// @return A `Result` containing the `RfcommChannel` on success or a
  ///     `Status` on failure.
  /// @retval `OK` if the channel was successfully acquired.
  /// @retval `ALREADY_EXISTS` if the channel already exists.
  /// @retval `RESOURCE_EXHAUSTED` if the system is out of resources to open a
  ///     new channel.
  Result<RfcommChannel> AcquireRfcommChannel(
      MultiBufAllocator& multibuf_allocator,
      ConnectionHandle connection_handle,
      uint8_t channel_number,
      bool mux_initiator,
      const RfcommChannelConfig& rx_config,
      const RfcommChannelConfig& tx_config,
      RfcommReceiveCallback&& receive_fn,
      RfcommEventCallback&& event_fn) {
    return DoAcquireRfcommChannel(multibuf_allocator,
                                  connection_handle,
                                  channel_number,
                                  mux_initiator,
                                  rx_config,
                                  tx_config,
                                  std::move(receive_fn),
                                  std::move(event_fn));
  }

  /// @brief Writes a payload to the RFCOMM channel. If no credits are
  /// available, the payload is queued.
  ///
  /// @param connection_handle The handle for the ACL connection.
  /// @param channel_number The server channel number for the channel.
  /// @param payload The payload to write.
  ///
  /// @return A `StatusWithMultiBuf` containing the status of the write
  ///     operation and any unwritten payload.
  /// @retval `OK` if the payload was successfully written or queued.
  /// @retval `NOT_FOUND` if the channel was not found or closed.
  /// @retval `UNAVAILABLE` if the transmit queue is full. In this case,
  ///     the unwritten payload is returned to the caller.
  StatusWithMultiBuf Write(ConnectionHandle connection_handle,
                           uint8_t channel_number,
                           FlatConstMultiBuf&& payload) {
    return DoWrite(connection_handle, channel_number, std::move(payload));
  }

  /// @brief Releases a previously acquired RFCOMM channel. If
  /// `close_connection_if_empty_channel` is true and this is the last RFCOMM
  /// channel for the connection, the connection is also closed.
  ///
  /// @param connection_handle The handle for the ACL connection.
  /// @param channel_number The server channel number for the channel.
  ///
  /// @return A `Status` indicating the result of the release operation.
  /// @retval `OK` if the channel was successfully released.
  /// @retval `NOT_FOUND` if the channel was not found.
  Status ReleaseRfcommChannel(ConnectionHandle connection_handle,
                              uint8_t channel_number) {
    return DoReleaseRfcommChannel(connection_handle, channel_number);
  }

 private:
  virtual Result<RfcommChannel> DoAcquireRfcommChannel(
      MultiBufAllocator& multibuf_allocator,
      ConnectionHandle connection_handle,
      uint8_t channel_number,
      bool mux_initiator,
      const RfcommChannelConfig& rx_config,
      const RfcommChannelConfig& tx_config,
      RfcommReceiveCallback&& receive_fn,
      RfcommEventCallback&& event_fn) = 0;

  virtual StatusWithMultiBuf DoWrite(ConnectionHandle connection_handle,
                                     uint8_t channel_number,
                                     FlatConstMultiBuf&& payload) = 0;

  virtual Status DoReleaseRfcommChannel(ConnectionHandle connection_handle,
                                        uint8_t channel_number) = 0;
};

}  // namespace pw::bluetooth::proxy::rfcomm
