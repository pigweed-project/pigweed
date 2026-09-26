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
#include <unordered_map>

#include "pw_bluetooth_sapphire/internal/host/common/uint128.h"
#include "pw_bluetooth_sapphire/internal/host/gap/gap.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/link_key.h"
#include "pw_bluetooth_sapphire/internal/host/hci/connection.h"
#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_connection.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/channel.h"
#include "pw_bluetooth_sapphire/internal/host/sm/delegate.h"
#include "pw_bluetooth_sapphire/internal/host/sm/error.h"
#include "pw_bluetooth_sapphire/internal/host/sm/security_manager.h"
#include "pw_bluetooth_sapphire/internal/host/sm/smp.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"

namespace bt::sm::testing {

// TestSecurityManager implements the public interface of the SM library. The
// intended use is in unit tests of code directly dependent on SM (currently,
// GAP). The implementation is currently limited to a basic test spy, with
// stubbed out responses and request tracking for a few functions and empty
// implementations for others.
class TestSecurityManager final : public SecurityManager {
 public:
  ~TestSecurityManager() override;

  // SecurityManager overrides:
  void UpgradeSecurity(SecurityLevel level, PairingCallback callback) override;
  void InitiateBrEdrCrossTransportKeyDerivation(
      CrossTransportKeyDerivationResultCallback) override;
  void Reset(IOCapability io_capability) override;
  void Abort(ErrorCode ecode) override;

  // Returns the most recent security upgrade request received by this SM, if
  // one has been made.
  std::optional<SecurityLevel> last_requested_upgrade() const {
    return last_requested_upgrade_;
  }

  std::optional<sm::IdentityInfo> last_identity_info() const {
    return last_identity_info_;
  }

  // Set pairing data to return to InitiateBrEdrCrossTransportKeyDerivation().
  // If not set, CTKD will fail.
  void set_pairing_data(std::optional<sm::PairingData> data) {
    pairing_data_ = std::move(data);
  }

  void TriggerPairingComplete(sm::PairingData data);

  void set_delay_ctkd(bool delay) { delay_ctkd_ = delay; }
  void trigger_ctkd_callback(sm::Result<> status) {
    if (ctkd_callback_) {
      auto cb = std::move(ctkd_callback_);
      cb(status);
    }
  }

  using WeakPtr = WeakSelf<TestSecurityManager>::WeakPtr;
  WeakPtr GetWeakPtr() { return weak_self_.GetWeakPtr(); }

 private:
  friend class TestSecurityManagerFactory;
  TestSecurityManager(hci::LowEnergyConnection::WeakPtr link,
                      hci::BrEdrConnection::WeakPtr bredr_link,
                      l2cap::Channel::WeakPtr smp,
                      IOCapability io_capability,
                      Delegate::WeakPtr delegate,
                      BondableMode bondable_mode,
                      gap::LESecurityMode security_mode,
                      gap::Peer::WeakPtr peer);
  Role role_;
  std::optional<LTK> current_ltk_;
  std::optional<SecurityLevel> last_requested_upgrade_;
  Delegate::WeakPtr delegate_;
  std::optional<sm::IdentityInfo> last_identity_info_;
  std::optional<sm::PairingData> pairing_data_;
  gap::Peer::WeakPtr peer_;

  bool delay_ctkd_ = false;
  CrossTransportKeyDerivationResultCallback ctkd_callback_;
  l2cap::Channel::WeakPtr smp_;
  WeakSelf<TestSecurityManager> weak_self_;
};

// TestSecurityManagerFactory provides a public factory method to create
// TestSecurityManagers for dependency injection. It stores these TestSMs so
// test code can access them to set and verify expectations. A separate storage
// object is needed because SecurityManagers cannot be directly injected, e.g.
// during construction, as they are created on demand upon connection creation.
// Storing the TestSMs in a factory object is preferable to a static member of
// TestSM itself so that each test is sandboxed from TestSMs in other tests.
// This is done by tying the lifetime of the factory to the test.
class TestSecurityManagerFactory {
 public:
  TestSecurityManagerFactory() = default;
  // Code which uses TestSecurityManagers should create these objects through
  // `CreateSm`. |link|: The LE logical link over which pairing procedures
  // occur. |smp|: The L2CAP LE SMP fixed channel that operates over |link|.
  // |io_capability|: The initial I/O capability.
  // |delegate|: Delegate which handles SMP interactions with the rest of the
  // Bluetooth stack. |bondable_mode|: the operating bondable mode of the device
  // (see v5.2, Vol. 3, Part C 9.4). |security_mode|: the security mode this
  // SecurityManager is in (see v5.2, Vol. 3, Part C 10.2).
  std::unique_ptr<SecurityManager> CreateSm(
      hci::LowEnergyConnection::WeakPtr link,
      l2cap::Channel::WeakPtr smp,
      IOCapability io_capability,
      Delegate::WeakPtr delegate,
      BondableMode bondable_mode,
      gap::LESecurityMode security_mode,
      pw::async::Dispatcher& dispatcher,
      gap::Peer::WeakPtr peer);

  std::unique_ptr<SecurityManager> CreateBrEdr(
      hci::BrEdrConnection::WeakPtr link,
      l2cap::Channel::WeakPtr smp,
      Delegate::WeakPtr delegate,
      bool is_controller_remote_public_key_validation_supported,
      pw::async::Dispatcher& dispatcher,
      bt::gap::Peer::WeakPtr peer);

  // Obtain a reference to the TestSecurityManager associated with
  // |conn_handle|'s connection for use in test code.
  testing::TestSecurityManager::WeakPtr GetTestSm(
      hci_spec::ConnectionHandle conn_handle);

 private:
  std::unordered_map<hci_spec::ConnectionHandle,
                     testing::TestSecurityManager::WeakPtr>
      test_sms_;
};

}  // namespace bt::sm::testing
