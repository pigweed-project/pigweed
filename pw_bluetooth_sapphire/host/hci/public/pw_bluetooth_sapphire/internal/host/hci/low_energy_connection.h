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
#include "pw_bluetooth_sapphire/internal/host/hci-spec/le_connection_parameters.h"
#include "pw_bluetooth_sapphire/internal/host/hci/acl_connection.h"

namespace bt::hci {

class LowEnergyConnection : public AclConnection,
                            public WeakSelf<LowEnergyConnection> {
 public:
  LowEnergyConnection(hci_spec::ConnectionHandle handle,
                      const DeviceAddress& local_address,
                      const DeviceAddress& peer_address,
                      const hci_spec::LEConnectionParameters& params,
                      pw::bluetooth::emboss::ConnectionRole role,
                      const Transport::WeakPtr& hci,
                      pw::async::Dispatcher& dispatcher);

  ~LowEnergyConnection() override;

  // Authenticate (i.e. encrypt) this connection using its current link key.
  // Returns false if the procedure cannot be initiated. The result of the
  // authentication procedure will be reported via the encryption change
  // callback.  If the link layer procedure fails, the connection will be
  // disconnected. The encryption change callback will be notified of the
  // failure.
  bool StartEncryption() override;

  // Sets the active LE parameters of this connection.
  void set_low_energy_parameters(
      const hci_spec::LEConnectionParameters& params) {
    parameters_ = params;
  }

  // The active LE connection parameters of this connection.
  const hci_spec::LEConnectionParameters& low_energy_parameters() const {
    return parameters_;
  }

  // Returns a string representation of this connection.
  std::string ToString() const override;

  using AclConnection::reset_ltk;
  using AclConnection::set_ltk;

 private:
  void HandleEncryptionStatus(Result<bool /*enabled*/> result,
                              bool key_refreshed) override;

  // HCI event handlers.
  CommandChannel::EventCallbackResult OnLELongTermKeyRequestEvent(
      const EventPacket& event);

  // IDs for encryption related HCI event handlers.
  CommandChannel::EventHandlerId le_ltk_request_id_;

  hci_spec::LEConnectionParameters parameters_;
};

}  // namespace bt::hci
