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
#include "pw_bluetooth_sapphire/internal/host/common/identifier.h"
#include "pw_bluetooth_sapphire/internal/host/gap/periodic_advertising_sync_manager.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/iso/iso_common.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"

namespace bt::gap {

namespace internal {
class LowEnergyConnection;
}

class LowEnergyConnectionManager;

class LowEnergyConnectionHandle final {
 public:
  using AcceptCisCallback = fit::function<iso::AcceptCisStatus(
      iso::CigCisIdentifier, iso::CisEstablishedCallback)>;

  // |release_cb| will be called when this handle releases its reference to the
  // connection. |accept_cis_cb| will be called to allow an incoming Isochronous
  // stream to be established with the specified CIG/CIS identifier pair.
  // |bondable_cb| returns the current bondable mode of the connection. It will
  // only be called while the connection is active. |security_mode| returns the
  // current security properties of the connection. It will only be called while
  // the connection is active.
  LowEnergyConnectionHandle(
      PeerId peer_id,
      hci_spec::ConnectionHandle handle,
      fit::callback<void(LowEnergyConnectionHandle*)> release_cb,
      AcceptCisCallback accept_cis_cb,
      fit::function<sm::BondableMode()> bondable_cb,
      fit::function<sm::SecurityProperties()> security_cb,
      fit::function<pw::bluetooth::emboss::ConnectionRole()> role_cb,
      PeriodicAdvertisingSyncManager::TransferSyncFn&&
          transfer_periodic_advertising_sync_fn);

  // Destroying this object releases its reference to the underlying connection.
  ~LowEnergyConnectionHandle();

  // Releases this object's reference to the underlying connection.
  void Release();

  // Returns true if the underlying connection is still active.
  bool active() const { return active_; }

  // Sets a callback to be called when the underlying connection is closed.
  void set_closed_callback(fit::closure callback) {
    closed_cb_ = std::move(callback);
  }

  // Allow an incoming Isochronous stream for the specified CIG/CIS identifier
  // pair. Upon receiving the request, invoke |cis_established_cb| with the
  // status and if successful, connection parameters.
  [[nodiscard]] iso::AcceptCisStatus AcceptCis(
      iso::CigCisIdentifier id, iso::CisEstablishedCallback cis_established_cb);

  // Transfers periodic advertising synchronization parameters over this
  // connection.
  //
  // @param id The ID of the synchronization to transfer.
  // @param service_data Application-specific data to transfer with the sync.
  // @param callback The callback to be invoked with the result of the
  // transfer.
  void TransferPeriodicAdvertisingSync(
      hci::SyncId sync_id,
      uint16_t service_data,
      pw::Callback<void(hci::Result<>)> callback);

  // Returns the operational bondable mode of the underlying connection. See
  // spec V5.1 Vol 3 Part C Section 9.4 for more details.
  sm::BondableMode bondable_mode() const;

  sm::SecurityProperties security() const;

  pw::bluetooth::emboss::ConnectionRole role() const;

  PeerId peer_identifier() const { return peer_id_; }
  hci_spec::ConnectionHandle handle() const { return handle_; }

  // Called by LowEnergyConnectionManager when the underlying connection is
  // closed. Notifies |closed_cb_|. Clients should NOT call this method.
  void MarkClosed();

 private:
  bool active_;
  PeerId peer_id_;
  hci_spec::ConnectionHandle handle_;
  fit::closure closed_cb_;
  fit::callback<void(LowEnergyConnectionHandle*)> release_cb_;
  AcceptCisCallback accept_cis_cb_;
  fit::function<sm::BondableMode()> bondable_cb_;
  fit::function<sm::SecurityProperties()> security_cb_;
  fit::function<pw::bluetooth::emboss::ConnectionRole()> role_cb_;
  PeriodicAdvertisingSyncManager::TransferSyncFn
      transfer_periodic_advertising_sync_fn_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LowEnergyConnectionHandle);
};

}  // namespace bt::gap
