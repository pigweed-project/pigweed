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
#include <pw_assert/check.h>

#include <optional>

#include "pw_bluetooth_sapphire/internal/host/common/identifier.h"
#include "pw_bluetooth_sapphire/internal/host/common/inspect.h"
#include "pw_bluetooth_sapphire/internal/host/gap/bredr_connection_request.h"
#include "pw_bluetooth_sapphire/internal/host/gap/bredr_interrogator.h"
#include "pw_bluetooth_sapphire/internal/host/gap/gap.h"
#include "pw_bluetooth_sapphire/internal/host/gap/pairing_state_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gap/peer.h"
#include "pw_bluetooth_sapphire/internal/host/hci/bredr_connection.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/channel_manager.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/sco/sco_connection_manager.h"
#include "pw_bluetooth_sapphire/internal/host/sm/security_manager.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"

namespace bt::gap {

class PeerCache;
class SecureSimplePairingState;

// Represents an ACL connection that is currently open with the controller (i.e.
// after receiving a Connection Complete and before either user disconnection or
// Disconnection Complete).
class BrEdrConnection final {
 public:
  // |send_auth_request_cb| is called during pairing, and should send the
  // authenticaion request HCI command. |disconnect_cb| is called when an error
  // occurs and the link should be disconnected. |on_peer_disconnect_cb| is
  // called when the peer disconnects and this connection should be destroyed.
  BrEdrConnection(Peer::WeakPtr peer,
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
                  pw::async::Dispatcher& pw_dispatcher);

  ~BrEdrConnection();

  BrEdrConnection(BrEdrConnection&&) = default;

  void Interrogate(BrEdrInterrogator::ResultCallback callback);

  // Called after interrogation completes to mark this connection as available
  // for upper layers, i.e. L2CAP. Also signals any requesters with a successful
  // status and this connection. If not called and this connection is deleted
  // (e.g. by disconnection), requesters will be signaled with
  // |HostError::kNotSupported| (to indicate interrogation error).
  void OnInterrogationComplete();

  // Add a request callback that will be called when OnInterrogationComplete()
  // is called (or immediately if OnInterrogationComplete() has already been
  // called).
  void AddRequestCallback(BrEdrConnectionRequest::OnComplete cb);

  // Create a SecureSimplePairingState or LegacyPairingState object based on
  // |type|. If the object for corresponding |type| has already been created,
  // this method does nothing.
  void CreateOrUpdatePairingState(
      PairingStateType type,
      const PairingDelegate::WeakPtr& pairing_delegate,
      BrEdrSecurityMode security_mode);

  // If |OnInterrogationComplete| has been called, opens an L2CAP channel using
  // the preferred parameters |params| on the L2cap provided. Otherwise, calls
  // |cb| with a nullptr.
  void OpenL2capChannel(l2cap::Psm psm,
                        l2cap::ChannelParameters params,
                        l2cap::ChannelCallback cb);

  // See ScoConnectionManager for documentation.
  using ScoRequestHandle = sco::ScoConnectionManager::RequestHandle;
  ScoRequestHandle OpenScoConnection(
      bt::StaticPacket<
          pw::bluetooth::emboss::SynchronousConnectionParametersWriter>,
      sco::ScoConnectionManager::OpenConnectionCallback callback);
  ScoRequestHandle AcceptScoConnection(
      std::vector<bt::StaticPacket<
          pw::bluetooth::emboss::SynchronousConnectionParametersWriter>>
          parameters,
      sco::ScoConnectionManager::AcceptConnectionCallback callback);

  // Attach connection inspect node as a child of |parent| named |name|.
  void AttachInspect(inspect::Node& parent, std::string name);

  const hci::BrEdrConnection& link() const { return *link_; }
  hci::BrEdrConnection& link() { return *link_; }
  PeerId peer_id() const { return peer_id_; }
  PairingStateManager& pairing_state_manager() {
    return *pairing_state_manager_;
  }

  pw::chrono::SystemClock::time_point create_time() const {
    return create_time_;
  }

  bool interrogation_complete() const { return !request_.has_value(); }

  sm::SecurityProperties security_properties() const {
    PW_CHECK(pairing_state_manager_);
    return pairing_state_manager_->security_properties();
  }

  void set_security_mode(BrEdrSecurityMode mode) {
    PW_CHECK(pairing_state_manager_);
    pairing_state_manager_->set_security_mode(mode);
  }

  void SetSecurityManagerChannel(
      l2cap::Channel::WeakPtr security_manager_channel);

 private:
  // |conn_token| is a token received from Peer::MutBrEdr::RegisterConnection().
  void set_peer_connection_token(Peer::ConnectionToken conn_token);

  // Called by |pairing_state_manager_| when pairing completes with |status|.
  void OnPairingStateStatus(hci_spec::ConnectionHandle handle,
                            hci::Result<> status);

  PeerId peer_id_;
  Peer::WeakPtr peer_;
  std::unique_ptr<hci::BrEdrConnection> link_;
  std::optional<BrEdrConnectionRequest> request_;
  bool controller_remote_public_key_validation_supported_;
  sm::BrEdrSecurityManagerFactory security_manager_factory_;
  std::unique_ptr<PairingStateManager> pairing_state_manager_;
  l2cap::Channel::WeakPtr security_manager_channel_;
  l2cap::ChannelManager* l2cap_;
  std::unique_ptr<sco::ScoConnectionManager> sco_manager_;
  std::unique_ptr<BrEdrInterrogator> interrogator_;
  // Time this object was constructed.
  pw::chrono::SystemClock::time_point create_time_;
  // Called when an error occurs and this connection should be disconnected.
  fit::callback<void()> disconnect_cb_;

  struct InspectProperties {
    inspect::StringProperty peer_id;
    inspect::IntProperty connected_time;
  };
  InspectProperties inspect_properties_;
  inspect::Node inspect_node_;

  std::optional<Peer::InitializingConnectionToken> peer_init_token_;
  // Ensures that this peer is marked "connected" once pairing completes.
  // Unregisters the connection from PeerCache when this connection is
  // destroyed.
  Peer::ConnectionToken peer_conn_token_;

  pw::async::Dispatcher& dispatcher_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(BrEdrConnection);
};

}  // namespace bt::gap
