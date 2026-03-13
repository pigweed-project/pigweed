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

#include "pw_bluetooth_sapphire/internal/host/gap/bredr_connection.h"

#include <pw_assert/check.h>

#include <utility>

namespace bt::gap {

namespace {

const char* const kInspectPeerIdPropertyName = "peer_id";
const char* const kInspectConnectionCompletedTimePropertyName = "@time";
const char* const kInspectPairingStateNodeName = "pairing_state_manager";

}  // namespace

BrEdrConnection::BrEdrConnection(
    Peer::WeakPtr peer,
    std::unique_ptr<hci::BrEdrConnection> link,
    fit::closure send_auth_request_cb,
    fit::callback<void()> disconnect_cb,
    fit::closure on_peer_disconnect_cb,
    l2cap::ChannelManager* l2cap,
    hci::Transport::WeakPtr transport,
    std::optional<BrEdrConnectionRequest> request,
    hci::LocalAddressDelegate* low_energy_address_delegate,
    bool controller_remote_public_key_validation_supported,
    sm::BrEdrSecurityManagerFactory security_manager_factory,
    pw::async::Dispatcher& dispatcher)
    : peer_id_(peer->identifier()),
      peer_(std::move(peer)),
      link_(std::move(link)),
      request_(std::move(request)),
      controller_remote_public_key_validation_supported_(
          controller_remote_public_key_validation_supported),
      security_manager_factory_(std::move(security_manager_factory)),
      l2cap_(l2cap),
      sco_manager_(
          std::make_unique<sco::ScoConnectionManager>(peer_id_,
                                                      link_->handle(),
                                                      link_->peer_address(),
                                                      link_->local_address(),
                                                      transport)),
      interrogator_(new BrEdrInterrogator(
          peer_, link_->handle(), transport->command_channel()->AsWeakPtr())),
      create_time_(dispatcher.now()),
      disconnect_cb_(std::move(disconnect_cb)),
      peer_init_token_(request_->take_peer_init_token()),
      peer_conn_token_(peer_->MutBrEdr().RegisterConnection()),
      dispatcher_(dispatcher) {
  link_->set_peer_disconnect_callback(
      [peer_disconnect_cb = std::move(on_peer_disconnect_cb)](
          const auto&, auto) { peer_disconnect_cb(); });

  std::unique_ptr<LegacyPairingState> legacy_pairing_state = nullptr;
  if (request_ && request_->legacy_pairing_state()) {
    // This means that we were responding to Legacy Pairing before the
    // ACL connection between the two devices was complete.
    legacy_pairing_state = request_->take_legacy_pairing_state();

    // TODO(fxbug.dev/356165942): Check that legacy pairing is done
  }

  pairing_state_manager_ = std::make_unique<PairingStateManager>(
      peer_,
      link_->GetWeakPtr(),
      std::move(legacy_pairing_state),
      request_ && request_->AwaitingOutgoing(),
      std::move(send_auth_request_cb),
      fit::bind_member<&BrEdrConnection::OnPairingStateStatus>(this),
      low_energy_address_delegate,
      controller_remote_public_key_validation_supported_,
      security_manager_factory_,
      dispatcher_);
}

BrEdrConnection::~BrEdrConnection() {
  if (auto request = std::exchange(request_, std::nullopt);
      request.has_value()) {
    // Connection never completed so signal the requester(s).
    request->NotifyCallbacks(ToResult(HostError::kNotSupported),
                             [] { return nullptr; });
  }

  sco_manager_.reset();
  pairing_state_manager_.reset();
  link_.reset();
}

void BrEdrConnection::Interrogate(BrEdrInterrogator::ResultCallback callback) {
  interrogator_->Start(std::move(callback));
}

void BrEdrConnection::OnInterrogationComplete() {
  PW_CHECK(!interrogation_complete(),
           "%s on a connection that's already been interrogated",
           __FUNCTION__);

  // Fulfill and clear request so that the dtor does not signal requester(s)
  // with errors.
  if (auto request = std::exchange(request_, std::nullopt);
      request.has_value()) {
    request->NotifyCallbacks(fit::ok(), [this] { return this; });
  }
}

void BrEdrConnection::AddRequestCallback(
    BrEdrConnectionRequest::OnComplete cb) {
  if (!request_.has_value()) {
    cb(fit::ok(), this);
    return;
  }

  PW_CHECK(request_);
  request_->AddCallback(std::move(cb));
}

void BrEdrConnection::CreateOrUpdatePairingState(
    PairingStateType type,
    const PairingDelegate::WeakPtr& pairing_delegate,
    BrEdrSecurityMode security_mode) {
  PW_CHECK(pairing_state_manager_);
  pairing_state_manager_->CreateOrUpdatePairingState(type, pairing_delegate);
  set_security_mode(security_mode);
}

void BrEdrConnection::OpenL2capChannel(l2cap::Psm psm,
                                       l2cap::ChannelParameters params,
                                       l2cap::ChannelCallback cb) {
  if (!interrogation_complete()) {
    // Connection is not yet ready for L2CAP; return a null channel.
    bt_log(INFO,
           "gap-bredr",
           "connection not ready; canceling connect to PSM %.4x (peer: %s)",
           psm,
           bt_str(peer_id_));
    cb(l2cap::Channel::WeakPtr());
    return;
  }

  bt_log(DEBUG,
         "gap-bredr",
         "opening l2cap channel on psm %#.4x (peer: %s)",
         psm,
         bt_str(peer_id_));
  l2cap_->OpenL2capChannel(link().handle(), psm, params, std::move(cb));
}

BrEdrConnection::ScoRequestHandle BrEdrConnection::OpenScoConnection(
    bt::StaticPacket<
        pw::bluetooth::emboss::SynchronousConnectionParametersWriter>
        parameters,
    sco::ScoConnectionManager::OpenConnectionCallback callback) {
  return sco_manager_->OpenConnection(std::move(parameters),
                                      std::move(callback));
}

BrEdrConnection::ScoRequestHandle BrEdrConnection::AcceptScoConnection(
    std::vector<bt::StaticPacket<
        pw::bluetooth::emboss::SynchronousConnectionParametersWriter>>
        parameters,
    sco::ScoConnectionManager::AcceptConnectionCallback callback) {
  return sco_manager_->AcceptConnection(std::move(parameters),
                                        std::move(callback));
}

void BrEdrConnection::AttachInspect(inspect::Node& parent, std::string name) {
  inspect_node_ = parent.CreateChild(name);
  inspect_properties_.peer_id = inspect_node_.CreateString(
      kInspectPeerIdPropertyName, peer_id_.ToString());

  int64_t time_ns = create_time_.time_since_epoch().count();
  inspect_properties_.connected_time = inspect_node_.CreateInt(
      kInspectConnectionCompletedTimePropertyName, time_ns);

  pairing_state_manager_->AttachInspect(inspect_node_,
                                        kInspectPairingStateNodeName);
}

void BrEdrConnection::OnPairingStateStatus(hci_spec::ConnectionHandle,
                                           hci::Result<> status) {
  if (bt_is_error(
          status,
          DEBUG,
          "gap-bredr",
          "SecureSimplePairingState error status, disconnecting (peer id: %s)",
          bt_str(peer_id_))) {
    if (disconnect_cb_) {
      disconnect_cb_();
    }
    return;
  }

  // Once pairing succeeds for the first time, the transition from Initializing
  // -> Connected can happen.
  peer_init_token_.reset();
}

void BrEdrConnection::SetSecurityManagerChannel(
    l2cap::Channel::WeakPtr security_manager_channel) {
  if (!pairing_state_manager_->secure_simple_pairing_state()) {
    bt_log(DEBUG,
           "gap-bredr",
           "dropping SMP channel because SecureSimplePairingState not in use");
    return;
  }
  pairing_state_manager_->secure_simple_pairing_state()
      ->SetSecurityManagerChannel(std::move(security_manager_channel));
}

}  // namespace bt::gap
