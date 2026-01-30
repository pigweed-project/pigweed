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

#include "pw_async2/dispatcher.h"
#include "pw_async2/value_future.h"
#include "pw_bluetooth/gatt/client2.h"
#include "pw_bluetooth/internal/raii_ptr.h"
#include "pw_bluetooth/low_energy/channel.h"
#include "pw_bluetooth/types.h"

namespace pw::bluetooth::low_energy {

/// @module{pw_bluetooth}

/// Class that represents a connection to a peer. This can be used to interact
/// with GATT services and establish LE L2CAP channels.
///
/// The lifetime of this object is tied to that of the LE connection it
/// represents. Destroying the object results in a disconnection.
class Connection2 {
 public:
  /// Possible errors when updating the connection parameters.
  enum class ConnectionParameterUpdateError : uint8_t {
    kFailure,
    kInvalidParameters,
    kRejected,
  };

  /// Possible reasons a connection was disconnected.
  enum class DisconnectReason : uint8_t {
    kFailure,
    kRemoteUserTerminatedConnection,
    /// This usually indicates that the link supervision timeout expired.
    kConnectionTimeout,
  };

  /// Actual connection parameters returned by the controller.
  struct ConnectionParameters {
    /// The connection interval indicates the frequency of link layer connection
    /// events over which data channel PDUs can be transmitted. See Core Spec
    /// v6, Vol 6, Part B, Section 4.5.1 for more information on the link
    /// layer connection events.
    /// - Range: 0x0006 to 0x0C80
    /// - Time: N * 1.25 ms
    /// - Time Range: 7.5 ms to 4 s.
    uint16_t interval;

    /// The maximum allowed peripheral connection latency in number of
    /// connection events. See Core Spec v5, Vol 6, Part B, Section 4.5.1.
    /// - Range: 0x0000 to 0x01F3
    uint16_t latency;

    /// This defines the maximum time between two received data packet PDUs
    /// before the connection is considered lost. See Core Spec v6, Vol 6,
    /// Part B, Section 4.5.2.
    /// - Range: 0x000A to 0x0C80
    /// - Time: N * 10 ms
    /// - Time Range: 100 ms to 32 s
    uint16_t supervision_timeout;
  };

  /// Connection parameters that either the local device or a peer device are
  /// requesting.
  struct RequestedConnectionParameters {
    /// Minimum value for the connection interval. This shall be less than or
    /// equal to `max_interval`. The connection interval indicates the frequency
    /// of link layer connection events over which data channel PDUs can be
    /// transmitted. See Core Spec v6, Vol 6, Part B, Section 4.5.1 for more
    /// information on the link layer connection events.
    /// - Range: 0x0006 to 0x0C80
    /// - Time: N * 1.25 ms
    /// - Time Range: 7.5 ms to 4 s.
    uint16_t min_interval;

    /// Maximum value for the connection interval. This shall be greater than or
    /// equal to `min_interval`. The connection interval indicates the frequency
    /// of link layer connection events over which data channel PDUs can be
    /// transmitted.  See Core Spec v6, Vol 6, Part B, Section 4.5.1 for more
    /// information on the link layer connection events.
    /// - Range: 0x0006 to 0x0C80
    /// - Time: N * 1.25 ms
    /// - Time Range: 7.5 ms to 4 s.
    uint16_t max_interval;

    /// Maximum peripheral latency for the connection in number of connection
    /// events. See Core Spec v6, Vol 6, Part B, Section 4.5.1.
    /// - Range: 0x0000 to 0x01F3
    uint16_t max_latency;

    /// This defines the maximum time between two received data packet PDUs
    /// before the connection is considered lost. See Core Spec v6, Vol 6,
    /// Part B, Section 4.5.2.
    /// - Range: 0x000A to 0x0C80
    /// - Time: N * 10 ms
    /// - Time Range: 100 ms to 32 s
    uint16_t supervision_timeout;
  };

  /// Represents parameters that are set on a per-connection basis.
  struct ConnectionOptions {
    /// When true, the connection operates in bondable mode. This means pairing
    /// will form a bond, or persist across disconnections, if the peer is also
    /// in bondable mode. When false, the connection operates in non-bondable
    /// mode, which means the local device only allows pairing that does not
    /// form a bond.
    bool bondable_mode = true;

    /// When present, service discovery performed following the connection is
    /// restricted to primary services that match this field. Otherwise, by
    /// default all available services are discovered.
    std::optional<Uuid> service_filter;

    /// When present, specifies the initial connection parameters. Otherwise,
    /// the connection parameters will be selected by the implementation.
    std::optional<RequestedConnectionParameters> parameters;

    /// When present, specifies the ATT MTU to request. The actual MTU used may
    /// be smaller depending on peer and controller support. If none is
    /// specified, the host implementation will select the ATT MTU. Note that an
    /// MTU of 247 is the largest that can fit into a single LE data packet with
    /// the Data Length Extension.
    /// - LE ATT MTU Range: 23 to 517
    /// - LE EATT MTU Range: 64 to 517
    std::optional<uint16_t> att_mtu;
  };

  struct ConnectL2capParameters {
    /// The identifier of the service to connect to.
    Psm psm;
    /// Maximum supported packet size for receiving.
    uint16_t max_receive_packet_size;
    /// The security requirements that must be met before data is exchanged on
    /// the channel. If the requirements cannot be met, channel establishment
    /// will fail.
    SecurityRequirements security_requirements;
  };

  /// If a disconnection has not occurred, destroying this object will result in
  /// disconnection.
  virtual ~Connection2() = default;

  /// Returns Ready after the peer disconnects or there is a connection error
  /// that caused a disconnection. Awakens `cx` on disconnect.
  virtual async2::Poll<DisconnectReason> PendDisconnect(
      async2::Context& cx) = 0;

  /// Returns a GATT client to the connected peer that is valid for the lifetime
  /// of this `Connection2` object. `Connection2` is considered alive as long as
  /// `PendDisconnect()` returns pending and the object hasn't been destroyed.
  virtual gatt::Client2* GattClient() = 0;

  /// Returns the current ATT Maximum Transmission Unit. By subtracting ATT
  /// headers from the MTU, the maximum payload size of messages can be
  /// calculated.
  virtual uint16_t AttMtu() = 0;

  /// Returns Pending until the ATT MTU changes, at which point `cx` will be
  /// awoken. Returns Ready with the new ATT MTU once the ATT MTU has been
  /// changed. The ATT MTU can only be changed once.
  virtual async2::Poll<uint16_t> PendAttMtuChange(async2::Context& cx) = 0;

  /// Returns the current connection parameters.
  virtual ConnectionParameters Parameters() = 0;

  /// Requests an update to the connection parameters.
  /// @returns Asynchronously returns the result of the request.
  virtual async2::ValueFuture<
      std::optional<pw::expected<void, ConnectionParameterUpdateError>>>
  RequestParameterUpdate(RequestedConnectionParameters parameters) = 0;

  /// Connect to an L2CAP LE connection-oriented channel.
  /// @param parameters The parameters to configure the channel with.
  /// @return The result of the connection procedure. On success, contains a
  /// `Channel` that can be used to exchange data.
  virtual async2::OptionalValueFuture<pw::Result<Channel::Ptr>> ConnectL2cap(
      ConnectL2capParameters parameters) = 0;

 private:
  /// Request to destroy this connection. This method is called by the
  /// ~Connection2::Ptr() when it goes out of scope, the API client should never
  /// call this method.
  virtual void Release() = 0;

 public:
  /// Movable `Connection2` smart pointer. When `Connection::Ptr` is destroyed
  /// the `Connection2` will disconnect automatically.
  using Ptr = internal::RaiiPtr<Connection2, &Connection2::Release>;
};

}  // namespace pw::bluetooth::low_energy
