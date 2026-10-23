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

#pragma once

#include "pw_bluetooth_sapphire/internal/host/common/identifier.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/gap/pairing_delegate.h"
#include "pw_bluetooth_sapphire/internal/host/gap/peer.h"
#include "pw_bluetooth_sapphire/internal/host/gap/secure_simple_pairing_state.h"
#include "pw_bluetooth_sapphire/internal/host/gap/types.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/link_key.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/hci/bredr_connection.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"

namespace bt::gap {

// Implements event handlers and tracks the state of a peer's BR/EDR link
// throughout the Legacy Pairing process in order to drive HCI and user
// transactions.
//
// Each class instance occurs on a per-connection basis and is destroyed when
// the connection is destroyed.
//
// This class handles both the peer bonded case (both Hosts furnish their Link
// Keys to their Controllers) and the unbonded case (both Controllers perform
// Legacy Pairing and deliver the resulting Link Keys to their Hosts).
//
// Pairing is considered complete when the Link Keys have been used to
// successfully encrypt the link, at which time pairing may be restarted
// (possibly with different capabilities).
class LegacyPairingState final {
 public:
  // The amount of time to wait before retrying enabling encryption if we
  // receive a transaction collision while doing so.
  static constexpr pw::chrono::SystemClock::duration
      kDelayRetryEnableEncryption = std::chrono::seconds(2);

  // Used to report the status of each pairing procedure on this link. The
  // callback's result will contain |HostError::kFailed| if the pairing
  // procedure does not proceed in the order of events expected.
  using StatusCallback =
      fit::function<void(hci_spec::ConnectionHandle, hci::Result<>)>;

  // Constructs a LegacyPairingState to handle pairing protocols, commands, and
  // events to the |peer|, prior to the ACL connection being established. We
  // cannot populate |link_|, |send_auth_request_callback_|, and
  // |status_callback_| until the ACL connection is complete.
  LegacyPairingState(Peer::WeakPtr peer,
                     PairingDelegate::WeakPtr pairing_delegate,
                     bool outgoing_connection,
                     pw::async::Dispatcher* dispatcher);

  // Constructs a LegacyPairingState for the ACL connection |link| to |peer| to
  // handle pairing protocols, commands, and events. |link| must be valid for
  // the lifetime of this object.
  //
  // |outgoing_connection| is true if this device initiated the connection, and
  // false if it was an incoming connection.
  //
  // |auth_cb| will be called to indicate that the device should initiate an
  // HCI_Authentication_Requested command for this |peer|. This should only
  // occur when |outgoing_connection| is true.
  //
  // Successful pairing is reported through |status_cb| after encryption is
  // enabled.
  //
  // This object will be put in a |kFailed| state upon any errors and the owner
  // shall disconnect the link and destroy the LegacyPairingState. When
  // destroyed, status callbacks for any queued pairing requests are called.
  // |status_cb| is not called on destruction.
  LegacyPairingState(Peer::WeakPtr peer,
                     PairingDelegate::WeakPtr pairing_delegate,
                     WeakPtr<hci::BrEdrConnection> link,
                     bool outgoing_connection,
                     pw::async::Dispatcher* dispatcher,
                     fit::closure auth_cb,
                     StatusCallback status_cb);
  ~LegacyPairingState();

  // Sets the |link|'s callbacks fields and link key when the ACL connection is
  // complete (i.e. after HCI_Connection_Complete event). |auth_cb| and
  // |status_cb| are passed in from either the LegacyPairingState or
  // PairingStateManager constructor, depending on which object is first created
  // due to the ordering of legacy pairing events (which may occur before or
  // after the ACL connection is created).
  void BuildEstablishedLink(WeakPtr<hci::BrEdrConnection> link,
                            fit::closure auth_cb,
                            StatusCallback status_cb);

  // Set a handler for user-interactive authentication challenges. If not set or
  // set to nullptr, all pairing requests will be rejected. This does not cause
  // a fatal error and should not result in link disconnection.
  //
  // If the delegate indicates passkey display capabilities, then it will always
  // be asked to confirm pairing, even when Core Spec v5.4, Vol 3, Part C,
  // Section 5.2.2.6 indicates "automatic confirmation".
  void SetPairingDelegate(PairingDelegate::WeakPtr pairing_delegate) {
    pairing_delegate_ = std::move(pairing_delegate);
  }

  // If pairing is not already in progress, this device starts pairing against
  // the peer and becomes the pairing initiator. If pairing is in progress, the
  // request will be queued until the current pairing completes or an additional
  // pairing that upgrades the link key succeeds or fails.
  //
  // If no PairingDelegate is available, |status_cb| is immediately called with
  // |HostError::kNotReady|. The LegacyPairingState status callback (provided in
  // the constructor) is not called.
  //
  // When pairing completes or errors out, the |status_cb| of each call to this
  // function will be invoked with the result.
  void InitiatePairing(StatusCallback status_cb);

  // Caller should send the returned link key in a HCI_Link_Key_Request_Reply
  // (or HCI_Link_Key_Request_Negative_Reply if the returned value is null).
  [[nodiscard]] std::optional<hci_spec::LinkKey> OnLinkKeyRequest();

  // |cb| is called with the pin code value to send HCI_PIN_Code_Request_Reply
  // or std::nullopt to send HCI_PIN_Code_Request_Negative_Reply.
  using UserPinCodeCallback =
      fit::callback<void(std::optional<uint16_t> passkey)>;
  void OnPinCodeRequest(UserPinCodeCallback cb);

  // If the connection is complete, store |link_key| in the connection.
  // Otherwise store temporarily in |link_key_|.
  void OnLinkKeyNotification(const UInt128& link_key,
                             hci_spec::LinkKeyType key_type);

  // Retry pairing if the peer is missing a PIN or link key. Otherwise enable
  // encryption.
  void OnAuthenticationComplete(pw::bluetooth::emboss::StatusCode status_code);

  // Handler for hci::Connection::set_encryption_change_callback.
  void OnEncryptionChange(hci::Result<bool> result);

  // Attach inspect node named |name| as a child of |parent|.
  void AttachInspect(inspect::Node& parent, std::string name);

  // True if there is currently a pairing procedure in progress that the local
  // device initiated.
  bool initiator() const {
    return is_pairing() ? current_pairing_->initiator : false;
  }

  Peer::WeakPtr peer() { return peer_; }

  bool outgoing_connection() const { return outgoing_connection_; }

  sm::SecurityProperties& security_properties() { return bredr_security_; }

  std::optional<hci_spec::LinkKey> link_key() { return link_key_; }

  std::optional<hci_spec::LinkKey> link_ltk() const { return link_->ltk(); }

 private:
  enum class State : uint8_t {
    // Either waiting to locally initiate pairing, or for the pairing
    // initiator's HCI_Link_Key_Request or HCI_PIN_Code_Request_Reply event
    // (depending if the pairing initiator has a valid link key or not
    // respectively).
    kIdle,

    // Wait for HCI_Link_Key_Request event (only when pairing initiator)
    kInitiatorWaitLinkKeyRequest,

    // Wait for HCI_PIN_Code_Request event
    kWaitPinCodeRequest,

    // Wait for the user to provide or confirm a PIN code through the
    // PairingDelegate
    kWaitUserPinCode,

    // Wait for HCI_Link_Key_Notification
    kWaitLinkKey,

    // Wait for HCI_Authentication_Complete event (nly when pairing initiator)
    kInitiatorWaitAuthComplete,

    // Wait for HCI_Encryption_Change event
    kWaitEncryption,

    // Wait for link closure and ignore events due to an error
    kFailed,
  };

  // Extra information for pairing constructed when a pairing procedure begins
  // and destroyed when the pairing procedure is reset or errors out.
  // |outgoing_connection| should be true if this device connected, and false if
  // it was an incoming connection.
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
        bool outgoing_connection,
        Peer::PairingToken&& token,
        pw::async::Dispatcher& dispatcher,
        std::optional<pw::bluetooth::emboss::IoCapability> peer_iocap =
            std::nullopt);
    // Make a responder for a peer that has initiated pairing.
    static std::unique_ptr<Pairing> MakeResponderForBonded(
        Peer::PairingToken&& token, pw::async::Dispatcher& dispatcher);

    // Used to prevent PairingDelegate callbacks from using captured stale
    // pointers.
    using WeakPtr = WeakSelf<Pairing>::WeakPtr;
    Pairing::WeakPtr GetWeakPtr() { return weak_self_.GetWeakPtr(); }

    // True if the local device initiated pairing.
    bool initiator;

    // True if we allow automatic pairing (i.e. not re-pairing and
    // |outgoing_connection_| is true).
    bool allow_automatic;

    // Device's IO capabilities obtained from the pairing delegate.
    pw::bluetooth::emboss::IoCapability local_iocap;

    // Peer's IO capabilities obtained through HCI_IO_Capability_Response.
    pw::bluetooth::emboss::IoCapability peer_iocap;

    // User interaction to perform after receiving HCI user event.
    bt::gap::PairingAction action;

    // HCI event to respond to in order to complete or reject pairing.
    hci_spec::EventCode expected_event;

    // True if this pairing is expected to be resistant to on-path attacks.
    bool authenticated;

    // Security properties of the link key received from the controller.
    std::optional<sm::SecurityProperties> security_properties;

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

  bool is_pairing() const { return current_pairing_ != nullptr; }

  hci_spec::ConnectionHandle handle() const { return link_->handle(); }

  // Called to enable encryption on the link for this peer. Sets |state_| to
  // |kWaitEncryption|.
  void EnableEncryption();

  // Call the permanent status callback this object was created with as well as
  // any completed request callbacks from local initiators. Resets the current
  // pairing and may initiate a new pairing if any requests have not been
  // completed. |caller| is used for logging.
  void SignalStatus(hci::Result<> status, const char* caller);

  // Starts the pairing procedure for the next queued pairing request, if any.
  void InitiateNextPairingRequest();

  // Determines which pairing requests have been completed by the current link
  // key and/or status and removes them from the queue. If any pairing requests
  // were not completed, starts a new pairing procedure. Returns a list of
  // closures that call the status callbacks of completed pairing requests.
  std::vector<fit::closure> CompletePairingRequests(hci::Result<> status);

  // Called when an event is received while in a state that doesn't expect that
  // event. Invokes |status_callback_| with |HostError::kFailed| and sets
  // |state_| to |kFailed|. Logs an error using |handler_name| for
  // identification.
  void FailWithUnexpectedEvent(const char* handler_name);

  enum class ActionOnError { kIgnore, kRetry, kFail };
  ActionOnError GetActionOnError(
      const bt::Error<pw::bluetooth::emboss::StatusCode>& error);

  static const char* ToString(State state);

  PeerId peer_id_;
  Peer::WeakPtr peer_;

  // The BR/EDR link whose pairing is being driven by this object.
  WeakPtr<hci::BrEdrConnection> link_;

  // True when the BR/EDR |link_| was initiated by local device.
  bool outgoing_connection_;

  // Current security properties of the ACL-U link.
  sm::SecurityProperties bredr_security_;

  std::unique_ptr<Pairing> current_pairing_;

  PairingDelegate::WeakPtr pairing_delegate_;

  // Before the ACL connection is complete, we can temporarily store the link
  // key in LegacyPairingState. Once the connection is complete, this value will
  // be stored into the created connection.
  std::optional<hci_spec::LinkKey> link_key_;

  // True when the peer has reported it doesn't have a link key.
  bool peer_missing_key_ = false;

  // State machine representation to track transition between pairing events.
  State state_ = State::kIdle;

  struct PairingRequest {
    // Security properties required by the pairing initiator for pairing to be
    // considered a success.
    BrEdrSecurityRequirements security_requirements;

    // Callback called when the pairing procedure is complete.
    StatusCallback status_callback;
  };
  // Represents ongoing and queued pairing requests. Will contain a value when
  // the state isn't |kIdle| or |kFailed|. Requests may be completed
  // out-of-order as their security requirements are satisfied.
  std::list<PairingRequest> request_queue_;

  // Callback used to indicate an HCI_Authentication_Requested for this peer
  // should be sent.
  fit::closure send_auth_request_callback_;

  // Callback that status of this pairing is reported back through.
  StatusCallback status_callback_;

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

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LegacyPairingState);
};

}  // namespace bt::gap
