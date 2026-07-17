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
#include "pw_async/dispatcher.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/link_key.h"
#include "pw_bluetooth_sapphire/internal/host/hci/connection.h"

namespace bt::hci {

// Represents an ACL-U or LE-U link, both of which use the ACL data channel and
// support encryption procedures.
// Concrete implementations are found in BrEdrConnection and
// LowEnergyConnection.
class AclConnection : public Connection {
 public:
  ~AclConnection() override;

  // Authenticate (i.e. encrypt) this connection using its current link key.
  // Returns false if the procedure cannot be initiated. The result of the
  // authentication procedure will be reported via the encryption change
  // callback.
  //
  // If the link layer procedure fails, the connection will be disconnected. The
  // encryption change callback will be notified of the failure.
  virtual bool StartEncryption() = 0;

  // Assigns a callback that will run when the encryption state of the
  // underlying link changes. The bool value parameter represents the new state.
  void set_encryption_change_callback(ResultFunction<bool> callback) {
    encryption_change_callback_ = std::move(callback);
  }

  // Returns the role of the local device in the established connection.
  pw::bluetooth::emboss::ConnectionRole role() const { return role_; }

  // Update the role of the local device when a role change occurs.
  void set_role(pw::bluetooth::emboss::ConnectionRole role) { role_ = role; }

  // The current long term key of the connection.
  const std::optional<hci_spec::LinkKey>& ltk() const { return ltk_; }

  void set_use_secure_connections(bool use_secure_connections) {
    use_secure_connections_ = use_secure_connections;
  }

  pw::bluetooth::emboss::EncryptionStatus encryption_status() const {
    return encryption_status_;
  }
  void set_encryption_status(pw::bluetooth::emboss::EncryptionStatus status) {
    encryption_status_ = status;
  }

  // The local device address used while establishing the connection.
  const DeviceAddress& local_address() const { return local_address_; }

  // The peer address used while establishing the connection.
  const DeviceAddress& peer_address() const { return peer_address_; }

 protected:
  AclConnection(hci_spec::ConnectionHandle handle,
                const DeviceAddress& local_address,
                const DeviceAddress& peer_address,
                pw::bluetooth::emboss::ConnectionRole role,
                const Transport::WeakPtr& hci,
                pw::async::Dispatcher& dispatcher);

  void set_ltk(const hci_spec::LinkKey& link_key) { ltk_ = link_key; }
  void reset_ltk() { ltk_.reset(); }

  // Notifies subclasses of a change in encryption status.
  virtual void HandleEncryptionStatus(Result<bool /*enabled*/> result,
                                      bool key_refreshed) = 0;

  ResultFunction<bool>& encryption_change_callback() {
    return encryption_change_callback_;
  }

 private:
  // This method must be static since it may be invoked after the connection
  // associated with it is destroyed.
  static void OnDisconnectionComplete(hci_spec::ConnectionHandle handle,
                                      const Transport::WeakPtr& hci);

  // HCI event handlers.
  CommandChannel::EventCallbackResult OnEncryptionChangeEvent(
      const EventPacket& event);
  CommandChannel::EventCallbackResult OnEncryptionKeyRefreshCompleteEvent(
      const EventPacket& event);

  // IDs for encryption related HCI event handlers.
  CommandChannel::EventHandlerId enc_change_id_;
  CommandChannel::EventHandlerId enc_key_refresh_cmpl_id_;

  // This connection's current link key.
  std::optional<hci_spec::LinkKey> ltk_;

  // Flag indicating if peer and local Secure Connections support are both
  // present. Set in OnLinkKeyNotification in SecureSimplePairingState
  bool use_secure_connections_ = false;

  pw::bluetooth::emboss::EncryptionStatus encryption_status_ =
      pw::bluetooth::emboss::EncryptionStatus::OFF;

  pw::bluetooth::emboss::ConnectionRole role_;

  ResultFunction<bool> encryption_change_callback_;

  // Addresses used while creating the link.
  DeviceAddress local_address_;
  DeviceAddress peer_address_;

  WeakSelf<AclConnection> weak_self_;
};

}  // namespace bt::hci
