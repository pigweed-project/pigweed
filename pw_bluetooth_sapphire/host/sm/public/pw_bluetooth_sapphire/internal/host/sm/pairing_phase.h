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
#include <cpp-string/string_printf.h>
#include <pw_assert/check.h>

#include <string>

#include "pw_bluetooth_sapphire/internal/host/hci/connection.h"
#include "pw_bluetooth_sapphire/internal/host/sm/pairing_channel.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"

namespace bt::sm {

// Abstract class representing one of the four in-progress phases of pairing
// described in Vol. 3 Part H 2.1.
//
// After a PairingPhase fails (i.e. through calling OnFailure), it is invalid to
// make any further method calls on the phase.
using PairingChannelHandler = PairingChannel::Handler;
class PairingPhase {
 public:
  // Interface for notifying the owner of the phase object.
  class Listener {
   public:
    virtual ~Listener() = default;

    // Polls for the local identity information, which must be handled by
    // another component of the Bluetooth stack. Returns std::nullopt if no
    // local identity info is available.
    virtual std::optional<IdentityInfo> OnIdentityRequest() = 0;

    using ConfirmCallback = fit::function<void(bool confirm)>;
    virtual void ConfirmPairing(ConfirmCallback confirm) = 0;

    // Show the user the 6-digit |passkey| that should be compared to the peer's
    // passkey or entered into the peer. |confirm| may be called to accept a
    // comparison or to reject the pairing.
    virtual void DisplayPasskey(uint32_t passkey,
                                Delegate::DisplayMethod method,
                                ConfirmCallback confirm) = 0;

    // Ask the user to enter a 6-digit passkey or reject pairing. Reports the
    // result by invoking |respond| with |passkey| - a negative value of
    // |passkey| indicates entry failed.
    // TODO(fxbug.dev/42126992): Use an optional to convey success/failure
    // instead of the signedness of passkey.
    using PasskeyResponseCallback = fit::function<void(int64_t passkey)>;
    virtual void RequestPasskey(PasskeyResponseCallback respond) = 0;

    // Called when an on-going pairing procedure terminates with an error. This
    // method should destroy the Phase that calls it.
    virtual void OnPairingFailed(Error error) = 0;

    // Returns true if BR/EDR cross-transport key derivation is currently
    // allowed. Only checked when pairing over a BR/EDR ACL link.
    virtual bool IsBrEdrCrossTransportKeyDerivationAllowed() const = 0;

    using WeakPtr = WeakSelf<Listener>::WeakPtr;
  };

  virtual ~PairingPhase() = default;

  // Kick off the state machine for the concrete PairingPhase.
  virtual void Start() = 0;

  // Return a representation of the current state of the pairing phase for
  // display purposes.
  std::string ToString() {
    return bt_lib_cpp_string::StringPrintf(
        "%s Role: SMP %s%s",
        ToStringInternal().c_str(),
        role_ == Role::kInitiator ? "initiator" : "responder",
        has_failed_ ? " - pairing has failed" : "");
  }

  // Cleans up pairing state and and invokes Listener::OnPairingFailed.
  void OnFailure(Error error);

  // Ends the current pairing procedure unsuccessfully with |ecode| as the
  // reason, and calls OnFailure.
  void Abort(ErrorCode ecode);

  Role role() const { return role_; }

 protected:
  // Protected constructor as PairingPhases should not be created directly.
  // Initializes this PairingPhase with the following parameters:
  //   - |chan|: The L2CAP SMP fixed channel.
  //   - |listener|: The class that will handle higher-level requests from the
  //   current phase.
  //   - |role|: The local connection role.
  PairingPhase(PairingChannel::WeakPtr chan,
               Listener::WeakPtr listener,
               Role role);

  // For derived final classes to implement PairingChannel::Handler:
  void HandleChannelClosed();

  PairingChannel& sm_chan() const {
    PW_CHECK(sm_chan_.is_alive());
    return sm_chan_.get();
  }

  Listener::WeakPtr listener() const { return listener_; }

  // Concrete classes of PairingPhase must be PairingChannelHandlers and call
  // this function when the phase is ready to handle requests.
  void SetPairingChannelHandler(PairingChannelHandler& self);

  // Invalidate handling requests to this phase.  This function should only be
  // called once during destruction of the phase.
  void InvalidatePairingChannelHandler();

  // To PW_CHECK that methods are not called on a phase that has already failed.
  bool has_failed() const { return has_failed_; }

  // For subclasses to provide more detailed inspect information.
  virtual std::string ToStringInternal() = 0;

 private:
  PairingChannel::WeakPtr sm_chan_;
  Listener::WeakPtr listener_;
  Role role_;
  bool has_failed_;
  WeakSelf<PairingChannel::Handler> weak_channel_handler_;
  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(PairingPhase);
};

}  // namespace bt::sm
