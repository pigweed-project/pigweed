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

#include <list>
#include <optional>
#include <vector>

#include "pw_bluetooth_sapphire/internal/host/common/identifier.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/gap/gap.h"
#include "pw_bluetooth_sapphire/internal/host/gap/pairing_delegate.h"
#include "pw_bluetooth_sapphire/internal/host/gap/peer.h"
#include "pw_bluetooth_sapphire/internal/host/gap/types.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/hci/bredr_connection.h"
#include "pw_bluetooth_sapphire/internal/host/hci/local_address_delegate.h"
#include "pw_bluetooth_sapphire/internal/host/sm/security_manager.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"
#include "pw_bluetooth_sapphire/internal/host/transport/error.h"

namespace bt::gap {

// Represents the local user interaction that will occur, as inferred from Core
// Spec v5.0 Vol 3, Part C, Sec 5.2.2.6 (Table 5.7). This is not directly
// coupled to the reply action for the HCI "User" event for pairing; e.g.
// kDisplayPasskey may mean automatically confirming User Confirmation Request
// or displaying the value from User Passkey Notification.
enum class PairingAction {
  // Don't involve the user.
  kAutomatic,

  // Request yes/no consent.
  kGetConsent,

  // Display 6-digit value with "cancel."
  kDisplayPasskey,

  // Display 6-digit value with "yes/no."
  kComparePasskey,

  // Request a 6-digit value entry.
  kRequestPasskey,
};

// Tracks the pairing state of a peer's BR/EDR link. This drives HCI
// transactions and user interactions for pairing in order to obtain the highest
// possible level of link security given the capabilities of the controllers
// and hosts participating in the pairing.
//
// This implements Core Spec v5.0 Vol 2, Part F, Sec 4.2 through Sec 4.4, per
// logic requirements in Vol 3, Part C, Sec 5.2.2.
//
// This tracks both the bonded case (both hosts furnish their Link Keys to their
// controllers) and the unbonded case (both controllers perform Secure Simple
// Pairing and deliver the resulting Link Keys to their hosts).
//
// Pairing is considered complete when the Link Keys have been used to
// successfully encrypt the link, at which time pairing may be restarted (e.g.
// with different capabilities).
//
// This state machine navigates the following HCI message sequences, in which
// both the host subsystem and the Link Manager use knowledge of both peers' IO
// Capabilities and Authentication Requirements to decide on the same
// association model.
// ▶ means command.
// ◀ means event.
//
// Initiator flow
// --------------
// Authentication Requested▶
// (◀ Authentication Complete with an error is possible at any time after this)
//     ◀ Link Key Request
// Link Key Request Reply▶ (skip to "Authentication Complete")
//     or
// Link Key Request Negative Reply▶ (continue with pairing)
//     ◀ Command Complete
//     ◀ IO Capability Request
// (◀ Simple Pairing Complete with an error is possible at any time after this)
// IO Capability Request Reply▶
//     or
// IO Capability Request Negative Reply▶ (reject pairing)
//     ◀ Command Complete
//     ◀ IO Capability Response
//     ◀ User Confirmation Request
//         or
//     ◀ User Passkey Request
//         or
//     ◀ User Passkey Notification
//         or
//     ◀ Remote OOB Data Request
// User Confirmation Request Reply▶
//     or
// User Confirmation Request Negative Reply▶ (reject pairing)
//     or
// User Passkey Request Reply▶
//     or
// User Passkey Request Negative Reply▶ (reject pairing)
//     or
// Remote OOB Data Request Reply▶
//     or
// Remote OOB Extended Data Request Reply▶
//     or
// Remote OOB Data Request Negative Reply▶ (reject pairing)
//     ◀ Simple Pairing Complete (status may be error)
//     ◀ Link Key Notification (key may be insufficient)
//     ◀ Authentication Complete (status may be error)
//       If status is PIN or Key missing, return to:
//         Authentication Requested▶ (use Link Key Request Negative Reply)
// Set Connection Encryption▶
//     ◀ Command Status
//     ◀ Encryption Change (status may be error or encryption may be disabled)
// Cross transport key derivation procedure (if central)
//
// Responder flow
// --------------
// If initiator has key:
//     ◀ Link Key Request
// Link Key Request Reply▶ (skip to "Encryption Change")
//     or
// Link Key Request Negative Reply▶ (Authentication failed, skip pairing)
//
// If initiator doesn't have key:
//     ◀ IO Capability Response
//     ◀ IO Capability Request
// (◀ Simple Pairing Complete with an error is possible at any time after this)
// IO Capability Request Reply▶
//     or
// IO Capability Request Negative Reply▶ (reject pairing)
//     ◀ Command Complete
// Pairing
//     ◀ User Confirmation Request
//         or
//     ◀ User Passkey Request
//         or
//     ◀ User Passkey Notification
//         or
//     ◀ Remote OOB Data Request
// User Confirmation Request Reply▶
//     or
// User Confirmation Request Negative Reply▶ (reject pairing)
//     or
// User Passkey Request Reply▶
//     or
// User Passkey Request Negative Reply▶ (reject pairing)
//     or
// Remote OOB Data Request Reply▶
//     or
// Remote OOB Extended Data Request Reply▶
//     or
// Remote OOB Data Request Negative Reply▶ (reject pairing)
//     ◀ Simple Pairing Complete (status may contain error)
//     ◀ Link Key Notification (key may be insufficient)
// Set Connection Encryption▶
//     ◀ Command Status
//     ◀ Encryption Change (status may be error or encryption may be disabled)
// Cross transport key derivation procedure (if central)
//
// This class is not thread-safe and should only be called on the thread on
// which it was created.
class SecureSimplePairingState final {
 public:
  // The amount of time to wait before retrying enabling encryption if we
  // receive a transaction collision while doing so.
  static constexpr pw::chrono::SystemClock::duration
      kDelayRetryEnableEncryption = std::chrono::seconds(2);

  // Used to report the status of each pairing procedure on this link. |status|
  // will contain HostError::kNotSupported if the pairing procedure does not
  // proceed in the order of events expected.
  using StatusCallback =
      fit::function<void(hci_spec::ConnectionHandle, hci::Result<>)>;

  // Constructs a SecureSimplePairingState for the ACL connection |link| to
  // |peer_id|. |outgoing_connection| should be true if this device connected,
  // and false if it was an incoming connection. This object will receive
  // "encryption change" callbacks associate with |peer_id|. Successful pairing
  // is reported through |status_cb| after encryption is enabled. When errors
  // occur, this object will be put in a "failed" state and the owner shall
  // disconnect the link and destroy its SecureSimplePairingState.  When
  // destroyed, status callbacks for any waiting pairings are called.
  // |status_cb| is not called on destruction.
  //
  // |auth_cb| will be called to indicate that the caller should send an
  // Authentication Request for this peer.
  //
  // |link| must be valid for the lifetime of this object.
  SecureSimplePairingState(
      Peer::WeakPtr peer,
      PairingDelegate::WeakPtr pairing_delegate,
      WeakPtr<hci::BrEdrConnection> link,
      bool outgoing_connection,
      fit::closure auth_cb,
      StatusCallback status_cb,
      hci::LocalAddressDelegate* low_energy_address_delegate,
      bool controller_remote_public_key_validation_supported,
      sm::BrEdrSecurityManagerFactory security_manager_factory,
      pw::async::Dispatcher& dispatcher);
  ~SecureSimplePairingState();

  // True if there is currently a pairing procedure in progress that the local
  // device initiated.
  bool initiator() const {
    return is_pairing() ? current_pairing_->initiator : false;
  }

  // Set a handler for user-interactive authentication challenges. If not set or
  // set to nullptr, all pairing requests will be rejected, but this does not
  // cause a fatal error and should not result in link disconnection.
  //
  // If the delegate indicates passkey display capabilities, then it will always
  // be asked to confirm pairing, even when Core Spec v5.0, Vol 3, Part C,
  // Section 5.2.2.6 indicates "automatic confirmation."
  void SetPairingDelegate(PairingDelegate::WeakPtr pairing_delegate) {
    pairing_delegate_ = std::move(pairing_delegate);
  }

  // Starts pairing against the peer, if pairing is not already in progress.
  // If not, this device becomes the pairing initiator. If pairing is in
  // progress, the request will be queued until the current pairing completes or
  // an additional pairing that upgrades the link key succeeds or fails.
  //
  // If no PairingDelegate is available, |status_cb| is immediately called with
  // HostError::kNotReady, but the SecureSimplePairingState status callback
  // (provided in the ctor) is not called.
  //
  // When pairing completes or errors out, the |status_cb| of each call to this
  // function will be invoked with the result.
  void InitiatePairing(BrEdrSecurityRequirements security_requirements,
                       StatusCallback status_cb);

  // Event handlers. Caller must ensure that the event is addressed to the link
  // for this SecureSimplePairingState.

  // Returns value for IO Capability Request Reply, else std::nullopt for IO
  // Capability Negative Reply.
  //
  // TODO(fxbug.dev/42138242): Indicate presence of out-of-band (OOB) data.
  [[nodiscard]] std::optional<pw::bluetooth::emboss::IoCapability>
  OnIoCapabilityRequest();

  // Caller is not expected to send a response.
  void OnIoCapabilityResponse(pw::bluetooth::emboss::IoCapability peer_iocap);

  // |cb| is called with: true to send User Confirmation Request Reply, else
  // for to send User Confirmation Request Negative Reply. It may be called from
  // a different thread than the one that called OnUserConfirmationRequest.
  using UserConfirmationCallback = fit::callback<void(bool confirm)>;
  void OnUserConfirmationRequest(uint32_t numeric_value,
                                 UserConfirmationCallback cb);

  // |cb| is called with: passkey value to send User Passkey Request Reply, else
  // std::nullopt to send User Passkey Request Negative Reply. It may not be
  // called from the same thread that called OnUserPasskeyRequest.
  using UserPasskeyCallback =
      fit::callback<void(std::optional<uint32_t> passkey)>;
  void OnUserPasskeyRequest(UserPasskeyCallback cb);

  // Caller is not expected to send a response.
  void OnUserPasskeyNotification(uint32_t numeric_value);

  // Caller is not expected to send a response.
  void OnSimplePairingComplete(pw::bluetooth::emboss::StatusCode status_code);

  // Caller should send the returned link key in a Link Key Request Reply (or
  // Link Key Request Negative Reply if the returned value is null).
  [[nodiscard]] std::optional<hci_spec::LinkKey> OnLinkKeyRequest();

  // Caller is not expected to send a response.
  void OnLinkKeyNotification(const UInt128& link_key,
                             hci_spec::LinkKeyType key_type,
                             bool local_secure_connections_supported = false);

  // Caller is not expected to send a response.
  void OnAuthenticationComplete(pw::bluetooth::emboss::StatusCode status_code);

  // Handler for hci::Connection::set_encryption_change_callback.
  void OnEncryptionChange(hci::Result<bool> result);

  sm::SecurityProperties& security_properties() { return bredr_security_; }

  // Sets the BR/EDR Security Mode of the pairing state - see enum definition
  // for details of each mode. If a security upgrade is in-progress, only takes
  // effect on the next security upgrade.
  void set_security_mode(gap::BrEdrSecurityMode mode) { security_mode_ = mode; }

  void SetSecurityManagerChannel(
      l2cap::Channel::WeakPtr security_manager_channel);

  // Attach pairing state inspect node named |name| as a child of |parent|.
  void AttachInspect(inspect::Node& parent, std::string name);

 private:
  // Current security properties of the ACL-U link.
  sm::SecurityProperties bredr_security_;

  enum class State {
    // Wait for initiator's IO Capability Response, Link Key Request, or for
    // locally-initiated
    // pairing.
    kIdle,

    // As initiator, wait for the Low Energy pairing procedure to complete
    // (before doing SSP).
    kInitiatorWaitLEPairingComplete,

    // As initiator, wait for Link Key Request.
    kInitiatorWaitLinkKeyRequest,

    // As initiator, wait for IO Capability Request.
    kInitiatorWaitIoCapRequest,

    // As initiator, wait for IO Capability Response.
    kInitiatorWaitIoCapResponse,

    // As responder, wait for IO Capability Request.
    kResponderWaitIoCapRequest,

    // Wait for controller event for pairing action. Only one of these will
    // occur in a given pairing
    // (see class documentation for pairing flow).
    kWaitUserConfirmationRequest,
    kWaitUserPasskeyRequest,
    kWaitUserPasskeyNotification,

    // Wait for Simple Pairing Complete.
    kWaitPairingComplete,

    // Wait for Link Key Notification.
    kWaitLinkKey,

    // As initiator, wait for Authentication Complete.
    kInitiatorWaitAuthComplete,

    // Wait for Encryption Change.
    kWaitEncryption,

    // Wait for CTKD to complete over SMP. This state is only used as Central.
    kWaitCrossTransportKeyDerivation,

    // Error occurred; wait for link closure and ignore events.
    kFailed,
  };

  // Extra information for pairing constructed when a pairing procedure begins
  // and destroyed when the pairing procedure is reset or errors
  // out. |outgoing_connection| should be true if this device connected, and
  // false if it was an incoming connection.
  //
  // Instances must be heap allocated so that they can be moved without
  // destruction, preserving their WeakPtr holders. WeakPtrs are vended to
  // PairingDelegate callbacks to uniquely identify each attempt to pair because
  // |current_pairing_| is not synchronized to the user's actions through
  // PairingDelegate.
  class Pairing final {
   public:
    static std::unique_ptr<Pairing> MakeInitiator(
        BrEdrSecurityRequirements security_requirements,
        bool outgoing_connection,
        Peer::PairingToken&& token,
        pw::async::Dispatcher& dispatcher);
    static std::unique_ptr<Pairing> MakeResponder(
        pw::bluetooth::emboss::IoCapability peer_iocap,
        bool allow_automatic,
        Peer::PairingToken&& token,
        pw::async::Dispatcher& dispatcher);
    // Make a responder for a peer that has initiated a pairing (asked for our
    // key while in idle)
    static std::unique_ptr<Pairing> MakeResponderForBonded(
        Peer::PairingToken&& token, pw::async::Dispatcher& dispatcher);

    // For a Pairing whose |initiator|, |local_iocap|, and |peer_iocap| are
    // already set, compute and set |action|, |expected_event|, |authenticated|,
    // and |security_properties| for the pairing procedure and bonding data that
    // we expect.
    void ComputePairingData();

    // Used to prevent PairingDelegate callbacks from using captured stale
    // pointers.
    using WeakPtr = WeakSelf<Pairing>::WeakPtr;
    Pairing::WeakPtr GetWeakPtr() { return weak_self_.GetWeakPtr(); }

    // True if the local device initiated pairing.
    bool initiator;

    // True if we allow automatic pairing. (when outgoing connection and not
    // re-pairing)
    bool allow_automatic;

    // IO Capability obtained from the pairing delegate.
    pw::bluetooth::emboss::IoCapability local_iocap;

    // IO Capability from peer through IO Capability Response.
    pw::bluetooth::emboss::IoCapability peer_iocap;

    // User interaction to perform after receiving HCI user event.
    PairingAction action;

    // HCI event to respond to in order to complete or reject pairing.
    hci_spec::EventCode expected_event;

    // inclusive-language: ignore
    // True if this pairing is expected to be resistant to MITM attacks.
    bool authenticated;

    // Security properties of the link key received from the controller.
    std::optional<sm::SecurityProperties> received_link_key_security_properties;

    // If the preferred security is greater than the existing link key, a new
    // link key will be negotiated (which may still have insufficient security
    // properties).
    BrEdrSecurityRequirements preferred_security;

    Peer::PairingToken pairing_token;

    SmartTask retry_enable_encryption_task;

   private:
    Pairing(bool automatic,
            Peer::PairingToken&& token,
            pw::async::Dispatcher& dispatcher)
        : allow_automatic(automatic),
          pairing_token(std::move(token)),
          retry_enable_encryption_task(dispatcher),
          weak_self_(this) {}

    WeakSelf<Pairing> weak_self_;
  };

  class SecurityManagerDelegate final
      : public sm::Delegate,
        public WeakSelf<SecurityManagerDelegate> {
   public:
    SecurityManagerDelegate(SecureSimplePairingState* state)
        : WeakSelf(this), ssp_state_(state) {}

   private:
    std::optional<sm::IdentityInfo> OnIdentityInformationRequest() override;

    // These methods are not used in BR/EDR.
    void OnPairingComplete(sm::Result<>) override {}
    void ConfirmPairing(ConfirmCallback) override {}
    void DisplayPasskey(uint32_t, DisplayMethod, ConfirmCallback) override {}
    void RequestPasskey(PasskeyResponseCallback) override {}
    void OnAuthenticationFailure(hci::Result<>) override {}
    void OnNewSecurityProperties(const sm::SecurityProperties&) override {}

    SecureSimplePairingState* ssp_state_;
  };

  static const char* ToString(State state);

  // Returns state for the three pairing action events, kFailed otherwise.
  static State GetStateForPairingEvent(hci_spec::EventCode event_code);

  // Peer for this pairing.
  PeerId peer_id() const { return peer_id_; }

  State state() const { return state_; }

  bool is_pairing() const { return current_pairing_ != nullptr; }

  hci_spec::ConnectionHandle handle() const { return link_->handle(); }

  // Returns nullptr if the delegate is not set or no longer alive.
  const PairingDelegate::WeakPtr& pairing_delegate() const {
    return pairing_delegate_;
  }

  // Call the permanent status callback this object was created with as well as
  // any completed request callbacks from local initiators. Resets the current
  // pairing and may initiate a new pairing if any requests have not been
  // completed. |caller| is used for logging.
  void SignalStatus(hci::Result<> status, const char* caller);

  // Determines which pairing requests have been completed by the current link
  // key and/or status and removes them from the queue. If any pairing requests
  // were not completed, starts a new pairing procedure. Returns a list of
  // closures that call the status callbacks of completed pairing requests.
  std::vector<fit::closure> CompletePairingRequests(hci::Result<> status);

  // Starts the pairing procedure for the next queued pairing request, if any.
  void InitiateNextPairingRequest();

  // Called to enable encryption on the link for this peer. Sets |state_| to
  // kWaitEncryption.
  void EnableEncryption();

  // Called when an event is received while in a state that doesn't expect that
  // event. Invokes |status_callback_| with HostError::kNotSupported and sets
  // |state_| to kFailed. Logs an error using |handler_name| for identification.
  void FailWithUnexpectedEvent(const char* handler_name);

  // Returns true when the peer's host and peer's controller support Secure
  // Connections
  bool IsPeerSecureConnectionsSupported() const;

  void OnCrossTransportKeyDerivationComplete(sm::Result<> result);

  enum class ActionOnError { kIgnore, kRetry, kFail };
  ActionOnError GetActionOnError(
      const bt::Error<pw::bluetooth::emboss::StatusCode>& error);

  PeerId peer_id_;
  Peer::WeakPtr peer_;

  // The current GAP security mode of the device (v5.2 Vol. 3 Part C
  // Section 5.2.2)
  gap::BrEdrSecurityMode security_mode_ = gap::BrEdrSecurityMode::Mode4;

  // The BR/EDR link whose pairing is being driven by this object.
  WeakPtr<hci::BrEdrConnection> link_;

  // True when the BR/EDR |link_| was locally requested.
  bool outgoing_connection_;

  // True when the remote device has reported it doesn't have a link key.
  bool peer_missing_key_;

  hci::LocalAddressDelegate* low_energy_address_delegate_;

  PairingDelegate::WeakPtr pairing_delegate_;

  // State machine representation.
  State state_;

  std::unique_ptr<Pairing> current_pairing_;

  struct PairingRequest {
    // Security properties required by the pairing initiator for pairing to be
    // considered a success.
    BrEdrSecurityRequirements security_requirements;

    // Callback called when the pairing procedure is complete.
    StatusCallback status_callback;
  };
  // Represents ongoing and queued pairing requests. Will contain a value when
  // the state isn't kIdle or kFailed. Requests may be completed out-of-order as
  // their security requirements are satisfied.
  std::list<PairingRequest> request_queue_;

  // Callback used to indicate an Authentication Request for this peer should be
  // sent.
  fit::closure send_auth_request_callback_;

  // Callback that status of this pairing is reported back through.
  StatusCallback status_callback_;

  // Cleanup work that should occur only once per connection; uniqueness is
  // guaranteed by being moved with SecureSimplePairingState. |self| shall be a
  // pointer to the moved-to instance being cleaned up.
  fit::callback<void(SecureSimplePairingState* self)> cleanup_cb_;

  bool controller_remote_public_key_validation_supported_;
  SecurityManagerDelegate security_manager_delegate_{this};
  sm::BrEdrSecurityManagerFactory security_manager_factory_;
  std::unique_ptr<sm::SecurityManager> security_manager_;

  pw::async::Dispatcher& dispatcher_;

  struct InspectProperties {
    inspect::StringProperty encryption_status;
  };
  InspectProperties inspect_properties_;

  struct InspectMetrics {
    UintMetricCounter central_lmp_transaction_collision;
    UintMetricCounter peripheral_lmp_transaction_collision;
    UintMetricCounter central_different_transaction_collision;
    UintMetricCounter peripheral_different_transaction_collision;
  };
  InspectMetrics inspect_metrics_;

  inspect::Node inspect_node_;

  WeakSelf<SecureSimplePairingState> weak_self_{this};

  BT_DISALLOW_COPY_ASSIGN_AND_MOVE(SecureSimplePairingState);
};

PairingAction GetInitiatorPairingAction(
    pw::bluetooth::emboss::IoCapability initiator_cap,
    pw::bluetooth::emboss::IoCapability responder_cap);
PairingAction GetResponderPairingAction(
    pw::bluetooth::emboss::IoCapability initiator_cap,
    pw::bluetooth::emboss::IoCapability responder_cap);
hci_spec::EventCode GetExpectedEvent(
    pw::bluetooth::emboss::IoCapability local_cap,
    pw::bluetooth::emboss::IoCapability peer_cap);
bool IsPairingAuthenticated(pw::bluetooth::emboss::IoCapability local_cap,
                            pw::bluetooth::emboss::IoCapability peer_cap);

// Get the Authentication Requirements for a locally-initiated pairing according
// to Core Spec v5.0, Vol 2, Part E, Sec 7.1.29.
//
// Non-Bondable Mode and Dedicated Bonding over BR/EDR are not supported and
// this always returns kMITMGeneralBonding if |local_cap| is not
// kNoInputNoOutput, kGeneralBonding otherwise. This requests authentication
// when possible (based on IO Capabilities), as we don't know the peer's
// authentication requirements yet.
pw::bluetooth::emboss::AuthenticationRequirements
GetInitiatorAuthenticationRequirements(
    pw::bluetooth::emboss::IoCapability local_cap);

// Get the Authentication Requirements for a peer-initiated pairing. This will
// inclusive-language: ignore
// request MITM protection whenever possible to obtain an "authenticated" link
// encryption key.
//
// Local service requirements and peer authentication bonding type should be
// available by the time this is called, but Non-Bondable Mode and Dedicated
// Bonding over BR/EDR are not supported, so this always returns
// kMITMGeneralBonding if this pairing can result in an authenticated link key,
// kGeneralBonding otherwise.
pw::bluetooth::emboss::AuthenticationRequirements
GetResponderAuthenticationRequirements(
    pw::bluetooth::emboss::IoCapability local_cap,
    pw::bluetooth::emboss::IoCapability peer_cap);

}  // namespace bt::gap
