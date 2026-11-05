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
#include <memory>

#include "pw_bluetooth_sapphire/internal/host/gap/gap.h"
#include "pw_bluetooth_sapphire/internal/host/gap/peer.h"
#include "pw_bluetooth_sapphire/internal/host/hci/bredr_connection.h"
#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_connection.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/channel.h"
#include "pw_bluetooth_sapphire/internal/host/sm/delegate.h"
#include "pw_bluetooth_sapphire/internal/host/sm/error.h"
#include "pw_bluetooth_sapphire/internal/host/sm/smp.h"
#include "pw_bluetooth_sapphire/internal/host/sm/util.h"

namespace bt::sm {

// SecurityManager provides a per-peer interface to Security Manager Protocol
// functionality in v5.2 Vol. 3 Part H. The peer device must be a LE or
// BR/EDR/LE device. SecurityManager is an abstract class so that SM-dependent
// code can dependency-inject test doubles in unit tests.
//
// The production implementation of SecurityManager is the SecurityManagerImpl
// class, which implements the functionality detailed in README.md. Clients
// should obtain a production object through the SecurityManager::Create factory
// function.
//
// A SecurityManager test double can be obtained through
// `TestSecurityManager::Create`.
//
/// See README.md for more overview of this library.
class SecurityManager {
 public:
  // Factory function which returns a production LE SecurityManager instance, or
  // nullptr if starting encryption with an existing LTK fails:
  // |link|: The LE logical link over which pairing procedures occur.
  // |smp|: The L2CAP LE SMP fixed channel that operates over |link|.
  // |io_capability|: The initial I/O capability.
  // |delegate|: Delegate which handles SMP interactions with the rest of the
  // Bluetooth stack.
  // |bondable_mode|: the operating bondable mode of the device
  // (see v5.2, Vol. 3, Part C 9.4).
  // |security_mode|: the security mode of this SecurityManager (see v5.2, Vol.
  // 3, Part C 10.2).
  // |peer|: The peer that the SMP fixed channel corresponds to.
  static std::unique_ptr<SecurityManager> CreateLE(
      hci::LowEnergyConnection::WeakPtr link,
      l2cap::Channel::WeakPtr smp,
      IOCapability io_capability,
      Delegate::WeakPtr delegate,
      BondableMode bondable_mode,
      gap::LESecurityMode security_mode,
      pw::async::Dispatcher& dispatcher,
      bt::gap::Peer::WeakPtr peer);

  // Factory function that returns a production BR/EDR SecurityManager object.
  // |link|: The BR/EDR link over which the SMP channel operates.
  // |smp|: The L2CAP BR/EDR SMP fixed channel.
  // |delegate|: Delegate which handles SMP interactions with the rest of the
  // Bluetooth stack.
  // |is_controller_remote_public_key_validation_supported|: Indicates
  // controller support for remote public key validation. |peer|: The peer that
  // the link and SMP channel correspond to.
  static std::unique_ptr<SecurityManager> CreateBrEdr(
      hci::BrEdrConnection::WeakPtr link,
      l2cap::Channel::WeakPtr smp,
      Delegate::WeakPtr delegate,
      bool is_controller_remote_public_key_validation_supported,
      pw::async::Dispatcher& dispatcher,
      bt::gap::Peer::WeakPtr peer);

  virtual ~SecurityManager() = default;

  // Attempt to raise the security level of the connection to the desired
  // |level| and notify the result in |callback|.
  //
  // If the desired security properties are already satisfied, this procedure
  // will succeed immediately (|callback| will be run with the current security
  // properties).
  //
  // If a pairing procedure has already been initiated (either by us or the
  // peer), the request will be queued and |callback| will be notified when the
  // procedure completes. If the resulting security level does not satisfy
  // |level|, pairing will be re-initiated. Note that this means security
  // requests of different |level|s may not complete in the order they are made.
  //
  // If no pairing is in progress then the local device will initiate pairing.
  //
  // If pairing fails |callback| will be called with a |status| that represents
  // the error.
  using PairingCallback =
      fit::function<void(Result<> status, const SecurityProperties& sec_props)>;
  virtual void UpgradeSecurity(SecurityLevel level,
                               PairingCallback callback) = 0;

  // Attempt to perform the BR/EDR cross-transport key derivation procedure.
  // |callback| will be called on completion or failure. On success, the LE LTK
  // will be delivered to Delegate::OnNewPairingData() in
  // PairingData::cross_transport_key. Only 1 CTKD procedure is allowed at a
  // time. Additional calls will return an error. Only the Central can initiate
  // CTKD.
  using CrossTransportKeyDerivationResultCallback =
      fit::callback<void(Result<> result)>;
  virtual void InitiateBrEdrCrossTransportKeyDerivation(
      CrossTransportKeyDerivationResultCallback callback) = 0;

  // Assign I/O capabilities. This aborts any ongoing pairing procedure and sets
  // up the I/O capabilities to use for future requests.
  virtual void Reset(IOCapability io_capability) = 0;

  // Abort all ongoing pairing procedures and notify pairing callbacks with the
  // provided error.
  void Abort() { Abort(ErrorCode::kUnspecifiedReason); }
  virtual void Abort(ErrorCode ecode) = 0;

  // Returns the current security properties of the LE link.
  const SecurityProperties& security() const {
    return low_energy_security_properties_;
  }

  // Returns whether or not the SecurityManager is in bondable mode. Note that
  // being in bondable mode does not guarantee that pairing will necessarily
  // bond.
  BondableMode bondable_mode() const { return low_energy_bondable_mode_; }

  // Sets the bondable mode of the SecurityManager. Any in-progress pairings
  // will not be affected - if bondable mode needs to be reset during a pairing
  // Reset() or Abort() must be called first.
  void set_bondable_mode(sm::BondableMode mode) {
    low_energy_bondable_mode_ = mode;
  }

  // Sets the LE Security mode of the SecurityManager - see enum definition for
  // details of each mode. If a security upgrade is in-progress, only takes
  // effect on the next security upgrade.
  void set_security_mode(gap::LESecurityMode mode) {
    low_energy_security_mode_ = mode;
  }
  gap::LESecurityMode security_mode() { return low_energy_security_mode_; }

 protected:
  SecurityManager(BondableMode bondable_mode,
                  gap::LESecurityMode security_mode);
  void set_security(SecurityProperties security) {
    low_energy_security_properties_ = security;
  }

 private:
  // The operating bondable mode of the device.
  BondableMode low_energy_bondable_mode_ = BondableMode::Bondable;

  // The current GAP security mode of the device (v5.2 Vol. 3 Part C
  // Section 10.2)
  gap::LESecurityMode low_energy_security_mode_ = gap::LESecurityMode::Mode1;

  // Current security properties of the LE-U link.
  SecurityProperties low_energy_security_properties_ = SecurityProperties();
};

using SecurityManagerFactory =
    std::function<decltype(sm::SecurityManager::CreateLE)>;

using BrEdrSecurityManagerFactory =
    std::function<decltype(sm::SecurityManager::CreateBrEdr)>;

}  // namespace bt::sm
