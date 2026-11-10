// Copyright 2024 The Pigweed Authors
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

#include "pw_bluetooth_sapphire/internal/host/gap/pairing_state_manager.h"

#include <pw_assert/check.h>

#include <memory>
#include <utility>

#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/gap/legacy_pairing_state.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"

namespace bt::gap {

namespace {

const char* const kInspectPairingStateTypePropertyName = "pairing_state_type";
const char* const kInspectSecureSimplePairingStateNodeName =
    "secure_simple_pairing_state";
const char* const kInspectLegacyPairingStateNodeName = "legacy_pairing_state";

}  // namespace

PairingStateManager::PairingStateManager(
    Peer::WeakPtr peer,
    WeakPtr<hci::BrEdrConnection> link,
    std::unique_ptr<LegacyPairingState> legacy_pairing_state,
    bool outgoing_connection,
    fit::closure auth_cb,
    StatusCallback status_cb,
    hci::LocalAddressDelegate* low_energy_address_delegate,
    bool controller_remote_public_key_validation_supported,
    sm::BrEdrSecurityManagerFactory security_manager_factory,
    pw::async::Dispatcher& dispatcher)
    : peer_(std::move(peer)),
      link_(std::move(link)),
      outgoing_connection_(outgoing_connection),
      auth_cb_(std::move(auth_cb)),
      status_cb_(std::move(status_cb)),
      dispatcher_(&dispatcher),
      low_energy_address_delegate_(low_energy_address_delegate),
      controller_remote_public_key_validation_supported_(
          controller_remote_public_key_validation_supported),
      security_manager_factory_(std::move(security_manager_factory)) {
  // If |legacy_pairing_state| is non-null, this means we were responding to
  // Legacy Pairing before the ACL connection between the two devices was
  // complete
  if (legacy_pairing_state) {
    pairing_state_type_ = PairingStateType::kLegacyPairing;

    // Use |legacy_pairing_state| because it already contains information and
    // state we want to keep
    legacy_pairing_state_ = std::move(legacy_pairing_state);

    // Since PairingStateManager is created when the ACL connection is complete,
    // we need to initialize |legacy_pairing_state_| with information that we
    // didn't have until after the connection was complete (e.g. link, auth_cb,
    // status_cb) and set the connection's link key.
    legacy_pairing_state_->BuildEstablishedLink(
        link_, auth_cb_.share(), status_cb_.share());

    // We should also check that |peer| and |outgoing_connection| are unchanged
    // before and after connection is complete
    PW_CHECK(legacy_pairing_state_->peer()->identifier() ==
             peer_->identifier());
    PW_CHECK(legacy_pairing_state_->outgoing_connection() ==
             outgoing_connection);
  }
}

void PairingStateManager::InitiatePairing(
    BrEdrSecurityRequirements security_requirements, StatusCallback status_cb) {
  if (pairing_state_type_ == PairingStateType::kSecureSimplePairing) {
    secure_simple_pairing_state_->InitiatePairing(security_requirements,
                                                  std::move(status_cb));
    return;
  }
  if (pairing_state_type_ == PairingStateType::kLegacyPairing) {
    legacy_pairing_state_->InitiatePairing(std::move(status_cb));
    return;
  }
  bt_log(WARN,
         "gap",
         "Trying to initiate pairing without knowing SSP or Legacy. Will not "
         "initiate.");
}

std::optional<pw::bluetooth::emboss::IoCapability>
PairingStateManager::OnIoCapabilityRequest() {
  if (pairing_state_type_ == PairingStateType::kLegacyPairing) {
    LogSspEventInLegacyPairing(__func__);
    return std::nullopt;
  }
  return secure_simple_pairing_state_->OnIoCapabilityRequest();
}

void PairingStateManager::OnIoCapabilityResponse(
    pw::bluetooth::emboss::IoCapability peer_iocap) {
  if (pairing_state_type_ == PairingStateType::kLegacyPairing) {
    LogSspEventInLegacyPairing(__func__);
    return;
  }
  secure_simple_pairing_state_->OnIoCapabilityResponse(peer_iocap);
}

void PairingStateManager::OnUserConfirmationRequest(
    uint32_t numeric_value, UserConfirmationCallback cb) {
  if (pairing_state_type_ == PairingStateType::kLegacyPairing) {
    LogSspEventInLegacyPairing(__func__);
    return;
  }
  secure_simple_pairing_state_->OnUserConfirmationRequest(numeric_value,
                                                          std::move(cb));
}

void PairingStateManager::OnUserPasskeyRequest(UserPasskeyCallback cb) {
  if (pairing_state_type_ == PairingStateType::kLegacyPairing) {
    LogSspEventInLegacyPairing(__func__);
    return;
  }
  secure_simple_pairing_state_->OnUserPasskeyRequest(std::move(cb));
}

void PairingStateManager::OnUserPasskeyNotification(uint32_t numeric_value) {
  if (pairing_state_type_ == PairingStateType::kLegacyPairing) {
    LogSspEventInLegacyPairing(__func__);
    return;
  }
  secure_simple_pairing_state_->OnUserPasskeyNotification(numeric_value);
}

void PairingStateManager::OnSimplePairingComplete(
    pw::bluetooth::emboss::StatusCode status_code) {
  if (pairing_state_type_ == PairingStateType::kLegacyPairing) {
    LogSspEventInLegacyPairing(__func__);
    return;
  }
  secure_simple_pairing_state_->OnSimplePairingComplete(status_code);
}

std::optional<hci_spec::LinkKey> PairingStateManager::OnLinkKeyRequest() {
  if (pairing_state_type_ == PairingStateType::kSecureSimplePairing) {
    return secure_simple_pairing_state_->OnLinkKeyRequest();
  }
  if (pairing_state_type_ == PairingStateType::kLegacyPairing) {
    return legacy_pairing_state_->OnLinkKeyRequest();
  }
  return std::nullopt;
}

void PairingStateManager::OnPinCodeRequest(UserPinCodeCallback cb) {
  if (pairing_state_type_ == PairingStateType::kSecureSimplePairing) {
    bt_log(WARN,
           "gap",
           "Received a Legacy Pairing event for a %u pairing type",
           static_cast<uint8_t>(pairing_state_type_));
    cb(std::nullopt);
    return;
  }
  legacy_pairing_state_->OnPinCodeRequest(std::move(cb));
}

bool PairingStateManager::OnLinkKeyNotification(
    const UInt128& link_key,
    hci_spec::LinkKeyType key_type,
    bool local_secure_connections_supported) {
  if (pairing_state_type_ == PairingStateType::kSecureSimplePairing) {
    return secure_simple_pairing_state_->OnLinkKeyNotification(
        link_key, key_type, local_secure_connections_supported);
  }
  if (pairing_state_type_ == PairingStateType::kLegacyPairing) {
    return legacy_pairing_state_->OnLinkKeyNotification(link_key, key_type);
  }
  // No pairing state exists to evaluate the key (e.g. the link key notification
  // arrived before interrogation completed), so treat it as a pairing failure.
  // This disconnects the link, so this object may be destroyed by |status_cb_|.
  bt_log(WARN,
         "gap",
         "link key notification with no pairing state (handle: %#.4x)",
         link_->handle());
  status_cb_(link_->handle(), ToResult(HostError::kInsufficientSecurity));
  return false;
}

void PairingStateManager::OnAuthenticationComplete(
    pw::bluetooth::emboss::StatusCode status_code) {
  if (pairing_state_type_ == PairingStateType::kSecureSimplePairing) {
    secure_simple_pairing_state_->OnAuthenticationComplete(status_code);
  } else if (pairing_state_type_ == PairingStateType::kLegacyPairing) {
    legacy_pairing_state_->OnAuthenticationComplete(status_code);
  }
}

void PairingStateManager::OnEncryptionChange(hci::Result<bool> result) {
  if (pairing_state_type_ == PairingStateType::kSecureSimplePairing) {
    secure_simple_pairing_state_->OnEncryptionChange(result);
  } else if (pairing_state_type_ == PairingStateType::kLegacyPairing) {
    legacy_pairing_state_->OnEncryptionChange(result);
  }
}

void PairingStateManager::CreateOrUpdatePairingState(
    PairingStateType type, PairingDelegate::WeakPtr pairing_delegate) {
  if (type == PairingStateType::kSecureSimplePairing &&
      !secure_simple_pairing_state_) {
    secure_simple_pairing_state_ = std::make_unique<SecureSimplePairingState>(
        peer_,
        std::move(pairing_delegate),
        link_,
        outgoing_connection_,
        auth_cb_.share(),
        status_cb_.share(),
        low_energy_address_delegate_,
        controller_remote_public_key_validation_supported_,
        security_manager_factory_,
        *dispatcher_);

    secure_simple_pairing_state_->AttachInspect(
        inspect_node_, kInspectSecureSimplePairingStateNodeName);
  } else if (type == PairingStateType::kLegacyPairing &&
             !legacy_pairing_state_) {
    legacy_pairing_state_ =
        std::make_unique<LegacyPairingState>(peer_,
                                             std::move(pairing_delegate),
                                             link_,
                                             outgoing_connection_,
                                             dispatcher_,
                                             auth_cb_.share(),
                                             status_cb_.share());

    legacy_pairing_state_->AttachInspect(inspect_node_,
                                         kInspectLegacyPairingStateNodeName);
  }
  pairing_state_type_ = type;
  inspect_properties_.pairing_state_type.Set(PairingStateTypeToString(type));
}

void PairingStateManager::AttachInspect(inspect::Node& parent,
                                        std::string name) {
  inspect_node_ = parent.CreateChild(name);
  inspect_properties_.pairing_state_type =
      inspect_node_.CreateString(kInspectPairingStateTypePropertyName,
                                 PairingStateTypeToString(pairing_state_type_));
}

}  // namespace bt::gap
