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

#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_connection_handle.h"

#include <pw_assert/check.h>

#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_connection.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_connection_manager.h"

namespace bt::gap {

LowEnergyConnectionHandle::LowEnergyConnectionHandle(
    PeerId peer_id,
    hci_spec::ConnectionHandle handle,
    fit::callback<void(LowEnergyConnectionHandle*)> release_cb,
    AcceptCisCallback accept_cis_cb,
    fit::function<sm::BondableMode()> bondable_cb,
    fit::function<sm::SecurityProperties()> security_cb,
    fit::function<pw::bluetooth::emboss::ConnectionRole()> role_cb,
    PeriodicAdvertisingSyncManager::TransferSyncFn&&
        transfer_periodic_advertising_sync_fn)
    : active_(true),
      peer_id_(peer_id),
      handle_(handle),
      release_cb_(std::move(release_cb)),
      accept_cis_cb_(std::move(accept_cis_cb)),
      bondable_cb_(std::move(bondable_cb)),
      security_cb_(std::move(security_cb)),
      role_cb_(std::move(role_cb)),
      transfer_periodic_advertising_sync_fn_(
          std::move(transfer_periodic_advertising_sync_fn)) {
  PW_CHECK(peer_id_.IsValid());
}

LowEnergyConnectionHandle::~LowEnergyConnectionHandle() {
  if (active_) {
    Release();
  }
}

void LowEnergyConnectionHandle::Release() {
  PW_CHECK(active_);
  active_ = false;
  if (release_cb_) {
    release_cb_(this);
  }
}

void LowEnergyConnectionHandle::MarkClosed() {
  active_ = false;
  if (closed_cb_) {
    // Move the callback out of |closed_cb_| to prevent it from deleting itself
    // by deleting |this|.
    auto f = std::move(closed_cb_);
    f();
  }
}

iso::AcceptCisStatus LowEnergyConnectionHandle::AcceptCis(
    iso::CigCisIdentifier id, iso::CisEstablishedCallback cis_established_cb) {
  PW_CHECK(active_);
  return accept_cis_cb_(id, std::move(cis_established_cb));
}

void LowEnergyConnectionHandle::TransferPeriodicAdvertisingSync(
    hci::SyncId sync_id,
    uint16_t service_data,
    pw::Callback<void(hci::Result<>)> callback) {
  PW_CHECK(active_);
  transfer_periodic_advertising_sync_fn_(
      sync_id, handle_, service_data, std::move(callback));
}

sm::BondableMode LowEnergyConnectionHandle::bondable_mode() const {
  PW_CHECK(active_);
  return bondable_cb_();
}

sm::SecurityProperties LowEnergyConnectionHandle::security() const {
  PW_CHECK(active_);
  return security_cb_();
}

pw::bluetooth::emboss::ConnectionRole LowEnergyConnectionHandle::role() const {
  PW_CHECK(active_);
  return role_cb_();
}

}  // namespace bt::gap
