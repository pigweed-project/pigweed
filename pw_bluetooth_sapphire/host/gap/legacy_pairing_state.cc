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

#include "pw_bluetooth_sapphire/internal/host/gap/legacy_pairing_state.h"

#include <pw_assert/check.h>

#include <optional>

#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/common/random.h"
#include "pw_bluetooth_sapphire/internal/host/gap/types.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/util.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"
#include "pw_bluetooth_sapphire/internal/host/sm/util.h"

namespace bt::gap {

namespace {
const char* const kInspectEncryptionStatusPropertyName = "encryption_status";
const char* const kInspectSecurityPropertiesPropertyName =
    "security_properties";
}  // namespace

LegacyPairingState::LegacyPairingState(
    Peer::WeakPtr peer,
    PairingDelegate::WeakPtr pairing_delegate,
    bool outgoing_connection,
    pw::async::Dispatcher* dispatcher)
    : peer_id_(peer->identifier()),
      peer_(std::move(peer)),
      outgoing_connection_(outgoing_connection),
      dispatcher_(*dispatcher) {
  if (!pairing_delegate.is_alive()) {
    bt_log(WARN,
           "gap-bredr",
           "No pairing delegate set for peer id %s",
           bt_str(peer_id_));
    // We set the state_ to Idle instead of Failed because it is possible that a
    // PairingDelegate will be set before the next pairing attempt, allowing it
    // to succeed.
    state_ = State::kIdle;
    return;
  }
  pairing_delegate_ = std::move(pairing_delegate);
}

LegacyPairingState::LegacyPairingState(
    Peer::WeakPtr peer,
    PairingDelegate::WeakPtr pairing_delegate,
    WeakPtr<hci::BrEdrConnection> link,
    bool outgoing_connection,
    pw::async::Dispatcher* dispatcher,
    fit::closure auth_cb,
    StatusCallback status_cb)
    : LegacyPairingState(std::move(peer),
                         std::move(pairing_delegate),
                         outgoing_connection,
                         dispatcher) {
  // We can only populate |link_|, |send_auth_request_callback_|, and
  // |status_callback_| if the ACL connection is complete.
  PW_CHECK(link.is_alive());

  BuildEstablishedLink(
      std::move(link), std::move(auth_cb), std::move(status_cb));
}

LegacyPairingState::~LegacyPairingState() {
  if (link_.is_alive()) {
    link_->set_encryption_change_callback(nullptr);

    // Pairing requests are only initiated after |link_| is established
    auto callbacks_to_signal =
        CompletePairingRequests(ToResult(HostError::kLinkDisconnected));

    bt_log(TRACE,
           "gap-bredr",
           "Signaling %zu unresolved pairing listeners for link %#.4x",
           callbacks_to_signal.size(),
           handle());

    for (auto& cb : callbacks_to_signal) {
      cb();
    }
  }
}

void LegacyPairingState::BuildEstablishedLink(
    WeakPtr<hci::BrEdrConnection> link,
    fit::closure auth_cb,
    StatusCallback status_cb) {
  link_ = std::move(link);
  send_auth_request_callback_ = std::move(auth_cb);
  status_callback_ = std::move(status_cb);

  link_->set_encryption_change_callback(
      fit::bind_member<&LegacyPairingState::OnEncryptionChange>(this));

  if (link_key_.has_value()) {
    link_->set_link_key(link_key_.value(), hci_spec::LinkKeyType::kCombination);
  }
}

void LegacyPairingState::InitiatePairing(StatusCallback status_cb) {
  if (!link_.is_alive()) {
    bt_log(WARN,
           "gap-bredr",
           "Do not initiate Legacy Pairing before ACL connection is complete");
    state_ = State::kFailed;
    SignalStatus(ToResult(HostError::kFailed), __func__);
    return;
  }

  PW_CHECK(peer_.is_alive());

  // If we interrogated the peer and they support SSP, we should be using SSP
  // since we also support SSP.
  if (peer_->IsSecureSimplePairingSupported()) {
    bt_log(WARN,
           "gap-bredr",
           "Do not use Legacy Pairing when peer actually supports SSP");
    state_ = State::kFailed;
    SignalStatus(ToResult(HostError::kFailed), __func__);
    return;
  }

  PW_CHECK(pairing_delegate_.is_alive());

  // Only initiate pairing if we have output capabilities to display a PIN
  pw::bluetooth::emboss::IoCapability io_capability =
      sm::util::IOCapabilityForHci(pairing_delegate_->io_capability());
  if (io_capability ==
          pw::bluetooth::emboss::IoCapability::NO_INPUT_NO_OUTPUT ||
      io_capability == pw::bluetooth::emboss::IoCapability::KEYBOARD_ONLY) {
    bt_log(DEBUG,
           "gap-bredr",
           "Do not initiate Legacy Pairing without display output capability");
    state_ = State::kFailed;
    SignalStatus(ToResult(HostError::kFailed), __func__);
    return;
  }

  if (state_ == State::kIdle) {
    PW_CHECK(!is_pairing());

    // TODO(fxbug.dev/348676274): Do not downgrade to LP if peer was
    // previously bonded with SSP
    // TODO(fxbug.dev/348674937): Re-pair with SSP if peer was
    // previously bonded with LP

    // If the current link key already meets the security requirements, skip
    // pairing and report success.
    if (link_->ltk_type() &&
        SecurityPropertiesMeetRequirements(
            sm::SecurityProperties(link_->ltk_type().value()),
            kNoSecurityRequirements)) {
      status_cb(handle(), fit::ok());
      return;
    }

    // TODO(fxbug.dev/42118593): If there is no pairing delegate set AND
    // the current peer does not have a bonded link key, there is no way to
    // upgrade the link security, so we Do not need to bother calling
    // |send_auth_request_callback_|.

    PairingRequest request{.security_requirements = kNoSecurityRequirements,
                           .status_callback = std::move(status_cb)};
    request_queue_.push_back(std::move(request));
    InitiateNextPairingRequest();
    return;
  }

  // Multiple consumers may wish to initiate pairing (e.g. concurrent outbound
  // L2CAP channels), but each should wait for the results of any ongoing
  // pairing procedure before sending their own HCI_Authentication_Request.
  if (is_pairing()) {
    PW_CHECK(state_ != State::kIdle);
    bt_log(INFO,
           "gap-bredr",
           "Already pairing on link %#.4x for peer id %s; blocking callback on "
           "completion",
           handle(),
           bt_str(peer_id_));

    PairingRequest request{.security_requirements = kNoSecurityRequirements,
                           .status_callback = std::move(status_cb)};
    request_queue_.push_back(std::move(request));
  } else {
    // In the error state, we should expect no pairing to be created and cancel
    // this particular request immediately.
    PW_CHECK(state_ == State::kFailed);
    status_cb(handle(), ToResult(HostError::kCanceled));
  }
}

std::optional<hci_spec::LinkKey> LegacyPairingState::OnLinkKeyRequest() {
  if (state_ != State::kIdle && state_ != State::kInitiatorWaitLinkKeyRequest) {
    FailWithUnexpectedEvent(__func__);
    return std::nullopt;
  }

  PW_CHECK(peer_.is_alive());

  // If we interrogated the peer and they support SSP, we should be using SSP
  // since we also support SSP.
  if (link_.is_alive() && peer_->IsSecureSimplePairingSupported()) {
    bt_log(WARN,
           "gap-bredr",
           "Do not use Legacy Pairing when peer actually supports SSP");
    state_ = State::kFailed;
    SignalStatus(ToResult(HostError::kFailed), __func__);
    return std::nullopt;
  }

  std::optional<sm::LTK> link_key;

  // Determine if we can reuse a current link key or not. The current link key
  // is valid only if the peer is bonded, has a valid link key, and the key
  // meets the expected security requirements. Otherwise we do not return a link
  // key in order to start the PIN code request process.
  if (peer_missing_key_) {
    bt_log(
        INFO,
        "gap-bredr",
        "Peer %s is missing a link key. Ignore our link key and retry pairing",
        bt_str(peer_id_));
  } else if (peer_->bredr() && peer_->bredr()->bonded()) {
    bt_log(INFO,
           "gap-bredr",
           "Recalling link key for bonded peer %s",
           bt_str(peer_id_));

    PW_CHECK(peer_->bredr()->link_key().has_value());
    link_key = peer_->bredr()->link_key();
    PW_CHECK(link_key->security().enc_key_size() ==
             hci_spec::kBrEdrLinkKeySize);

    if (link_.is_alive()) {
      const hci_spec::LinkKeyType link_key_type =
          link_key->security().GetLinkKeyType();
      link_->set_link_key(link_key->key(), link_key_type);
    } else {
      // Connection is not complete yet so temporarily store this to later give
      // to HCI link on the HCI_Connection_Complete event.
      link_key_ = link_key->key();
    }
  } else {
    bt_log(INFO, "gap-bredr", "Peer %s is not bonded", bt_str(peer_id_));
  }

  // The link key request may be received outside of Legacy Pairing (e.g. when
  // the peer initiates the authentication procedure and has a valid link key).
  if (state_ == State::kIdle) {
    if (link_key.has_value()) {
      PW_CHECK(!is_pairing());
      current_pairing_ = Pairing::MakeResponderForBonded(
          peer_->MutBrEdr().RegisterPairing(), dispatcher_);
      state_ = State::kWaitEncryption;
      return link_key->key();
    }
    return std::nullopt;
  }

  PW_CHECK(is_pairing());

  // TODO(fxbug.dev/348676274): Do not downgrade to LP if peer was
  // previously bonded with SSP
  // TODO(fxbug.dev/348674937): Re-pair with SSP if peer was previously
  // bonded with LP

  if (link_key.has_value() &&
      SecurityPropertiesMeetRequirements(
          link_key->security(), current_pairing_->preferred_security)) {
    // Skip Legacy Pairing and just perform authentication with existing key
    PW_CHECK(current_pairing_->initiator);
    state_ = State::kInitiatorWaitAuthComplete;
    return link_key->key();
  }

  // Request that the controller perform Legacy Pairing to generate a new key
  state_ = State::kWaitPinCodeRequest;
  return std::nullopt;
}

void LegacyPairingState::OnPinCodeRequest(UserPinCodeCallback cb) {
  if (state_ != State::kIdle && state_ != State::kWaitPinCodeRequest) {
    FailWithUnexpectedEvent(__func__);
    cb(std::nullopt);
    return;
  }

  if (state_ == State::kIdle) {
    PW_CHECK(!is_pairing());
    current_pairing_ = Pairing::MakeResponder(
        outgoing_connection_, peer_->MutBrEdr().RegisterPairing(), dispatcher_);
  }

  if (!pairing_delegate_.is_alive()) {
    bt_log(WARN,
           "gap-bredr",
           "No pairing delegate set for peer id %s; rejecting PIN code request",
           bt_str(peer_id_));
    current_pairing_ = nullptr;
    // We set the state_ to Idle instead of Failed because it is possible that a
    // PairingDelegate will be set before the next pairing attempt, allowing it
    // to succeed.
    state_ = State::kIdle;
    cb(std::nullopt);
    SignalStatus(ToResult(HostError::kNotReady), __func__);
    return;
  }

  // Get our I/O capabilities
  pw::bluetooth::emboss::IoCapability io_capability =
      sm::util::IOCapabilityForHci(pairing_delegate_->io_capability());

  // If this was our in-flight request (i.e. we are initiator), we should have
  // only initiated pairing if we have output capabilities to display our PIN.
  // Devices with keyboard input and numeric output will use DisplayYesNo IO
  // Capability (Core Spec v5.4, Vol 3, Part C, 5.2.2.5, Table 5.5) so all PINs
  // will be randomly generated.
  if (initiator()) {
    PW_CHECK(io_capability !=
             pw::bluetooth::emboss::IoCapability::NO_INPUT_NO_OUTPUT);
    PW_CHECK(io_capability !=
             pw::bluetooth::emboss::IoCapability::KEYBOARD_ONLY);

    // Randomly generate a 4-digit passkey
    uint16_t random_pin;
    random_generator()->GetInt<uint16_t>(random_pin,
                                         /*exclusive_upper_bound=*/10000);

    auto confirm_cb = [this,
                       callback = std::move(cb),
                       pairing = current_pairing_->GetWeakPtr(),
                       peer_id = peer_id_,
                       handle = handle(),
                       random_pin](bool confirm) mutable {
      if (!pairing.is_alive()) {
        return;
      }

      bt_log(DEBUG,
             "gap-bredr",
             "%sing User Confirmation Request (peer: %s, handle: %#.4x)",
             confirm ? "Confirm" : "Cancel",
             bt_str(peer_id),
             handle);

      if (confirm) {
        state_ = State::kWaitLinkKey;
        callback(random_pin);
      } else {
        callback(std::nullopt);
      }
    };
    pairing_delegate_->DisplayPasskey(
        peer_id_,
        random_pin,
        PairingDelegate::DisplayMethod::kPeerEntry,
        std::move(confirm_cb));
    return;
  }

  // When we are the responder (regardless of whether the ACL connection is
  // complete), we will request a PIN code from the product. The pairing
  // delegate and product configuration determine if the device wants to try
  // common pins (e.g. "0000") when it has no input capability. Otherwise the
  // user will be requested to input their own 4-digit PIN code.
  auto pairing = current_pairing_->GetWeakPtr();
  auto passkey_cb =
      [this, callback = std::move(cb), pairing](int64_t passkey) mutable {
        if (!pairing.is_alive()) {
          return;
        }
        bt_log(DEBUG, "gap-bredr", "Replying to User Passkey Request");
        if (passkey >= 0) {
          state_ = State::kWaitLinkKey;
          callback(static_cast<uint16_t>(passkey));
        } else {
          callback(std::nullopt);
        }
      };
  pairing_delegate_->RequestPasskey(peer_id_, std::move(passkey_cb));
}

void LegacyPairingState::OnLinkKeyNotification(const UInt128& link_key,
                                               hci_spec::LinkKeyType key_type) {
  if (state_ != State::kWaitLinkKey) {
    FailWithUnexpectedEvent(__func__);
    return;
  }

  PW_CHECK(peer_.is_alive());

  // Legacy Pairing generates a Combination key type (Core Spec v5.4, Vol 4,
  // Part E, 7.7.24)
  if (key_type != hci_spec::LinkKeyType::kCombination) {
    bt_log(WARN,
           "gap-bredr",
           "Legacy Pairing requires Combination key type but link %#.4x for "
           "peer id %s has type %s",
           handle(),
           bt_str(peer_id_),
           hci_spec::LinkKeyTypeToString(key_type));
    state_ = State::kFailed;
    SignalStatus(ToResult(HostError::kFailed), __func__);
    return;
  }

  // The resulting link security properties are computed by both the Link
  // Manager (Controller) and the Host subsystem, so check that they agree.
  PW_CHECK(is_pairing());
  sm::SecurityProperties sec_props = sm::SecurityProperties(key_type);
  current_pairing_->security_properties = sec_props;

  // Set security properties for this BR/EDR connection
  bredr_security_ = sec_props;

  // Link keys resulting from legacy pairing are assigned lowest security level.
  PW_CHECK(sec_props.level() == sm::SecurityLevel::kNoSecurity);

  if (!link_.is_alive()) {
    // Connection is not complete yet so temporarily store this to later give
    // to HCI link on the HCI_Connection_Complete event.
    link_key_ = hci_spec::LinkKey(link_key, /*rand=*/0, /*ediv=*/0);

    state_ = State::kIdle;
    SignalStatus(hci::Result<>(fit::ok()), __func__);
    return;
  }

  // If we interrogated the peer and they support SSP, we should be using SSP
  // since we also support SSP.
  if (peer_->IsSecureSimplePairingSupported()) {
    bt_log(WARN,
           "gap-bredr",
           "Do not use Legacy Pairing when peer actually supports SSP");
    state_ = State::kFailed;
    SignalStatus(ToResult(HostError::kFailed), __func__);
    return;
  }

  link_->set_link_key(hci_spec::LinkKey(link_key, /*rand=*/0, /*ediv=*/0),
                      key_type);
  if (initiator()) {
    // Initiators will receive a HCI_Authentication_Complete event
    state_ = State::kInitiatorWaitAuthComplete;
  } else {
    // Responders can now enable encryption after generating a valid link key
    EnableEncryption();
  }
}

void LegacyPairingState::OnAuthenticationComplete(
    pw::bluetooth::emboss::StatusCode status_code) {
  PW_CHECK(link_.is_alive());

  if (is_pairing() && peer_->bredr() && peer_->bredr()->bonded() &&
      status_code == pw::bluetooth::emboss::StatusCode::PIN_OR_KEY_MISSING) {
    // Even though we have provided our link key, the peer does not have a valid
    // link key. We restart the pairing process again by sending a
    // HCI_Authentication_Requested command.
    bt_log(INFO,
           "gap-bredr",
           "Re-initiating pairing on link %#.4x for peer id %s as remote "
           "device reports no key",
           handle(),
           bt_str(peer_id_));
    peer_missing_key_ = true;
    current_pairing_->allow_automatic = false;
    state_ = State::kInitiatorWaitLinkKeyRequest;
    send_auth_request_callback_();
    return;
  }

  // The pairing process may fail early, which the controller will deliver as an
  // HCI_Authentication_Complete with a non-success status.
  if (const fit::result result = ToResult(status_code);
      bt_is_error(result,
                  INFO,
                  "gap-bredr",
                  "Authentication failed on link %#.4x for peer id %s",
                  handle(),
                  bt_str(peer_id_))) {
    state_ = State::kFailed;
    SignalStatus(result, __func__);
    return;
  }

  // Fail on unexpected HCI_Authentication_Complete events
  if (state_ != State::kInitiatorWaitAuthComplete) {
    FailWithUnexpectedEvent(__func__);
    return;
  }

  // HCI_Authentication_Complete events are only received by initiators
  PW_CHECK(initiator());

  // After successful authentication, we can now enable encryption
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
LegacyPairingState::ActionOnError LegacyPairingState::GetActionOnError(
    const bt::Error<pw::bluetooth::emboss::StatusCode>& error) {
  if (!error.is_protocol_error()) {
    return ActionOnError::kFail;
  }

  bool is_central = outgoing_connection();
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

void LegacyPairingState::OnEncryptionChange(hci::Result<bool> result) {
  PW_CHECK(link_.is_alive());

  if (state_ != State::kWaitEncryption) {
    // Ignore encryption changes when not expecting them because they may be
    // triggered by the peer at any time (Core Spec v5.4, Vol 2, Part F, 4.4)
    bt_log(TRACE,
           "gap-bredr",
           "Ignoring encryption change event with result %s on link %#.4x for "
           "peer id %s in %s state",
           bt_str(result),
           handle(),
           bt_str(peer_id_),
           ToString(state_));
    return;
  }

  pw::bluetooth::emboss::EncryptionStatus encryption_status =
      link_->encryption_status();
  // Update inspect properties
  inspect_properties_.encryption_status.Set(
      EncryptionStatusToString(encryption_status));

  // E0 encryption shall be used for Legacy Pairing when encryption is enabled
  // (Core Spec v5.4, Vol 2, Part C, 4.2.5)
  if (encryption_status != pw::bluetooth::emboss::EncryptionStatus::
                               ON_WITH_E0_FOR_BREDR_OR_AES_FOR_LE) {
    bt_log(WARN,
           "gap-bredr",
           "E0 encryption must be used for legacy pairing when encryption is "
           "enabled");
    state_ = State::kFailed;
    SignalStatus(ToResult(HostError::kFailed), __func__);
    return;
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

  // Encryption indicates the end of pairing so reset state for another pairing
  state_ = State::kIdle;
  SignalStatus(hci::Result<>(fit::ok()), __func__);
}

std::unique_ptr<LegacyPairingState::Pairing>
LegacyPairingState::Pairing::MakeInitiator(
    BrEdrSecurityRequirements security_requirements,
    bool outgoing_connection,
    Peer::PairingToken&& token,
    pw::async::Dispatcher& dispatcher) {
  // Private constructor is inaccessible to std::make_unique
  std::unique_ptr<Pairing> pairing(
      new Pairing(outgoing_connection, std::move(token), dispatcher));
  pairing->initiator = true;
  pairing->preferred_security = security_requirements;
  return pairing;
}

std::unique_ptr<LegacyPairingState::Pairing>
LegacyPairingState::Pairing::MakeResponder(
    bool outgoing_connection,
    Peer::PairingToken&& token,
    pw::async::Dispatcher& dispatcher,
    std::optional<pw::bluetooth::emboss::IoCapability> peer_iocap) {
  // Private constructor is inaccessible to std::make_unique
  std::unique_ptr<Pairing> pairing(
      new Pairing(outgoing_connection, std::move(token), dispatcher));
  pairing->initiator = false;
  if (peer_iocap.has_value()) {
    pairing->peer_iocap = peer_iocap.value();
  }
  // Do not try to upgrade security as responder
  pairing->preferred_security = kNoSecurityRequirements;
  return pairing;
}

std::unique_ptr<LegacyPairingState::Pairing>
LegacyPairingState::Pairing::MakeResponderForBonded(
    Peer::PairingToken&& token, pw::async::Dispatcher& dispatcher) {
  std::unique_ptr<Pairing> pairing(new Pairing(
      /*automatic=*/false, std::move(token), dispatcher));
  pairing->initiator = false;
  // Do not try to upgrade security as responder
  pairing->preferred_security = kNoSecurityRequirements;
  return pairing;
}

void LegacyPairingState::EnableEncryption() {
  PW_CHECK(link_.is_alive());

  if (!link_->StartEncryption()) {
    bt_log(
        ERROR,
        "gap-bredr",
        "Failed to enable encryption on link %#.4x for peer id %s in %s state",
        handle(),
        bt_str(peer_id_),
        ToString(state_));
    status_callback_(link_->handle(), ToResult(HostError::kFailed));
    state_ = State::kFailed;
    return;
  }
  state_ = State::kWaitEncryption;
}

void LegacyPairingState::SignalStatus(hci::Result<> status,
                                      const char* caller) {
  bt_log(INFO,
         "gap-bredr",
         "Signaling pairing listeners for peer id %s from %s with status %s",
         bt_str(peer_id_),
         caller,
         bt_str(status));

  // Collect the callbacks before invoking them so that
  // |CompletePairingRequests()| can safely access members.
  auto callbacks_to_signal = CompletePairingRequests(status);

  if (link_.is_alive()) {
    // This LegacyPairingState may be destroyed by these callbacks (e.g. if
    // signaling an error causes a disconnection), so care must be taken not to
    // access any members.
    status_callback_(handle(), status);
  }

  for (auto& cb : callbacks_to_signal) {
    cb();
  }
}

void LegacyPairingState::InitiateNextPairingRequest() {
  PW_CHECK(state_ == State::kIdle);
  PW_CHECK(!is_pairing());

  if (!link_.is_alive()) {
    bt_log(WARN,
           "gap-bredr",
           "Do not initiate Legacy Pairing before ACL connection is complete");
    state_ = State::kFailed;
    SignalStatus(ToResult(HostError::kFailed), __func__);
    return;
  }

  PW_CHECK(peer_.is_alive());

  // If we interrogated the peer and they support SSP, we should be using
  // SSP since we also support SSP.
  if (peer_->IsSecureSimplePairingSupported()) {
    bt_log(WARN,
           "gap-bredr",
           "Do not use Legacy Pairing when peer actually supports SSP");
    state_ = State::kFailed;
    SignalStatus(ToResult(HostError::kFailed), __func__);
    return;
  }

  if (request_queue_.empty()) {
    return;
  }

  PairingRequest& request = request_queue_.front();

  current_pairing_ = Pairing::MakeInitiator(request.security_requirements,
                                            outgoing_connection_,
                                            peer_->MutBrEdr().RegisterPairing(),
                                            dispatcher_);
  bt_log(DEBUG,
         "gap-bredr",
         "Initiating queued pairing on link %#.4x for peer id %s",
         handle(),
         bt_str(peer_id_));
  state_ = State::kInitiatorWaitLinkKeyRequest;
  send_auth_request_callback_();
}

std::vector<fit::closure> LegacyPairingState::CompletePairingRequests(
    hci::Result<> status) {
  std::vector<fit::closure> callbacks_to_signal;

  if (!is_pairing()) {
    PW_CHECK(request_queue_.empty());
    return callbacks_to_signal;
  }

  if (status.is_error()) {
    // On pairing failure, signal all requests
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

  sm::SecurityProperties security_properties =
      sm::SecurityProperties(hci_spec::LinkKeyType::kCombination);

  // If a new link key was received, notify all callbacks because we always
  // negotiate the best security possible. Even though pairing succeeded,
  // send an error status if the individual request security requirements
  // are not satisfied.
  // TODO(fxbug.dev/42075714): Only notify failure to callbacks of
  // requests that have the same (or none) on-path attack requirements as
  // the current pairing.
  bool link_key_received = current_pairing_->security_properties.has_value();
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
    // If no new link key was received, then only authentication with an old
    // key was performed (Legacy Pairing was not required), and unsatisfied
    // requests should initiate a new pairing rather than failing. If any
    // pairing requests are satisfied by the existing key, notify them.
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

void LegacyPairingState::FailWithUnexpectedEvent(const char* handler_name) {
  bt_log(ERROR,
         "gap-bredr",
         "Unexpected %s event while in %s state on link %#.4x for peer id %s",
         handler_name,
         ToString(state_),
         handle(),
         bt_str(peer_id_));
  state_ = State::kFailed;
  SignalStatus(ToResult(HostError::kFailed), __func__);
}

const char* LegacyPairingState::ToString(LegacyPairingState::State state) {
  switch (state) {
    case State::kIdle:
      return "Idle";
    case State::kInitiatorWaitLinkKeyRequest:
      return "InitiatorWaitLinkKeyRequest";
    case State::kWaitPinCodeRequest:
      return "kWaitPinCodeRequest";
    case State::kWaitLinkKey:
      return "WaitLinkKey";
    case State::kInitiatorWaitAuthComplete:
      return "InitiatorWaitAuthComplete";
    case State::kWaitEncryption:
      return "WaitEncryption";
    case State::kFailed:
      return "Failed";
    default:
      break;
  }
  return "";
}

void LegacyPairingState::AttachInspect(inspect::Node& parent,
                                       std::string name) {
  inspect_node_ = parent.CreateChild(name);

  if (link_.is_alive()) {
    inspect_properties_.encryption_status = inspect_node_.CreateString(
        kInspectEncryptionStatusPropertyName,
        EncryptionStatusToString(link_->encryption_status()));
  }

  inspect_metrics_.central_lmp_transaction_collision.AttachInspect(
      inspect_node_, "central_lmp_transaction_collision");
  inspect_metrics_.peripheral_lmp_transaction_collision.AttachInspect(
      inspect_node_, "peripheral_lmp_transaction_collision");
  inspect_metrics_.central_different_transaction_collision.AttachInspect(
      inspect_node_, "central_different_transaction_collision");
  inspect_metrics_.peripheral_different_transaction_collision.AttachInspect(
      inspect_node_, "peripheral_different_transaction_collision");

  bredr_security_.AttachInspect(inspect_node_,
                                kInspectSecurityPropertiesPropertyName);
}
}  // namespace bt::gap
