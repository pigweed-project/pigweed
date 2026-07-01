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
#include "pw_bluetooth_sapphire/internal/host/hci/acl_connection.h"

namespace bt::hci {

// BrEdrConnection represents a BR/EDR logical link connection to a peer. In
// addition to general link lifetime and encryption procedures provided by
// AclConnection, BrEdrConnection manages BR/EDR-specific encryption procedures.
class BrEdrConnection : public AclConnection, public WeakSelf<BrEdrConnection> {
 public:
  BrEdrConnection(hci_spec::ConnectionHandle handle,
                  const DeviceAddress& local_address,
                  const DeviceAddress& peer_address,
                  pw::bluetooth::emboss::ConnectionRole role,
                  const Transport::WeakPtr& hci,
                  pw::async::Dispatcher& dispatcher);

  // Sends a HCI_Change_Connection_Packet_Type command to the controller to
  // enable all supported packet types for this connection.
  void EnsureAllPacketTypesEnabled();

  bool StartEncryption() override;

  // Assigns a link key with its corresponding HCI type to this BR/EDR
  // connection. This will be used for bonding procedures and determines the
  // resulting security properties of the link.
  void set_link_key(const hci_spec::LinkKey& link_key,
                    hci_spec::LinkKeyType type) {
    set_ltk(link_key);
    ltk_type_ = type;
  }

  const std::optional<hci_spec::LinkKeyType>& ltk_type() { return ltk_type_; }

 private:
  void HandleEncryptionStatus(Result<bool /*enabled*/> result,
                              bool key_refreshed) override;

  void HandleEncryptionStatusValidated(Result<bool> result);

  void ValidateEncryptionKeySize(hci::ResultFunction<> key_size_validity_cb);

  // BR/EDR-specific type of the assigned link key.
  std::optional<hci_spec::LinkKeyType> ltk_type_;
};

}  // namespace bt::hci
