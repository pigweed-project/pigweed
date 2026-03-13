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
#include <lib/fit/function.h>

#include <list>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "lib/fit/result.h"
#include "pw_bluetooth_sapphire/internal/host/common/bounded_inspect_list_node.h"
#include "pw_bluetooth_sapphire/internal/host/common/error.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/common/metrics.h"
#include "pw_bluetooth_sapphire/internal/host/common/windowed_inspect_numeric_property.h"
#include "pw_bluetooth_sapphire/internal/host/gap/adapter_state.h"
#include "pw_bluetooth_sapphire/internal/host/gap/gap.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_connection_request.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_connector.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_discovery_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gap/periodic_advertising_sync_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/gatt.h"
#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_connection.h"
#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_connector.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/channel_manager.h"
#include "pw_bluetooth_sapphire/internal/host/sm/error.h"
#include "pw_bluetooth_sapphire/internal/host/sm/security_manager.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"
#include "pw_bluetooth_sapphire/internal/host/transport/command_channel.h"
#include "pw_bluetooth_sapphire/internal/host/transport/control_packets.h"
#include "pw_bluetooth_sapphire/internal/host/transport/error.h"
#include "pw_bluetooth_sapphire/internal/host/transport/transport.h"

namespace bt {

namespace hci {
class LocalAddressDelegate;
}  // namespace hci

namespace gap {

namespace internal {
class LowEnergyConnection;
}  // namespace internal

// TODO(armansito): Document the usage pattern.

class LowEnergyConnectionManager;
class PairingDelegate;
class Peer;
class PeerCache;

enum class LowEnergyDisconnectReason : uint8_t {
  // Explicit disconnect request
  kApiRequest,
  // An internal error was encountered
  kError,
  // All references were dropped
  kZeroRef,
  // Remote device disconnected
  kPeerDisconnection,
};

// LowEnergyConnectionManager is responsible for connecting and initializing new
// connections, interrogating connections, intiating pairing, and disconnecting
// connections.
class LowEnergyConnectionManager final {
 public:
  // Duration after which connection failures are removed from Inspect.
  static constexpr pw::chrono::SystemClock::duration
      kInspectRecentConnectionFailuresExpiryDuration = std::chrono::minutes(10);

  // |hci|: The HCI transport used to track link layer connection events from
  //        the controller.
  // |addr_delegate|: Used to obtain local identity information during pairing
  //                  procedures.
  // |connector|: Adapter object for initiating link layer connections. This
  //              object abstracts the legacy and extended HCI command sets.
  // |peer_cache|: The cache that stores peer peer data. The connection
  //                 manager stores and retrieves pairing data and connection
  //                 parameters to/from the cache. It also updates the
  //                 connection and bonding state of a peer via the cache.
  // |l2cap|: Used to interact with the L2CAP layer.
  // |gatt|: Used to interact with the GATT profile layer.
  // |adapter_state|: Provides information on controller capabilities.
  // |transfer_periodic_advertising_sync_fn|: Function to call to request a
  //   periodic advertising sync transfer (PAST).
  LowEnergyConnectionManager(
      hci::Transport::WeakPtr hci,
      hci::LocalAddressDelegate* addr_delegate,
      hci::LowEnergyConnector* connector,
      PeerCache* peer_cache,
      l2cap::ChannelManager* l2cap,
      gatt::GATT::WeakPtr gatt,
      LowEnergyDiscoveryManager::WeakPtr discovery_manager,
      sm::SecurityManagerFactory sm_creator,
      const AdapterState& adapter_state,
      pw::async::Dispatcher& dispatcher,
      pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider,
      PeriodicAdvertisingSyncManager::TransferSyncFn&&
          transfer_periodic_advertising_sync_fn);
  ~LowEnergyConnectionManager();

  // Allows a caller to claim shared ownership over a connection to the
  // requested remote LE peer identified by |peer_id|.
  //   * If |peer_id| is not recognized, |callback| is called with an error.
  //
  //   * If the requested peer is already connected, |callback| is called with a
  //     LowEnergyConnectionHandle immediately.
  //     This is done for both local and remote initiated connections (i.e. the
  //     local adapter can either be in the LE central or peripheral roles).
  //
  //   * If the requested peer is NOT connected, then this method initiates a
  //     connection to the requested peer using the
  //     internal::LowEnergyConnector. See that class's documentation for a more
  //     detailed overview of the Connection process. A
  //     LowEnergyConnectionHandle is asynchronously returned to the caller once
  //     the connection has been set up.
  //
  // The status of the procedure is reported in |callback| in the case of an
  // error.
  using ConnectionResult =
      fit::result<HostError, std::unique_ptr<LowEnergyConnectionHandle>>;
  using ConnectionResultCallback = fit::function<void(ConnectionResult)>;
  void Connect(PeerId peer_id,
               ConnectionResultCallback callback,
               LowEnergyConnectionOptions connection_options);

  hci::LocalAddressDelegate* local_address_delegate() const {
    return local_address_delegate_;
  }

  // Disconnects any existing or pending LE connection to |peer_id|,
  // invalidating all active LowEnergyConnectionHandles. Returns false if the
  // peer can not be disconnected.
  bool Disconnect(PeerId peer_id,
                  LowEnergyDisconnectReason reason =
                      LowEnergyDisconnectReason::kApiRequest);

  // Initializes a new connection over the given |link| and asynchronously
  // returns a connection reference.
  //
  // |link| must be the result of a remote initiated connection.
  //
  // |callback| will be called with a connection status and connection
  // reference. The connection reference will be nullptr if the connection was
  // rejected (as indicated by a failure status).
  //
  // TODO(armansito): Add an |own_address| parameter for the locally advertised
  // address that was connected to.
  //
  // A link with the given handle should not have been previously registered.
  void RegisterRemoteInitiatedLink(
      std::unique_ptr<hci::LowEnergyConnection> link,
      sm::BondableMode bondable_mode,
      ConnectionResultCallback callback);

  // Returns the PairingDelegate currently assigned to this connection manager.
  const PairingDelegate::WeakPtr& pairing_delegate() const {
    return pairing_delegate_;
  }

  // Assigns a new PairingDelegate to handle LE authentication challenges.
  // Replacing an existing pairing delegate cancels all ongoing pairing
  // procedures. If a delegate is not set then all pairing requests will be
  // rejected.
  void SetPairingDelegate(const PairingDelegate::WeakPtr& delegate);

  // Opens a new L2CAP channel to service |psm| on |peer_id| using the preferred
  // parameters |params|.
  //
  // |cb| will be called with the channel created to the peer, or nullptr if the
  // channel creation resulted in an error.
  void OpenL2capChannel(PeerId peer_id,
                        l2cap::Psm psm,
                        l2cap::ChannelParameters params,
                        sm::SecurityLevel security_level,
                        l2cap::ChannelCallback cb);

  // TODO(armansito): Add a PeerCache::Observer interface and move these
  // callbacks there.

  // Called when a link with the given handle gets disconnected. This event is
  // guaranteed to be called before invalidating connection references.
  // |callback| is run on the creation thread.
  //
  // NOTE: This is intended ONLY for unit tests. Clients should watch for
  // disconnection events using LowEnergyConnectionHandle::set_closed_callback()
  // instead. DO NOT use outside of tests.
  using DisconnectCallback = fit::function<void(hci_spec::ConnectionHandle)>;
  void SetDisconnectCallbackForTesting(DisconnectCallback callback);

  // Sets the timeout interval to be used on future connect requests. The
  // default value is kLECreateConnectionTimeout.
  void set_request_timeout_for_testing(
      pw::chrono::SystemClock::duration value) {
    request_timeout_ = value;
  }

  // Callback for hci::Connection, called when the peer disconnects.
  // |reason| is used to control retry logic.
  void OnPeerDisconnect(const hci::Connection* connection,
                        pw::bluetooth::emboss::StatusCode reason);

  // Initiates the pairing process. Expected to only be called during
  // higher-level testing.
  //   |peer_id|: the peer to pair to - if the peer is not connected, |cb| is
  //   called with an error. |pairing_level|: determines the security level of
  //   the pairing. **Note**: If the security
  //                    level of the link is already >= |pairing level|, no
  //                    pairing takes place.
  //   |bondable_mode|: sets the bonding mode of this connection. A device in
  //   bondable mode forms a
  //                    bond to the peer upon pairing, assuming the peer is also
  //                    in bondable mode. A device in non-bondable mode will not
  //                    allow pairing that forms a bond.
  //   |cb|: callback called upon completion of this function, whether pairing
  //   takes place or not.
  void Pair(PeerId peer_id,
            sm::SecurityLevel pairing_level,
            sm::BondableMode bondable_mode,
            sm::ResultFunction<> cb);

  // Sets the LE security mode of the local device (see v5.2 Vol. 3 Part C
  // Section 10.2). If set to SecureConnectionsOnly, any currently encrypted
  // links not meeting the requirements of Security Mode 1 Level 4 will be
  // disconnected.
  void SetSecurityMode(LESecurityMode mode);

  // Attach manager inspect node as a child node of |parent|.
  void AttachInspect(inspect::Node& parent, std::string name);

  LESecurityMode security_mode() const { return security_mode_; }
  sm::SecurityManagerFactory sm_factory_func() const {
    return sm_factory_func_;
  }

  using WeakPtr = WeakSelf<LowEnergyConnectionManager>::WeakPtr;

 private:
  friend class internal::LowEnergyConnection;

  // Mapping from peer identifiers to open LE connections.
  using ConnectionMap =
      std::unordered_map<PeerId,
                         std::unique_ptr<internal::LowEnergyConnection>>;

  // Called by LowEnergyConnectionHandle::Release().
  void ReleaseReference(LowEnergyConnectionHandle* handle);

  // Initiates a new connection attempt for the next peer in the pending list,
  // if any.
  void TryCreateNextConnection();

  // Called by internal::LowEnergyConnector to indicate the result of a local
  // connect request.
  void OnLocalInitiatedConnectResult(
      hci::Result<std::unique_ptr<internal::LowEnergyConnection>> result);

  // Called by internal::LowEnergyConnector to indicate the result of a remote
  // connect request.
  void OnRemoteInitiatedConnectResult(
      PeerId peer_id,
      hci::Result<std::unique_ptr<internal::LowEnergyConnection>> result);

  // Either report an error to clients or initialize the connection and report
  // success to clients.
  void ProcessConnectResult(
      hci::Result<std::unique_ptr<internal::LowEnergyConnection>> result,
      internal::LowEnergyConnectionRequest request);

  // Finish setting up connection, adding to |connections_| map, and notifying
  // clients.
  bool InitializeConnection(
      std::unique_ptr<internal::LowEnergyConnection> connection,
      internal::LowEnergyConnectionRequest request);

  // Cleans up a connection state. This results in a HCI_Disconnect command if
  // the connection has not already been disconnected, and notifies any
  // referenced LowEnergyConnectionHandles of the disconnection. Marks the
  // corresponding PeerCache entry as disconnected and cleans up all data
  // bearers.
  //
  // |conn_state| will have been removed from the underlying map at the time of
  // a call. Its ownership is passed to the method for disposal.
  //
  // This is also responsible for unregistering the link from managed subsystems
  // (e.g. L2CAP).
  void CleanUpConnection(std::unique_ptr<internal::LowEnergyConnection> conn,
                         LowEnergyDisconnectReason reason);

  void RecordDisconnectInspect(const internal::LowEnergyConnection& conn,
                               LowEnergyDisconnectReason reason);

  // Updates |peer_cache_| with the given |link| and returns the corresponding
  // Peer.
  //
  // Creates a new Peer if |link| matches a peer that did not
  // previously exist in the cache. Otherwise this updates and returns an
  // existing Peer.
  //
  // The returned peer is marked as non-temporary and its connection
  // parameters are updated.
  //
  // Called by RegisterRemoteInitiatedLink() and RegisterLocalInitiatedLink().
  Peer* UpdatePeerWithLink(const hci::LowEnergyConnection& link);

  // Called when the peer disconnects with a "Connection Failed to be
  // Established" error. Cleans up the existing connection and adds the
  // connection request back to the queue for a retry.
  void CleanUpAndRetryConnection(
      std::unique_ptr<internal::LowEnergyConnection> connection);

  // Returns an iterator into |connections_| if a connection is found that
  // matches the given logical link |handle|. Otherwise, returns an iterator
  // that is equal to |connections_.end()|.
  //
  // The general rules of validity around std::unordered_map::iterator apply to
  // the returned value.
  ConnectionMap::iterator FindConnection(hci_spec::ConnectionHandle handle);

  pw::async::Dispatcher& dispatcher_;

  hci::Transport::WeakPtr hci_;

  // The pairing delegate used for authentication challenges. If nullptr, all
  // pairing requests will be rejected.
  PairingDelegate::WeakPtr pairing_delegate_;

  // The GAP LE security mode of the device (v5.2 Vol. 3 Part C 10.2).
  LESecurityMode security_mode_;

  // The function used to create each channel's SecurityManager implementation.
  sm::SecurityManagerFactory sm_factory_func_;

  // Time after which a connection attempt is considered to have timed out. This
  // is configurable to allow unit tests to set a shorter value.
  pw::chrono::SystemClock::duration request_timeout_;

  // The peer cache is used to look up and persist remote peer data that is
  // relevant during connection establishment (such as the address, preferred
  // connection parameters, etc). Expected to outlive this instance.
  PeerCache* peer_cache_;  // weak

  // The reference to L2CAP, used to interact with the L2CAP layer to
  // manage LE logical links, fixed channels, and LE-specific L2CAP signaling
  // events (e.g. connection parameter update).
  l2cap::ChannelManager* l2cap_;

  // The GATT layer reference, used to add and remove ATT data bearers and
  // service discovery.
  gatt::GATT::WeakPtr gatt_;

  // Provides us with information on the capabilities of our controller
  AdapterState adapter_state_;

  // Local GATT service registry.
  std::unique_ptr<gatt::LocalServiceManager> gatt_registry_;

  LowEnergyDiscoveryManager::WeakPtr discovery_manager_;

  // Callbacks used by unit tests to observe connection state events.
  DisconnectCallback test_disconn_cb_;

  // Outstanding connection requests based on remote peer ID.
  std::unordered_map<PeerId, internal::LowEnergyConnectionRequest>
      pending_requests_;

  // Mapping from peer identifiers to currently open LE connections.
  ConnectionMap connections_;

  struct RequestAndConnector {
    internal::LowEnergyConnectionRequest request;
    std::unique_ptr<internal::LowEnergyConnector> connector;
  };
  // The in-progress locally initiated connection request, if any.
  std::optional<RequestAndConnector> current_request_;

  // Active connectors for remote connection requests.
  std::unordered_map<PeerId, RequestAndConnector> remote_connectors_;

  // For passing to internal::LowEnergyConnector. |hci_connector_| must
  // out-live this connection manager.
  hci::LowEnergyConnector* hci_connector_;  // weak

  // Address manager is used to obtain local identity information during pairing
  // procedures. Expected to outlive this instance.
  hci::LocalAddressDelegate* local_address_delegate_;  // weak

  pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider_;

  // True if the connection manager is performing a scan for a peer before
  // connecting.
  bool scanning_ = false;

  PeriodicAdvertisingSyncManager::TransferSyncFn
      transfer_periodic_advertising_sync_fn_;

  struct InspectProperties {
    // Count of connection failures in the past 10 minutes.
    explicit InspectProperties(pw::async::Dispatcher& pw_dispatcher)
        : recent_connection_failures(
              pw_dispatcher, kInspectRecentConnectionFailuresExpiryDuration) {}
    WindowedInspectIntProperty recent_connection_failures;

    UintMetricCounter outgoing_connection_success_count_;
    UintMetricCounter outgoing_connection_failure_count_;
    UintMetricCounter incoming_connection_success_count_;
    UintMetricCounter incoming_connection_failure_count_;

    UintMetricCounter disconnect_explicit_disconnect_count_;
    UintMetricCounter disconnect_link_error_count_;
    UintMetricCounter disconnect_zero_ref_count_;
    UintMetricCounter disconnect_remote_disconnection_count_;
  };
  InspectProperties inspect_properties_{dispatcher_};
  inspect::Node inspect_node_;
  // Container node for pending request nodes.
  inspect::Node inspect_pending_requests_node_;
  // container node for connection nodes.
  inspect::Node inspect_connections_node_;

  BoundedInspectListNode last_disconnected_list_ =
      BoundedInspectListNode(/*capacity=*/5);

  // Keep this as the last member to make sure that all weak pointers are
  // invalidated before other members get destroyed.
  WeakSelf<LowEnergyConnectionManager> weak_self_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LowEnergyConnectionManager);
};

}  // namespace gap
}  // namespace bt
