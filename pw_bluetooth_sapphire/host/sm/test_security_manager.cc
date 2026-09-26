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
#include "pw_bluetooth_sapphire/internal/host/sm/test_security_manager.h"

#include <pw_assert/check.h>

#include <memory>

#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/hci/connection.h"
#include "pw_bluetooth_sapphire/internal/host/sm/smp.h"

namespace bt::sm::testing {

namespace {
Role RoleFromLinks(hci::LowEnergyConnection::WeakPtr link,
                   hci::BrEdrConnection::WeakPtr bredr_link) {
  pw::bluetooth::emboss::ConnectionRole conn_role =
      pw::bluetooth::emboss::ConnectionRole::CENTRAL;
  if (link.is_alive()) {
    conn_role = link->role();
  }
  if (bredr_link.is_alive()) {
    conn_role = bredr_link->role();
  }
  return conn_role == pw::bluetooth::emboss::ConnectionRole::CENTRAL
             ? Role::kInitiator
             : Role::kResponder;
}
}  // namespace

TestSecurityManager::TestSecurityManager(
    hci::LowEnergyConnection::WeakPtr link,
    hci::BrEdrConnection::WeakPtr bredr_link,
    l2cap::Channel::WeakPtr smp,
    IOCapability,
    Delegate::WeakPtr delegate,
    BondableMode bondable_mode,
    gap::LESecurityMode security_mode,
    gap::Peer::WeakPtr peer)
    : SecurityManager(bondable_mode, security_mode),
      role_(RoleFromLinks(link, bredr_link)),
      delegate_(std::move(delegate)),
      peer_(std::move(peer)),
      smp_(std::move(smp)),
      weak_self_(this) {
  if (smp_.is_alive()) {
    smp_->Activate([](auto) {},
                   [self = GetWeakPtr()] {
                     if (self.is_alive() && self->ctkd_callback_) {
                       auto cb = std::move(self->ctkd_callback_);
                       cb(fit::error(Error(HostError::kLinkDisconnected)));
                     }
                   });
  }
  if (link.is_alive()) {
    if (peer_->le() && peer_->le()->bond_data()) {
      current_ltk_ =
          (link->role() == pw::bluetooth::emboss::ConnectionRole::CENTRAL)
              ? peer_->le()->bond_data()->peer_ltk
              : peer_->le()->bond_data()->local_ltk;
    }
    if (current_ltk_) {
      if (role_ == Role::kInitiator) {
        set_security(current_ltk_->security());
      }
    }
  }
}

TestSecurityManager::~TestSecurityManager() {
  if (ctkd_callback_) {
    ctkd_callback_(fit::error(Error(HostError::kLinkDisconnected)));
  }
}

void TestSecurityManager::UpgradeSecurity(SecurityLevel level,
                                          PairingCallback callback) {
  last_requested_upgrade_ = level;
  set_security(SecurityProperties(
      level, kMaxEncryptionKeySize, /*secure_connections=*/true));
  callback(fit::ok(), security());
}

void TestSecurityManager::InitiateBrEdrCrossTransportKeyDerivation(
    CrossTransportKeyDerivationResultCallback callback) {
  if (delay_ctkd_) {
    ctkd_callback_ = std::move(callback);
    return;
  }
  if (!pairing_data_) {
    callback(ToResult(HostError::kFailed));
    return;
  }
  last_identity_info_ = delegate_->OnIdentityInformationRequest();
  delegate_->OnPairingComplete(fit::ok());
  peer_->MutLe().StoreBond(pairing_data_.value());
  callback(fit::ok());
}

void TestSecurityManager::TriggerPairingComplete(sm::PairingData data) {
  last_identity_info_ = delegate_->OnIdentityInformationRequest();
  delegate_->OnPairingComplete(fit::ok());
  peer_->MutLe().StoreBond(data);
}

void TestSecurityManager::Reset(IOCapability) {}
void TestSecurityManager::Abort(ErrorCode) {}

std::unique_ptr<SecurityManager> TestSecurityManagerFactory::CreateSm(
    hci::LowEnergyConnection::WeakPtr link,
    l2cap::Channel::WeakPtr smp,
    IOCapability io_capability,
    Delegate::WeakPtr delegate,
    BondableMode bondable_mode,
    gap::LESecurityMode security_mode,
    pw::async::Dispatcher&,
    gap::Peer::WeakPtr peer) {
  hci_spec::ConnectionHandle conn = link->handle();
  auto test_sm = std::unique_ptr<TestSecurityManager>(
      new TestSecurityManager(std::move(link),
                              hci::BrEdrConnection::WeakPtr(),
                              std::move(smp),
                              io_capability,
                              std::move(delegate),
                              bondable_mode,
                              security_mode,
                              std::move(peer)));
  test_sms_[conn] = test_sm->GetWeakPtr();
  return test_sm;
}

std::unique_ptr<SecurityManager> TestSecurityManagerFactory::CreateBrEdr(
    hci::BrEdrConnection::WeakPtr link,
    l2cap::Channel::WeakPtr smp,
    Delegate::WeakPtr delegate,
    bool /*is_controller_remote_public_key_validation_supported*/,
    pw::async::Dispatcher&,
    bt::gap::Peer::WeakPtr peer) {
  PW_CHECK(smp.is_alive());
  hci_spec::ConnectionHandle conn = link->handle();
  auto test_sm = std::unique_ptr<TestSecurityManager>(
      new TestSecurityManager(hci::LowEnergyConnection::WeakPtr(),
                              std::move(link),
                              std::move(smp),
                              IOCapability::kNoInputNoOutput,
                              std::move(delegate),
                              BondableMode::Bondable,
                              gap::LESecurityMode::SecureConnectionsOnly,
                              std::move(peer)));
  test_sms_[conn] = test_sm->GetWeakPtr();
  return test_sm;
}

WeakSelf<TestSecurityManager>::WeakPtr TestSecurityManagerFactory::GetTestSm(
    hci_spec::ConnectionHandle conn_handle) {
  auto iter = test_sms_.find(conn_handle);
  PW_CHECK(iter != test_sms_.end());
  return iter->second;
}

}  // namespace bt::sm::testing
