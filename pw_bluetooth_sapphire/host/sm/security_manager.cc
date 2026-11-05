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

#include <chrono>
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
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
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

constexpr pw::chrono::SystemClock::duration kRepeatedAttemptsMinTimeout =
    std::chrono::seconds(1);
constexpr pw::chrono::SystemClock::duration kRepeatedAttemptsMaxTimeout =
    std::chrono::minutes(10);

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

  // Loads any existing LTK from the peer cache and starts encryption if
  // acting as the LE Central. Returns false if starting encryption fails.
  [[nodiscard]] bool Init();

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
  // SecurityRequest). Returns an ErrorCode if repeated-attempts backoff is
  // active or if local IOCapabilities cannot support `level`, otherwise
  // returns fit::ok().
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

  // Drains `request_queue_` and invokes each queued callback with `error` and
  // the current security properties.
  void FailPendingRequests(Error error);

  // Assign the current security properties and notify the delegate of the
  // change.
  void SetSecurityProperties(const SecurityProperties& sec);

  // Directly assigns the current |ltk_| and the underlying |le_link_|'s link
  // key. This function does not initiate link layer encryption and can be
  // called during and outside of pairing.
  void OnNewLongTermKey(const LTK& ltk);

  // Restores `ltk_` to `previous_ltk` and synchronizes `low_energy_link_`'s
  // link key (setting it when `previous_ltk` is present or clearing it when
  // `std::nullopt`).
  void RestoreLtk(std::optional<LTK> previous_ltk);

  // Assigns `ltk` to the link and starts link-layer encryption, entering the
  // `StartingEncryption` phase on success or rolling back `ltk_`, failing
  // pending requests, and signaling a link error on failure.
  bool StartExistingLtkEncryption(const LTK& ltk);

  // PairingPhase::Listener overrides:
  void OnPairingFailed(Error error) override;
  std::optional<IdentityInfo> OnIdentityRequest() override;
  void ConfirmPairing(ConfirmCallback confirm) override;
  void DisplayPasskey(uint32_t passkey,
                      Delegate::DisplayMethod method,
                      ConfirmCallback cb) override;
  void RequestPasskey(PasskeyResponseCallback respond) override;
  bool IsBrEdrCrossTransportKeyDerivationAllowed() const override;

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

  // Returns a pointer to the currently active PairingPhase, or nullptr if no
  // PairingPhase is active.
  PairingPhase* ActivePairingPhase();

  // Validates that both SM and the link have stored LTKs, and that these values
  // match. Disconnects the link if it finds an issue. Should only be called
  // when an LTK is expected to exist.
  Result<> ValidateExistingLocalLtk();

  // Returns true if BR/EDR pairing is active on the peer or this security
  // manager is currently waiting for BR/EDR pairing to complete.
  bool IsBrEdrPairingInProgress() const;

  std::optional<sm::LTK> GetExistingLtkFromPeerCache();

  // Updates repeated attempts backoff state following a pairing failure.
  void UpdateRepeatedAttemptsBackoff();

  // Resets repeated attempts backoff state following a successful pairing or
  // administrative reset.
  void ResetRepeatedAttemptsBackoff();

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

  // Unmasked 128-bit LE Secure Connections LTK generated in Phase 2, retained
  // for cross-transport BR/EDR link key derivation prior to key-size masking
  // per Core Spec v6.2, Vol 3, Part H, Section 2.3.4 and Section 2.4.2.4.
  std::optional<UInt128> unmasked_sc_ltk_;

  // If a pairing is in progress and Phase 1 (feature exchange) has completed,
  // this will store the result of that feature exchange. Otherwise, this will
  // be std::nullopt.
  std::optional<PairingFeatures> features_;

  // Pre-Phase-2 `ltk_` snapshot saved before `OnPhase2EncryptionKey()`
  // installs an unverified LE Secure Connections LTK, allowing
  // `OnPairingFailed()` to roll back `ltk_` and `low_energy_link_->ltk()` if
  // Phase 2 fails before `EndPhase2()`.
  std::optional<std::optional<LTK>> pre_phase_2_ltk_;

  // The pending security requests added via UpgradeSecurity().
  std::queue<PendingRequest> request_queue_;

  CrossTransportKeyDerivationResultCallback
      bredr_cross_transport_key_derivation_callback_ = nullptr;

  // Fixed SMP Channel used to send/receive packets
  std::unique_ptr<PairingChannel> sm_chan_;

  SmartTask timeout_task_{pw_dispatcher_};

  // Held while an active pairing phase (SecurityRequestPhase or Phase 1-3) is
  // in progress so gap::Peer tracks active transport pairing; not set during
  // the WaitForBrEdrPairing or StartingEncryption pseudo-phases.
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

  // True only after `OnPhase2EncryptionKey()` has delivered the key generated
  // by the active Phase 2 exchange, ensuring `OnEncryptionChange()` cannot
  // treat encryption with a pre-existing LTK as Phase 2 completion.
  bool phase_2_encryption_key_ready_ = false;

  // Repeated attempts backoff state (Core Spec Vol 3, Part H, §2.3.6).
  // Tracks the next waiting interval and when the current backoff expires.
  pw::chrono::SystemClock::duration next_pairing_waiting_interval_ =
      kRepeatedAttemptsMinTimeout;
  pw::chrono::SystemClock::time_point pairing_backoff_expiry_ =
      pw::chrono::SystemClock::time_point();

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
  }
}

bool SecurityManagerImpl::Init() {
  if (!low_energy_link_.is_alive()) {
    return true;
  }

  // Obtain existing pairing data, if any.
  std::optional<sm::LTK> ltk = GetExistingLtkFromPeerCache();
  if (!ltk) {
    return true;
  }

  bt_log(INFO,
         "sm",
         "starting encryption with existing LTK (peer: %s, handle: %#.4x)",
         bt_str(peer_->identifier()),
         low_energy_link_->handle());

  // The initiator starts encryption when there is an LTK.
  if (low_energy_link_->role() ==
      pw::bluetooth::emboss::ConnectionRole::CENTRAL) {
    return StartExistingLtkEncryption(*ltk);
  }

  // Sets LTK in low_energy_link_
  OnNewLongTermKey(*ltk);
  return true;
}

void SecurityManagerImpl::OnSecurityRequest(AuthReqField auth_req) {
  // Core Spec 6.0, Vol 3, Part H, Section 2.3.6: a waiting interval shall pass
  // before responding to a Security Request initiated by a remote device
  // claiming the same identity as the failed device.
  if (pw_dispatcher_.now() < pairing_backoff_expiry_) {
    bt_log(WARN,
           "sm",
           "rejecting security request due to repeated attempts backoff");
    sm_chan_->SendMessageNoTimerReset(kPairingFailed,
                                      ErrorCode::kRepeatedAttempts);
    return;
  }

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

  // Core Spec v6.2, Vol 3, Part H, Section 2.4.6: "The Central shall ignore the
  // Peripheral's Security Request if the Central has sent a Pairing Request
  // without receiving a Pairing Response from the Peripheral or if the Central
  // has initiated encryption mode setup."
  if (std::holds_alternative<StartingEncryption>(current_phase_)) {
    bt_log(INFO,
           "sm",
           "ignoring Security Request while encryption setup is in progress");
    return;
  }

  SecurityLevel requested_level;
  // Core Spec v6.2, Vol 3, Part C, Section 10.2.4 / 14.3: Secure Connections
  // Only mode requires authenticated LE Secure Connections pairing with a
  // 128-bit encryption key.
  if (security_mode() == gap::LESecurityMode::SecureConnectionsOnly) {
    requested_level = SecurityLevel::kSecureAuthenticated;
  } else {
    // inclusive-language: ignore
    requested_level = (auth_req & AuthReq::kMITM)
                          ? SecurityLevel::kAuthenticated
                          : SecurityLevel::kEncrypted;
  }

  // Core Spec v6.2, Vol 3, Part C, Section 14.2 & Part H, Section 2.4.6 /
  // 3.5.5: Reject incoming LE Security Requests whenever BR/EDR pairing with
  // the same peer is in progress (regardless of the peer's Secure Connections
  // flag) so concurrent cross-transport pairing cannot run or overwrite an
  // active WaitForBrEdrPairing phase.
  if (IsBrEdrPairingInProgress()) {
    bt_log(INFO,
           "sm",
           "rejecting Security Request because BREDR pairing in progress");
    sm_chan_->SendMessageNoTimerReset(kPairingFailed,
                                      ErrorCode::kBREDRPairingInProgress);
    return;
  }

  // If we already have a LTK and its security properties satisfy the request,
  // then we start link layer encryption (which will either encrypt the link or
  // perform a key refresh). See Vol. 3, Part H, Figure 2.7 for the algorithm.
  if (ltk_ && (ltk_->security().level() >= requested_level) &&
      (!(auth_req & AuthReq::kSC) || ltk_->security().secure_connections())) {
    if (bt_is_error(
            ValidateExistingLocalLtk(),
            ERROR,
            "sm",
            "disconnecting link as it cannot be encrypted with LTK status")) {
      return;
    }
    StartExistingLtkEncryption(*ltk_);
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

  if (level == SecurityLevel::kNoSecurity) {
    const SecurityProperties current_security = security();
    callback(fit::ok(), current_security);
    return;
  }

  // Secure Connections only mode only permits Secure Connections authenticated
  // pairing with a 128-bit encryption key, except for services that only
  // require Mode 1 Level 1 (No Security) (Core Spec v6.2, Vol 3, Part C,
  // Section 10.2.4), so we force all security upgrade requests above
  // kNoSecurity to that level before checking in-progress or current security.
  if (security_mode() == gap::LESecurityMode::SecureConnectionsOnly) {
    level = SecurityLevel::kSecureAuthenticated;
  }

  if (SecurityUpgradeInProgress()) {
    bt_log(TRACE,
           "sm",
           "LE security upgrade in progress; request for %s security queued",
           LevelToString(level));
    request_queue_.emplace(level, std::move(callback));
    return;
  }

  if (level <= security().level()) {
    const SecurityProperties current_security = security();
    callback(fit::ok(), current_security);
    return;
  }

  // If `UpgradeSecurity` is called re-entrantly from a completion callback (in
  // `NotifySecurityCallbacks` or `UpgradeSecurityInternal`) while other queued
  // requests are already waiting, only enqueue the request here; the active
  // callback loop will invoke `UpgradeSecurityInternal()` once it finishes.
  if (!request_queue_.empty()) {
    request_queue_.emplace(level, std::move(callback));
    return;
  }
  request_queue_.emplace(level, std::move(callback));
  UpgradeSecurityInternal();
}

void SecurityManagerImpl::OnPairingRequest(
    const PairingRequestParams& req_params) {
  // Core Spec 6.0, Vol 3, Part H, Section 2.3.6: "a waiting interval shall pass
  // before the verifier [...] will respond to a Pairing Request command [...]
  // initiated by a device claiming the same identity as the failed device."
  if (pw_dispatcher_.now() < pairing_backoff_expiry_) {
    bt_log(WARN,
           "sm",
           "rejecting pairing request due to repeated attempts backoff");
    sm_chan_->SendMessageNoTimerReset(kPairingFailed,
                                      ErrorCode::kRepeatedAttempts);
    return;
  }

  // Only the initiator may send the Pairing Request (V5.0 Vol. 3 Part H 3.5.1).
  if (role() != Role::kResponder) {
    bt_log(INFO, "sm", "rejecting \"Pairing Request\" as initiator");
    sm_chan_->SendMessageNoTimerReset(kPairingFailed,
                                      ErrorCode::kCommandNotSupported);
    return;
  }

  // Reject incoming LE Pairing Requests whenever BR/EDR pairing with the same
  // peer is in progress (regardless of the peer's Secure Connections flag) so
  // concurrent cross-transport pairing cannot run or overwrite an active
  // WaitForBrEdrPairing phase.
  if (low_energy_link_.is_alive() && IsBrEdrPairingInProgress()) {
    bt_log(INFO,
           "sm",
           "LE: rejecting Pairing Request because BREDR pairing in progress");
    sm_chan_->SendMessageNoTimerReset(kPairingFailed,
                                      ErrorCode::kBREDRPairingInProgress);
    if (std::holds_alternative<SecurityRequestPhase>(current_phase_)) {
      ResetState();
      if (!request_queue_.empty()) {
        UpgradeSecurityInternal();
      }
    }
    return;
  }
  PW_CHECK(!SecurityUpgradeInProgress() ||
           std::holds_alternative<SecurityRequestPhase>(current_phase_));

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

  // prevent an attacker from bypassing authentication using a previously known
  // unauthenticated LTK. The connection LTK is restored if pairing fails.
  if (low_energy_link_.is_alive()) {
    low_energy_link_->reset_ltk();
  }

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

  WeakSelf<SecurityManagerImpl>::WeakPtr self = weak_self_.GetWeakPtr();
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
      StartExistingLtkEncryption(*ltk);
      return;
    }
  }

  if (fit::result<ErrorCode> result = RequestSecurityUpgrade(next_req.level);
      result.is_error()) {
    const SecurityProperties current_security = security();
    PendingRequest failed_req = std::move(request_queue_.front());
    request_queue_.pop();
    failed_req.callback(ToResult(result.error_value()), current_security);
    if (self.is_alive() && !request_queue_.empty() &&
        !SecurityUpgradeInProgress()) {
      UpgradeSecurityInternal();
    }
  }
}

fit::result<ErrorCode> SecurityManagerImpl::RequestSecurityUpgrade(
    SecurityLevel level) {
  PW_CHECK(!bredr_link_.is_alive());

  // Core Spec 6.0, Vol 3, Part H, Section 2.3.6: a waiting interval shall pass
  // before responding to a Security Request initiated by a remote device
  // claiming the same identity as the failed device.
  if (pw_dispatcher_.now() < pairing_backoff_expiry_) {
    bt_log(WARN,
           "sm",
           "rejecting outgoing security upgrade request due to repeated "
           "attempts backoff");
    return fit::error(ErrorCode::kRepeatedAttempts);
  }

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
    if (low_energy_link_.is_alive()) {
      low_energy_link_->reset_ltk();
    }
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
  phase_2_encryption_key_ready_ = false;
  features_ = features;

  auto self = weak_listener_.GetWeakPtr();
  if (bredr_link_.is_alive()) {
    if (!features_->generate_ct_key.has_value() ||
        !IsBrEdrCrossTransportKeyDerivationAllowed()) {
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
    std::optional<uint8_t> key_size = bredr_link_->encryption_key_size();
    if (!key_size.has_value()) {
      bt_log(ERROR, "sm", "encryption key size unknown in OnFeatureExchange");
      Abort(ErrorCode::kUnspecifiedReason);
      return;
    }
    SecurityProperties bredr_security_properties(
        bredr_link_->ltk_type().value(), key_size.value());

    current_phase_.emplace<Phase3>(
        sm_chan_->GetWeakPtr(),
        self,
        role(),
        *features_,
        bredr_security_properties,
        bredr_link_->peer_address(),
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
  phase_2_encryption_key_ready_ = true;
  // EDiv and Rand values are 0 for Phase 2 keys generated by Legacy or Secure
  // Connections (Vol 3, Part H, 2.4.4 / 2.4.4.1). Secure Connections generates
  // an LTK, while Legacy generates an STK.
  if (features_->secure_connections) {
    unmasked_sc_ltk_ = new_key;
    // Core Spec v6.2, Vol 3, Part H, Section 2.3.4: "If a key has an encryption
    // key size that is shorter than 16 octets (128 bits), it shall be created
    // by masking the appropriate number of most significant octets of the
    // generated key to provide a resulting key that has the agreed encryption
    // key size. The key shall be masked after generation and, if required,
    // after the key is used to derive a BR/EDR link key. The key shall be
    // masked before the key is distributed, used for encryption, or stored."
    UInt128 masked_key = new_key;
    util::MaskKeyToSize(masked_key, features_->encryption_key_size);
    pre_phase_2_ltk_ = ltk_;
    OnNewLongTermKey(LTK(FeaturesToProperties(*features_),
                         hci_spec::LinkKey(masked_key, 0, 0)));
  } else {
    // `set_ltk` sets the encryption key of the LE link (which is the STK for
    // Legacy), not the long-term key that results from pairing (which is
    // generated in Phase 3 for Legacy).
    low_energy_link_->set_ltk(hci_spec::LinkKey(new_key, 0, 0));
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
  WeakSelf<SecurityManagerImpl>::WeakPtr self = weak_self_.GetWeakPtr();

  // First notify the delegate in case of failure.
  if (bt_is_error(
          enabled_result, ERROR, "sm", "link layer authentication failed")) {
    PW_CHECK(delegate_.is_alive());
    delegate_->OnAuthenticationFailure(
        fit::error(enabled_result.error_value()));
    if (!self.is_alive()) {
      return;
    }
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
           SecurityUpgradeInProgress() ? " during security upgrade" : "");
    SetSecurityProperties(SecurityProperties());
    if (!self.is_alive()) {
      return;
    }
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
    if (SecurityUpgradeInProgress()) {
      ResetState();
      FailPendingRequests(Error(HostError::kInsufficientSecurity));
      if (!self.is_alive()) {
        return;
      }
    }
    delegate_->OnAuthenticationFailure(
        ToResult(HostError::kInsufficientSecurity));
    if (self.is_alive()) {
      sm_chan_->SignalLinkError();
    }
    return;
  }

  if (InPhase1() || (InPhase2() && !phase_2_encryption_key_ready_)) {
    bt_log(WARN,
           "sm",
           "link encrypted before Phase 2 key was generated; aborting pairing");
    const PairingProcedureId pairing_id = next_pairing_id_;
    const bool had_ltk = ltk_.has_value();
    if (had_ltk) {
      SetSecurityProperties(ltk_->security());
      if (!self.is_alive()) {
        return;
      }
    }
    if (next_pairing_id_ == pairing_id) {
      Abort(ErrorCode::kUnspecifiedReason);
      if (!self.is_alive()) {
        return;
      }
    }
    if (!had_ltk || !ltk_.has_value()) {
      bt_is_error(
          ValidateExistingLocalLtk(),
          ERROR,
          "sm",
          "disconnecting link as it cannot be encrypted with LTK status");
    }
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
    if (security_request_phase) {
      PW_CHECK(role() == Role::kResponder);
      PW_CHECK(!request_queue_.empty());
      // The central satisfied our Security Request by LL-encrypting with the
      // bonded LTK. Clear the SecurityRequestPhase and stop the SMP timer so
      // that NotifySecurityCallbacks can re-enter UpgradeSecurityInternal for
      // any higher-level queued request and so that the 30 s timer does not
      // spuriously tear down the now-encrypted link.
      ResetState();
    } else if (starting_encryption_phase) {
      ResetState();
    }
    // If encryption is enabled while not pairing, we update the security
    // properties to those of `ltk_`. Otherwise, we let the EndPhase2 pairing
    // function determine the security properties.
    SetSecurityProperties(ltk_->security());
    if (!self.is_alive()) {
      return;
    }
    NotifySecurityCallbacks();
    return;
  }

  if (InPhase2()) {
    PW_CHECK(phase_2_encryption_key_ready_);
    bt_log(DEBUG, "sm", "link encrypted with phase 2 generated key");
    EndPhase2();
  }
}

void SecurityManagerImpl::EndPhase2() {
  PW_CHECK(features_.has_value());
  PW_CHECK(InPhase2());
  PW_CHECK(!bredr_link_.is_alive());
  phase_2_encryption_key_ready_ = false;
  pre_phase_2_ltk_.reset();

  WeakSelf<SecurityManagerImpl>::WeakPtr self = weak_self_.GetWeakPtr();
  SetSecurityProperties(FeaturesToProperties(*features_));
  if (!self.is_alive() || !InPhase2()) {
    return;
  }
  // If there are no keys to distribute, don't bother creating Phase 3
  if (!HasKeysToDistribute(*features_, bredr_link_.is_alive())) {
    OnLowEnergyPairingComplete(PairingData());
    return;
  }
  PW_CHECK(low_energy_link_.is_alive());
  PairingPhase::Listener::WeakPtr listener = weak_listener_.GetWeakPtr();
  current_phase_.emplace<Phase3>(
      sm_chan_->GetWeakPtr(),
      listener,
      role(),
      *features_,
      security(),
      low_energy_link_->peer_address(),
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

  WeakSelf<SecurityManagerImpl>::WeakPtr self = weak_self_.GetWeakPtr();
  const PairingFeatures features = *features_;

  // Core Spec v6.0, Volume 3, Part H, Section 2.3.6: The waiting interval shall
  // be reset to its initial value if a pairing attempt succeeds.
  ResetRepeatedAttemptsBackoff();

  // In Secure Connections, the LTK will be generated in Phase 2, not exchanged
  // in Phase 3, so we want to ensure that it is still put in the pairing_data.
  if (features.secure_connections) {
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

  if (features.generate_ct_key.has_value()) {
    // If we are generating the CT key, we must be using secure connections, and
    // as such the peer and local LTKs will be equivalent.
    PW_CHECK(features.secure_connections);
    PW_CHECK(pairing_data.peer_ltk == pairing_data.local_ltk);
    PW_CHECK(unmasked_sc_ltk_.has_value());
    // Core Spec v6.2, Vol 3, Part C, Section 14.1: Do not derive a BR/EDR link
    // key from an LE LTK when an existing BR/EDR link key has stronger
    // encryption key size or authentication protection.
    const bool existing_bredr_key_stronger =
        peer_->bredr().has_value() && peer_->bredr()->link_key().has_value() &&
        !ltk_->security().IsAsSecureAs(peer_->bredr()->link_key()->security());
    // Core Spec v6.2, Vol 3, Part C, Section 14.2: Do not overwrite or derive a
    // BR/EDR link key while BR/EDR pairing is currently in progress.
    if (IsBrEdrPairingInProgress()) {
      bt_log(INFO,
             "sm",
             "LE: skipping BR/EDR cross-transport key derivation because "
             "BR/EDR pairing is in progress");
    } else if (existing_bredr_key_stronger) {
      bt_log(INFO,
             "sm",
             "skipping BR/EDR cross-transport key derivation because existing "
             "BR/EDR link key is stronger than LE LTK");
    } else {
      // Core Spec v6.2, Vol 3, Part H, Section 2.4.2.4: "If the LTK has an
      // encryption key size that is shorter than 16 octets (128 bits), then the
      // BR/EDR link key is derived before the LTK gets masked."
      std::optional<UInt128> ct_key_value = util::LeLtkToBrEdrLinkKey(
          *unmasked_sc_ltk_, features.generate_ct_key.value());
      if (ct_key_value) {
        // Core Spec v6.2, Vol 2, Part H, Section 3.1: BR/EDR link keys are
        // always 16 bytes (128 bits).
        SecurityProperties sec_props(ltk_->security().level(),
                                     hci_spec::kBrEdrLinkKeySize,
                                     ltk_->security().secure_connections());
        pairing_data.cross_transport_key =
            LTK(sec_props, hci_spec::LinkKey(*ct_key_value, 0, 0));
      } else {
        bt_log(WARN, "sm", "failed to generate cross-transport key");
      }
    }
  }

  // So we can pair again or handle re-entrant Reset()/UpgradeSecurity() calls
  // inside delegate, bond-storage, or completion callbacks cleanly. Hold the
  // pairing token until scope exit so
  // `Peer::LowEnergyData::OnPairingMaybeComplete` fires only after `StoreBond`
  // persists the LE bond and any CTKD link key.
  std::optional<PairingToken> token = std::move(pairing_token_);
  ResetState();

  delegate_->OnPairingComplete(fit::ok());
  if (!self.is_alive()) {
    return;
  }

  if (features.will_bond) {
    std::optional<LTK> ltk;
    if (pairing_data.peer_ltk) {
      ltk = pairing_data.peer_ltk;
    } else {
      ltk = pairing_data.local_ltk;
    }

    if (ltk.has_value()) {
      const PeerId peer_id = peer_->identifier();
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
          bt_str(peer_id));

      if (!peer_->MutLe().StoreBond(pairing_data)) {
        bt_log(ERROR,
               "sm",
               "failed to cache bonding data (id: %s)",
               bt_str(peer_id));
      }
      if (!self.is_alive()) {
        return;
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
           features.secure_connections ? "secure connections" : "legacy",
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

  if (!IsBrEdrCrossTransportKeyDerivationAllowed()) {
    bt_log(WARN,
           "sm",
           "aborting completed BR/EDR CTKD because CTKD is no longer allowed");
    Abort(ErrorCode::kCrossTransportKeyDerivationNotAllowed);
    return;
  }

  bt_log(INFO, "sm", "BR/EDR cross-transport key derivation complete");

  WeakSelf<SecurityManagerImpl>::WeakPtr self = weak_self_.GetWeakPtr();
  const PairingFeatures features = *features_;
  CrossTransportKeyDerivationResultCallback ctkd_cb =
      std::move(bredr_cross_transport_key_derivation_callback_);

  // Core Spec Volume 3, Part H, Section 2.3.6: The waiting interval shall be
  // reset to its initial value if a pairing attempt succeeds.
  ResetRepeatedAttemptsBackoff();
  std::optional<PairingToken> token = std::move(pairing_token_);
  ResetState();

  delegate_->OnPairingComplete(fit::ok());
  if (!self.is_alive()) {
    return;
  }

  // If the LE identity address provided by the peer doesn't match its existing
  // BR/EDR address, drop its address and IRK. This prevents a BR/EDR connection
  // from claiming a different LE address and overwriting another peer's LE
  // bonding data.
  if (pairing_data.identity_address.has_value() &&
      pairing_data.identity_address->value() !=
          bredr_link_->peer_address().value()) {
    bt_log(WARN,
           "sm",
           "dropping mismatched LE identity %s (expected %s)",
           bt_str(*pairing_data.identity_address),
           bt_str(bredr_link_->peer_address()));
    pairing_data.identity_address.reset();
    pairing_data.irk.reset();
    pairing_data.csrk.reset();
  }

  std::optional<UInt128> ct_key_value = util::BrEdrLinkKeyToLeLtk(
      bredr_link_->ltk()->value(), features.generate_ct_key.value());
  if (ct_key_value) {
    // The LE LTK will have the same security properties as the BR/EDR key.
    std::optional<uint8_t> key_size = bredr_link_->encryption_key_size();
    if (!key_size.has_value()) {
      bt_log(
          ERROR, "sm", "encryption key size unknown in OnBrEdrPairingComplete");
      if (ctkd_cb) {
        ctkd_cb(ToResult(HostError::kFailed));
      }
      return;
    }
    SecurityProperties bredr_properties(bredr_link_->ltk_type().value(),
                                        key_size.value());
    LTK le_ltk(bredr_properties, hci_spec::LinkKey(*ct_key_value, 0, 0));
    pairing_data.local_ltk = le_ltk;
    pairing_data.peer_ltk = le_ltk;
  } else {
    bt_log(ERROR, "sm", "BR/EDR CTKD key generation failed");
    if (ctkd_cb) {
      ctkd_cb(ToResult(HostError::kFailed));
    }
    return;
  }

  const PeerId peer_id = peer_->identifier();
  if (!peer_->MutLe().StoreBond(pairing_data)) {
    bt_log(
        ERROR, "sm", "failed to cache bonding data (id: %s)", bt_str(peer_id));
  }
  if (!self.is_alive()) {
    return;
  }

  if (ctkd_cb) {
    ctkd_cb(fit::ok());
  }
}

void SecurityManagerImpl::NotifySecurityCallbacks() {
  WeakSelf<SecurityManagerImpl>::WeakPtr self = weak_self_.GetWeakPtr();
  // Separate out the requests that are satisfied by the current security level
  // from those that require a higher level. We'll retry pairing for the latter.
  std::queue<PendingRequest> satisfied;
  std::queue<PendingRequest> unsatisfied;
  const SecurityProperties current_security = security();
  while (!request_queue_.empty()) {
    PendingRequest& request = request_queue_.front();
    if (request.level <= current_security.level()) {
      satisfied.push(std::move(request));
    } else {
      unsatisfied.push(std::move(request));
    }
    request_queue_.pop();
  }

  request_queue_ = std::move(unsatisfied);

  // Notify the satisfied requests with success.
  while (!satisfied.empty()) {
    PendingRequest req = std::move(satisfied.front());
    satisfied.pop();
    req.callback(fit::ok(), current_security);
  }

  if (self.is_alive() && !request_queue_.empty() &&
      !SecurityUpgradeInProgress()) {
    UpgradeSecurityInternal();
  }
}

void SecurityManagerImpl::FailPendingRequests(Error error) {
  const SecurityProperties current_security = security();
  std::queue<PendingRequest> requests = std::exchange(request_queue_, {});
  while (!requests.empty()) {
    PendingRequest req = std::move(requests.front());
    requests.pop();
    req.callback(fit::error(error), current_security);
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
  WeakSelf<SecurityManagerImpl>::WeakPtr self = weak_self_.GetWeakPtr();
  low_energy_io_cap_ = io_capability;
  Abort(ErrorCode::kUnspecifiedReason);
  if (!self.is_alive()) {
    return;
  }
  ResetRepeatedAttemptsBackoff();
}

PairingPhase* SecurityManagerImpl::ActivePairingPhase() {
  if (std::unique_ptr<Phase1>* phase_1 =
          std::get_if<std::unique_ptr<Phase1>>(&current_phase_)) {
    return phase_1->get();
  }
  if (SecurityRequestPhase* security_request =
          std::get_if<SecurityRequestPhase>(&current_phase_)) {
    return security_request;
  }
  if (Phase2Legacy* phase_2_legacy =
          std::get_if<Phase2Legacy>(&current_phase_)) {
    return phase_2_legacy;
  }
  if (Phase2SecureConnections* phase_2_sc =
          std::get_if<Phase2SecureConnections>(&current_phase_)) {
    return phase_2_sc;
  }
  if (Phase3* phase_3 = std::get_if<Phase3>(&current_phase_)) {
    return phase_3;
  }
  return nullptr;
}

void SecurityManagerImpl::ResetState() {
  next_pairing_id_++;
  StopTimer();
  features_.reset();
  if (pre_phase_2_ltk_.has_value()) {
    RestoreLtk(*pre_phase_2_ltk_);
    pre_phase_2_ltk_.reset();
  }
  unmasked_sc_ltk_.reset();
  sm_chan_->SetChannelHandler(weak_handler_.GetWeakPtr());
  current_phase_ = std::monostate{};
  phase_2_encryption_key_ready_ = false;
  pairing_token_.reset();
}

void SecurityManagerImpl::SetSecurityProperties(const SecurityProperties& sec) {
  if (sec != security()) {
    const SecurityProperties props = sec;
    bt_log(DEBUG,
           "sm",
           "security properties changed - handle: %#.4x, new: %s, old: %s",
           low_energy_link_->handle(),
           bt_str(props),
           bt_str(security()));
    set_security(props);
    delegate_->OnNewSecurityProperties(props);
  }
}

void SecurityManagerImpl::Abort(ErrorCode ecode) {
  if (PairingPhase* phase = ActivePairingPhase()) {
    phase->Abort(ecode);
    return;
  }
  if (SecurityUpgradeInProgress()) {
    ResetState();
    FailPendingRequests(Error(ecode));
    return;
  }
  bt_log(
      DEBUG, "sm", "Attempted to abort security upgrade while not in progress");
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
  if (SecurityUpgradeInProgress()) {
    ResetState();
    FailPendingRequests(Error(HostError::kLinkDisconnected));
  }
}

void SecurityManagerImpl::OnPairingFailed(Error error) {
  PairingPhase* active_phase = ActivePairingPhase();
  PW_CHECK(active_phase != nullptr,
           "security upgrade cannot fail without an active PairingPhase!");
  std::string phase_status = active_phase->ToString();

  bt_log(ERROR,
         "sm",
         "SM pairing failed: %s. Current pairing phase: %s. Backing off for "
         "%" PRId64 " s",
         bt_str(error),
         phase_status.c_str(),
         int64_t{std::chrono::duration_cast<std::chrono::seconds>(
                     next_pairing_waiting_interval_)
                     .count()});

  WeakSelf<SecurityManagerImpl>::WeakPtr self = weak_self_.GetWeakPtr();
  UpdateRepeatedAttemptsBackoff();

  if (SecurityUpgradeInProgress() && !bredr_link_.is_alive()) {
    PW_CHECK(low_energy_link_.is_alive());
    RestoreLtk(pre_phase_2_ltk_.value_or(ltk_));
    pre_phase_2_ltk_.reset();
  }

  const SecurityProperties current_security = security();
  CrossTransportKeyDerivationResultCallback ctkd_cb =
      std::move(bredr_cross_transport_key_derivation_callback_);
  std::queue<PendingRequest> failed_requests = std::move(request_queue_);

  // Reset state before invoking callbacks or disconnecting link to avoid
  // re-entrant double-failure or draining re-entrantly queued requests.
  ResetState();

  if (delegate_.is_alive()) {
    delegate_->OnPairingComplete(fit::error(error));
    if (!self.is_alive()) {
      return;
    }
  }

  while (!failed_requests.empty()) {
    PendingRequest req = std::move(failed_requests.front());
    failed_requests.pop();
    req.callback(fit::error(error), current_security);
  }
  if (!self.is_alive()) {
    return;
  }

  if (ctkd_cb) {
    ctkd_cb(ToResult(HostError::kFailed));
    if (!self.is_alive()) {
      return;
    }
  }

  if (error.is(HostError::kTimedOut)) {
    // Per v5.2 Vol. 3 Part H 3.4, after a pairing timeout "No further SMP
    // commands shall be sent over the L2CAP Security Manager Channel. A new
    // Pairing process shall only be performed when a new physical link has been
    // established."
    bt_log(WARN, "sm", "pairing timed out! disconnecting link");
    sm_chan_->SignalLinkError();
  }
}

void SecurityManagerImpl::UpdateRepeatedAttemptsBackoff() {
  // Core Spec 6.0, Vol 3, Part H, Section 2.3.6: For each subsequent failure,
  // the waiting interval shall be increased exponentially [...] twice as long
  // as the waiting interval prior to the previous attempt [...] limited to a
  // maximum.
  pairing_backoff_expiry_ =
      pw_dispatcher_.now() + next_pairing_waiting_interval_;
  next_pairing_waiting_interval_ =
      std::min(next_pairing_waiting_interval_ * 2, kRepeatedAttemptsMaxTimeout);
}

void SecurityManagerImpl::ResetRepeatedAttemptsBackoff() {
  next_pairing_waiting_interval_ = kRepeatedAttemptsMinTimeout;
  pairing_backoff_expiry_ = pw::chrono::SystemClock::time_point();
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
  PairingPhase* phase = ActivePairingPhase();
  PW_CHECK(phase != nullptr,
           "cannot timeout when current_phase_ has no active PairingPhase!");
  phase->OnFailure(Error(HostError::kTimedOut));
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

void SecurityManagerImpl::RestoreLtk(std::optional<LTK> previous_ltk) {
  ltk_ = std::move(previous_ltk);
  if (!low_energy_link_.is_alive()) {
    return;
  }
  if (ltk_.has_value()) {
    low_energy_link_->set_ltk(ltk_->key());
  } else {
    low_energy_link_->reset_ltk();
  }
}

bool SecurityManagerImpl::StartExistingLtkEncryption(const LTK& ltk) {
  PW_CHECK(low_energy_link_.is_alive());
  const std::optional<LTK> previous_ltk = ltk_;
  if (ltk_ != ltk) {
    OnNewLongTermKey(ltk);
  }
  if (!low_energy_link_->StartEncryption()) {
    bt_log(WARN,
           "sm",
           "failed to start encryption with existing LTK (peer: %s, handle: "
           "%#.4x)",
           bt_str(peer_->identifier()),
           low_energy_link_->handle());
    WeakSelf<SecurityManagerImpl>::WeakPtr self = weak_self_.GetWeakPtr();
    RestoreLtk(previous_ltk);
    if (SecurityUpgradeInProgress()) {
      ResetState();
    }
    FailPendingRequests(Error(ErrorCode::kUnspecifiedReason));
    if (self.is_alive()) {
      sm_chan_->SignalLinkError();
    }
    return false;
  }
  current_phase_ = StartingEncryption();
  return true;
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
    WeakSelf<SecurityManagerImpl>::WeakPtr self = weak_self_.GetWeakPtr();
    if (SecurityUpgradeInProgress()) {
      ResetState();
      FailPendingRequests(status.error_value());
      if (!self.is_alive()) {
        return status;
      }
    }
    delegate_->OnAuthenticationFailure(
        ToResult(pw::bluetooth::emboss::StatusCode::PIN_OR_KEY_MISSING));
    if (!self.is_alive()) {
      return status;
    }
    sm_chan_->SignalLinkError();
  }
  return status;
}

bool SecurityManagerImpl::IsBrEdrCrossTransportKeyDerivationAllowed() const {
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
  std::optional<uint8_t> key_size = bredr_link_->encryption_key_size();
  if (!key_size.has_value()) {
    bt_log(DEBUG, "sm", "%s: encryption key size unknown", __FUNCTION__);
    return false;
  }
  SecurityProperties bredr_security_props(bredr_link_->ltk_type().value(),
                                          key_size.value());
  if (peer_->le() && peer_->le()->bond_data()) {
    const auto& bond_data = peer_->le()->bond_data().value();
    if (bond_data.local_ltk &&
        !bredr_security_props.IsAsSecureAs(bond_data.local_ltk->security())) {
      bt_log(INFO,
             "sm",
             "%s: local LE LTK stronger than current BR/EDR link key",
             __FUNCTION__);
      return false;
    }
    if (bond_data.peer_ltk &&
        !bredr_security_props.IsAsSecureAs(bond_data.peer_ltk->security())) {
      bt_log(INFO,
             "sm",
             "%s: peer LE LTK stronger than current BR/EDR link key",
             __FUNCTION__);
      return false;
    }
  }

  if (peer_->le() && peer_->le()->is_pairing()) {
    bt_log(INFO,
           "sm",
           "%s: LE pairing in progress; disallowing BR/EDR CTKD",
           __FUNCTION__);
    return false;
  }

  return true;
}

bool SecurityManagerImpl::IsBrEdrPairingInProgress() const {
  return (peer_->bredr() && peer_->bredr()->is_pairing()) ||
         std::holds_alternative<WaitForBrEdrPairing>(current_phase_);
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
  std::unique_ptr<SecurityManagerImpl> sm =
      std::make_unique<SecurityManagerImpl>(
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
  if (!sm->Init()) {
    return nullptr;
  }
  return sm;
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
  std::unique_ptr<SecurityManagerImpl> sm =
      std::make_unique<SecurityManagerImpl>(
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
  if (!sm->Init()) {
    return nullptr;
  }
  return sm;
}

SecurityManager::SecurityManager(BondableMode bondable_mode,
                                 gap::LESecurityMode security_mode)
    : low_energy_bondable_mode_(bondable_mode),
      low_energy_security_mode_(security_mode) {}

}  // namespace bt::sm
