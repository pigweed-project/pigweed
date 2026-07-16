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

#include "pw_bluetooth_sapphire/internal/host/sm/security_manager.h"

#include <pw_assert/check.h>

#include <cinttypes>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include "lib/fit/function.h"
#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/common/device_address.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/common/random.h"
#include "pw_bluetooth_sapphire/internal/host/common/uint128.h"
#include "pw_bluetooth_sapphire/internal/host/gap/gap.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/link_key.h"
#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_connection.h"
#include "pw_bluetooth_sapphire/internal/host/sm/error.h"
#include "pw_bluetooth_sapphire/internal/host/sm/packet.h"
#include "pw_bluetooth_sapphire/internal/host/sm/pairing_phase.h"
#include "pw_bluetooth_sapphire/internal/host/sm/phase_1.h"
#include "pw_bluetooth_sapphire/internal/host/sm/phase_2_legacy.h"
#include "pw_bluetooth_sapphire/internal/host/sm/phase_2_secure_connections.h"
#include "pw_bluetooth_sapphire/internal/host/sm/phase_3.h"
#include "pw_bluetooth_sapphire/internal/host/sm/security_request_phase.h"
#include "pw_bluetooth_sapphire/internal/host/sm/smp.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"
#include "pw_bluetooth_sapphire/internal/host/sm/util.h"
#include "pw_bluetooth_sapphire/internal/host/transport/error.h"

namespace bt::sm {

namespace {

using PairingToken = gap::Peer::PairingToken;

SecurityProperties FeaturesToProperties(const PairingFeatures& features) {
  return SecurityProperties(features.method == PairingMethod::kJustWorks
                                ? SecurityLevel::kEncrypted
                                : SecurityLevel::kAuthenticated,
                            features.encryption_key_size,
                            features.secure_connections);
}
}  // namespace

class SecurityManagerImpl final : public SecurityManager,
                                  public PairingPhase::Listener,
                                  public PairingChannel::Handler {
 public:
  ~SecurityManagerImpl() override;
  SecurityManagerImpl(hci::LowEnergyConnection::WeakPtr low_energy_link,
                      hci::BrEdrConnection::WeakPtr bredr_link,
                      l2cap::Channel::WeakPtr smp,
                      IOCapability io_capability,
                      Delegate::WeakPtr delegate,
                      BondableMode bondable_mode,
                      gap::LESecurityMode security_mode,
                      bool is_controller_remote_public_key_validation_supported,
                      pw::async::Dispatcher& dispatcher,
                      bt::gap::Peer::WeakPtr peer);
  // SecurityManager overrides:
  void UpgradeSecurity(SecurityLevel level, PairingCallback callback) override;
  void InitiateBrEdrCrossTransportKeyDerivation(
      CrossTransportKeyDerivationResultCallback callback) override;
  void Reset(IOCapability io_capability) override;
  void Abort(ErrorCode ecode) override;

 private:
  // Represents a pending request to update the security level.
  struct PendingRequest {
    PendingRequest(SecurityLevel level_in, PairingCallback callback_in);
    PendingRequest(PendingRequest&&) = default;
    PendingRequest& operator=(PendingRequest&&) = default;

    SecurityLevel level;
    PairingCallback callback;
  };

  // Pseudo-phase where we are waiting for BR/EDR pairing to complete.
  struct WaitForBrEdrPairing {};

  // Pseudo-phase that indicates that encryption is being started while no other
  // phase is in progress.
  struct StartingEncryption {};

  // Called when we receive a peer security request as initiator, will start
  // Phase 1.
  void OnSecurityRequest(AuthReqField auth_req);

  // Called when we receive a peer pairing request as responder, will start
  // Phase 1.
  void OnPairingRequest(const PairingRequestParams& req_params);

  // Pulls the next PendingRequest off |request_queue_| and starts a security
  // upgrade to that |level| by either sending a Pairing Request as initiator or
  // a Security Request as responder.
  void UpgradeSecurityInternal();

  // Creates the pairing phase responsible for sending the security upgrade
  // request to the peer (a PairingRequest if we are initiator, otherwise a
  // SecurityRequest). Returns fit::error(ErrorCode::
  // kAuthenticationRequirements) if the local IOCapabilities are insufficient
  // for SecurityLevel, otherwise returns fit::ok().
  [[nodiscard]] fit::result<ErrorCode> RequestSecurityUpgrade(
      SecurityLevel level);

  // Called when the feature exchange (Phase 1) completes and the relevant
  // features of both sides have been resolved into `features`. `preq` and
  // `pres` need to be retained for cryptographic calculations in Phase 2.
  // Causes a state transition from Phase 1 to Phase 2
  void OnFeatureExchange(PairingFeatures features,
                         PairingRequestParams preq,
                         PairingResponseParams pres);

  // Called when Phase 2 generates an encryption key, so the link can be
  // encrypted with it.
  void OnPhase2EncryptionKey(const UInt128& new_key);

  // Check if encryption using `current_ltk` will satisfy the current security
  // requirements.
  static bool CurrentLtkInsufficientlySecureForEncryption(
      std::optional<LTK> current_ltk,
      SecurityRequestPhase* security_request_phase,
      gap::LESecurityMode mode);

  // Called when the encryption state of the LE link changes.
  void OnEncryptionChange(hci::Result<bool> enabled_result);

  // Called when the link is encrypted at the end of pairing Phase 2.
  void EndPhase2();

  // Cleans up pairing state, updates the current security level, and notifies
  // parties that requested security of the link's updated security properties.
  void OnLowEnergyPairingComplete(PairingData data);

  // Derives LE LTK, notifies clients, and resets pairing state.
  void OnBrEdrPairingComplete(PairingData pairing_data);

  // After a call to UpgradeSecurity results in an increase of the link security
  // level (through pairing completion or SMP Security Requested encryption),
  // this method notifies all the callbacks associated with SecurityUpgrade
  // requests.
  void NotifySecurityCallbacks();

  // Assign the current security properties and notify the delegate of the
  // change.
  void SetSecurityProperties(const SecurityProperties& sec);

  // Directly assigns the current |ltk_| and the underlying |le_link_|'s link
  // key. This function does not initiate link layer encryption and can be
  // called during and outside of pairing.
  void OnNewLongTermKey(const LTK& ltk);

  // PairingPhase::Listener overrides:
  void OnPairingFailed(Error error) override;
  std::optional<IdentityInfo> OnIdentityRequest() override;
  void ConfirmPairing(ConfirmCallback confirm) override;
  void DisplayPasskey(uint32_t passkey,
                      Delegate::DisplayMethod method,
                      ConfirmCallback cb) override;
  void RequestPasskey(PasskeyResponseCallback respond) override;

  // PairingChannel::Handler overrides. SecurityManagerImpl is only the fallback
  // handler, meaning these methods are only called by PairingChannel when no
  // security upgrade is in progress:
  void OnRxBFrame(ByteBufferPtr sdu) override;
  void OnChannelClosed() override;

  // Starts the SMP timer. Stops and cancels any in-progress timers.
  void StartNewTimer();
  // Stops and resets the SMP Pairing Timer.
  void StopTimer();
  // Called when the pairing timer expires, forcing the pairing process to stop
  void OnPairingTimeout();

  // Returns a std::pair<InitiatorAddress, ResponderAddress>. Will assert if
  // called outside active pairing or before Phase 1 is complete.
  std::pair<DeviceAddress, DeviceAddress> LEPairingAddresses();

  // Puts the class into a non-pairing state.
  void ResetState();

  bool InPhase1() const {
    return std::holds_alternative<std::unique_ptr<Phase1>>(current_phase_);
  }

  // Returns true if the pairing state machine is currently in Phase 2 of
  // pairing.
  bool InPhase2() const {
    return std::holds_alternative<Phase2Legacy>(current_phase_) ||
           std::holds_alternative<Phase2SecureConnections>(current_phase_);
  }

  bool SecurityUpgradeInProgress() const {
    return !std::holds_alternative<std::monostate>(current_phase_);
  }

  // Validates that both SM and the link have stored LTKs, and that these values
  // match. Disconnects the link if it finds an issue. Should only be called
  // when an LTK is expected to exist.
  Result<> ValidateExistingLocalLtk();

  // Returns true only if all security conditions are met for BR/EDR CTKD.
  bool IsBrEdrCrossTransportKeyDerivationAllowed();

  std::optional<sm::LTK> GetExistingLtkFromPeerCache();

  // The role of the local device in pairing.
  // LE roles are fixed for the lifetime of a connection, but BR/EDR roles can
  // be changed after a connection is established, so we cannot cache it during
  // construction.
  Role role() {
    pw::bluetooth::emboss::ConnectionRole conn_role =
        pw::bluetooth::emboss::ConnectionRole::CENTRAL;
    if (low_energy_link_.is_alive()) {
      conn_role = low_energy_link_->role();
    } else if (bredr_link_.is_alive()) {
      conn_role = bredr_link_->role();
    } else {
      PW_CRASH("no active link");
    }
    return conn_role == pw::bluetooth::emboss::ConnectionRole::CENTRAL
               ? Role::kInitiator
               : Role::kResponder;
  }

  pw::async::Dispatcher& pw_dispatcher_;

  // The ID that will be assigned to the next pairing operation.
  PairingProcedureId next_pairing_id_;

  // The higher-level class acting as a delegate for operations outside of SMP.
  Delegate::WeakPtr delegate_;

  // Data for the currently registered LE-U link, if any.
  hci::LowEnergyConnection::WeakPtr low_energy_link_;

  hci::BrEdrConnection::WeakPtr bredr_link_;

  // Whether the controller performs remote public key validation for BR/EDR
  // keys.
  bool is_controller_remote_public_key_validation_supported_ = false;

  // The IO capabilities of the device
  IOCapability low_energy_io_cap_;

  // The current LTK assigned to this connection. This can be assigned directly
  // by calling AssignLongTermKey() or as a result of a pairing procedure.
  std::optional<LTK> ltk_;

  // If a pairing is in progress and Phase 1 (feature exchange) has completed,
  // this will store the result of that feature exchange. Otherwise, this will
  // be std::nullopt.
  std::optional<PairingFeatures> features_;

  // The pending security requests added via UpgradeSecurity().
  std::queue<PendingRequest> request_queue_;

  CrossTransportKeyDerivationResultCallback
      bredr_cross_transport_key_derivation_callback_ = nullptr;

  // Fixed SMP Channel used to send/receive packets
  std::unique_ptr<PairingChannel> sm_chan_;

  SmartTask timeout_task_{pw_dispatcher_};

  // Set to a PairingToken when the current phase is not monostate (always null
  // in monostate).
  std::optional<PairingToken> pairing_token_;
  bt::gap::Peer::WeakPtr peer_;

  // The presence of a particular phase in this variant indicates that a
  // security upgrade is in progress at the stored phase. No security upgrade is
  // in progress if std::monostate is present.
  std::variant<std::monostate,
               WaitForBrEdrPairing,
               StartingEncryption,
               SecurityRequestPhase,
               std::unique_ptr<Phase1>,
               Phase2Legacy,
               Phase2SecureConnections,
               Phase3>
      current_phase_;

  WeakSelf<SecurityManagerImpl> weak_self_;
  WeakSelf<PairingPhase::Listener> weak_listener_;
  WeakSelf<PairingChannel::Handler> weak_handler_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(SecurityManagerImpl);
};

SecurityManagerImpl::PendingRequest::PendingRequest(SecurityLevel level_in,
                                                    PairingCallback callback_in)
    : level(level_in), callback(std::move(callback_in)) {}

SecurityManagerImpl::~SecurityManagerImpl() {
  if (low_energy_link_.is_alive()) {
    low_energy_link_->set_encryption_change_callback({});
  }
}

SecurityManagerImpl::SecurityManagerImpl(
    hci::LowEnergyConnection::WeakPtr low_energy_link,
    hci::BrEdrConnection::WeakPtr bredr_link,
    l2cap::Channel::WeakPtr smp,
    IOCapability io_capability,
    Delegate::WeakPtr delegate,
    BondableMode bondable_mode,
    gap::LESecurityMode security_mode,
    bool is_controller_remote_public_key_validation_supported,
    pw::async::Dispatcher& dispatcher,
    bt::gap::Peer::WeakPtr peer)
    : SecurityManager(bondable_mode, security_mode),
      pw_dispatcher_(dispatcher),
      next_pairing_id_(0),
      delegate_(std::move(delegate)),
      low_energy_link_(std::move(low_energy_link)),
      bredr_link_(std::move(bredr_link)),
      is_controller_remote_public_key_validation_supported_(
          is_controller_remote_public_key_validation_supported),
      low_energy_io_cap_(io_capability),
      sm_chan_(std::make_unique<PairingChannel>(
          smp, fit::bind_member<&SecurityManagerImpl::StartNewTimer>(this))),
      peer_(std::move(peer)),
      weak_self_(this),
      weak_listener_(this),
      weak_handler_(this) {
  PW_CHECK(delegate_.is_alive());
  PW_CHECK(smp.is_alive());

  sm_chan_->SetChannelHandler(weak_handler_.GetWeakPtr());

  timeout_task_.set_function(
      [this](pw::async::Context /*ctx*/, pw::Status status) {
        if (status.ok()) {
          OnPairingTimeout();
        }
      });

  if (smp->id() == l2cap::kLESMPChannelId) {
    PW_CHECK(low_energy_link_.is_alive());
    PW_CHECK(low_energy_link_->handle() == smp->link_handle());

    // Set up HCI encryption event.
    low_energy_link_->set_encryption_change_callback(
        fit::bind_member<&SecurityManagerImpl::OnEncryptionChange>(this));

    // Obtain existing pairing data, if any.
    std::optional<sm::LTK> ltk = GetExistingLtkFromPeerCache();
    if (ltk) {
      bt_log(INFO,
             "sm",
             "starting encryption with existing LTK (peer: %s, handle: %#.4x)",
             bt_str(peer_->identifier()),
             low_energy_link_->handle());

      // Sets LTK in low_energy_link_
      OnNewLongTermKey(*ltk);

      // The initiatior starts encryption when there is an LTK.
      if (low_energy_link_->role() ==
          pw::bluetooth::emboss::ConnectionRole::CENTRAL) {
        current_phase_ = StartingEncryption();
        PW_CHECK(low_energy_link_->StartEncryption());
      }
    }
  }
}

void SecurityManagerImpl::OnSecurityRequest(AuthReqField auth_req) {
  if (bredr_link_.is_alive()) {
    sm_chan_->SendMessageNoTimerReset(kPairingFailed,
                                      ErrorCode::kCommandNotSupported);
    return;
  }

  PW_CHECK(!SecurityUpgradeInProgress() ||
           std::holds_alternative<WaitForBrEdrPairing>(current_phase_) ||
           std::holds_alternative<StartingEncryption>(current_phase_));

  if (role() != Role::kInitiator) {
    bt_log(
        INFO,
        "sm",
        "Received spurious Security Request while not acting as SM initiator");
    sm_chan_->SendMessageNoTimerReset(kPairingFailed,
                                      ErrorCode::kCommandNotSupported);
    return;
  }

  SecurityLevel requested_level;
  // inclusive-language: ignore
  if (auth_req & AuthReq::kMITM) {
    requested_level = SecurityLevel::kAuthenticated;
  } else {
    requested_level = SecurityLevel::kEncrypted;
  }

  // "If pairing has been initiated by the local device on the BR/EDR transport,
  // and a pairing request is received from the same remote device on the LE
  // transport, the LE pairing shall be rejected with SMP error code BR/EDR
  // Pairing in Progress if both sides support LE Secure Connections." (v6.0,
  // Vol. 3, Part C, Sec. 14.2)
  bool peer_supports_secure_connections = auth_req & kSC;
  bool bredr_pairing_in_progress =
      peer_->bredr() && peer_->bredr()->is_pairing();
  if (peer_supports_secure_connections && bredr_pairing_in_progress) {
    bt_log(INFO,
           "sm",
           "rejecting Security Request because BREDR pairing in progress");
    sm_chan_->SendMessageNoTimerReset(kPairingFailed,
                                      ErrorCode::kBREDRPairingInProgress);
    return;
  }

  // If we already have a LTK and its security properties satisfy the request,
  // then we start link layer encryption (which will either encrypt the link or
  // perform a key refresh). See Vol 3, Part H, Figure 2.7 for the algorithm.
  if (ltk_ && (ltk_->security().level() >= requested_level) &&
      (!(auth_req & AuthReq::kSC) || ltk_->security().secure_connections())) {
    if (bt_is_error(
            ValidateExistingLocalLtk(),
            ERROR,
            "sm",
            "disconnecting link as it cannot be encrypted with LTK status")) {
      return;
    }
    current_phase_ = StartingEncryption();
    PW_CHECK(low_energy_link_->StartEncryption());
    return;
  }

  // V5.1 Vol. 3 Part H Section 3.4: "Upon [...] reception of the Security
  // Request command, the Security Manager Timer shall be [...] restarted."
  StartNewTimer();
  if (fit::result result = RequestSecurityUpgrade(requested_level);
      result.is_error()) {
    // Per v5.3 Vol. 3 Part H 2.4.6, "When a Central receives a Security Request
    // command it may
    // [...] reject the request.", which we do here as we know we are unable to
    // fulfill it.
    sm_chan_->SendMessageNoTimerReset(kPairingFailed, result.error_value());
    // If we fail to start pairing, we need to stop the timer.
    StopTimer();
  }
}

void SecurityManagerImpl::UpgradeSecurity(SecurityLevel level,
                                          PairingCallback callback) {
  PW_CHECK(!bredr_link_.is_alive());

  if (SecurityUpgradeInProgress()) {
    bt_log(TRACE,
           "sm",
           "LE security upgrade in progress; request for %s security queued",
           LevelToString(level));
    request_queue_.emplace(level, std::move(callback));
    return;
  }

  if (level <= security().level()) {
    callback(fit::ok(), security());
    return;
  }

  // Secure Connections only mode only permits Secure Connections authenticated
  // pairing with a 128- bit encryption key, so we force all security upgrade
  // requests to that level.
  if (security_mode() == gap::LESecurityMode::SecureConnectionsOnly) {
    level = SecurityLevel::kSecureAuthenticated;
  }

  // |request_queue| must be empty if there is no active security upgrade
  // request, which is equivalent to being in idle phase with no pending
  // security request.
  PW_CHECK(request_queue_.empty());
  request_queue_.emplace(level, std::move(callback));
  UpgradeSecurityInternal();
}

void SecurityManagerImpl::OnPairingRequest(
    const PairingRequestParams& req_params) {
  // Only the initiator may send the Pairing Request (V5.0 Vol. 3 Part H 3.5.1).
  if (role() != Role::kResponder) {
    bt_log(INFO, "sm", "rejecting \"Pairing Request\" as initiator");
    sm_chan_->SendMessageNoTimerReset(kPairingFailed,
                                      ErrorCode::kCommandNotSupported);
    return;
  }

  // "If pairing has been initiated by the local device on the BR/EDR transport,
  // and a pairing request is received from the same remote device on the LE
  // transport, the LE pairing shall be rejected with SMP error code BR/EDR
  // Pairing in Progress if both sides support LE Secure Connections." (v6.0,
  // Vol. 3, Part C, Sec. 14.2)
  bool peer_supports_secure_connections = req_params.auth_req & kSC;
  bool le_secure_connections =
      low_energy_link_.is_alive() && peer_supports_secure_connections;
  bool bredr_pairing_in_progress =
      peer_->bredr() && peer_->bredr()->is_pairing();
  if (le_secure_connections && bredr_pairing_in_progress) {
    bt_log(INFO,
           "sm",
           "LE: rejecting Pairing Request because BREDR pairing in progress");
    sm_chan_->SendMessageNoTimerReset(kPairingFailed,
                                      ErrorCode::kBREDRPairingInProgress);
    return;
  }

  // We only require authentication as Responder if there is a pending
  // Security Request for it.
  SecurityRequestPhase* security_req_phase =
      std::get_if<SecurityRequestPhase>(&current_phase_);
  auto required_level = security_req_phase
                            ? security_req_phase->pending_security_request()
                            : SecurityLevel::kEncrypted;

  // Secure Connections only mode only permits Secure Connections authenticated
  // pairing with a 128- bit encryption key, so we force all security upgrade
  // requests to that level.
  if (security_mode() == gap::LESecurityMode::SecureConnectionsOnly) {
    required_level = SecurityLevel::kSecureAuthenticated;
  }

  if (required_level >= SecurityLevel::kAuthenticated &&
      low_energy_io_cap_ == IOCapability::kNoInputNoOutput) {
    bt_log(WARN,
           "sm",
           "cannot fulfill authenticated security request as IOCapabilities "
           "are NoInputNoOutput");
    sm_chan_->SendMessageNoTimerReset(kPairingFailed,
                                      ErrorCode::kAuthenticationRequirements);
    return;
  }

  if (bredr_link_.is_alive()) {
    if (!IsBrEdrCrossTransportKeyDerivationAllowed()) {
      bt_log(INFO,
             "sm",
             "BR/EDR: rejecting \"Pairing Request\" because CTKD not allowed");
      sm_chan_->SendMessageNoTimerReset(
          kPairingFailed, ErrorCode::kCrossTransportKeyDerivationNotAllowed);
      return;
    }
  }

  if (!pairing_token_) {
    if (low_energy_link_.is_alive()) {
      pairing_token_ = peer_->MutLe().RegisterPairing();
    } else {
      pairing_token_ = peer_->MutBrEdr().RegisterPairing();
    }
  }

  // V5.1 Vol. 3 Part H Section 3.4: "Upon [...] reception of the Pairing
  // Request command, the Security Manager Timer shall be reset and started."
  StartNewTimer();

  current_phase_ = Phase1::CreatePhase1Responder(
      sm_chan_->GetWeakPtr(),
      weak_listener_.GetWeakPtr(),
      req_params,
      low_energy_io_cap_,
      bondable_mode(),
      required_level,
      fit::bind_member<&SecurityManagerImpl::OnFeatureExchange>(this));
  std::get<std::unique_ptr<Phase1>>(current_phase_)->Start();
}

void SecurityManagerImpl::UpgradeSecurityInternal() {
  PW_CHECK(!bredr_link_.is_alive());
  PW_CHECK(
      !SecurityUpgradeInProgress(),
      "cannot upgrade security while security upgrade already in progress!");
  PW_CHECK(!request_queue_.empty());

  // "If a BR/EDR/LE device supports LE Secure Connections, then it shall
  // initiate pairing on only one transport at a time to the same remote
  // device." (v6.0, Vol 3, Part C, Sec. 14.2)
  if (peer_->bredr() && peer_->bredr()->is_pairing()) {
    bt_log(DEBUG,
           "sm",
           "Delaying security upgrade until BR/EDR pairing completes");
    current_phase_ = WaitForBrEdrPairing();
    peer_->MutBrEdr().add_pairing_completion_callback(
        [self = weak_self_.GetWeakPtr()]() {
          if (!self.is_alive() ||
              !std::get_if<WaitForBrEdrPairing>(&self->current_phase_)) {
            return;
          }
          self->ResetState();
          if (self->request_queue_.empty()) {
            return;
          }
          self->UpgradeSecurityInternal();
        });
    return;
  }

  const PendingRequest& next_req = request_queue_.front();

  // BR/EDR cross-transport key derivation could have created a new LE LTK that
  // meets the requirements of the next request. Only central can start
  // encryption, so we skip this and request a security upgrade as peripheral.
  if (low_energy_link_->role() ==
      pw::bluetooth::emboss::ConnectionRole::CENTRAL) {
    std::optional<sm::LTK> ltk = GetExistingLtkFromPeerCache();
    // If the new LTK isn't going to satisfy the request anyway, we can ignore
    // it and start pairing.
    if (ltk && ltk != ltk_ && ltk->security().level() >= next_req.level) {
      bt_log(INFO,
             "sm",
             "starting encryption with LTK from PeerCache (peer: %s, handle: "
             "%#.4x)",
             bt_str(peer_->identifier()),
             low_energy_link_->handle());

      OnNewLongTermKey(*ltk);

      current_phase_ = StartingEncryption();
      PW_CHECK(low_energy_link_->StartEncryption());
      return;
    }
  }

  if (fit::result result = RequestSecurityUpgrade(next_req.level);
      result.is_error()) {
    next_req.callback(ToResult(result.error_value()), security());
    request_queue_.pop();
    if (!request_queue_.empty()) {
      UpgradeSecurityInternal();
    }
  }
}

fit::result<ErrorCode> SecurityManagerImpl::RequestSecurityUpgrade(
    SecurityLevel level) {
  PW_CHECK(!bredr_link_.is_alive());
  if (level >= SecurityLevel::kAuthenticated &&
      low_energy_io_cap_ == IOCapability::kNoInputNoOutput) {
    bt_log(WARN,
           "sm",
           "cannot fulfill authenticated security request as IOCapabilities "
           "are NoInputNoOutput");
    return fit::error(ErrorCode::kAuthenticationRequirements);
  }

  if (!pairing_token_) {
    pairing_token_ = peer_->MutLe().RegisterPairing();
  }

  if (role() == Role::kInitiator) {
    current_phase_ = Phase1::CreatePhase1Initiator(
        sm_chan_->GetWeakPtr(),
        weak_listener_.GetWeakPtr(),
        low_energy_io_cap_,
        bondable_mode(),
        level,
        fit::bind_member<&SecurityManagerImpl::OnFeatureExchange>(this));
    std::get<std::unique_ptr<Phase1>>(current_phase_)->Start();
  } else {
    current_phase_.emplace<SecurityRequestPhase>(
        sm_chan_->GetWeakPtr(),
        weak_listener_.GetWeakPtr(),
        level,
        bondable_mode(),
        fit::bind_member<&SecurityManagerImpl::OnPairingRequest>(this));
    std::get<SecurityRequestPhase>(current_phase_).Start();
  }
  return fit::ok();
}

void SecurityManagerImpl::OnFeatureExchange(PairingFeatures features,
                                            PairingRequestParams preq,
                                            PairingResponseParams pres) {
  PW_CHECK(std::holds_alternative<std::unique_ptr<Phase1>>(current_phase_));
  bt_log(DEBUG, "sm", "SMP feature exchange complete");
  next_pairing_id_++;
  features_ = features;

  auto self = weak_listener_.GetWeakPtr();
  if (bredr_link_.is_alive()) {
    if (!features_->generate_ct_key.has_value()) {
      Abort(ErrorCode::kCrossTransportKeyDerivationNotAllowed);
      return;
    }

    // If there are no keys to distribute, skip Phase3
    if (!HasKeysToDistribute(*features_, /*is_bredr=*/true)) {
      OnBrEdrPairingComplete(PairingData());
      return;
    }

    // We checked the ltk before Phase1.
    PW_CHECK(bredr_link_->ltk().has_value());
    // Phase3 needs to know the security properties.
    SecurityProperties bredr_security_properties(
        bredr_link_->ltk_type().value());

    current_phase_.emplace<Phase3>(
        sm_chan_->GetWeakPtr(),
        self,
        role(),
        *features_,
        bredr_security_properties,
        fit::bind_member<&SecurityManagerImpl::OnBrEdrPairingComplete>(this));
    std::get<Phase3>(current_phase_).Start();
  } else if (features.secure_connections) {
    const auto [initiator_addr, responder_addr] = LEPairingAddresses();
    current_phase_.emplace<Phase2SecureConnections>(
        sm_chan_->GetWeakPtr(),
        self,
        role(),
        features,
        preq,
        pres,
        initiator_addr,
        responder_addr,
        fit::bind_member<&SecurityManagerImpl::OnPhase2EncryptionKey>(this));
    std::get<Phase2SecureConnections>(current_phase_).Start();
  } else {
    const auto [initiator_addr, responder_addr] = LEPairingAddresses();
    auto preq_pdu = util::NewPdu(sizeof(PairingRequestParams)),
         pres_pdu = util::NewPdu(sizeof(PairingResponseParams));
    PacketWriter preq_writer(kPairingRequest, preq_pdu.get()),
        pres_writer(kPairingResponse, pres_pdu.get());
    *preq_writer.mutable_payload<PairingRequestParams>() = preq;
    *pres_writer.mutable_payload<PairingRequestParams>() = pres;
    current_phase_.emplace<Phase2Legacy>(
        sm_chan_->GetWeakPtr(),
        self,
        role(),
        features,
        *preq_pdu,
        *pres_pdu,
        initiator_addr,
        responder_addr,
        fit::bind_member<&SecurityManagerImpl::OnPhase2EncryptionKey>(this));
    std::get<Phase2Legacy>(current_phase_).Start();
  }
}

void SecurityManagerImpl::OnPhase2EncryptionKey(const UInt128& new_key) {
  PW_CHECK(!bredr_link_.is_alive());
  PW_CHECK(low_energy_link_.is_alive());
  PW_CHECK(features_);
  PW_CHECK(InPhase2());
  // EDiv and Rand values are 0 for Phase 2 keys generated by Legacy or Secure
  // Connections (Vol 3, Part H, 2.4.4 / 2.4.4.1). Secure Connections generates
  // an LTK, while Legacy generates an STK.
  auto new_link_key = hci_spec::LinkKey(new_key, 0, 0);

  if (features_->secure_connections) {
    OnNewLongTermKey(LTK(FeaturesToProperties(*features_), new_link_key));
  } else {
    // `set_le_ltk` sets the encryption key of the LE link (which is the STK for
    // Legacy), not the long-term key that results from pairing (which is
    // generated in Phase 3 for Legacy).
    low_energy_link_->set_ltk(new_link_key);
  }
  // If we're the initiator, we encrypt the link. If we're the responder, we
  // wait for the initiator to encrypt the link with the new key.|le_link_| will
  // respond to the HCI "LTK request" event with the `new_link_key` assigned
  // above, which should trigger OnEncryptionChange.
  if (role() == Role::kInitiator) {
    if (!low_energy_link_->StartEncryption()) {
      bt_log(ERROR, "sm", "failed to start encryption");
      Abort(ErrorCode::kUnspecifiedReason);
    }
  }
}

bool SecurityManagerImpl::CurrentLtkInsufficientlySecureForEncryption(
    std::optional<LTK> current_ltk,
    SecurityRequestPhase* security_request_phase,
    gap::LESecurityMode mode) {
  SecurityLevel current_ltk_sec = current_ltk ? current_ltk->security().level()
                                              : SecurityLevel::kNoSecurity;
  return (security_request_phase &&
          security_request_phase->pending_security_request() >
              current_ltk_sec) ||
         (mode == gap::LESecurityMode::SecureConnectionsOnly &&
          current_ltk_sec != SecurityLevel::kSecureAuthenticated);
}

void SecurityManagerImpl::OnEncryptionChange(hci::Result<bool> enabled_result) {
  PW_CHECK(!bredr_link_.is_alive());

  // First notify the delegate in case of failure.
  if (bt_is_error(
          enabled_result, ERROR, "sm", "link layer authentication failed")) {
    PW_CHECK(delegate_.is_alive());
    delegate_->OnAuthenticationFailure(
        fit::error(enabled_result.error_value()));
  }

  if (enabled_result.is_error() || !enabled_result.value()) {
    bt_log(WARN,
           "sm",
           "encryption of link (handle: %#.4x) %s%s!",
           low_energy_link_->handle(),
           enabled_result.is_error()
               ? bt_lib_cpp_string::StringPrintf("failed with %s",
                                                 bt_str(enabled_result))
                     .c_str()
               : "disabled",
           SecurityUpgradeInProgress() ? "" : " during security upgrade");
    SetSecurityProperties(sm::SecurityProperties());
    if (SecurityUpgradeInProgress()) {
      Abort(ErrorCode::kUnspecifiedReason);
    }
    return;
  }

  SecurityRequestPhase* security_request_phase =
      std::get_if<SecurityRequestPhase>(&current_phase_);
  if (CurrentLtkInsufficientlySecureForEncryption(
          ltk_, security_request_phase, security_mode())) {
    bt_log(WARN,
           "sm",
           "peer encrypted link with insufficiently secure key, disconnecting");
    delegate_->OnAuthenticationFailure(
        ToResult(HostError::kInsufficientSecurity));
    sm_chan_->SignalLinkError();
    return;
  }

  bool wait_for_bredr_pairing_phase =
      std::holds_alternative<WaitForBrEdrPairing>(current_phase_);
  bool starting_encryption_phase =
      std::holds_alternative<StartingEncryption>(current_phase_);

  if (!SecurityUpgradeInProgress() || security_request_phase ||
      starting_encryption_phase || wait_for_bredr_pairing_phase) {
    bt_log(DEBUG, "sm", "encryption enabled while not pairing");
    if (bt_is_error(
            ValidateExistingLocalLtk(),
            ERROR,
            "sm",
            "disconnecting link as it cannot be encrypted with LTK status")) {
      return;
    }
    // If encryption is enabled while not pairing, we update the security
    // properties to those of `ltk_`. Otherwise, we let the EndPhase2 pairing
    // function determine the security properties.
    SetSecurityProperties(ltk_->security());

    if (security_request_phase) {
      PW_CHECK(role() == Role::kResponder);
      PW_CHECK(!request_queue_.empty());
      // The central satisfied our Security Request by LL-encrypting with the
      // bonded LTK. Clear the SecurityRequestPhase and stop the SMP timer so
      // that NotifySecurityCallbacks can re-enter UpgradeSecurityInternal for
      // any higher-level queued request and so that the 30 s timer does not
      // spuriously tear down the now-encrypted link.
      ResetState();
    }
    if (starting_encryption_phase) {
      ResetState();
    }
    NotifySecurityCallbacks();
    return;
  }

  if (InPhase2()) {
    bt_log(DEBUG, "sm", "link encrypted with phase 2 generated key");
    EndPhase2();
  }
}

void SecurityManagerImpl::EndPhase2() {
  PW_CHECK(features_.has_value());
  PW_CHECK(InPhase2());
  PW_CHECK(!bredr_link_.is_alive());

  SetSecurityProperties(FeaturesToProperties(*features_));
  // If there are no keys to distribute, don't bother creating Phase 3
  if (!HasKeysToDistribute(*features_, bredr_link_.is_alive())) {
    OnLowEnergyPairingComplete(PairingData());
    return;
  }
  auto self = weak_listener_.GetWeakPtr();
  current_phase_.emplace<Phase3>(
      sm_chan_->GetWeakPtr(),
      self,
      role(),
      *features_,
      security(),
      fit::bind_member<&SecurityManagerImpl::OnLowEnergyPairingComplete>(this));
  std::get<Phase3>(current_phase_).Start();
}

void SecurityManagerImpl::OnLowEnergyPairingComplete(PairingData pairing_data) {
  // We must either be in Phase3 or Phase 2 with no keys to distribute if
  // pairing has completed.
  if (!std::holds_alternative<Phase3>(current_phase_)) {
    PW_CHECK(InPhase2());
    PW_CHECK(!HasKeysToDistribute(*features_, /*is_bredr=*/false));
  }
  PW_CHECK(delegate_.is_alive());
  PW_CHECK(features_.has_value());
  bt_log(DEBUG, "sm", "LE pairing complete");
  delegate_->OnPairingComplete(fit::ok());
  // In Secure Connections, the LTK will be generated in Phase 2, not exchanged
  // in Phase 3, so we want to ensure that it is still put in the pairing_data.
  if (features_->secure_connections) {
    PW_CHECK(ltk_.has_value());
    pairing_data.peer_ltk = pairing_data.local_ltk = ltk_;
  } else {
    // The SM-internal LTK is used to validate future encryption events on the
    // existing link. Encryption with LTKs generated by LE legacy pairing uses
    // the key received by the link-layer central - so as initiator, this is the
    // peer key, and as responder, this is the local key.
    const std::optional<LTK>& new_ltk = role() == Role::kInitiator
                                            ? pairing_data.peer_ltk
                                            : pairing_data.local_ltk;
    if (new_ltk.has_value()) {
      OnNewLongTermKey(*new_ltk);
    }
  }

  if (features_->generate_ct_key.has_value()) {
    // If we are generating the CT key, we must be using secure connections, and
    // as such the peer and local LTKs will be equivalent.
    PW_CHECK(features_->secure_connections);
    PW_CHECK(pairing_data.peer_ltk == pairing_data.local_ltk);
    std::optional<UInt128> ct_key_value = util::LeLtkToBrEdrLinkKey(
        ltk_->key().value(), features_->generate_ct_key.value());
    if (ct_key_value) {
      pairing_data.cross_transport_key =
          sm::LTK(ltk_->security(), hci_spec::LinkKey(*ct_key_value, 0, 0));
    } else {
      bt_log(WARN, "sm", "failed to generate cross-transport key");
    }
  }

  if (features_->will_bond) {
    std::optional<sm::LTK> ltk;
    if (pairing_data.peer_ltk) {
      ltk = pairing_data.peer_ltk;
    } else {
      ltk = pairing_data.local_ltk;
    }

    if (ltk.has_value()) {
      bt_log(
          INFO,
          "sm",
          "new %s pairing data: [%s%s%s%s%s%s] (peer: %s)",
          ltk->security().secure_connections() ? "secure connections"
                                               : "legacy",
          pairing_data.peer_ltk ? "peer_ltk " : "",
          pairing_data.local_ltk ? "local_ltk " : "",
          pairing_data.irk ? "irk " : "",
          pairing_data.cross_transport_key ? "ct_key " : "",
          pairing_data.identity_address
              ? bt_lib_cpp_string::StringPrintf(
                    "(identity: %s) ", bt_str(*pairing_data.identity_address))
                    .c_str()
              : "",
          pairing_data.csrk ? "csrk " : "",
          bt_str(peer_->identifier()));

      if (!peer_->MutLe().StoreBond(pairing_data)) {
        bt_log(ERROR,
               "sm",
               "failed to cache bonding data (id: %s)",
               bt_str(peer_->identifier()));
      }
    } else {
      // Consider the pairing temporary if no link key was received. This
      // means we'll remain encrypted with the STK without creating a bond and
      // reinitiate pairing when we reconnect in the future.
      bt_log(INFO,
             "sm",
             "temporarily paired with peer (peer: %s)",
             bt_str(peer_->identifier()));
    }

  } else {
    bt_log(INFO,
           "gap-le",
           " %s pairing complete in non-bondable mode with [%s%s%s%s%s]",
           features_->secure_connections ? "secure connections" : "legacy",
           pairing_data.peer_ltk ? "peer_ltk " : "",
           pairing_data.local_ltk ? "local_ltk " : "",
           pairing_data.irk ? "irk " : "",
           pairing_data.identity_address
               ? bt_lib_cpp_string::StringPrintf(
                     "(identity: %s) ", bt_str(*pairing_data.identity_address))
                     .c_str()
               : "",
           pairing_data.csrk ? "csrk " : "");
  }
  // So we can pair again if need be.
  ResetState();

  NotifySecurityCallbacks();
}

void SecurityManagerImpl::OnBrEdrPairingComplete(PairingData pairing_data) {
  PW_CHECK(bredr_link_.is_alive());
  // We must either be in Phase3 or Phase1 with no keys to distribute if pairing
  // has completed.
  if (!std::holds_alternative<Phase3>(current_phase_)) {
    PW_CHECK(InPhase1());
    PW_CHECK(!HasKeysToDistribute(*features_, /*is_bredr=*/true));
  }
  PW_CHECK(features_.has_value());
  PW_CHECK(features_->generate_ct_key.has_value());
  PW_CHECK(delegate_.is_alive());

  bt_log(INFO, "sm", "BR/EDR cross-transport key derivation complete");
  delegate_->OnPairingComplete(fit::ok());

  std::optional<UInt128> ct_key_value = util::BrEdrLinkKeyToLeLtk(
      bredr_link_->ltk()->value(), features_->generate_ct_key.value());
  if (ct_key_value) {
    // The LE LTK will have the same security properties as the BR/EDR key.
    SecurityProperties bredr_properties(bredr_link_->ltk_type().value());
    sm::LTK le_ltk =
        sm::LTK(bredr_properties, hci_spec::LinkKey(*ct_key_value, 0, 0));
    pairing_data.local_ltk = le_ltk;
    pairing_data.peer_ltk = le_ltk;
  } else {
    bt_log(ERROR, "sm", "BR/EDR CTKD key generation failed");
    if (bredr_cross_transport_key_derivation_callback_) {
      bredr_cross_transport_key_derivation_callback_(
          ToResult(HostError::kFailed));
    }
    ResetState();
    return;
  }

  if (!peer_->MutLe().StoreBond(pairing_data)) {
    bt_log(ERROR,
           "sm",
           "failed to cache bonding data (id: %s)",
           bt_str(peer_->identifier()));
  }

  if (bredr_cross_transport_key_derivation_callback_) {
    bredr_cross_transport_key_derivation_callback_(fit::ok());
  }

  ResetState();
}

void SecurityManagerImpl::NotifySecurityCallbacks() {
  // Separate out the requests that are satisfied by the current security level
  // from those that require a higher level. We'll retry pairing for the latter.
  std::queue<PendingRequest> satisfied;
  std::queue<PendingRequest> unsatisfied;
  while (!request_queue_.empty()) {
    auto& request = request_queue_.front();
    if (request.level <= security().level()) {
      satisfied.push(std::move(request));
    } else {
      unsatisfied.push(std::move(request));
    }
    request_queue_.pop();
  }

  request_queue_ = std::move(unsatisfied);

  // Notify the satisfied requests with success.
  while (!satisfied.empty()) {
    satisfied.front().callback(fit::ok(), security());
    satisfied.pop();
  }

  if (!request_queue_.empty()) {
    UpgradeSecurityInternal();
  }
}

void SecurityManagerImpl::InitiateBrEdrCrossTransportKeyDerivation(
    CrossTransportKeyDerivationResultCallback callback) {
  PW_CHECK(bredr_link_.is_alive());
  PW_CHECK(role() == Role::kInitiator);

  if (bredr_cross_transport_key_derivation_callback_ ||
      SecurityUpgradeInProgress()) {
    callback(ToResult(HostError::kInProgress));
    return;
  }

  if (!IsBrEdrCrossTransportKeyDerivationAllowed()) {
    callback(ToResult(HostError::kInsufficientSecurity));
    return;
  }

  bredr_cross_transport_key_derivation_callback_ = std::move(callback);

  if (!pairing_token_) {
    pairing_token_ = peer_->MutBrEdr().RegisterPairing();
  }
  current_phase_ = Phase1::CreatePhase1Initiator(
      sm_chan_->GetWeakPtr(),
      weak_listener_.GetWeakPtr(),
      IOCapability::kDisplayOnly,  // arbitrary
      BondableMode::Bondable,
      SecurityLevel::kEncrypted,  // arbitrary
      fit::bind_member<&SecurityManagerImpl::OnFeatureExchange>(this));
  std::get<std::unique_ptr<Phase1>>(current_phase_)->Start();
}

void SecurityManagerImpl::Reset(IOCapability io_capability) {
  Abort(ErrorCode::kUnspecifiedReason);
  low_energy_io_cap_ = io_capability;
  ResetState();
}

void SecurityManagerImpl::ResetState() {
  StopTimer();
  features_.reset();
  sm_chan_->SetChannelHandler(weak_handler_.GetWeakPtr());
  pairing_token_.reset();
  current_phase_ = std::monostate{};
}

void SecurityManagerImpl::SetSecurityProperties(const SecurityProperties& sec) {
  if (sec != security()) {
    bt_log(DEBUG,
           "sm",
           "security properties changed - handle: %#.4x, new: %s, old: %s",
           low_energy_link_->handle(),
           bt_str(sec),
           bt_str(security()));
    set_security(sec);
    delegate_->OnNewSecurityProperties(security());
  }
}

void SecurityManagerImpl::Abort(ErrorCode ecode) {
  std::visit(
      [=](auto& arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<std::unique_ptr<Phase1>, T>) {
          arg->Abort(ecode);
        } else if constexpr (std::is_base_of_v<PairingPhase, T>) {
          arg.Abort(ecode);
        } else {
          bt_log(DEBUG,
                 "sm",
                 "Attempted to abort security upgrade while not in progress");
        }
      },
      current_phase_);
  // "Abort" should trigger OnPairingFailed.
}

std::optional<IdentityInfo> SecurityManagerImpl::OnIdentityRequest() {
  // This is called by the bearer to determine if we have local identity
  // information to distribute.
  PW_CHECK(delegate_.is_alive());
  return delegate_->OnIdentityInformationRequest();
}

void SecurityManagerImpl::ConfirmPairing(ConfirmCallback confirm) {
  PW_CHECK(!bredr_link_.is_alive());
  PW_CHECK(delegate_.is_alive());
  delegate_->ConfirmPairing([id = next_pairing_id_,
                             self = weak_self_.GetWeakPtr(),
                             cb = std::move(confirm)](bool is_confirmed) {
    if (!self.is_alive() || self->next_pairing_id_ != id) {
      bt_log(TRACE,
             "sm",
             "ignoring user confirmation for expired pairing: id = %" PRIu64,
             id);
      return;
    }
    cb(is_confirmed);
  });
}

void SecurityManagerImpl::DisplayPasskey(uint32_t passkey,
                                         Delegate::DisplayMethod method,
                                         ConfirmCallback confirm) {
  PW_CHECK(!bredr_link_.is_alive());
  PW_CHECK(delegate_.is_alive());
  delegate_->DisplayPasskey(
      passkey,
      method,
      [id = next_pairing_id_,
       self = weak_self_.GetWeakPtr(),
       method,
       cb = std::move(confirm)](bool is_confirmed) {
        if (!self.is_alive() || self->next_pairing_id_ != id) {
          bt_log(TRACE,
                 "sm",
                 "ignoring %s response for expired pairing: id = %" PRIu64,
                 util::DisplayMethodToString(method).c_str(),
                 id);
          return;
        }
        cb(is_confirmed);
      });
}

void SecurityManagerImpl::RequestPasskey(PasskeyResponseCallback respond) {
  PW_CHECK(!bredr_link_.is_alive());
  PW_CHECK(delegate_.is_alive());
  delegate_->RequestPasskey([id = next_pairing_id_,
                             self = weak_self_.GetWeakPtr(),
                             cb = std::move(respond)](int64_t passkey) {
    if (!self.is_alive() || self->next_pairing_id_ != id) {
      bt_log(
          TRACE,
          "sm",
          "ignoring passkey input response for expired pairing: id = %" PRIx64,
          id);
      return;
    }
    cb(passkey);
  });
}

void SecurityManagerImpl::OnRxBFrame(ByteBufferPtr sdu) {
  fit::result<ErrorCode, ValidPacketReader> maybe_reader =
      ValidPacketReader::ParseSdu(sdu);
  if (maybe_reader.is_error()) {
    bt_log(INFO,
           "sm",
           "dropped SMP packet: %s",
           bt_str(ToResult(maybe_reader.error_value())));
    return;
  }
  ValidPacketReader reader = maybe_reader.value();
  Code smp_code = reader.code();

  if (smp_code == kPairingRequest) {
    OnPairingRequest(reader.payload<PairingRequestParams>());
  } else if (smp_code == kSecurityRequest) {
    OnSecurityRequest(reader.payload<AuthReqField>());
  } else {
    bt_log(INFO,
           "sm",
           "dropped unexpected SMP code %#.2X when not pairing",
           smp_code);
  }
}

void SecurityManagerImpl::OnChannelClosed() {
  bt_log(DEBUG, "sm", "SMP channel closed while not pairing");
}

void SecurityManagerImpl::OnPairingFailed(Error error) {
  std::string phase_status = std::visit(
      [=](auto& arg) {
        using T = std::decay_t<decltype(arg)>;
        std::string s;
        if constexpr (std::is_same_v<std::unique_ptr<Phase1>, T>) {
          s = arg->ToString();
        } else if constexpr (std::is_base_of_v<PairingPhase, T>) {
          s = arg.ToString();
        } else {
          PW_CRASH(
              "security upgrade cannot fail when current_phase_ is "
              "std::monostate!");
        }
        return s;
      },
      current_phase_);
  bt_log(ERROR,
         "sm",
         "LE pairing failed: %s. Current pairing phase: %s",
         bt_str(error),
         phase_status.c_str());
  StopTimer();
  // TODO(fxbug.dev/42172514): implement "waiting interval" to prevent repeated
  // attempts as described in Vol 3, Part H, 2.3.6.

  PW_CHECK(delegate_.is_alive());
  delegate_->OnPairingComplete(fit::error(error));

  auto requests = std::move(request_queue_);
  while (!requests.empty()) {
    requests.front().callback(fit::error(error), security());
    requests.pop();
  }

  if (SecurityUpgradeInProgress() && !bredr_link_.is_alive()) {
    PW_CHECK(low_energy_link_.is_alive());
    low_energy_link_->set_ltk(hci_spec::LinkKey());
  }

  if (bredr_cross_transport_key_derivation_callback_) {
    bredr_cross_transport_key_derivation_callback_(
        ToResult(HostError::kFailed));
  }

  ResetState();
  // Reset state before potentially disconnecting link to avoid causing pairing
  // phase to fail twice.
  if (error.is(HostError::kTimedOut)) {
    // Per v5.2 Vol. 3 Part H 3.4, after a pairing timeout "No further SMP
    // commands shall be sent over the L2CAP Security Manager Channel. A new
    // Pairing process shall only be performed when a new physical link has been
    // established."
    bt_log(WARN, "sm", "pairing timed out! disconnecting link");
    sm_chan_->SignalLinkError();
  }
}

void SecurityManagerImpl::StartNewTimer() {
  if (timeout_task_.is_pending()) {
    PW_CHECK(timeout_task_.Cancel());
  }
  timeout_task_.PostAfter(kPairingTimeout);
}

void SecurityManagerImpl::StopTimer() {
  if (timeout_task_.is_pending() && !timeout_task_.Cancel()) {
    bt_log(TRACE, "sm", "smp: failed to stop timer");
  }
}

void SecurityManagerImpl::OnPairingTimeout() {
  std::visit(
      [=](auto& arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<std::unique_ptr<Phase1>, T>) {
          arg->OnFailure(Error(HostError::kTimedOut));
        } else if constexpr (std::is_base_of_v<PairingPhase, T>) {
          arg.OnFailure(Error(HostError::kTimedOut));
        } else {
          PW_CRASH("cannot timeout when current_phase_ is std::monostate!");
        }
      },
      current_phase_);
}

std::pair<DeviceAddress, DeviceAddress>
SecurityManagerImpl::LEPairingAddresses() {
  PW_CHECK(!bredr_link_.is_alive());
  PW_CHECK(SecurityUpgradeInProgress());
  const DeviceAddress *initiator = &low_energy_link_->local_address(),
                      *responder = &low_energy_link_->peer_address();
  if (role() == Role::kResponder) {
    std::swap(initiator, responder);
  }
  return std::make_pair(*initiator, *responder);
}

void SecurityManagerImpl::OnNewLongTermKey(const LTK& ltk) {
  ltk_ = ltk;
  low_energy_link_->set_ltk(ltk.key());
}

Result<> SecurityManagerImpl::ValidateExistingLocalLtk() {
  Result<> status = fit::ok();
  if (!ltk_.has_value()) {
    // Should always be present when this method is called.
    bt_log(ERROR, "sm", "SM LTK not found");
    status = fit::error(Error(HostError::kNotFound));
  } else if (!low_energy_link_->ltk().has_value()) {
    // Should always be present when this method is called.
    bt_log(ERROR, "sm", "Link LTK not found");
    status = fit::error(Error(HostError::kNotFound));
  } else if (low_energy_link_->ltk().value() != ltk_->key()) {
    // As only SM should ever change the LE Link encryption key, these two
    // values should always be in sync, i.e. something in the system is acting
    // unreliably if they get out of sync.
    bt_log(ERROR, "sm", "SM LTK differs from LE link LTK");
    status = fit::error(Error(HostError::kNotReliable));
  }
  if (status.is_error()) {
    // SM does not own the link, so although the checks above should never fail,
    // disconnecting the link (vs. ASSERTing these checks) is safer against
    // non-SM code potentially touching the key.
    delegate_->OnAuthenticationFailure(
        ToResult(pw::bluetooth::emboss::StatusCode::PIN_OR_KEY_MISSING));
    sm_chan_->SignalLinkError();
  }
  return status;
}

bool SecurityManagerImpl::IsBrEdrCrossTransportKeyDerivationAllowed() {
  if (!is_controller_remote_public_key_validation_supported_) {
    bt_log(DEBUG,
           "sm",
           "%s: remote public key validation not supported",
           __FUNCTION__);
    return false;
  }

  if (bredr_link_->encryption_status() !=
      pw::bluetooth::emboss::EncryptionStatus::ON_WITH_AES_FOR_BREDR) {
    bt_log(DEBUG, "sm", "%s: encryption status not AES", __FUNCTION__);
    return false;
  }

  if (!bredr_link_->ltk().has_value() || !bredr_link_->ltk_type() ||
      !(bredr_link_->ltk_type().value() ==
            hci_spec::LinkKeyType::kUnauthenticatedCombination256 ||
        bredr_link_->ltk_type().value() ==
            hci_spec::LinkKeyType::kAuthenticatedCombination256)) {
    bt_log(DEBUG, "sm", "%s: Link Key has insufficient security", __FUNCTION__);
    return false;
  }

  // Do not derive LE LTK if existing LE LTK is stronger than current
  // BR/EDR link key.
  SecurityProperties bredr_security_props(bredr_link_->ltk_type().value());
  bool has_le_ltk = peer_->le() && peer_->le()->bond_data() &&
                    peer_->le()->bond_data()->local_ltk;
  if (has_le_ltk && !bredr_security_props.IsAsSecureAs(
                        peer_->le()->bond_data()->local_ltk->security())) {
    bt_log(DEBUG,
           "sm",
           "%s: LE LTK stronger than current BR/EDR link key",
           __FUNCTION__);
    return false;
  }

  // TODO(fxbug.dev/388607971): check for LE pairing in progress

  return true;
}

std::optional<sm::LTK> SecurityManagerImpl::GetExistingLtkFromPeerCache() {
  if (peer_->le() && peer_->le()->bond_data()) {
    // Legacy pairing allows both devices to generate and exchange LTKs. "The
    // Central must have the security information (LTK, EDIV, and Rand)
    // distributed by the Peripheral in LE legacy [...] to setup an encrypted
    // session" (v5.3, Vol. 3 Part H 2.4.4.2). For Secure Connections peer_ltk
    // and local_ltk will be equal, so this check is unnecessary but correct.
    if (low_energy_link_->role() ==
        pw::bluetooth::emboss::ConnectionRole::CENTRAL) {
      return peer_->le()->bond_data()->peer_ltk;
    }
    return peer_->le()->bond_data()->local_ltk;
  }
  return std::nullopt;
}

std::unique_ptr<SecurityManager> SecurityManager::CreateLE(
    hci::LowEnergyConnection::WeakPtr link,
    l2cap::Channel::WeakPtr smp,
    IOCapability io_capability,
    Delegate::WeakPtr delegate,
    BondableMode bondable_mode,
    gap::LESecurityMode security_mode,
    pw::async::Dispatcher& dispatcher,
    bt::gap::Peer::WeakPtr peer) {
  PW_CHECK(link.is_alive());
  return std::make_unique<SecurityManagerImpl>(
      std::move(link),
      hci::BrEdrConnection::WeakPtr(),
      std::move(smp),
      io_capability,
      std::move(delegate),
      bondable_mode,
      security_mode,
      /*is_controller_remote_public_key_validation_supported=*/false,
      dispatcher,
      std::move(peer));
}

std::unique_ptr<SecurityManager> SecurityManager::CreateBrEdr(
    hci::BrEdrConnection::WeakPtr link,
    l2cap::Channel::WeakPtr smp,
    Delegate::WeakPtr delegate,
    bool is_controller_remote_public_key_validation_supported,
    pw::async::Dispatcher& dispatcher,
    bt::gap::Peer::WeakPtr peer) {
  PW_CHECK(smp.is_alive());
  PW_CHECK(smp->id() == l2cap::kSMPChannelId);
  return std::make_unique<SecurityManagerImpl>(
      hci::LowEnergyConnection::WeakPtr(),
      std::move(link),
      std::move(smp),
      IOCapability::kNoInputNoOutput,  // arbitrary, RFU in BR/EDR
      std::move(delegate),
      BondableMode::Bondable,      // BR/EDR is always "bondable".
      gap::LESecurityMode::Mode1,  // arbitrary, not used by
                                   // BR/EDR
      is_controller_remote_public_key_validation_supported,
      dispatcher,
      std::move(peer));
}

SecurityManager::SecurityManager(BondableMode bondable_mode,
                                 gap::LESecurityMode security_mode)
    : low_energy_bondable_mode_(bondable_mode),
      low_energy_security_mode_(security_mode) {}

}  // namespace bt::sm
