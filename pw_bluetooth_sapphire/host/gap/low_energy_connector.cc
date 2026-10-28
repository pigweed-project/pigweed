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

#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_connector.h"

#include <pw_assert/check.h>

#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_connection_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gap/peer_cache.h"

namespace bt::gap::internal {

namespace {

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

constexpr int kMaxConnectionAttempts = 3;
constexpr int kRetryExponentialBackoffBase = 2;

constexpr const char* kInspectPeerIdPropertyName = "peer_id";
constexpr const char* kInspectConnectionAttemptPropertyName =
    "connection_attempt";
constexpr const char* kInspectStatePropertyName = "state";
constexpr const char* kInspectIsOutboundPropertyName = "is_outbound";

}  // namespace

LowEnergyConnector::LowEnergyConnector(
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
    pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider)
    : dispatcher_(dispatcher),
      peer_id_(peer_id),
      peer_cache_(peer_cache),
      l2cap_(l2cap),
      gatt_(std::move(gatt)),
      adapter_state_(adapter_state),
      options_(options),
      hci_(std::move(hci)),
      le_connection_manager_(std::move(conn_mgr)),
      local_address_delegate_(local_address_delegate),
      wake_lease_provider_(wake_lease_provider) {
  PW_CHECK(peer_cache_);
  PW_CHECK(l2cap_);
  PW_CHECK(gatt_.is_alive());
  PW_CHECK(hci_.is_alive());
  PW_CHECK(le_connection_manager_.is_alive());

  cmd_ = hci_->command_channel()->AsWeakPtr();
  PW_CHECK(cmd_.is_alive());

  auto peer = peer_cache_->FindById(peer_id_);
  PW_CHECK(peer);
  peer_address_ = peer->address();

  request_create_connection_task_.set_function(
      [this](pw::async::Context /*ctx*/, pw::Status status) {
        if (status.ok()) {
          RequestCreateConnection();
        }
      });
}

LowEnergyConnector::~LowEnergyConnector() {
  if (*state_ != State::kComplete && *state_ != State::kFailed) {
    bt_log(
        WARN,
        "gap-le",
        "destroying LowEnergyConnector before procedure completed (peer: %s)",
        bt_str(peer_id_));
    NotifyFailure(ToResult(HostError::kCanceled));
  }

  if (hci_connector_ && hci_connector_->request_pending()) {
    // NOTE: LowEnergyConnector will be unable to wait for the connection to be
    // canceled. The hci::LowEnergyConnector may still be waiting to cancel the
    // connection when a later gap::internal::LowEnergyConnector is created.
    hci_connector_->Cancel();
  }
}

void LowEnergyConnector::StartOutbound(
    pw::chrono::SystemClock::duration request_timeout,
    hci::LowEnergyConnector* connector,
    LowEnergyDiscoveryManager::WeakPtr discovery_manager,
    ResultCallback cb) {
  PW_CHECK(*state_ == State::kDefault);
  PW_CHECK(discovery_manager.is_alive());
  PW_CHECK(connector);
  PW_CHECK(request_timeout.count() != 0);
  hci_connector_ = connector;
  discovery_manager_ = std::move(discovery_manager);
  hci_request_timeout_ = request_timeout;
  result_cb_ = std::move(cb);
  set_is_outbound(true);

  EnsureLocalAddress();
}

void LowEnergyConnector::StartInbound(
    std::unique_ptr<hci::LowEnergyConnection> connection, ResultCallback cb) {
  PW_CHECK(*state_ == State::kDefault);
  PW_CHECK(connection);
  // Connection address should resolve to same peer as the given peer ID.
  Peer* conn_peer = peer_cache_->FindByAddress(connection->peer_address());
  PW_CHECK(conn_peer);
  PW_CHECK(peer_id_ == conn_peer->identifier(),
           "peer_id_ (%s) != connection peer (%s)",
           bt_str(peer_id_),
           bt_str(conn_peer->identifier()));
  result_cb_ = std::move(cb);
  set_is_outbound(false);

  if (!InitializeConnection(std::move(connection))) {
    return;
  }

  StartInterrogation();
}

void LowEnergyConnector::Cancel() {
  bt_log(INFO,
         "gap-le",
         "canceling connector (peer: %s, state: %s)",
         bt_str(peer_id_),
         StateToString(*state_));

  switch (*state_) {
    case State::kDefault:
      // There is nothing to do if cancel is called before the procedure has
      // started. There is no result callback to call yet.
      break;
    case State::kEnsuringLocalAddress:
      NotifyFailure(ToResult(HostError::kCanceled));
      break;
    case State::kConnecting:
      // The connector will call the result callback with a cancelled result.
      hci_connector_->Cancel();
      break;
    case State::kInterrogating:
      // The interrogator will call the result callback with a cancelled result.
      interrogator_->Cancel();
      break;
    case State::kPauseBeforeConnectionRetry:
      request_create_connection_task_.Cancel();
      NotifyFailure(ToResult(HostError::kCanceled));
      break;
    case State::kAwaitingConnectionFailedToBeEstablishedDisconnect:
      // Waiting for disconnect complete, nothing to do.
    case State::kComplete:
    case State::kFailed:
      // Cancelling completed/failed connector is a no-op.
      break;
  }
}

void LowEnergyConnector::AttachInspect(inspect::Node& parent,
                                       std::string name) {
  inspect_node_ = parent.CreateChild(name);
  inspect_properties_.peer_id = inspect_node_.CreateString(
      kInspectPeerIdPropertyName, peer_id_.ToString());
  connection_attempt_.AttachInspect(inspect_node_,
                                    kInspectConnectionAttemptPropertyName);
  state_.AttachInspect(inspect_node_, kInspectStatePropertyName);
  if (is_outbound_.has_value()) {
    inspect_properties_.is_outbound =
        inspect_node_.CreateBool(kInspectIsOutboundPropertyName, *is_outbound_);
  }
}

const char* LowEnergyConnector::StateToString(State state) {
  switch (state) {
    case State::kDefault:
      return "Default";
    case State::kEnsuringLocalAddress:
      return "EnsuringLocalAddress";
    case State::kConnecting:
      return "Connecting";
    case State::kInterrogating:
      return "Interrogating";
    case State::kAwaitingConnectionFailedToBeEstablishedDisconnect:
      return "AwaitingConnectionFailedToBeEstablishedDisconnect";
    case State::kPauseBeforeConnectionRetry:
      return "PauseBeforeConnectionRetry";
    case State::kComplete:
      return "Complete";
    case State::kFailed:
      return "Failed";
  }
}

void LowEnergyConnector::EnsureLocalAddress() {
  PW_CHECK(*state_ == State::kDefault);
  state_.Set(State::kEnsuringLocalAddress);
  local_address_delegate_->EnsureLocalAddress(
      /*address_type=*/std::nullopt, [self = weak_self_.GetWeakPtr()](auto) {
        if (!self.is_alive() || *self->state_ == State::kFailed) {
          return;
        }
        self->RequestCreateConnection();
      });
}

void LowEnergyConnector::RequestCreateConnection() {
  // When the peer disconnects during/after interrogation, a retry may be
  // initiated by calling this method.
  PW_CHECK(*state_ == State::kDefault ||
           *state_ == State::kEnsuringLocalAddress ||
           *state_ == State::kPauseBeforeConnectionRetry);

  // Pause discovery until connection complete.
  std::optional<LowEnergyDiscoveryManager::PauseToken> pause_token;
  if (discovery_manager_.is_alive()) {
    pause_token = discovery_manager_->PauseDiscovery();
  }

  auto self = weak_self_.GetWeakPtr();
  auto status_cb = [self, pause = std::move(pause_token)](hci::Result<> status,
                                                          auto link) {
    if (self.is_alive()) {
      self->OnConnectResult(status, std::move(link));
    }
  };

  state_.Set(State::kConnecting);

  // TODO(fxbug.dev/42149416): Use slow interval & window for auto connections
  // during background scan.
  PW_CHECK(hci_connector_->CreateConnection(
      /*use_accept_list=*/false,
      peer_address_,
      kLEScanFastInterval,
      kLEScanFastWindow,
      kInitialConnectionParameters,
      std::move(status_cb),
      hci_request_timeout_));
}

void LowEnergyConnector::OnConnectResult(
    hci::Result<> status, std::unique_ptr<hci::LowEnergyConnection> link) {
  if (status.is_error()) {
    bt_log(INFO,
           "gap-le",
           "failed to connect to peer (id: %s, status: %s)",
           bt_str(peer_id_),
           bt_str(status));

    NotifyFailure(status);
    return;
  }
  PW_CHECK(link);

  bt_log(INFO,
         "gap-le",
         "connection request successful (peer: %s, %s)",
         bt_str(peer_id_),
         bt_str(link->low_energy_parameters()));

  if (InitializeConnection(std::move(link))) {
    StartInterrogation();
  }
}

bool LowEnergyConnector::InitializeConnection(
    std::unique_ptr<hci::LowEnergyConnection> link) {
  PW_CHECK(link);

  // Only one LowEnergyConnection may exist per peer, as per-peer state (e.g.
  // the GATT registration) is keyed by PeerId. Refuse this link if a link to
  // the peer was already established in the other direction, as destroying a
  // duplicate connection would tear down the state of the existing one.
  if (le_connection_manager_->HasConnectionToPeer(peer_id_)) {
    bt_log(INFO,
           "gap-le",
           "link to peer already exists; refusing duplicate link (peer: %s, "
           "handle: %#.4x)",
           bt_str(peer_id_),
           link->handle());
    NotifyFailure();
    return false;
  }

  auto peer_disconnect_cb =
      fit::bind_member<&LowEnergyConnector::OnPeerDisconnect>(this);
  auto error_cb = [this]() { NotifyFailure(); };

  Peer* peer = peer_cache_->FindById(peer_id_);
  PW_CHECK(peer);
  auto connection =
      LowEnergyConnection::Create(peer->GetWeakPtr(),
                                  std::move(link),
                                  options_,
                                  peer_disconnect_cb,
                                  error_cb,
                                  le_connection_manager_,
                                  l2cap_,
                                  gatt_,
                                  hci_,
                                  wake_lease_provider_,
                                  dispatcher_,
                                  adapter_state_.low_energy_state);
  if (!connection) {
    bt_log(WARN,
           "gap-le",
           "connection initialization failed (peer: %s)",
           bt_str(peer_id_));
    NotifyFailure();
    return false;
  }

  connection_ = std::move(connection);
  return true;
}

void LowEnergyConnector::StartInterrogation() {
  PW_CHECK((*is_outbound_ && *state_ == State::kConnecting) ||
           (!*is_outbound_ && *state_ == State::kDefault));
  PW_CHECK(connection_);

  state_.Set(State::kInterrogating);
  auto peer = peer_cache_->FindById(peer_id_);
  PW_CHECK(peer);
  bool sca_supported =
      adapter_state_.SupportedCommands().le_request_peer_sca().Read();
  interrogator_.emplace(
      peer->GetWeakPtr(), connection_->handle(), cmd_, sca_supported);
  interrogator_->Start(
      fit::bind_member<&LowEnergyConnector::OnInterrogationComplete>(this));
}

void LowEnergyConnector::OnInterrogationComplete(hci::Result<> status) {
  // If a disconnect event is received before interrogation completes, state_
  // will be either kFailed or kPauseBeforeConnectionRetry depending on the
  // status of the disconnect.
  PW_CHECK(*state_ == State::kInterrogating || *state_ == State::kFailed ||
           *state_ == State::kPauseBeforeConnectionRetry);
  if (*state_ == State::kFailed ||
      *state_ == State::kPauseBeforeConnectionRetry) {
    return;
  }

  PW_CHECK(connection_);

  // If the controller responds to an interrogation command with the 0x3e
  // "kConnectionFailedToBeEstablished" error, it will send a Disconnection
  // Complete event soon after. Wait for this event before initiating a retry.
  if (status == ToResult(pw::bluetooth::emboss::StatusCode::
                             CONNECTION_FAILED_TO_BE_ESTABLISHED)) {
    bt_log(INFO,
           "gap-le",
           "Received kConnectionFailedToBeEstablished during interrogation. "
           "Waiting for Disconnect "
           "Complete. (peer: %s)",
           bt_str(peer_id_));
    state_.Set(State::kAwaitingConnectionFailedToBeEstablishedDisconnect);
    return;
  }

  if (status.is_error()) {
    bt_log(INFO,
           "gap-le",
           "interrogation failed with %s (peer: %s)",
           bt_str(status),
           bt_str(peer_id_));
    NotifyFailure();
    return;
  }

  connection_->OnInterrogationComplete();
  NotifySuccess();
}

void LowEnergyConnector::OnPeerDisconnect(
    pw::bluetooth::emboss::StatusCode status_code) {
  // The peer can't disconnect while connecting, and we unregister from
  // disconnects after kFailed & kComplete.
  PW_CHECK(
      *state_ == State::kInterrogating ||
          *state_ == State::kAwaitingConnectionFailedToBeEstablishedDisconnect,
      "Received peer disconnect during invalid state (state: %s, status: %s)",
      StateToString(*state_),
      bt_str(ToResult(status_code)));
  if (*state_ == State::kInterrogating &&
      status_code != pw::bluetooth::emboss::StatusCode::
                         CONNECTION_FAILED_TO_BE_ESTABLISHED) {
    NotifyFailure(ToResult(status_code));
    return;
  }

  // state_ is kAwaitingConnectionFailedToBeEstablished or kInterrogating with a
  // 0x3e error, so retry connection
  if (!MaybeRetryConnection()) {
    NotifyFailure(ToResult(status_code));
  }
}

bool LowEnergyConnector::MaybeRetryConnection() {
  // Only retry outbound connections.
  if (*is_outbound_ && *connection_attempt_ < kMaxConnectionAttempts - 1) {
    connection_.reset();
    state_.Set(State::kPauseBeforeConnectionRetry);

    // Exponential backoff (2s, 4s, 8s, ...)
    std::chrono::seconds retry_delay(kRetryExponentialBackoffBase
                                     << *connection_attempt_);

    connection_attempt_.Set(*connection_attempt_ + 1);
    bt_log(INFO,
           "gap-le",
           "Retrying connection in %llds (peer: %s, attempt: %d)",
           retry_delay.count(),
           bt_str(peer_id_),
           *connection_attempt_);
    request_create_connection_task_.PostAfter(retry_delay);
    return true;
  }
  return false;
}

void LowEnergyConnector::NotifySuccess() {
  PW_CHECK(*state_ == State::kInterrogating);
  PW_CHECK(connection_);
  PW_CHECK(result_cb_);

  state_.Set(State::kComplete);

  // LowEnergyConnectionManager should immediately set handlers to replace these
  // ones.
  connection_->set_peer_disconnect_callback([peer_id = peer_id_](auto) {
    PW_CRASH("Peer disconnected without handler set (peer: %s)",
             bt_str(peer_id));
  });

  connection_->set_error_callback([peer_id = peer_id_]() {
    PW_CRASH("connection error without handler set (peer: %s)",
             bt_str(peer_id));
  });

  result_cb_(fit::ok(std::move(connection_)));
}

void LowEnergyConnector::NotifyFailure(hci::Result<> status) {
  state_.Set(State::kFailed);
  // The result callback must only be called once, so extraneous failures should
  // be ignored.
  if (result_cb_) {
    result_cb_(fit::error(status.take_error()));
  }
}

void LowEnergyConnector::set_is_outbound(bool is_outbound) {
  is_outbound_ = is_outbound;
  inspect_properties_.is_outbound =
      inspect_node_.CreateBool(kInspectIsOutboundPropertyName, is_outbound);
}

}  // namespace bt::gap::internal
