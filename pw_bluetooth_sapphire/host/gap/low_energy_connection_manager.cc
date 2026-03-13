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

#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_connection_manager.h"

#include <cpp-string/string_printf.h>
#include <pw_assert/check.h>
#include <pw_preprocessor/compiler.h>

#include <optional>
#include <vector>

#include "pw_bluetooth_sapphire/internal/host/gap/gap.h"
#include "pw_bluetooth_sapphire/internal/host/gap/generic_access_client.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_connection.h"
#include "pw_bluetooth_sapphire/internal/host/gap/pairing_delegate.h"
#include "pw_bluetooth_sapphire/internal/host/gap/peer.h"
#include "pw_bluetooth_sapphire/internal/host/gap/peer_cache.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/local_service_manager.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/defaults.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/util.h"
#include "pw_bluetooth_sapphire/internal/host/hci/local_address_delegate.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/channel_manager.h"
#include "pw_bluetooth_sapphire/internal/host/sm/error.h"
#include "pw_bluetooth_sapphire/internal/host/sm/security_manager.h"
#include "pw_bluetooth_sapphire/internal/host/sm/smp.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"
#include "pw_bluetooth_sapphire/internal/host/sm/util.h"
#include "pw_bluetooth_sapphire/internal/host/transport/transport.h"

using bt::sm::BondableMode;

namespace bt::gap {

namespace {

// If an auto-connect attempt fails with any of the following error codes, we
// will stop auto- connecting to the peer until the next successful connection.
// We have only observed this issue with the 0x3e
// "kConnectionFailedToBeEstablished" error in the field, but have included
// these other errors based on their descriptions in v5.2 Vol. 1 Part F
// Section 2.
bool ShouldStopAlwaysAutoConnecting(pw::bluetooth::emboss::StatusCode err) {
  PW_MODIFY_DIAGNOSTICS_PUSH();
  PW_MODIFY_DIAGNOSTIC(ignored, "-Wswitch-enum");
  switch (err) {
    case pw::bluetooth::emboss::StatusCode::CONNECTION_TIMEOUT:
    case pw::bluetooth::emboss::StatusCode::CONNECTION_REJECTED_SECURITY:
    case pw::bluetooth::emboss::StatusCode::CONNECTION_ACCEPT_TIMEOUT_EXCEEDED:
    case pw::bluetooth::emboss::StatusCode::CONNECTION_TERMINATED_BY_LOCAL_HOST:
    case pw::bluetooth::emboss::StatusCode::CONNECTION_FAILED_TO_BE_ESTABLISHED:
      return true;
    default:
      return false;
  }
  PW_MODIFY_DIAGNOSTICS_POP();
}

// During the initial connection to a peripheral we use the initial high
// duty-cycle parameters to ensure that initiating procedures (bonding,
// encryption setup, service discovery) are completed quickly. Once these
// procedures are complete, we will change the connection interval to the
// peripheral's preferred connection parameters (see v5.0, Vol 3, Part C,
// Section 9.3.12).
static const hci_spec::LEPreferredConnectionParameters
    kInitialConnectionParameters(kLEInitialConnIntervalMin,
                                 kLEInitialConnIntervalMax,
                                 /*max_latency=*/0,
                                 hci_spec::defaults::kLESupervisionTimeout);

const char* LowEnergyDisconnectReasonToString(
    LowEnergyDisconnectReason reason) {
  switch (reason) {
    case LowEnergyDisconnectReason::kApiRequest:
      return "api request";
    case LowEnergyDisconnectReason::kError:
      return "error";
    case LowEnergyDisconnectReason::kZeroRef:
      return "zero ref";
    case LowEnergyDisconnectReason::kPeerDisconnection:
      return "peer disconnection";
  }
}

const char* kInspectRequestsNodeName = "pending_requests";
const char* kInspectRequestNodeNamePrefix = "pending_request_";
const char* kInspectConnectionsNodeName = "connections";
const char* kInspectConnectionNodePrefix = "connection_";
const char* kInspectOutboundConnectorNodeName = "outbound_connector";
const char* kInspectConnectionFailuresPropertyName =
    "recent_connection_failures";

const char* kInspectOutgoingSuccessCountNodeName =
    "outgoing_connection_success_count";
const char* kInspectOutgoingFailureCountNodeName =
    "outgoing_connection_failure_count";
const char* kInspectIncomingSuccessCountNodeName =
    "incoming_connection_success_count";
const char* kInspectIncomingFailureCountNodeName =
    "incoming_connection_failure_count";

const char* kInspectDisconnectExplicitDisconnectNodeName =
    "disconnect_explicit_disconnect_count";
const char* kInspectDisconnectLinkErrorNodeName = "disconnect_link_error_count";
const char* kInspectDisconnectZeroRefNodeName = "disconnect_zero_ref_count";
const char* kInspectDisconnectRemoteDisconnectionNodeName =
    "disconnect_remote_disconnection_count";
const char* kInspectLastDisconnectedListName = "last_disconnected";
const char* kInspectLastDisconnectedItemPeerIdPropertyName = "peer_id";
const char* kInspectLastDisconnectedItemConnectedTimePropertyName =
    "connected_@time";
const char* kInspectLastDisconnectedItemTimePropertyName = "@time";
const char* kInspectLastDisconnectedItemReasonPropertyName = "reason";

}  // namespace

LowEnergyConnectionManager::LowEnergyConnectionManager(
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
        transfer_periodic_advertising_sync_fn)
    : dispatcher_(dispatcher),
      hci_(std::move(hci)),
      security_mode_(LESecurityMode::Mode1),
      sm_factory_func_(std::move(sm_creator)),
      request_timeout_(kLECreateConnectionTimeout),
      peer_cache_(peer_cache),
      l2cap_(l2cap),
      gatt_(gatt),
      adapter_state_(adapter_state),
      discovery_manager_(discovery_manager),
      hci_connector_(connector),
      local_address_delegate_(addr_delegate),
      wake_lease_provider_(wake_lease_provider),
      transfer_periodic_advertising_sync_fn_(
          std::move(transfer_periodic_advertising_sync_fn)),
      weak_self_(this) {
  PW_DCHECK(peer_cache_);
  PW_DCHECK(l2cap_);
  PW_DCHECK(gatt_.is_alive());
  PW_DCHECK(hci_.is_alive());
  PW_DCHECK(hci_connector_);
  PW_DCHECK(local_address_delegate_);
}

LowEnergyConnectionManager::~LowEnergyConnectionManager() {
  bt_log(INFO, "gap-le", "LowEnergyConnectionManager shutting down");

  weak_self_.InvalidatePtrs();

  // Clear |pending_requests_| and notify failure.
  for (auto& iter : pending_requests_) {
    iter.second.NotifyCallbacks(fit::error(HostError::kFailed));
  }
  pending_requests_.clear();

  current_request_.reset();

  remote_connectors_.clear();

  // Clean up all connections.
  for (auto& iter : connections_) {
    CleanUpConnection(std::move(iter.second),
                      LowEnergyDisconnectReason::kApiRequest);
  }

  connections_.clear();
}

void LowEnergyConnectionManager::Connect(
    PeerId peer_id,
    ConnectionResultCallback callback,
    LowEnergyConnectionOptions connection_options) {
  Peer* peer = peer_cache_->FindById(peer_id);
  if (!peer) {
    bt_log(WARN, "gap-le", "peer not found (id: %s)", bt_str(peer_id));
    callback(fit::error(HostError::kNotFound));
    return;
  }

  if (peer->technology() == TechnologyType::kClassic) {
    bt_log(ERROR,
           "gap-le",
           "peer does not support LE: %s",
           peer->ToString().c_str());
    callback(fit::error(HostError::kNotFound));
    return;
  }

  if (!peer->connectable()) {
    bt_log(
        ERROR, "gap-le", "peer not connectable: %s", peer->ToString().c_str());
    callback(fit::error(HostError::kNotFound));
    return;
  }

  // If we are already waiting to connect to |peer_id| then we store
  // |callback| to be processed after the connection attempt completes (in
  // either success of failure).
  auto pending_iter = pending_requests_.find(peer_id);
  if (pending_iter != pending_requests_.end()) {
    if (!current_request_) {
      bt_log(WARN,
             "gap-le",
             "Connect called for peer with pending request while no "
             "current_request_ exists (peer: "
             "%s)",
             bt_str(peer_id));
    }
    // TODO(fxbug.dev/42144310): Merge connection_options with the options of
    // the pending request.
    pending_iter->second.AddCallback(std::move(callback));
    // TODO(fxbug.dev/42148775): Try to create this connection.
    return;
  }

  // Add callback to connecting request if |peer_id| matches.
  if (current_request_ && current_request_->request.peer_id() == peer_id) {
    // TODO(fxbug.dev/42144310): Merge connection_options with the options of
    // the current request.
    current_request_->request.AddCallback(std::move(callback));
    return;
  }

  auto conn_iter = connections_.find(peer_id);
  if (conn_iter != connections_.end()) {
    // TODO(fxbug.dev/42144310): Handle connection_options that conflict with
    // the existing connection.
    callback(fit::ok(conn_iter->second->AddRef()));
    return;
  }

  internal::LowEnergyConnectionRequest request(
      peer_id,
      std::move(callback),
      connection_options,
      peer->MutLe().RegisterInitializingConnection());
  request.AttachInspect(
      inspect_pending_requests_node_,
      inspect_pending_requests_node_.UniqueName(kInspectRequestNodeNamePrefix));
  pending_requests_.emplace(peer_id, std::move(request));

  TryCreateNextConnection();
}

bool LowEnergyConnectionManager::Disconnect(PeerId peer_id,
                                            LowEnergyDisconnectReason reason) {
  auto remote_connector_iter = remote_connectors_.find(peer_id);
  if (remote_connector_iter != remote_connectors_.end()) {
    // Result callback will clean up connector.
    remote_connector_iter->second.connector->Cancel();
  }

  auto request_iter = pending_requests_.find(peer_id);
  if (request_iter != pending_requests_.end()) {
    PW_CHECK(current_request_->request.peer_id() != peer_id);
    request_iter->second.NotifyCallbacks(fit::error(HostError::kCanceled));
    pending_requests_.erase(request_iter);
  }

  if (current_request_ && current_request_->request.peer_id() == peer_id) {
    // Connector will call result callback to clean up connection.
    current_request_->connector->Cancel();
  }

  // Ignore Disconnect for peer that is not pending or connected:
  auto iter = connections_.find(peer_id);
  if (iter == connections_.end()) {
    bt_log(INFO,
           "gap-le",
           "Disconnect called for unconnected peer (peer: %s)",
           bt_str(peer_id));
    return true;
  }

  // Handle peer that is already connected:

  // Remove the connection state from the internal map right away.
  auto conn = std::move(iter->second);
  connections_.erase(iter);

  // Since this was an intentional disconnect, update the auto-connection
  // behavior appropriately.
  peer_cache_->SetAutoConnectBehaviorForIntentionalDisconnect(peer_id);

  bt_log(INFO,
         "gap-le",
         "disconnecting (peer: %s, link: %s)",
         bt_str(conn->peer_id()),
         bt_str(*conn->link()));

  if (reason == LowEnergyDisconnectReason::kApiRequest) {
    inspect_properties_.disconnect_explicit_disconnect_count_.Add(1);
  } else {
    inspect_properties_.disconnect_link_error_count_.Add(1);
  }

  CleanUpConnection(std::move(conn), reason);
  return true;
}

void LowEnergyConnectionManager::Pair(PeerId peer_id,
                                      sm::SecurityLevel pairing_level,
                                      sm::BondableMode bondable_mode,
                                      sm::ResultFunction<> cb) {
  auto iter = connections_.find(peer_id);
  if (iter == connections_.end()) {
    bt_log(WARN,
           "gap-le",
           "cannot pair: peer not connected (peer: %s)",
           bt_str(peer_id));
    cb(bt::ToResult(bt::HostError::kNotFound));
    return;
  }
  bt_log(INFO,
         "gap-le",
         "pairing with security level: %d (peer: %s)",
         static_cast<int>(pairing_level),
         bt_str(peer_id));
  iter->second->UpgradeSecurity(pairing_level, bondable_mode, std::move(cb));
}

void LowEnergyConnectionManager::SetSecurityMode(LESecurityMode mode) {
  security_mode_ = mode;
  if (mode == LESecurityMode::SecureConnectionsOnly) {
    // `Disconnect`ing the peer must not be done while iterating through
    // `connections_` as it removes the connection from `connections_`, hence
    // the helper vector.
    std::vector<PeerId> insufficiently_secure_peers;
    for (auto& [peer_id, connection] : connections_) {
      if (connection->security().level() !=
              sm::SecurityLevel::kSecureAuthenticated &&
          connection->security().level() != sm::SecurityLevel::kNoSecurity) {
        insufficiently_secure_peers.push_back(peer_id);
      }
    }
    for (PeerId id : insufficiently_secure_peers) {
      Disconnect(id);
    }
  }
  for (auto& iter : connections_) {
    iter.second->set_security_mode(mode);
  }
}

void LowEnergyConnectionManager::AttachInspect(inspect::Node& parent,
                                               std::string name) {
  inspect_node_ = parent.CreateChild(name);
  inspect_properties_.recent_connection_failures.AttachInspect(
      inspect_node_, kInspectConnectionFailuresPropertyName);
  inspect_pending_requests_node_ =
      inspect_node_.CreateChild(kInspectRequestsNodeName);
  inspect_connections_node_ =
      inspect_node_.CreateChild(kInspectConnectionsNodeName);
  for (auto& request : pending_requests_) {
    request.second.AttachInspect(inspect_pending_requests_node_,
                                 inspect_pending_requests_node_.UniqueName(
                                     kInspectRequestNodeNamePrefix));
  }
  for (auto& conn : connections_) {
    conn.second->AttachInspect(
        inspect_connections_node_,
        inspect_connections_node_.UniqueName(kInspectConnectionNodePrefix));
  }
  if (current_request_) {
    current_request_->connector->AttachInspect(
        inspect_node_, kInspectOutboundConnectorNodeName);
  }

  inspect_properties_.outgoing_connection_success_count_.AttachInspect(
      inspect_node_, kInspectOutgoingSuccessCountNodeName);
  inspect_properties_.outgoing_connection_failure_count_.AttachInspect(
      inspect_node_, kInspectOutgoingFailureCountNodeName);
  inspect_properties_.incoming_connection_success_count_.AttachInspect(
      inspect_node_, kInspectIncomingSuccessCountNodeName);
  inspect_properties_.incoming_connection_failure_count_.AttachInspect(
      inspect_node_, kInspectIncomingFailureCountNodeName);

  inspect_properties_.disconnect_explicit_disconnect_count_.AttachInspect(
      inspect_node_, kInspectDisconnectExplicitDisconnectNodeName);
  inspect_properties_.disconnect_link_error_count_.AttachInspect(
      inspect_node_, kInspectDisconnectLinkErrorNodeName);
  inspect_properties_.disconnect_zero_ref_count_.AttachInspect(
      inspect_node_, kInspectDisconnectZeroRefNodeName);
  inspect_properties_.disconnect_remote_disconnection_count_.AttachInspect(
      inspect_node_, kInspectDisconnectRemoteDisconnectionNodeName);
  last_disconnected_list_.AttachInspect(inspect_node_,
                                        kInspectLastDisconnectedListName);
}

void LowEnergyConnectionManager::RegisterRemoteInitiatedLink(
    std::unique_ptr<hci::LowEnergyConnection> link,
    sm::BondableMode bondable_mode,
    ConnectionResultCallback callback) {
  PW_CHECK(link);

  Peer* peer = UpdatePeerWithLink(*link);
  auto peer_id = peer->identifier();

  bt_log(INFO,
         "gap-le",
         "new remote-initiated link (peer: %s, local addr: %s, link: %s)",
         bt_str(peer_id),
         bt_str(link->local_address()),
         bt_str(*link));

  // TODO(fxbug.dev/42143994): Use own address when storing the connection.
  // Currently this will refuse the connection and disconnect the link if |peer|
  // is already connected to us by a different local address.
  if (connections_.find(peer_id) != connections_.end()) {
    bt_log(INFO,
           "gap-le",
           "multiple links from peer; remote-initiated connection refused "
           "(peer: %s)",
           bt_str(peer_id));
    callback(fit::error(HostError::kFailed));
    return;
  }

  if (remote_connectors_.find(peer_id) != remote_connectors_.end()) {
    bt_log(INFO,
           "gap-le",
           "remote connector for peer already exists; connection refused "
           "(peer: %s)",
           bt_str(peer_id));
    callback(fit::error(HostError::kFailed));
    return;
  }

  LowEnergyConnectionOptions connection_options{.bondable_mode = bondable_mode};
  internal::LowEnergyConnectionRequest request(
      peer_id,
      std::move(callback),
      connection_options,
      peer->MutLe().RegisterInitializingConnection());

  std::unique_ptr<internal::LowEnergyConnector> connector =
      std::make_unique<internal::LowEnergyConnector>(peer_id,
                                                     connection_options,
                                                     hci_,
                                                     peer_cache_,
                                                     weak_self_.GetWeakPtr(),
                                                     l2cap_,
                                                     gatt_,
                                                     adapter_state_,
                                                     dispatcher_,
                                                     local_address_delegate_,
                                                     wake_lease_provider_);
  auto [conn_iter, _] = remote_connectors_.emplace(
      peer_id, RequestAndConnector{std::move(request), std::move(connector)});
  // Wait until the connector is in the map to start in case the result callback
  // is called synchronously.
  auto result_cb =
      std::bind(&LowEnergyConnectionManager::OnRemoteInitiatedConnectResult,
                this,
                peer_id,
                std::placeholders::_1);
  conn_iter->second.connector->StartInbound(std::move(link),
                                            std::move(result_cb));
}

void LowEnergyConnectionManager::SetPairingDelegate(
    const PairingDelegate::WeakPtr& delegate) {
  // TODO(armansito): Add a test case for this once fxbug.dev/42169848 is done.
  pairing_delegate_ = delegate;

  // Tell existing connections to abort ongoing pairing procedures. The new
  // delegate will receive calls to PairingDelegate::CompletePairing, unless it
  // is null.
  for (auto& iter : connections_) {
    iter.second->ResetSecurityManager(delegate.is_alive()
                                          ? delegate->io_capability()
                                          : sm::IOCapability::kNoInputNoOutput);
  }
}

void LowEnergyConnectionManager::OpenL2capChannel(
    PeerId peer_id,
    l2cap::Psm psm,
    l2cap::ChannelParameters params,
    sm::SecurityLevel security_level,
    l2cap::ChannelCallback cb) {
  auto connection_iterator = connections_.find(peer_id);
  if (connection_iterator == connections_.end()) {
    bt_log(INFO,
           "gap-le",
           "can't open l2cap channel: connection not found (peer: %s)",
           bt_str(peer_id));
    cb(l2cap::Channel::WeakPtr());
    return;
  }

  auto& connection = connection_iterator->second;
  PW_DCHECK(connection);

  auto pairing_cb = [connection_weak = connection->GetWeakPtr(),
                     open_l2cap_cb = std::move(cb),
                     peer_id,
                     psm,
                     params](sm::Result<> result) mutable {
    if (!connection_weak.is_alive()) {
      bt_log(INFO,
             "gap-le",
             "can't open l2cap channel: connection destroyed before pairing "
             "completed (peer: %s)",
             bt_str(peer_id));
      open_l2cap_cb(l2cap::Channel::WeakPtr());
      return;
    }

    if (result.is_error()) {
      bt_log(
          WARN,
          "gap-le",
          "can't open l2cap channel: pairing failed with error: %s (peer: %s)",
          bt_str(result.error_value()),
          bt_str(peer_id));
      open_l2cap_cb(l2cap::Channel::WeakPtr());
      return;
    }

    connection_weak->OpenL2capChannel(psm, params, std::move(open_l2cap_cb));
  };

  connection->UpgradeSecurity(
      security_level, connection->bondable_mode(), std::move(pairing_cb));
}

void LowEnergyConnectionManager::SetDisconnectCallbackForTesting(
    DisconnectCallback callback) {
  test_disconn_cb_ = std::move(callback);
}

void LowEnergyConnectionManager::ReleaseReference(
    LowEnergyConnectionHandle* handle) {
  PW_CHECK(handle);

  auto iter = connections_.find(handle->peer_identifier());
  PW_CHECK(iter != connections_.end());

  iter->second->DropRef(handle);
  if (iter->second->ref_count() != 0u)
    return;

  // Move the connection object before erasing the entry.
  auto conn = std::move(iter->second);
  connections_.erase(iter);

  bt_log(INFO,
         "gap-le",
         "all refs dropped on connection (link: %s, peer: %s)",
         bt_str(*conn->link()),
         bt_str(conn->peer_id()));
  inspect_properties_.disconnect_zero_ref_count_.Add(1);
  CleanUpConnection(std::move(conn), LowEnergyDisconnectReason::kZeroRef);
}

void LowEnergyConnectionManager::TryCreateNextConnection() {
  if (current_request_.has_value()) {
    bt_log(DEBUG, "gap-le", "%s: request already in progress", __FUNCTION__);
    return;
  }

  if (pending_requests_.empty()) {
    bt_log(TRACE, "gap-le", "%s: no pending requests remaining", __FUNCTION__);
    return;
  }

  for (auto& iter : pending_requests_) {
    auto peer_id = iter.first;
    Peer* peer = peer_cache_->FindById(peer_id);
    if (peer) {
      auto request_pair = pending_requests_.extract(peer_id);
      internal::LowEnergyConnectionRequest request =
          std::move(request_pair.mapped());

      std::unique_ptr<internal::LowEnergyConnector> connector =
          std::make_unique<internal::LowEnergyConnector>(
              peer_id,
              request.connection_options(),
              hci_,
              peer_cache_,
              weak_self_.GetWeakPtr(),
              l2cap_,
              gatt_,
              adapter_state_,
              dispatcher_,
              local_address_delegate_,
              wake_lease_provider_);
      connector->AttachInspect(inspect_node_,
                               kInspectOutboundConnectorNodeName);

      current_request_ =
          RequestAndConnector{std::move(request), std::move(connector)};
      // Wait until the connector is in current_request_ to start in case the
      // result callback is called synchronously.
      current_request_->connector->StartOutbound(
          request_timeout_,
          hci_connector_,
          discovery_manager_,
          fit::bind_member<
              &LowEnergyConnectionManager::OnLocalInitiatedConnectResult>(
              this));
      return;
    }

    bt_log(WARN,
           "gap-le",
           "deferring connection attempt (peer: %s)",
           bt_str(peer_id));

    // TODO(fxbug.dev/42172291): For now the requests for this peer won't
    // complete until the next peer discovery. This will no longer be an issue
    // when we use background scanning.
  }
}

void LowEnergyConnectionManager::OnLocalInitiatedConnectResult(
    hci::Result<std::unique_ptr<internal::LowEnergyConnection>> result) {
  PW_CHECK(current_request_.has_value());

  internal::LowEnergyConnectionRequest request =
      std::move(current_request_->request);
  current_request_.reset();

  if (result.is_error()) {
    inspect_properties_.outgoing_connection_failure_count_.Add(1);
    bt_log(INFO,
           "gap-le",
           "failed to connect to peer (peer: %s, status: %s)",
           bt_str(request.peer_id()),
           bt_str(result));
  } else {
    inspect_properties_.outgoing_connection_success_count_.Add(1);
    bt_log(INFO,
           "gap-le",
           "connection request successful (peer: %s)",
           bt_str(request.peer_id()));
  }

  ProcessConnectResult(std::move(result), std::move(request));
  TryCreateNextConnection();
}

void LowEnergyConnectionManager::OnRemoteInitiatedConnectResult(
    PeerId peer_id,
    hci::Result<std::unique_ptr<internal::LowEnergyConnection>> result) {
  auto remote_connector_node = remote_connectors_.extract(peer_id);
  PW_CHECK(!remote_connector_node.empty());

  internal::LowEnergyConnectionRequest request =
      std::move(remote_connector_node.mapped().request);

  if (result.is_error()) {
    inspect_properties_.incoming_connection_failure_count_.Add(1);
    bt_log(INFO,
           "gap-le",
           "failed to complete remote initated connection with peer (peer: %s, "
           "status: %s)",
           bt_str(peer_id),
           bt_str(result));
  } else {
    inspect_properties_.incoming_connection_success_count_.Add(1);
    bt_log(INFO,
           "gap-le",
           "remote initiated connection successful (peer: %s)",
           bt_str(peer_id));
  }

  ProcessConnectResult(std::move(result), std::move(request));
}

void LowEnergyConnectionManager::ProcessConnectResult(
    hci::Result<std::unique_ptr<internal::LowEnergyConnection>> result,
    internal::LowEnergyConnectionRequest request) {
  PeerId peer_id = request.peer_id();
  if (result.is_error()) {
    const hci::Error err = result.error_value();
    Peer* const peer = peer_cache_->FindById(peer_id);
    // Peer may have been forgotten (causing this error).
    // A separate connection may have been established in the other direction
    // while this connection was connecting, in which case the peer state should
    // not be updated.
    if (peer && connections_.find(peer->identifier()) == connections_.end()) {
      if (request.connection_options().auto_connect &&
          err.is_protocol_error() &&
          ShouldStopAlwaysAutoConnecting(err.protocol_error())) {
        // We may see a peer's connectable advertisements, but fail to establish
        // a connection to the peer (e.g. due to asymmetrical radio TX power).
        // Unsetting the AutoConnect flag here prevents a loop of "see peer
        // device, attempt auto-connect, fail to establish connection".
        peer->MutLe().set_auto_connect_behavior(
            Peer::AutoConnectBehavior::kSkipUntilNextConnection);
      }
    }

    const HostError host_error =
        err.is_host_error() ? err.host_error() : HostError::kFailed;
    request.NotifyCallbacks(fit::error(host_error));

    inspect_properties_.recent_connection_failures.Add(1);

    return;
  }

  InitializeConnection(std::move(result).value(), std::move(request));
}

bool LowEnergyConnectionManager::InitializeConnection(
    std::unique_ptr<internal::LowEnergyConnection> connection,
    internal::LowEnergyConnectionRequest request) {
  PW_CHECK(connection);

  auto peer_id = connection->peer_id();

  // TODO(fxbug.dev/42143994): For now reject having more than one link with the
  // same peer. This should change once this has more context on the local
  // destination for remote initiated connections.
  if (connections_.find(peer_id) != connections_.end()) {
    bt_log(INFO,
           "gap-le",
           "cannot initialize multiple links to same peer; connection refused "
           "(peer: %s)",
           bt_str(peer_id));
    // Notify request that duplicate connection could not be initialized.
    request.NotifyCallbacks(fit::error(HostError::kFailed));
    // Do not update peer state, as there is another active LE connection in
    // connections_ for this peer.
    return false;
  }

  Peer* peer = peer_cache_->FindById(peer_id);
  PW_CHECK(peer);

  connection->AttachInspect(
      inspect_connections_node_,
      inspect_connections_node_.UniqueName(kInspectConnectionNodePrefix));
  connection->set_peer_disconnect_callback(
      std::bind(&LowEnergyConnectionManager::OnPeerDisconnect,
                this,
                connection->link(),
                std::placeholders::_1));
  connection->set_error_callback([this, peer_id]() {
    Disconnect(peer_id, LowEnergyDisconnectReason::kError);
  });

  auto [conn_iter, inserted] =
      connections_.try_emplace(peer_id, std::move(connection));
  PW_CHECK(inserted);

  conn_iter->second->set_peer_conn_token(peer->MutLe().RegisterConnection());

  // Create first ref to ensure that connection is cleaned up on early returns
  // or if first request callback does not retain a ref.
  auto first_ref = conn_iter->second->AddRef();

  UpdatePeerWithLink(*conn_iter->second->link());

  bt_log(TRACE,
         "gap-le",
         "notifying connection request callbacks (peer: %s)",
         bt_str(peer_id));

  request.NotifyCallbacks(fit::ok(std::bind(
      &internal::LowEnergyConnection::AddRef, conn_iter->second.get())));

  return true;
}

void LowEnergyConnectionManager::RecordDisconnectInspect(
    const internal::LowEnergyConnection& conn,
    LowEnergyDisconnectReason reason) {
  auto& inspect_item = last_disconnected_list_.CreateItem();
  inspect_item.node.RecordString(kInspectLastDisconnectedItemPeerIdPropertyName,
                                 conn.peer_id().ToString());
  inspect_item.node.RecordInt(
      kInspectLastDisconnectedItemConnectedTimePropertyName,
      conn.create_time().time_since_epoch().count());
  inspect_item.node.RecordInt(kInspectLastDisconnectedItemTimePropertyName,
                              dispatcher_.now().time_since_epoch().count());
  inspect_item.node.RecordString(kInspectLastDisconnectedItemReasonPropertyName,
                                 LowEnergyDisconnectReasonToString(reason));
}

void LowEnergyConnectionManager::CleanUpConnection(
    std::unique_ptr<internal::LowEnergyConnection> conn,
    LowEnergyDisconnectReason reason) {
  PW_CHECK(conn);

  // Mark the peer peer as no longer connected.
  Peer* peer = peer_cache_->FindById(conn->peer_id());
  PW_CHECK(peer,
           "A connection was active for an unknown peer! (id: %s)",
           bt_str(conn->peer_id()));
  RecordDisconnectInspect(*conn, reason);
  conn.reset();
}

Peer* LowEnergyConnectionManager::UpdatePeerWithLink(
    const hci::LowEnergyConnection& link) {
  Peer* peer = peer_cache_->FindByAddress(link.peer_address());
  if (!peer) {
    peer = peer_cache_->NewPeer(link.peer_address(), /*connectable=*/true);
  }
  peer->MutLe().SetConnectionParameters(link.low_energy_parameters());
  peer_cache_->SetAutoConnectBehaviorForSuccessfulConnection(
      peer->identifier());

  return peer;
}

void LowEnergyConnectionManager::OnPeerDisconnect(
    const hci::Connection* connection, pw::bluetooth::emboss::StatusCode) {
  auto handle = connection->handle();
  if (test_disconn_cb_) {
    test_disconn_cb_(handle);
  }

  // See if we can find a connection with a matching handle by walking the
  // connections list.
  auto iter = FindConnection(handle);
  if (iter == connections_.end()) {
    bt_log(WARN,
           "gap-le",
           "disconnect from unknown connection handle: %#.4x",
           handle);
    return;
  }

  // Found the connection. Remove the entry from |connections_| before notifying
  // the "closed" handlers.
  auto conn = std::move(iter->second);
  connections_.erase(iter);

  bt_log(INFO,
         "gap-le",
         "peer disconnected (peer: %s, handle: %#.4x)",
         bt_str(conn->peer_id()),
         handle);

  inspect_properties_.disconnect_remote_disconnection_count_.Add(1);

  CleanUpConnection(std::move(conn),
                    LowEnergyDisconnectReason::kPeerDisconnection);
}

LowEnergyConnectionManager::ConnectionMap::iterator
LowEnergyConnectionManager::FindConnection(hci_spec::ConnectionHandle handle) {
  auto iter = connections_.begin();
  for (; iter != connections_.end(); ++iter) {
    const auto& conn = *iter->second;
    if (conn.handle() == handle)
      break;
  }
  return iter;
}

}  // namespace bt::gap
