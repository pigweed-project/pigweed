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
#include "pw_bluetooth_sapphire/internal/host/common/device_address.h"
#include "pw_bluetooth_sapphire/internal/host/gap/adapter_state.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_connection.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_connection_request.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_interrogator.h"
#include "pw_bluetooth_sapphire/internal/host/hci/connection.h"
#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_connector.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/channel_manager.h"
#include "pw_bluetooth_sapphire/internal/host/transport/command_channel.h"

namespace bt::gap::internal {

// LowEnergyConnector is a single-use utility for executing either the outbound
// connection procedure or the inbound connection procedure (which is a subset
// of the outbound procedure). The outbound procedure first scans for and
// connects to a peer, whereas the inbound procedure starts with an existing
// connection. Next, both procedures interrogate the peer. After construction,
// the connection procedure may be started with either StartOutbound() or
// StartInbound() and will run to completion unless Cancel() is called.
class LowEnergyConnector final {
 public:
  using ResultCallback =
      hci::ResultCallback<std::unique_ptr<LowEnergyConnection>>;

  // Create a connector for connecting to |peer_id|. The connection will be
  // established with the parameters specified in |options|.
  LowEnergyConnector(
      PeerId peer_id,
      LowEnergyConnectionOptions options,
      hci::Transport::WeakPtr hci,
      PeerCache* peer_cache,
      WeakSelf<LowEnergyConnectionManager>::WeakPtr conn_mgr,
      l2cap::ChannelManager* l2cap,
      gatt::GATT::WeakPtr gatt,
      const AdapterState& adapter_state,
      pw::async::Dispatcher& dispatcher,
      hci::LocalAddressDelegate* local_address_delegate,
      pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider);

  // Instances should only be destroyed after the result callback is called
  // (except for stack tear down). Due to the asynchronous nature of cancelling
  // the connection process, it is NOT safe to destroy a connector before the
  // result callback has been called. The connector will be unable to wait for
  // the HCI connection cancellation to complete, which can lead to failure to
  // connect in later connectors (as the hci::LowEnergyConnector is still
  // pending).
  ~LowEnergyConnector();

  // Initiate an outbound connection. |cb| will be called with the result of the
  // procedure. Must only be called once.
  void StartOutbound(pw::chrono::SystemClock::duration request_timeout,
                     hci::LowEnergyConnector* connector,
                     LowEnergyDiscoveryManager::WeakPtr discovery_manager,
                     ResultCallback cb);

  // Start interrogating peer using an already established |connection|. |cb|
  // will be called with the result of the procedure. Must only be called once.
  void StartInbound(std::unique_ptr<hci::LowEnergyConnection> connection,
                    ResultCallback cb);

  // Canceling a connector that has not started or has already completed is a
  // no-op. Otherwise, the pending result callback will be called asynchronously
  // once cancelation has succeeded.
  void Cancel();

  // Attach connector inspect node as a child node of |parent| with the name
  // |name|.
  void AttachInspect(inspect::Node& parent, std::string name);

  // Returns true if this connector currently owns a LowEnergyConnection (i.e.
  // a link to the peer has been established and is being interrogated).
  bool has_connection() const { return connection_ != nullptr; }

 private:
  enum class State {
    kDefault,
    kEnsuringLocalAddress,                               // Outbound only
    kConnecting,                                         // Outbound only
    kInterrogating,                                      // Outbound & inbound
    kAwaitingConnectionFailedToBeEstablishedDisconnect,  // Outbound & inbound
    kPauseBeforeConnectionRetry,                         // Outbound only
    kComplete,                                           // Outbound & inbound
    kFailed,                                             // Outbound & inbound
  };

  static const char* StateToString(State);

  // Ensure the local address is updated (e.g. if privacy mode was just
  // changed).
  void EnsureLocalAddress();

  // Initiate HCI connection procedure.
  void RequestCreateConnection();
  void OnConnectResult(hci::Result<> status,
                       std::unique_ptr<hci::LowEnergyConnection> link);

  // Creates LowEnergyConnection and initializes fixed channels & timers.
  // Returns true on success, false on failure.
  bool InitializeConnection(std::unique_ptr<hci::LowEnergyConnection> link);

  void StartInterrogation();
  void OnInterrogationComplete(hci::Result<> status);

  // Handle a disconnect during kInterrogating or
  // kAwaitingConnectionFailedToBeEstablishedDisconnect.
  void OnPeerDisconnect(pw::bluetooth::emboss::StatusCode status);

  // Returns true if the connection is retried.
  //
  // The link layer only considers a connection established after a packet is
  // received from the peer before (6 * connInterval), even though it notifies
  // the host immediately after sending a CONNECT_IND pdu. See Core Spec v5.2,
  // Vol 6, Part B, Sec 4.5 for details.
  //
  // In the field, we have noticed a substantial amount of 0x3e (Connection
  // Failed to be Established) HCI link errors occurring on links AFTER being
  // notified of successful HCI-level connection. To work around this issue, we
  // perform link-layer interrogation on the peer before returning
  // gap::LowEnergyConnections to higher layer clients. If we receive the 0x3e
  // error during interrogation, we will retry the connection process a number
  // of times.
  bool MaybeRetryConnection();

  void NotifySuccess();
  void NotifyFailure(hci::Result<> status = ToResult(HostError::kFailed));

  // Set is_outbound_ and its Inspect property.
  void set_is_outbound(bool is_outbound);

  pw::async::Dispatcher& dispatcher_;

  StringInspectable<State> state_{
      State::kDefault,
      /*convert=*/[](auto s) { return StateToString(s); }};

  PeerId peer_id_;
  DeviceAddress peer_address_;
  PeerCache* peer_cache_;

  // Layer pointers to be passed to LowEnergyConnection.
  l2cap::ChannelManager* l2cap_;
  gatt::GATT::WeakPtr gatt_;

  const AdapterState& adapter_state_;

  // True if this connector is connecting an outbound connection, false if it is
  // connecting an inbound connection.
  std::optional<bool> is_outbound_;

  // Time after which an outbound HCI connection request is considered to have
  // timed out. This is configurable to allow unit tests to set a shorter value.
  pw::chrono::SystemClock::duration hci_request_timeout_;

  LowEnergyConnectionOptions options_;

  // Callback used to return the result of the connection procedure to the
  // owning class.
  ResultCallback result_cb_;

  // Used to connect outbound connections during the kConnecting state.
  hci::LowEnergyConnector* hci_connector_ = nullptr;

  // The LowEnergyConnection to be passed to LowEnergyConnectionManager. Created
  // during the kConnecting state for outbound connections, or during
  // construction for inbound connections.
  std::unique_ptr<internal::LowEnergyConnection> connection_;

  // For outbound connections, this is a 0-indexed counter of which connection
  // attempt the connector is on.
  IntInspectable<int> connection_attempt_{0};

  SmartTask request_create_connection_task_{dispatcher_};

  // Task called after the scan attempt times out.
  std::optional<SmartTask> scan_timeout_task_;

  // Sends HCI commands that request version and feature support information
  // from peer controllers. Initialized only during interrogation.
  std::optional<LowEnergyInterrogator> interrogator_;

  LowEnergyDiscoveryManager::WeakPtr discovery_manager_;

  hci::CommandChannel::WeakPtr cmd_;

  hci::Transport::WeakPtr hci_;

  // Used to construct a LowEnergyConnection and to check that no other
  // LowEnergyConnection to the peer exists before creating one.
  WeakSelf<LowEnergyConnectionManager>::WeakPtr le_connection_manager_;

  hci::LocalAddressDelegate* local_address_delegate_;

  pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider_;

  struct InspectProperties {
    inspect::StringProperty peer_id;
    inspect::BoolProperty is_outbound;
  };
  InspectProperties inspect_properties_;
  inspect::Node inspect_node_;

  WeakSelf<LowEnergyConnector> weak_self_{this};
};

}  // namespace bt::gap::internal
