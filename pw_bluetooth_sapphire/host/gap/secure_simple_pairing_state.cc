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

#include "pw_bluetooth_sapphire/internal/host/gap/secure_simple_pairing_state.h"

#include <inttypes.h>
#include <pw_assert/check.h>

#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/gap/bredr_connection_manager.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/sm/util.h"

namespace bt::gap {

using pw::bluetooth::emboss::AuthenticationRequirements;
using pw::bluetooth::emboss::IoCapability;

namespace {

const char* const kInspectEncryptionStatusPropertyName = "encryption_status";
const char* const kInspectSecurityPropertiesPropertyName =
    "security_properties";

}  // namespace

SecureSimplePairingState::SecureSimplePairingState(
    Peer::WeakPtr peer,
    PairingDelegate::WeakPtr pairing_delegate,
    WeakPtr<hci::BrEdrConnection> link,
    bool outgoing_connection,
    fit::closure auth_cb,
    StatusCallback status_cb,
    hci::LocalAddressDelegate* low_energy_address_delegate,
    bool controller_remote_public_key_validation_supported,
    sm::BrEdrSecurityManagerFactory security_manager_factory,
    pw::async::Dispatcher& dispatcher)
    : peer_id_(peer->identifier()),
      peer_(std::move(peer)),
      link_(std::move(link)),
      outgoing_connection_(outgoing_connection),
      peer_missing_key_(false),
      low_energy_address_delegate_(std::move(low_energy_address_delegate)),
      pairing_delegate_(std::move(pairing_delegate)),
      state_(State::kIdle),
      send_auth_request_callback_(std::move(auth_cb)),
      status_callback_(std::move(status_cb)),
      controller_remote_public_key_validation_supported_(
          controller_remote_public_key_validation_supported),
      security_manager_factory_(std::move(security_manager_factory)),
      dispatcher_(dispatcher) {
  PW_CHECK(link_.is_alive());
  PW_CHECK(send_auth_request_callback_);
  PW_CHECK(status_callback_);
  link_->set_encryption_change_callback(
      fit::bind_member<&SecureSimplePairingState::OnEncryptionChange>(this));
  cleanup_cb_ = [](SecureSimplePairingState* self) {
    self->link_->set_encryption_change_callback(nullptr);
    auto callbacks_to_signal =
        self->CompletePairingRequests(ToResult(HostError::kLinkDisconnected));

    bt_log(TRACE,
           "gap-bredr",
           "Signaling %zu unresolved pairing listeners for %#.4x",
           callbacks_to_signal.size(),
           self->handle());

    for (auto& cb : callbacks_to_signal) {
      cb();
    }
  };
}

SecureSimplePairingState::~SecureSimplePairingState() {
  if (cleanup_cb_) {
    cleanup_cb_(this);
  }
}

void SecureSimplePairingState::InitiatePairing(
    BrEdrSecurityRequirements security_requirements, StatusCallback status_cb) {
  // TODO(fxbug.dev/42082728): Reject pairing if peer/local device don't support
  // Secure Connections and SC is required
  if (state() == State::kIdle ||
      state() == State::kInitiatorWaitLEPairingComplete) {
    PW_CHECK(!is_pairing());

    // If the current link key already meets the security requirements, skip
    // pairing and report success.
    if (link_->ltk_type() && SecurityPropertiesMeetRequirements(
                                 sm::SecurityProperties(*link_->ltk_type()),
                                 security_requirements)) {
      status_cb(handle(), fit::ok());
      return;
    }
    // TODO(fxbug.dev/42118593): If there is no pairing delegate set AND the
    // current peer does not have a bonded link key, there is no way to upgrade
    // the link security, so we don't need to bother calling
    // `send_auth_request`.
    //
    // TODO(fxbug.dev/42133435): If current IO capabilities would make meeting
    // security requirements impossible, skip pairing and report failure
    // immediately.

    PairingRequest request{.security_requirements = security_requirements,
                           .status_callback = std::move(status_cb)};
    request_queue_.push_back(std::move(request));

    if (state() == State::kInitiatorWaitLEPairingComplete) {
      return;
    }

    InitiateNextPairingRequest();
    return;
  }

  // More than one consumer may wish to initiate pairing (e.g. concurrent
  // outbound L2CAP channels), but each should wait for the results of any
  // ongoing pairing procedure instead of sending their own Authentication
  // Request.
  if (is_pairing()) {
    PW_CHECK(state() != State::kIdle);
    bt_log(INFO,
           "gap-bredr",
           "Already pairing %#.4x (id: %s); blocking callback on completion",
           handle(),
           bt_str(peer_id()));
    PairingRequest request{.security_requirements = security_requirements,
                           .status_callback = std::move(status_cb)};
    request_queue_.push_back(std::move(request));
  } else {
    // In the error state, we should expect no pairing to be created and cancel
    // this particular request immediately.
    PW_CHECK(state() == State::kFailed);
    status_cb(handle(), ToResult(HostError::kCanceled));
  }
}

void SecureSimplePairingState::InitiateNextPairingRequest() {
  PW_CHECK(state() == State::kIdle);
  PW_CHECK(!is_pairing());

  if (request_queue_.empty()) {
    return;
  }

  // "If a BR/EDR/LE device supports LE Secure Connections, then it shall
  // initiate pairing on only one transport at a time to the same remote
  // device." (v6.0, Vol 3, Part C, Sec. 14.2)
  if (peer_->le() && peer_->le()->is_pairing()) {
    bt_log(INFO,
           "gap-bredr",
           "Waiting for LE pairing to complete on %#.4x (id %s)",
           handle(),
           bt_str(peer_id()));
    state_ = State::kInitiatorWaitLEPairingComplete;
    peer_->MutLe().add_pairing_completion_callback(
        [self = weak_self_.GetWeakPtr()]() {
          if (!self.is_alive() ||
              self->state_ != State::kInitiatorWaitLEPairingComplete) {
            return;
          }
          self->state_ = State::kIdle;
          self->InitiateNextPairingRequest();
        });
    return;
  }

  PairingRequest& request = request_queue_.front();

  current_pairing_ = Pairing::MakeInitiator(request.security_requirements,
                                            outgoing_connection_,
                                            peer_->MutBrEdr().RegisterPairing(),
                                            dispatcher_);

  bt_log(DEBUG,
         "gap-bredr",
         "Initiating queued pairing on %#.4x (id %s)",
         handle(),
         bt_str(peer_id()));
  state_ = State::kInitiatorWaitLinkKeyRequest;
  send_auth_request_callback_();
}

std::optional<IoCapability> SecureSimplePairingState::OnIoCapabilityRequest() {
  if (state() != State::kInitiatorWaitIoCapRequest &&
      state() != State::kResponderWaitIoCapRequest) {
    FailWithUnexpectedEvent(__func__);
    return std::nullopt;
  }

  // Log an error and return std::nullopt if we can't respond to a pairing
  // request because there's no pairing delegate. This corresponds to the
  // non-bondable state as outlined in spec v5.2 Vol. 3 Part C 4.3.1.
  if (!pairing_delegate().is_alive()) {
    bt_log(WARN,
           "gap-bredr",
           "No pairing delegate set; not pairing link %#.4x (peer: %s)",
           handle(),
           bt_str(peer_id()));
    // We set the state_ to Idle instead of Failed because it is possible that a
    // PairingDelegate will be set before the next pairing attempt, allowing it
    // to succeed.
    state_ = State::kIdle;
    SignalStatus(ToResult(HostError::kNotReady), __func__);
    return std::nullopt;
  }

  current_pairing_->local_iocap =
      sm::util::IOCapabilityForHci(pairing_delegate()->io_capability());
  if (state() == State::kInitiatorWaitIoCapRequest) {
    PW_CHECK(initiator());
    state_ = State::kInitiatorWaitIoCapResponse;
  } else {
    PW_CHECK(is_pairing());
    PW_CHECK(!initiator());
    current_pairing_->ComputePairingData();

    state_ = GetStateForPairingEvent(current_pairing_->expected_event);
  }

  return current_pairing_->local_iocap;
}

void SecureSimplePairingState::OnIoCapabilityResponse(IoCapability peer_iocap) {
  // If we previously provided a key for peer to pair, but that didn't work,
  // they may try to re-pair.  Cancel the previous pairing if they try to
  // restart.
  if (state() == State::kWaitEncryption) {
    PW_CHECK(is_pairing());
    current_pairing_ = nullptr;
    state_ = State::kIdle;
  }
  if (state() == State::kIdle ||
      state() == State::kInitiatorWaitLEPairingComplete) {
    PW_CHECK(!is_pairing());
    current_pairing_ =
        Pairing::MakeResponder(peer_iocap,
                               outgoing_connection_,
                               peer_->MutBrEdr().RegisterPairing(),
                               dispatcher_);

    // Defer gathering local IO Capability until OnIoCapabilityRequest, where
    // the pairing can be rejected if there's no pairing delegate.
    state_ = State::kResponderWaitIoCapRequest;
  } else if (state() == State::kInitiatorWaitIoCapResponse) {
    PW_CHECK(initiator());

    current_pairing_->peer_iocap = peer_iocap;
    current_pairing_->ComputePairingData();

    state_ = GetStateForPairingEvent(current_pairing_->expected_event);
  } else {
    FailWithUnexpectedEvent(__func__);
  }
}

void SecureSimplePairingState::OnUserConfirmationRequest(
    uint32_t numeric_value, UserConfirmationCallback cb) {
  if (state() != State::kWaitUserConfirmationRequest) {
    FailWithUnexpectedEvent(__func__);
    cb(false);
    return;
  }
  PW_CHECK(is_pairing());

  // TODO(fxbug.dev/42113087): Reject pairing if pairing delegate went away.
  PW_CHECK(pairing_delegate().is_alive());
  state_ = State::kWaitPairingComplete;

  if (current_pairing_->action == PairingAction::kAutomatic) {
    if (!outgoing_connection_) {
      bt_log(ERROR,
             "gap-bredr",
             "automatically rejecting incoming link pairing (peer: %s, handle: "
             "%#.4x)",
             bt_str(peer_id()),
             handle());
    } else {
      bt_log(DEBUG,
             "gap-bredr",
             "automatically confirming outgoing link pairing (peer: %s, "
             "handle: %#.4x)",
             bt_str(peer_id()),
             handle());
    }
    cb(outgoing_connection_);
    return;
  }
  auto confirm_cb = [callback = std::move(cb),
                     pairing = current_pairing_->GetWeakPtr(),
                     peer_id = peer_id(),
                     handle = handle()](bool confirm) mutable {
    if (!pairing.is_alive()) {
      return;
    }
    bt_log(DEBUG,
           "gap-bredr",
           "%sing User Confirmation Request (peer: %s, handle: %#.4x)",
           confirm ? "Confirm" : "Cancel",
           bt_str(peer_id),
           handle);
    callback(confirm);
  };
  // PairingAction::kDisplayPasskey indicates that this device has a display and
  // performs "Numeric Comparison with automatic confirmation" but
  // auto-confirmation is delegated to PairingDelegate.
  if (current_pairing_->action == PairingAction::kDisplayPasskey ||
      current_pairing_->action == PairingAction::kComparePasskey) {
    pairing_delegate()->DisplayPasskey(
        peer_id(),
        numeric_value,
        PairingDelegate::DisplayMethod::kComparison,
        std::move(confirm_cb));
  } else if (current_pairing_->action == PairingAction::kGetConsent) {
    pairing_delegate()->ConfirmPairing(peer_id(), std::move(confirm_cb));
  } else {
    PW_CRASH("%#.4x (id: %s): unexpected action %d",
             handle(),
             bt_str(peer_id()),
             static_cast<int>(current_pairing_->action));
  }
}

void SecureSimplePairingState::OnUserPasskeyRequest(UserPasskeyCallback cb) {
  if (state() != State::kWaitUserPasskeyRequest) {
    FailWithUnexpectedEvent(__func__);
    cb(std::nullopt);
    return;
  }
  PW_CHECK(is_pairing());

  // TODO(fxbug.dev/42113087): Reject pairing if pairing delegate went away.
  PW_CHECK(pairing_delegate().is_alive());
  state_ = State::kWaitPairingComplete;

  PW_CHECK(current_pairing_->action == PairingAction::kRequestPasskey,
           "%#.4x (id: %s): unexpected action %d",
           handle(),
           bt_str(peer_id()),
           static_cast<int>(current_pairing_->action));
  auto pairing = current_pairing_->GetWeakPtr();
  auto passkey_cb =
      [this, callback = std::move(cb), pairing](int64_t passkey) mutable {
        if (!pairing.is_alive()) {
          return;
        }
        bt_log(DEBUG,
               "gap-bredr",
               "%#.4x (id: %s): Replying %" PRId64 " to User Passkey Request",
               handle(),
               bt_str(peer_id()),
               passkey);
        if (passkey >= 0) {
          callback(static_cast<uint32_t>(passkey));
        } else {
          callback(std::nullopt);
        }
      };
  pairing_delegate()->RequestPasskey(peer_id(), std::move(passkey_cb));
}

void SecureSimplePairingState::OnUserPasskeyNotification(
    uint32_t numeric_value) {
  if (state() != State::kWaitUserPasskeyNotification) {
    FailWithUnexpectedEvent(__func__);
    return;
  }
  PW_CHECK(is_pairing());

  // TODO(fxbug.dev/42113087): Reject pairing if pairing delegate went away.
  PW_CHECK(pairing_delegate().is_alive());
  state_ = State::kWaitPairingComplete;

  auto pairing = current_pairing_->GetWeakPtr();
  auto confirm_cb = [this, pairing](bool confirm) {
    if (!pairing.is_alive()) {
      return;
    }
    bt_log(DEBUG,
           "gap-bredr",
           "%#.4x (id: %s): Can't %s pairing from Passkey Notification side",
           handle(),
           bt_str(peer_id()),
           confirm ? "confirm" : "cancel");
  };
  pairing_delegate()->DisplayPasskey(peer_id(),
                                     numeric_value,
                                     PairingDelegate::DisplayMethod::kPeerEntry,
                                     std::move(confirm_cb));
}

void SecureSimplePairingState::OnSimplePairingComplete(
    pw::bluetooth::emboss::StatusCode status_code) {
  // The pairing process may fail early, which the controller will deliver as an
  // Simple Pairing Complete with a non-success status. Log and proxy the error
  // code.
  if (const fit::result result = ToResult(status_code);
      is_pairing() && bt_is_error(result,
                                  INFO,
                                  "gap-bredr",
                                  "Pairing failed on link %#.4x (id: %s)",
                                  handle(),
                                  bt_str(peer_id()))) {
    // TODO(fxbug.dev/42113087): Checking pairing_delegate() for reset like this
    // isn't thread safe.
    if (pairing_delegate().is_alive()) {
      pairing_delegate()->CompletePairing(peer_id(),
                                          ToResult(HostError::kFailed));
    }
    state_ = State::kFailed;
    SignalStatus(result, __func__);
    return;
  }
  // Handle successful Authentication Complete events that are not expected.
  if (state() != State::kWaitPairingComplete) {
    FailWithUnexpectedEvent(__func__);
    return;
  }
  PW_CHECK(is_pairing());

  pairing_delegate()->CompletePairing(peer_id(), fit::ok());
  state_ = State::kWaitLinkKey;
}

std::optional<hci_spec::LinkKey> SecureSimplePairingState::OnLinkKeyRequest() {
  if (state() != State::kIdle &&
      state() != State::kInitiatorWaitLinkKeyRequest &&
      state() != State::kInitiatorWaitLEPairingComplete) {
    FailWithUnexpectedEvent(__func__);
    return std::nullopt;
  }

  PW_CHECK(peer_.is_alive());

  std::optional<sm::LTK> link_key;

  if (peer_missing_key_) {
    bt_log(INFO,
           "gap-bredr",
           "peer %s missing key, ignoring our key",
           bt_str(peer_->identifier()));
  } else if (peer_->bredr() && peer_->bredr()->bonded()) {
    bt_log(INFO,
           "gap-bredr",
           "recalling link key for bonded peer %s",
           bt_str(peer_->identifier()));

    PW_CHECK(peer_->bredr()->link_key().has_value());
    link_key = peer_->bredr()->link_key();
    PW_CHECK(link_key->security().enc_key_size() ==
             hci_spec::kBrEdrLinkKeySize);

    const auto link_key_type = link_key->security().GetLinkKeyType();
    link_->set_link_key(link_key->key(), link_key_type);
  } else {
    bt_log(
        INFO, "gap-bredr", "peer %s not bonded", bt_str(peer_->identifier()));
  }

  // The link key request may be received outside of Simple Pairing (e.g. when
  // the peer initiates the authentication procedure).
  if (!is_pairing()) {
    if (link_key.has_value()) {
      current_pairing_ = Pairing::MakeResponderForBonded(
          peer_->MutBrEdr().RegisterPairing(), dispatcher_);
      state_ = State::kWaitEncryption;
      return link_key->key();
    }
    return std::optional<hci_spec::LinkKey>();
  }
  PW_CHECK(is_pairing());

  if (link_key.has_value() &&
      SecurityPropertiesMeetRequirements(
          link_key->security(), current_pairing_->preferred_security)) {
    // Skip Simple Pairing and just perform authentication with existing key.
    state_ = State::kInitiatorWaitAuthComplete;
    return link_key->key();
  }

  // Request that the controller perform Simple Pairing to generate a new key.
  state_ = State::kInitiatorWaitIoCapRequest;
  return std::nullopt;
}

void SecureSimplePairingState::OnLinkKeyNotification(
    const UInt128& link_key,
    hci_spec::LinkKeyType key_type,
    bool local_secure_connections_supported) {
  // Reject pairing if the controller is in pairing debug mode because it's a
  // security hazard to pair and bond using Debug Combination link keys.
  if (key_type == hci_spec::LinkKeyType::kDebugCombination) {
    bt_log(WARN,
           "gap-bredr",
           "Pairing on link %#.4x (id: %s) resulted in insecure Debug "
           "Combination link key",
           handle(),
           bt_str(peer_id()));
    state_ = State::kFailed;
    SignalStatus(ToResult(HostError::kInsufficientSecurity),
                 "OnLinkKeyNotification used Debug Combination key");
    return;
  }

  // When not pairing, only connection link key changes are allowed.
  if (!is_pairing() && key_type == hci_spec::LinkKeyType::kChangedCombination) {
    if (!link_->ltk()) {
      bt_log(WARN,
             "gap-bredr",
             "Got Changed Combination key but link %#.4x (id: %s) has no "
             "current key",
             handle(),
             bt_str(peer_id()));
      state_ = State::kFailed;
      SignalStatus(ToResult(HostError::kInsufficientSecurity),
                   "OnLinkKeyNotification with no current key");
      return;
    }

    bt_log(DEBUG,
           "gap-bredr",
           "Changing link key on %#.4x (id: %s)",
           handle(),
           bt_str(peer_id()));
    link_->set_link_key(hci_spec::LinkKey(link_key, 0, 0), key_type);
    return;
  }

  if (state() != State::kWaitLinkKey) {
    FailWithUnexpectedEvent(__func__);
    return;
  }

  // The association model and resulting link security properties are computed
  // by both the Link Manager (controller) and the host subsystem, so check that
  // they agree.
  PW_CHECK(is_pairing());
  sm::SecurityProperties sec_props = sm::SecurityProperties(key_type);
  current_pairing_->received_link_key_security_properties = sec_props;

  // Link keys resulting from legacy pairing are assigned lowest security level
  // and we reject them.
  if (sec_props.level() == sm::SecurityLevel::kNoSecurity) {
    bt_log(WARN,
           "gap-bredr",
           "Link key (type %hhu) for %#.4x (id: %s) has insufficient security",
           static_cast<unsigned char>(key_type),
           handle(),
           bt_str(peer_id()));
    state_ = State::kFailed;
    SignalStatus(ToResult(HostError::kInsufficientSecurity),
                 "OnLinkKeyNotification with insufficient security");
    return;
  }

  // inclusive-language: ignore
  // If we performed an association procedure for MITM protection then expect
  // the controller to produce a corresponding "authenticated" link key.
  // Inversely, do not accept a link key reported as authenticated if we haven't
  // performed the corresponding association procedure because it may provide a
  // false high expectation of security to the user or application.
  if (sec_props.authenticated() != current_pairing_->authenticated) {
    bt_log(WARN,
           "gap-bredr",
           "Expected %sauthenticated link key for %#.4x (id: %s), got %hhu",
           current_pairing_->authenticated ? "" : "un",
           handle(),
           bt_str(peer_id()),
           static_cast<unsigned char>(key_type));
    state_ = State::kFailed;
    SignalStatus(ToResult(HostError::kInsufficientSecurity),
                 "OnLinkKeyNotification with incorrect link authorization");
    return;
  }

  // Set Security Properties for this BR/EDR connection
  bredr_security_ = sec_props;

  // TODO(fxbug.dev/42082735): When in SC Only mode, all services require
  // security mode 4, level 4
  if (security_mode_ == BrEdrSecurityMode::SecureConnectionsOnly &&
      security_properties().level() !=
          sm::SecurityLevel::kSecureAuthenticated) {
    bt_log(WARN,
           "gap-bredr",
           "BR/EDR link key has insufficient security for Secure Connections "
           "Only mode");
    state_ = State::kFailed;
    SignalStatus(ToResult(HostError::kInsufficientSecurity),
                 "OnLinkKeyNotification requires Secure Connections");
    return;
  }

  // If peer and local Secure Connections support are present, the pairing logic
  // needs to verify that the Link Key Type received in the Link Key
  // Notification event is one of the Secure Connections types (0x07 and 0x08).
  //
  // Core Spec v5.2 Vol 4, Part E, 7.7.24: The values 0x07 and 0x08 shall only
  // be used when the Host has indicated support for Secure Connections in the
  // Secure_Connections_Host_Support parameter.
  if (IsPeerSecureConnectionsSupported() &&
      local_secure_connections_supported) {
    if (!security_properties().secure_connections()) {
      bt_log(WARN,
             "gap-bredr",
             "Link Key Type must be a Secure Connections key type;"
             " Received type: %hhu (handle: %#.4x, id: %s)",
             static_cast<unsigned char>(key_type),
             handle(),
             bt_str(peer_id()));
      state_ = State::kFailed;
      SignalStatus(ToResult(HostError::kInsufficientSecurity),
                   "OnLinkKeyNotification requires Secure Connections");
      return;
    }
    link_->set_use_secure_connections(true);
  }

  link_->set_link_key(hci_spec::LinkKey(link_key, 0, 0), key_type);
  if (initiator()) {
    state_ = State::kInitiatorWaitAuthComplete;
  } else {
    EnableEncryption();
  }
}

void SecureSimplePairingState::OnAuthenticationComplete(
    pw::bluetooth::emboss::StatusCode status_code) {
  if (is_pairing() && peer_->bredr() && peer_->bredr()->bonded() &&
      status_code == pw::bluetooth::emboss::StatusCode::PIN_OR_KEY_MISSING) {
    // We have provided our link key, but the remote side says they don't have a
    // key. Pretend we don't have a link key, then start the pairing over. We
    // will get consent even if we are otherwise kAutomatic
    bt_log(
        INFO,
        "gap-bredr",
        "Re-initiating pairing on %#.4x (id %s) as remote side reports no key.",
        handle(),
        bt_str(peer_id()));
    peer_missing_key_ = true;
    current_pairing_->allow_automatic = false;
    state_ = State::kInitiatorWaitLinkKeyRequest;
    send_auth_request_callback_();
    return;
  }
  // The pairing process may fail early, which the controller will deliver as an
  // Authentication Complete with a non-success status. Log and proxy the error
  // code.
  if (const fit::result result = ToResult(status_code);
      bt_is_error(result,
                  INFO,
                  "gap-bredr",
                  "Authentication failed on link %#.4x (id: %s)",
                  handle(),
                  bt_str(peer_id()))) {
    state_ = State::kFailed;
    SignalStatus(result, __func__);
    return;
  }

  // Handle successful Authentication Complete events that are not expected.
  if (state() != State::kInitiatorWaitAuthComplete) {
    FailWithUnexpectedEvent(__func__);
    return;
  }
  PW_CHECK(initiator());
  EnableEncryption();
}

// Bluetooth core specification Version 6.0, Volume 2, Part C, Section 2.5.1:
// An LMP Transaction Collision indicates that both the Central and the
// Peripheral initiated the same LMP transaction at the same time. We only
// receive this error when we are the Peripheral. The Central's operation
// takes precedence. Ignore this event and wait for the completion event after
// the Central LM completes the procedure.
//
// In other cases, the Central and Peripheral can both initiate
// incongruent transactions at the same time. In such a situation, the
// Controller will respond with Different Transaction Collision and the
// Central's procedure will take precedence. We should then retry our failed
// transaction at a later time.
SecureSimplePairingState::ActionOnError
SecureSimplePairingState::GetActionOnError(
    const bt::Error<pw::bluetooth::emboss::StatusCode>& error) {
  if (!error.is_protocol_error()) {
    return ActionOnError::kFail;
  }

  bool is_central = outgoing_connection_;
  if (error.protocol_error() ==
      pw::bluetooth::emboss::StatusCode::LMP_ERROR_TRANSACTION_COLLISION) {
    if (is_central) {
      bt_log(WARN,
             "gap-bredr",
             "LMP transaction collision while attempting to enable encryption. "
             "We are the central and should never have gotten this "
             "notification from the Controller.");
      inspect_metrics_.central_lmp_transaction_collision.Add();
    } else {
      bt_log(INFO,
             "gap-bredr",
             "LMP transaction collision while attempting to enable encryption. "
             "Waiting for local LM to resolve the collision.");
      inspect_metrics_.peripheral_lmp_transaction_collision.Add();
    }

    return ActionOnError::kIgnore;
  }

  if (error.protocol_error() ==
      pw::bluetooth::emboss::StatusCode::DIFFERENT_TRANSACTION_COLLISION) {
    if (is_central) {
      bt_log(INFO,
             "gap-bredr",
             "Different transaction collision while attempting to enable "
             "encryption as the central. Ignoring and letting the LM complete "
             "the Central's operation.");
      inspect_metrics_.central_different_transaction_collision.Add();
      return ActionOnError::kIgnore;
    } else {
      bt_log(WARN,
             "gap-bredr",
             "Different transaction collision while attempting to enable "
             "encryption. Will retry the original operation shortly.");
      inspect_metrics_.peripheral_different_transaction_collision.Add();
      return ActionOnError::kRetry;
    }
  }

  return ActionOnError::kFail;
}

void SecureSimplePairingState::OnEncryptionChange(hci::Result<bool> result) {
  // Update inspect properties
  pw::bluetooth::emboss::EncryptionStatus encryption_status =
      link_->encryption_status();
  inspect_properties_.encryption_status.Set(
      EncryptionStatusToString(encryption_status));

  if (state() != State::kWaitEncryption) {
    // Ignore encryption changes when not expecting them because they may be
    // triggered by the peer at any time (v5.0 Vol 2, Part F, Sec 4.4).
    bt_log(TRACE,
           "gap-bredr",
           "%#.4x (id: %s): %s(%s, %s) in state \"%s\"; taking no action",
           handle(),
           bt_str(peer_id()),
           __func__,
           bt_str(result),
           result.is_ok() ? (result.value() ? "true" : "false") : "?",
           ToString(state()));
    return;
  }

  if (result.is_ok() && !result.value()) {
    // With Secure Connections, encryption should never be disabled (v5.0 Vol 2,
    // Part E, Sec 7.1.16) at all.
    bt_log(WARN,
           "gap-bredr",
           "Pairing failed due to encryption disable on link %#.4x (id: %s)",
           handle(),
           bt_str(peer_id()));
    result = fit::error(Error(HostError::kFailed));
  }

  if (result.is_error()) {
    current_pairing_->retry_enable_encryption_task.set_function(
        [this](pw::async::Context /*ctx*/, pw::Status status) {
          if (status.ok() && state_ == State::kWaitEncryption) {
            bt_log(INFO, "hci-le", "Retrying enabling encryption");
            EnableEncryption();
          }
        });

    const auto& error = result.error_value();
    ActionOnError action = GetActionOnError(error);
    switch (action) {
      case ActionOnError::kRetry:
        current_pairing_->retry_enable_encryption_task.PostAfter(
            kDelayRetryEnableEncryption);
        return;
      case ActionOnError::kIgnore:
        return;
      case ActionOnError::kFail:
        state_ = State::kFailed;
        SignalStatus(result.take_error(), __func__);
        return;
    }
  }

  if (!current_pairing_->received_link_key_security_properties) {
    bt_log(
        DEBUG,
        "gap-bredr",
        "skipping BR/EDR cross-transport key derivation (previously paired)");
  } else if (link_->role() != pw::bluetooth::emboss::ConnectionRole::CENTRAL) {
    // Only the central can initiate cross-transport key derivation.
    bt_log(DEBUG,
           "gap-bredr",
           "skipping BR/EDR cross-transport key derivation as peripheral");
  } else if (!security_manager_) {
    bt_log(INFO,
           "gap-bredr",
           "skipping BR/EDR cross-transport key derivation because SMP "
           "channel not set");
  } else {
    state_ = State::kWaitCrossTransportKeyDerivation;
    security_manager_->InitiateBrEdrCrossTransportKeyDerivation(
        fit::bind_member<
            &SecureSimplePairingState::OnCrossTransportKeyDerivationComplete>(
            this));
    return;
  }

  state_ = State::kIdle;
  SignalStatus(hci::Result<>(fit::ok()), __func__);
}

std::unique_ptr<SecureSimplePairingState::Pairing>
SecureSimplePairingState::Pairing::MakeInitiator(
    BrEdrSecurityRequirements security_requirements,
    bool outgoing_connection,
    Peer::PairingToken&& token,
    pw::async::Dispatcher& dispatcher) {
  // Private constructor is inaccessible to std::make_unique.
  std::unique_ptr<Pairing> pairing(
      new Pairing(outgoing_connection, std::move(token), dispatcher));
  pairing->initiator = true;
  pairing->preferred_security = security_requirements;
  return pairing;
}

std::unique_ptr<SecureSimplePairingState::Pairing>
SecureSimplePairingState::Pairing::MakeResponder(
    pw::bluetooth::emboss::IoCapability peer_iocap,
    bool outgoing_connection,
    Peer::PairingToken&& token,
    pw::async::Dispatcher& dispatcher) {
  // Private constructor is inaccessible to std::make_unique.
  std::unique_ptr<Pairing> pairing(
      new Pairing(outgoing_connection, std::move(token), dispatcher));
  pairing->initiator = false;
  pairing->peer_iocap = peer_iocap;
  // Don't try to upgrade security as responder.
  pairing->preferred_security = {.authentication = false,
                                 .secure_connections = false};
  return pairing;
}

std::unique_ptr<SecureSimplePairingState::Pairing>
SecureSimplePairingState::Pairing::MakeResponderForBonded(
    Peer::PairingToken&& token, pw::async::Dispatcher& dispatcher) {
  std::unique_ptr<Pairing> pairing(new Pairing(
      /* link initiated */ false, std::move(token), dispatcher));
  pairing->initiator = false;
  // Don't try to upgrade security as responder.
  pairing->preferred_security = {.authentication = false,
                                 .secure_connections = false};
  return pairing;
}

void SecureSimplePairingState::Pairing::ComputePairingData() {
  if (initiator) {
    action = GetInitiatorPairingAction(local_iocap, peer_iocap);
  } else {
    action = GetResponderPairingAction(peer_iocap, local_iocap);
  }
  if (!allow_automatic && action == PairingAction::kAutomatic) {
    action = PairingAction::kGetConsent;
  }
  expected_event = GetExpectedEvent(local_iocap, peer_iocap);
  PW_DCHECK(GetStateForPairingEvent(expected_event) != State::kFailed);
  authenticated = IsPairingAuthenticated(local_iocap, peer_iocap);
  bt_log(DEBUG,
         "gap-bredr",
         "As %s with local %hhu/peer %hhu capabilities, expecting an "
         "%sauthenticated %u pairing "
         "using %#x%s",
         initiator ? "initiator" : "responder",
         static_cast<unsigned char>(local_iocap),
         static_cast<unsigned char>(peer_iocap),
         authenticated ? "" : "un",
         static_cast<unsigned int>(action),
         expected_event,
         allow_automatic ? "" : " (auto not allowed)");
}

const char* SecureSimplePairingState::ToString(
    SecureSimplePairingState::State state) {
  switch (state) {
    case State::kIdle:
      return "Idle";
    case State::kInitiatorWaitLEPairingComplete:
      return "InitiatorWaitLEPairingComplete";
    case State::kInitiatorWaitLinkKeyRequest:
      return "InitiatorWaitLinkKeyRequest";
    case State::kInitiatorWaitIoCapRequest:
      return "InitiatorWaitIoCapRequest";
    case State::kInitiatorWaitIoCapResponse:
      return "InitiatorWaitIoCapResponse";
    case State::kResponderWaitIoCapRequest:
      return "ResponderWaitIoCapRequest";
    case State::kWaitUserConfirmationRequest:
      return "WaitUserConfirmationRequest";
    case State::kWaitUserPasskeyRequest:
      return "WaitUserPasskeyRequest";
    case State::kWaitUserPasskeyNotification:
      return "WaitUserPasskeyNotification";
    case State::kWaitPairingComplete:
      return "WaitPairingComplete";
    case State::kWaitLinkKey:
      return "WaitLinkKey";
    case State::kInitiatorWaitAuthComplete:
      return "InitiatorWaitAuthComplete";
    case State::kWaitEncryption:
      return "WaitEncryption";
    case State::kWaitCrossTransportKeyDerivation:
      return "WaitCrossTransportKeyDerivation";
    case State::kFailed:
      return "Failed";
    default:
      break;
  }
  return "";
}

SecureSimplePairingState::State
SecureSimplePairingState::GetStateForPairingEvent(
    hci_spec::EventCode event_code) {
  switch (event_code) {
    case hci_spec::kUserConfirmationRequestEventCode:
      return State::kWaitUserConfirmationRequest;
    case hci_spec::kUserPasskeyRequestEventCode:
      return State::kWaitUserPasskeyRequest;
    case hci_spec::kUserPasskeyNotificationEventCode:
      return State::kWaitUserPasskeyNotification;
    default:
      break;
  }
  return State::kFailed;
}

void SecureSimplePairingState::SignalStatus(hci::Result<> status,
                                            const char* caller) {
  bt_log(INFO,
         "gap-bredr",
         "Signaling pairing listeners for %#.4x (id: %s) from %s with %s",
         handle(),
         bt_str(peer_id()),
         caller,
         bt_str(status));

  // Collect the callbacks before invoking them so that
  // CompletePairingRequests() can safely access members.
  auto callbacks_to_signal = CompletePairingRequests(status);

  // This SecureSimplePairingState may be destroyed by these callbacks (e.g. if
  // signaling an error causes a disconnection), so care must be taken not to
  // access any members.
  status_callback_(handle(), status);
  for (auto& cb : callbacks_to_signal) {
    cb();
  }
}

std::vector<fit::closure> SecureSimplePairingState::CompletePairingRequests(
    hci::Result<> status) {
  std::vector<fit::closure> callbacks_to_signal;

  if (!is_pairing() || status.is_error()) {
    // On pairing failure, or if request_queue_ is non-empty while not pairing
    // (e.g. from kInitiatorWaitLEPairingComplete when an unexpected event
    // occurred), signal and clear all queued requests.
    for (auto& request : request_queue_) {
      callbacks_to_signal.emplace_back(
          [handle = handle(),
           status,
           cb = std::move(request.status_callback)]() { cb(handle, status); });
    }
    request_queue_.clear();
    current_pairing_ = nullptr;
    return callbacks_to_signal;
  }

  PW_CHECK(state_ == State::kIdle);
  PW_CHECK(link_->ltk_type().has_value());

  auto security_properties = sm::SecurityProperties(link_->ltk_type().value());

  // If a new link key was received, notify all callbacks because we always
  // negotiate the best security possible. Even though pairing succeeded, send
  // an error status if the individual request security requirements are not
  // satisfied.
  // TODO(fxbug.dev/42075714): Only notify failure to callbacks of requests that
  // inclusive-language: ignore
  // have the same (or none) MITM requirements as the current pairing.
  bool link_key_received =
      current_pairing_->received_link_key_security_properties.has_value();
  if (link_key_received) {
    for (auto& request : request_queue_) {
      auto sec_props_satisfied = SecurityPropertiesMeetRequirements(
          security_properties, request.security_requirements);
      auto request_status = sec_props_satisfied
                                ? status
                                : ToResult(HostError::kInsufficientSecurity);

      callbacks_to_signal.emplace_back(
          [handle = handle(),
           request_status,
           cb = std::move(request.status_callback)]() {
            cb(handle, request_status);
          });
    }
    request_queue_.clear();
  } else {
    // If no new link key was received, then only authentication with an old key
    // was performed (Simple Pairing was not required), and unsatisfied requests
    // should initiate a new pairing rather than failing. If any pairing
    // requests are satisfied by the existing key, notify them.
    auto it = request_queue_.begin();
    while (it != request_queue_.end()) {
      if (!SecurityPropertiesMeetRequirements(security_properties,
                                              it->security_requirements)) {
        it++;
        continue;
      }

      callbacks_to_signal.emplace_back(
          [handle = handle(), status, cb = std::move(it->status_callback)]() {
            cb(handle, status);
          });
      it = request_queue_.erase(it);
    }
  }
  current_pairing_ = nullptr;
  InitiateNextPairingRequest();

  return callbacks_to_signal;
}

void SecureSimplePairingState::EnableEncryption() {
  if (!link_->StartEncryption()) {
    bt_log(ERROR,
           "gap-bredr",
           "%#.4x (id: %s): Failed to enable encryption (state \"%s\")",
           handle(),
           bt_str(peer_id()),
           ToString(state()));
    status_callback_(link_->handle(), ToResult(HostError::kFailed));
    state_ = State::kFailed;
    return;
  }
  state_ = State::kWaitEncryption;
}

void SecureSimplePairingState::FailWithUnexpectedEvent(
    const char* handler_name) {
  bt_log(ERROR,
         "gap-bredr",
         "%#.4x (id: %s): Unexpected event %s while in state \"%s\"",
         handle(),
         bt_str(peer_id()),
         handler_name,
         ToString(state()));
  state_ = State::kFailed;
  SignalStatus(ToResult(HostError::kNotSupported), __func__);
}

bool SecureSimplePairingState::IsPeerSecureConnectionsSupported() const {
  return peer_->features().HasBit(
             /*page=*/1, hci_spec::LMPFeature::kSecureConnectionsHostSupport) &&
         peer_->features().HasBit(
             /*page=*/2,
             hci_spec::LMPFeature::kSecureConnectionsControllerSupport);
}

void SecureSimplePairingState::SetSecurityManagerChannel(
    l2cap::Channel::WeakPtr security_manager_channel) {
  if (!security_manager_channel.is_alive()) {
    return;
  }
  PW_CHECK(!security_manager_);
  security_manager_ = security_manager_factory_(
      link_,
      std::move(security_manager_channel),
      security_manager_delegate_.GetWeakPtr(),
      controller_remote_public_key_validation_supported_,
      dispatcher_,
      peer_);
}

std::optional<sm::IdentityInfo> SecureSimplePairingState::
    SecurityManagerDelegate::OnIdentityInformationRequest() {
  if (!ssp_state_->low_energy_address_delegate_->irk()) {
    bt_log(TRACE, "gap-bredr", "no local identity information to exchange");
    return std::nullopt;
  }

  bt_log(DEBUG,
         "gap-bredr",
         "will distribute local identity information (peer: %s)",
         bt_str(ssp_state_->peer_id_));
  sm::IdentityInfo id_info;
  id_info.irk = *ssp_state_->low_energy_address_delegate_->irk();
  id_info.address =
      ssp_state_->low_energy_address_delegate_->identity_address();
  return id_info;
}

void SecureSimplePairingState::AttachInspect(inspect::Node& parent,
                                             std::string name) {
  inspect_node_ = parent.CreateChild(name);

  inspect_properties_.encryption_status = inspect_node_.CreateString(
      kInspectEncryptionStatusPropertyName,
      EncryptionStatusToString(link_->encryption_status()));

  inspect_metrics_.central_lmp_transaction_collision.AttachInspect(
      inspect_node_, "central_lmp_transaction_collision");
  inspect_metrics_.peripheral_lmp_transaction_collision.AttachInspect(
      inspect_node_, "peripheral_lmp_transaction_collision");
  inspect_metrics_.central_different_transaction_collision.AttachInspect(
      inspect_node_, "central_different_transaction_collision");
  inspect_metrics_.peripheral_different_transaction_collision.AttachInspect(
      inspect_node_, "peripheral_different_transaction_collision");

  security_properties().AttachInspect(inspect_node_,
                                      kInspectSecurityPropertiesPropertyName);
}

void SecureSimplePairingState::OnCrossTransportKeyDerivationComplete(
    sm::Result<> result) {
  if (result.is_error()) {
    // CTKD failures are not treated as failures of the main BR/EDR pairing.
    bt_log(INFO,
           "gap-bredr",
           "BR/EDR cross-transport key derivation failed: %s",
           bt_str(result.error_value()));
  }
  state_ = State::kIdle;
  SignalStatus(hci::Result<>(fit::ok()), __func__);
}

PairingAction GetInitiatorPairingAction(IoCapability initiator_cap,
                                        IoCapability responder_cap) {
  if (initiator_cap == IoCapability::NO_INPUT_NO_OUTPUT) {
    return PairingAction::kAutomatic;
  }
  if (responder_cap == IoCapability::NO_INPUT_NO_OUTPUT) {
    if (initiator_cap == IoCapability::DISPLAY_YES_NO) {
      return PairingAction::kGetConsent;
    }
    return PairingAction::kAutomatic;
  }
  if (initiator_cap == IoCapability::KEYBOARD_ONLY) {
    return PairingAction::kRequestPasskey;
  }
  if (responder_cap == IoCapability::DISPLAY_ONLY) {
    if (initiator_cap == IoCapability::DISPLAY_YES_NO) {
      return PairingAction::kComparePasskey;
    }
    return PairingAction::kAutomatic;
  }
  return PairingAction::kDisplayPasskey;
}

PairingAction GetResponderPairingAction(IoCapability initiator_cap,
                                        IoCapability responder_cap) {
  if (initiator_cap == IoCapability::NO_INPUT_NO_OUTPUT &&
      responder_cap == IoCapability::KEYBOARD_ONLY) {
    return PairingAction::kGetConsent;
  }
  if (initiator_cap == IoCapability::DISPLAY_YES_NO &&
      responder_cap == IoCapability::DISPLAY_YES_NO) {
    return PairingAction::kComparePasskey;
  }
  return GetInitiatorPairingAction(responder_cap, initiator_cap);
}

hci_spec::EventCode GetExpectedEvent(IoCapability local_cap,
                                     IoCapability peer_cap) {
  if (local_cap == IoCapability::NO_INPUT_NO_OUTPUT ||
      peer_cap == IoCapability::NO_INPUT_NO_OUTPUT) {
    return hci_spec::kUserConfirmationRequestEventCode;
  }
  if (local_cap == IoCapability::KEYBOARD_ONLY) {
    return hci_spec::kUserPasskeyRequestEventCode;
  }
  if (peer_cap == IoCapability::KEYBOARD_ONLY) {
    return hci_spec::kUserPasskeyNotificationEventCode;
  }
  return hci_spec::kUserConfirmationRequestEventCode;
}

bool IsPairingAuthenticated(IoCapability local_cap, IoCapability peer_cap) {
  if (local_cap == IoCapability::NO_INPUT_NO_OUTPUT ||
      peer_cap == IoCapability::NO_INPUT_NO_OUTPUT) {
    return false;
  }
  if (local_cap == IoCapability::DISPLAY_YES_NO &&
      peer_cap == IoCapability::DISPLAY_YES_NO) {
    return true;
  }
  if (local_cap == IoCapability::KEYBOARD_ONLY ||
      peer_cap == IoCapability::KEYBOARD_ONLY) {
    return true;
  }
  return false;
}

AuthenticationRequirements GetInitiatorAuthenticationRequirements(
    IoCapability local_cap) {
  if (local_cap == IoCapability::NO_INPUT_NO_OUTPUT) {
    return AuthenticationRequirements::GENERAL_BONDING;
  }
  // inclusive-language: ignore
  return AuthenticationRequirements::MITM_GENERAL_BONDING;
}

AuthenticationRequirements GetResponderAuthenticationRequirements(
    IoCapability local_cap, IoCapability peer_cap) {
  if (IsPairingAuthenticated(local_cap, peer_cap)) {
    // inclusive-language: ignore
    return AuthenticationRequirements::MITM_GENERAL_BONDING;
  }
  return AuthenticationRequirements::GENERAL_BONDING;
}

}  // namespace bt::gap
