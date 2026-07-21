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

// inclusive-language: disable

#include "pw_bluetooth_sapphire/internal/host/gap/secure_simple_pairing_state.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "pw_bluetooth_sapphire/internal/host/gap/fake_pairing_delegate.h"
#include "pw_bluetooth_sapphire/internal/host/gap/peer_cache.h"
#include "pw_bluetooth_sapphire/internal/host/hci/fake_bredr_connection.h"
#include "pw_bluetooth_sapphire/internal/host/sm/test_security_manager.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"
#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_peer.h"
#include "pw_bluetooth_sapphire/internal/host/testing/gtest_helpers.h"
#include "pw_bluetooth_sapphire/internal/host/testing/inspect.h"
#include "pw_bluetooth_sapphire/internal/host/testing/inspect_util.h"
#include "pw_bluetooth_sapphire/internal/host/testing/mock_controller.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_packets.h"
#include "pw_bluetooth_sapphire/internal/host/transport/error.h"

namespace bt::gap {
namespace {

using namespace inspect::testing;

using hci::testing::FakeBrEdrConnection;
using hci_spec::kUserConfirmationRequestEventCode;
using hci_spec::kUserPasskeyNotificationEventCode;
using hci_spec::kUserPasskeyRequestEventCode;
using pw::bluetooth::emboss::AuthenticationRequirements;
using pw::bluetooth::emboss::IoCapability;

const DeviceAddress kLEIdentityAddress(DeviceAddress::Type::kLEPublic,
                                       {0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA});
const UInt128 kIrk{{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}};
const bt::sm::LTK kLtk(sm::SecurityProperties(/*encrypted=*/true,
                                              /*authenticated=*/true,
                                              /*secure_connections=*/true,
                                              sm::kMaxEncryptionKeySize),
                       hci_spec::LinkKey(UInt128{4}, 5, 6));

const hci_spec::ConnectionHandle kTestHandle(0x0A0B);
const DeviceAddress kLocalAddress(DeviceAddress::Type::kBREDR,
                                  {0x22, 0x11, 0x00, 0xCC, 0xBB, 0xAA});
const DeviceAddress kPeerAddress(DeviceAddress::Type::kBREDR,
                                 {0x99, 0x88, 0x77, 0xFF, 0xEE, 0xDD});
const auto kTestLocalIoCap = sm::IOCapability::kDisplayYesNo;
const auto kTestPeerIoCap = IoCapability::DISPLAY_ONLY;
const uint32_t kTestPasskey = 123456;
const auto kTestLinkKeyValue = UInt128{0x00,
                                       0x00,
                                       0x00,
                                       0x00,
                                       0x00,
                                       0x00,
                                       0x00,
                                       0x00,
                                       0x00,
                                       0x00,
                                       0x00,
                                       0x00,
                                       0x00,
                                       0x00,
                                       0x00,
                                       0x01};
const hci_spec::LinkKey kTestLinkKey(kTestLinkKeyValue, 0, 0);
const auto kTestUnauthenticatedLinkKeyType192 =
    hci_spec::LinkKeyType::kUnauthenticatedCombination192;
const auto kTestAuthenticatedLinkKeyType192 =
    hci_spec::LinkKeyType::kAuthenticatedCombination192;
const auto kTestUnauthenticatedLinkKeyType256 =
    hci_spec::LinkKeyType::kUnauthenticatedCombination256;
const auto kTestLegacyLinkKeyType = hci_spec::LinkKeyType::kCombination;
const auto kTestChangedLinkKeyType = hci_spec::LinkKeyType::kChangedCombination;
const BrEdrSecurityRequirements kNoSecurityRequirements{
    .authentication = false, .secure_connections = false};

void NoOpStatusCallback(hci_spec::ConnectionHandle, hci::Result<>) {}
void NoOpUserConfirmationCallback(bool) {}
void NoOpUserPasskeyCallback(std::optional<uint32_t>) {}

class NoOpPairingDelegate final : public PairingDelegate {
 public:
  NoOpPairingDelegate(sm::IOCapability io_capability)
      : io_capability_(io_capability), weak_self_(this) {}

  PairingDelegate::WeakPtr GetWeakPtr() { return weak_self_.GetWeakPtr(); }

  // PairingDelegate overrides that do nothing.
  ~NoOpPairingDelegate() override = default;
  sm::IOCapability io_capability() const override { return io_capability_; }
  void CompletePairing(PeerId, sm::Result<>) override {}
  void ConfirmPairing(PeerId, ConfirmCallback) override {}
  void DisplayPasskey(PeerId,
                      uint32_t,
                      DisplayMethod,
                      ConfirmCallback) override {}
  void RequestPasskey(PeerId, PasskeyResponseCallback) override {}

 private:
  const sm::IOCapability io_capability_;
  WeakSelf<PairingDelegate> weak_self_;
};

using TestBase = testing::FakeDispatcherControllerTest<testing::MockController>;
class PairingStateTest : public TestBase, public hci::LocalAddressDelegate {
 public:
  PairingStateTest() = default;
  ~PairingStateTest() override = default;

  void SetUp() override {
    TestBase::SetUp();
    InitializeACLDataChannel();

    peer_cache_ = std::make_unique<PeerCache>(dispatcher());
    peer_ = peer_cache_->NewPeer(kPeerAddress, /*connectable=*/true);

    auth_request_count_ = 0;
    send_auth_request_callback_ = [this]() { auth_request_count_++; };

    connection_ = MakeFakeConnection();
  }

  void TearDown() override {
    peer_ = nullptr;
    peer_cache_ = nullptr;

    EXPECT_CMD_PACKET_OUT(test_device(),
                          testing::DisconnectPacket(kTestHandle));
    connection_.reset();

    TestBase::TearDown();
  }

  fit::closure MakeAuthRequestCallback() {
    return send_auth_request_callback_.share();
  }

  std::unique_ptr<FakeBrEdrConnection> MakeFakeConnection() {
    return std::make_unique<FakeBrEdrConnection>(
        kTestHandle,
        kLocalAddress,
        kPeerAddress,
        pw::bluetooth::emboss::ConnectionRole::CENTRAL,
        transport()->GetWeakPtr(),
        dispatcher());
  }

  FakeBrEdrConnection* connection() const { return connection_.get(); }
  PeerCache* peer_cache() const { return peer_cache_.get(); }
  Peer* peer() const { return peer_; }
  size_t auth_request_count() const { return auth_request_count_; }
  sm::testing::TestSecurityManagerFactory* sm_factory() {
    return &security_manager_factory_;
  }
  sm::BrEdrSecurityManagerFactory sm_factory_func() {
    return fit::bind_member<
        &sm::testing::TestSecurityManagerFactory::CreateBrEdr>(sm_factory());
  }

 private:
  // LocalAddressDelegate overrides:
  std::optional<UInt128> irk() const override { return kIrk; }
  DeviceAddress identity_address() const override { return kLEIdentityAddress; }
  void EnsureLocalAddress(std::optional<DeviceAddress::Type>,
                          AddressCallback) override {
    ADD_FAILURE();
  }

  std::unique_ptr<PeerCache> peer_cache_;
  Peer* peer_;
  size_t auth_request_count_;
  fit::closure send_auth_request_callback_;
  sm::testing::TestSecurityManagerFactory security_manager_factory_;
  std::unique_ptr<FakeBrEdrConnection> connection_;
};

class PairingStateDeathTest : public PairingStateTest {};

TEST_F(PairingStateTest, PairingStateStartsAsResponder) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      NoOpStatusCallback,
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());
  EXPECT_FALSE(pairing_state.initiator());
}

TEST_F(PairingStateTest, PairingStateRemainsResponderAfterPeerIoCapResponse) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      NoOpStatusCallback,
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());
  pairing_state.OnIoCapabilityResponse(kTestPeerIoCap);
  EXPECT_EQ(0u, auth_request_count());
  EXPECT_FALSE(pairing_state.initiator());
}

TEST_F(PairingStateTest,
       PairingStateBecomesInitiatorAfterLocalPairingInitiated) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      NoOpStatusCallback,
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());
  pairing_state.InitiatePairing(kNoSecurityRequirements, NoOpStatusCallback);
  EXPECT_EQ(1u, auth_request_count());
  EXPECT_TRUE(pairing_state.initiator());
  EXPECT_TRUE(peer()->MutBrEdr().is_pairing());
}

TEST_F(PairingStateTest,
       PairingStateSendsAuthenticationRequestOnceForDuplicateRequest) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      NoOpStatusCallback,
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  pairing_state.InitiatePairing(kNoSecurityRequirements, NoOpStatusCallback);
  EXPECT_EQ(1u, auth_request_count());
  EXPECT_TRUE(pairing_state.initiator());

  pairing_state.InitiatePairing(kNoSecurityRequirements, NoOpStatusCallback);
  EXPECT_EQ(1u, auth_request_count());
  EXPECT_TRUE(pairing_state.initiator());
}

TEST_F(
    PairingStateTest,
    PairingStateRemainsResponderIfPairingInitiatedWhileResponderPairingInProgress) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      NoOpStatusCallback,
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());
  pairing_state.OnIoCapabilityResponse(kTestPeerIoCap);
  EXPECT_TRUE(peer()->MutBrEdr().is_pairing());
  ASSERT_FALSE(pairing_state.initiator());

  pairing_state.InitiatePairing(kNoSecurityRequirements, NoOpStatusCallback);
  EXPECT_EQ(0u, auth_request_count());
  EXPECT_FALSE(pairing_state.initiator());
}

TEST_F(PairingStateTest, StatusCallbackMayDestroyPairingState) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  std::unique_ptr<SecureSimplePairingState> pairing_state;
  bool cb_called = false;
  auto status_cb = [&pairing_state, &cb_called](hci_spec::ConnectionHandle,
                                                hci::Result<> status) {
    EXPECT_TRUE(status.is_error());
    cb_called = true;

    // Note that this lambda is owned by the SecureSimplePairingState so its
    // captures are invalid after this.
    pairing_state = nullptr;
  };

  pairing_state = std::make_unique<SecureSimplePairingState>(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_cb,
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  // Unexpected event that should cause the status callback to be called with an
  // error.
  pairing_state->OnUserPasskeyNotification(kTestPasskey);

  EXPECT_TRUE(cb_called);
}

TEST_F(PairingStateTest, InitiatorCallbackMayDestroyPairingState) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  std::unique_ptr<SecureSimplePairingState> pairing_state =
      std::make_unique<SecureSimplePairingState>(
          peer()->GetWeakPtr(),
          pairing_delegate.GetWeakPtr(),
          connection()->GetWeakPtr(),
          /*outgoing_connection=*/false,
          MakeAuthRequestCallback(),
          NoOpStatusCallback,
          /*low_energy_address_delegate=*/this,
          /*controller_remote_public_key_validation_supported=*/true,
          sm_factory_func(),
          dispatcher());
  bool cb_called = false;
  auto status_cb = [&pairing_state, &cb_called](hci_spec::ConnectionHandle,
                                                hci::Result<> status) {
    EXPECT_TRUE(status.is_error());
    cb_called = true;

    // Note that this lambda is owned by the SecureSimplePairingState so its
    // captures are invalid after this.
    pairing_state = nullptr;
  };
  pairing_state->InitiatePairing(kNoSecurityRequirements, status_cb);

  // Unexpected event that should cause the status callback to be called with an
  // error.
  pairing_state->OnUserPasskeyNotification(kTestPasskey);

  EXPECT_TRUE(cb_called);
}

// Test helper to inspect StatusCallback invocations.
class TestStatusHandler final {
 public:
  auto MakeStatusCallback() {
    return [this](hci_spec::ConnectionHandle handle, hci::Result<> status) {
      call_count_++;
      handle_ = handle;
      status_ = status;
    };
  }

  auto call_count() const { return call_count_; }

  // Returns std::nullopt if |call_count() < 1|, otherwise values from the most
  // recent callback invocation.
  auto& handle() const { return handle_; }
  auto& status() const { return status_; }

 private:
  int call_count_ = 0;
  std::optional<hci_spec::ConnectionHandle> handle_;
  std::optional<hci::Result<>> status_;
};

TEST_F(PairingStateTest, TestStatusHandlerTracksStatusCallbackInvocations) {
  TestStatusHandler handler;
  EXPECT_EQ(0, handler.call_count());
  EXPECT_FALSE(handler.status());

  SecureSimplePairingState::StatusCallback status_cb =
      handler.MakeStatusCallback();
  EXPECT_EQ(0, handler.call_count());
  EXPECT_FALSE(handler.status());

  status_cb(hci_spec::ConnectionHandle(0x0A0B),
            ToResult(pw::bluetooth::emboss::StatusCode::PAIRING_NOT_ALLOWED));
  EXPECT_EQ(1, handler.call_count());
  ASSERT_TRUE(handler.handle());
  EXPECT_EQ(hci_spec::ConnectionHandle(0x0A0B), *handler.handle());
  ASSERT_TRUE(handler.status());
  EXPECT_EQ(ToResult(pw::bluetooth::emboss::StatusCode::PAIRING_NOT_ALLOWED),
            *handler.status());
}

TEST_F(PairingStateTest,
       InitiatingPairingAfterErrorTriggersStatusCallbackWithError) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  TestStatusHandler link_status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      link_status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  // Unexpected event that should cause the status callback to be called with an
  // error.
  pairing_state.OnUserPasskeyNotification(kTestPasskey);

  EXPECT_EQ(1, link_status_handler.call_count());
  ASSERT_TRUE(link_status_handler.handle());
  EXPECT_EQ(kTestHandle, *link_status_handler.handle());
  ASSERT_TRUE(link_status_handler.status());
  EXPECT_EQ(ToResult(HostError::kNotSupported), *link_status_handler.status());
  EXPECT_FALSE(peer()->MutBrEdr().is_pairing());

  // Try to initiate pairing again.
  TestStatusHandler pairing_status_handler;
  pairing_state.InitiatePairing(kNoSecurityRequirements,
                                pairing_status_handler.MakeStatusCallback());

  // The status callback for pairing attempts made after a pairing failure
  // should be rejected as canceled.
  EXPECT_EQ(1, pairing_status_handler.call_count());
  ASSERT_TRUE(pairing_status_handler.handle());
  EXPECT_EQ(kTestHandle, *pairing_status_handler.handle());
  ASSERT_TRUE(pairing_status_handler.status());
  EXPECT_EQ(ToResult(HostError::kCanceled), *pairing_status_handler.status());
  EXPECT_FALSE(peer()->MutBrEdr().is_pairing());
}

TEST_F(PairingStateTest,
       UnexpectedEncryptionChangeDoesNotTriggerStatusCallback) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  // Advance state machine.
  pairing_state.InitiatePairing(kNoSecurityRequirements, NoOpStatusCallback);
  static_cast<void>(pairing_state.OnLinkKeyRequest());
  static_cast<void>(pairing_state.OnIoCapabilityRequest());
  pairing_state.OnIoCapabilityResponse(kTestPeerIoCap);

  ASSERT_EQ(0, connection()->start_encryption_count());
  ASSERT_EQ(0, status_handler.call_count());

  connection()->TriggerEncryptionChangeCallback(fit::ok(true));
  EXPECT_EQ(0, status_handler.call_count());
}

TEST_F(PairingStateTest, PeerMayNotChangeLinkKeyWhenNotEncrypted) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());
  ASSERT_FALSE(connection()->ltk().has_value());

  pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                      kTestChangedLinkKeyType);

  EXPECT_FALSE(connection()->ltk().has_value());
  EXPECT_EQ(1, status_handler.call_count());
  ASSERT_TRUE(status_handler.handle());
  EXPECT_EQ(kTestHandle, *status_handler.handle());
  ASSERT_TRUE(status_handler.status());
  EXPECT_EQ(ToResult(HostError::kInsufficientSecurity),
            *status_handler.status());
}

TEST_F(PairingStateTest, PeerMayChangeLinkKeyWhenInIdleState) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());
  connection()->set_link_key(hci_spec::LinkKey(UInt128(), 0, 0),
                             kTestAuthenticatedLinkKeyType192);

  pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                      kTestChangedLinkKeyType);

  ASSERT_TRUE(connection()->ltk().has_value());
  EXPECT_EQ(kTestLinkKeyValue, connection()->ltk().value().value());
  ASSERT_TRUE(connection()->ltk_type().has_value());
  EXPECT_EQ(kTestChangedLinkKeyType, connection()->ltk_type().value());
  EXPECT_EQ(0, status_handler.call_count());
  EXPECT_FALSE(peer()->MutBrEdr().is_pairing());
}

// Inject events that occur during the course of a successful pairing as an
// initiator, but not including enabling link encryption.
void AdvanceToEncryptionAsInitiator(SecureSimplePairingState* pairing_state) {
  static_cast<void>(pairing_state->OnLinkKeyRequest());
  static_cast<void>(pairing_state->OnIoCapabilityRequest());
  pairing_state->OnIoCapabilityResponse(kTestPeerIoCap);
  pairing_state->OnUserConfirmationRequest(kTestPasskey,
                                           NoOpUserConfirmationCallback);
  pairing_state->OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  pairing_state->OnLinkKeyNotification(kTestLinkKeyValue,
                                       kTestUnauthenticatedLinkKeyType192);
  pairing_state->OnAuthenticationComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
}

TEST_F(PairingStateTest, SuccessfulEncryptionChangeTriggersStatusCallback) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  // Advance state machine.
  pairing_state.InitiatePairing(kNoSecurityRequirements, NoOpStatusCallback);
  AdvanceToEncryptionAsInitiator(&pairing_state);

  ASSERT_EQ(0, status_handler.call_count());

  EXPECT_EQ(1, connection()->start_encryption_count());
  connection()->TriggerEncryptionChangeCallback(fit::ok(true));
  EXPECT_EQ(1, status_handler.call_count());
  ASSERT_TRUE(status_handler.handle());
  EXPECT_EQ(kTestHandle, *status_handler.handle());
  ASSERT_TRUE(status_handler.status());
  EXPECT_EQ(fit::ok(), *status_handler.status());
}

TEST_F(PairingStateTest, EncryptionChangeErrorTriggersStatusCallbackWithError) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  // Advance state machine.
  static_cast<void>(pairing_state.InitiatePairing(kNoSecurityRequirements,
                                                  NoOpStatusCallback));
  AdvanceToEncryptionAsInitiator(&pairing_state);

  ASSERT_EQ(0, status_handler.call_count());

  EXPECT_EQ(1, connection()->start_encryption_count());
  connection()->TriggerEncryptionChangeCallback(
      fit::error(Error(HostError::kInsufficientSecurity)));
  EXPECT_EQ(1, status_handler.call_count());
  ASSERT_TRUE(status_handler.handle());
  EXPECT_EQ(kTestHandle, *status_handler.handle());
  ASSERT_TRUE(status_handler.status());
  EXPECT_EQ(ToResult(HostError::kInsufficientSecurity),
            *status_handler.status());
}

TEST_F(PairingStateTest,
       EncryptionChangeToDisabledTriggersStatusCallbackWithError) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  // Advance state machine.
  pairing_state.InitiatePairing(kNoSecurityRequirements, NoOpStatusCallback);
  AdvanceToEncryptionAsInitiator(&pairing_state);

  ASSERT_EQ(0, status_handler.call_count());

  EXPECT_EQ(1, connection()->start_encryption_count());
  connection()->TriggerEncryptionChangeCallback(fit::ok(false));
  EXPECT_EQ(1, status_handler.call_count());
  ASSERT_TRUE(status_handler.handle());
  EXPECT_EQ(kTestHandle, *status_handler.handle());
  ASSERT_TRUE(status_handler.status());
  EXPECT_EQ(ToResult(HostError::kFailed), *status_handler.status());
}

TEST_F(PairingStateTest, EncryptionChangeToEnableCallsInitiatorCallbacks) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      NoOpStatusCallback,
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  // Advance state machine.
  TestStatusHandler status_handler_0;
  pairing_state.InitiatePairing(kNoSecurityRequirements,
                                status_handler_0.MakeStatusCallback());
  AdvanceToEncryptionAsInitiator(&pairing_state);
  EXPECT_TRUE(pairing_state.initiator());

  // Try to initiate pairing while pairing is in progress.
  TestStatusHandler status_handler_1;
  static_cast<void>(pairing_state.InitiatePairing(
      kNoSecurityRequirements, status_handler_1.MakeStatusCallback()));

  EXPECT_TRUE(pairing_state.initiator());
  ASSERT_EQ(0, status_handler_0.call_count());
  ASSERT_EQ(0, status_handler_1.call_count());

  connection()->TriggerEncryptionChangeCallback(fit::ok(true));
  EXPECT_EQ(1, status_handler_0.call_count());
  EXPECT_EQ(1, status_handler_1.call_count());
  ASSERT_TRUE(status_handler_0.handle());
  EXPECT_EQ(kTestHandle, *status_handler_0.handle());
  ASSERT_TRUE(status_handler_0.status());
  EXPECT_EQ(fit::ok(), *status_handler_0.status());
  ASSERT_TRUE(status_handler_1.handle());
  EXPECT_EQ(kTestHandle, *status_handler_1.handle());
  ASSERT_TRUE(status_handler_1.status());
  EXPECT_EQ(fit::ok(), *status_handler_1.status());

  // Errors for a new pairing shouldn't invoke the initiators' callbacks.
  pairing_state.OnUserPasskeyNotification(kTestPasskey);
  EXPECT_EQ(1, status_handler_0.call_count());
  EXPECT_EQ(1, status_handler_1.call_count());
}

TEST_F(PairingStateTest, InitiatingPairingOnResponderWaitsForPairingToFinish) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      NoOpStatusCallback,
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  // Advance state machine as pairing responder.
  pairing_state.OnIoCapabilityResponse(kTestPeerIoCap);
  ASSERT_FALSE(pairing_state.initiator());
  EXPECT_TRUE(peer()->MutBrEdr().is_pairing());
  static_cast<void>(pairing_state.OnIoCapabilityRequest());
  pairing_state.OnUserConfirmationRequest(kTestPasskey,
                                          NoOpUserConfirmationCallback);

  // Try to initiate pairing while pairing is in progress.
  TestStatusHandler status_handler;
  pairing_state.InitiatePairing(kNoSecurityRequirements,
                                status_handler.MakeStatusCallback());
  EXPECT_FALSE(pairing_state.initiator());

  // Keep advancing state machine.
  pairing_state.OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                      kTestUnauthenticatedLinkKeyType192);

  EXPECT_FALSE(pairing_state.initiator());
  ASSERT_EQ(0, status_handler.call_count());

  // The attempt to initiate pairing should have its status callback notified.
  connection()->TriggerEncryptionChangeCallback(fit::ok(true));
  EXPECT_EQ(1, status_handler.call_count());
  ASSERT_TRUE(status_handler.handle());
  EXPECT_EQ(kTestHandle, *status_handler.handle());
  ASSERT_TRUE(status_handler.status());
  EXPECT_EQ(fit::ok(), *status_handler.status());
  EXPECT_FALSE(peer()->MutBrEdr().is_pairing());

  // Errors for a new pairing shouldn't invoke the attempted initiator's
  // callback.
  pairing_state.OnUserPasskeyNotification(kTestPasskey);
  EXPECT_EQ(1, status_handler.call_count());
}

TEST_F(PairingStateTest, UnresolvedPairingCallbackIsCalledOnDestruction) {
  TestStatusHandler overall_status, request_status;
  {
    NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

    SecureSimplePairingState pairing_state(
        peer()->GetWeakPtr(),
        pairing_delegate.GetWeakPtr(),
        connection()->GetWeakPtr(),
        /*outgoing_connection=*/false,
        MakeAuthRequestCallback(),
        overall_status.MakeStatusCallback(),
        /*low_energy_address_delegate=*/this,
        /*controller_remote_public_key_validation_supported=*/true,
        sm_factory_func(),
        dispatcher());

    // Advance state machine as pairing responder.
    pairing_state.OnIoCapabilityResponse(kTestPeerIoCap);
    ASSERT_FALSE(pairing_state.initiator());
    static_cast<void>(pairing_state.OnIoCapabilityRequest());
    pairing_state.OnUserConfirmationRequest(kTestPasskey,
                                            NoOpUserConfirmationCallback);

    // Try to initiate pairing while pairing is in progress.
    pairing_state.InitiatePairing(kNoSecurityRequirements,
                                  request_status.MakeStatusCallback());
    EXPECT_FALSE(pairing_state.initiator());

    // Keep advancing state machine.
    pairing_state.OnSimplePairingComplete(
        pw::bluetooth::emboss::StatusCode::SUCCESS);
    pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                        kTestUnauthenticatedLinkKeyType192);

    // as pairing_state falls out of scope, we expect additional pairing
    // callbacks to be called
    ASSERT_EQ(0, overall_status.call_count());
    ASSERT_EQ(0, request_status.call_count());
  }

  ASSERT_EQ(0, overall_status.call_count());

  ASSERT_EQ(1, request_status.call_count());
  ASSERT_TRUE(request_status.handle());
  EXPECT_EQ(kTestHandle, *request_status.handle());
  EXPECT_EQ(ToResult(HostError::kLinkDisconnected), *request_status.status());
}

TEST_F(PairingStateTest,
       InitiatorPairingStateRejectsIoCapReqWithoutPairingDelegate) {
  TestStatusHandler owner_status_handler;
  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      PairingDelegate::WeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      owner_status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  TestStatusHandler initiator_status_handler;
  // Advance state machine to Initiator Waiting IOCap Request
  pairing_state.InitiatePairing(kNoSecurityRequirements,
                                initiator_status_handler.MakeStatusCallback());
  EXPECT_TRUE(pairing_state.initiator());
  EXPECT_TRUE(peer()->MutBrEdr().is_pairing());
  // We should permit the pairing state machine to continue even without a
  // PairingDelegate, as we may have an existing bond to restore, which can be
  // done without a PairingDelegate.
  EXPECT_EQ(0, owner_status_handler.call_count());
  EXPECT_EQ(0, initiator_status_handler.call_count());
  // We will only start the pairing process if there is no stored bond
  EXPECT_EQ(std::nullopt, pairing_state.OnLinkKeyRequest());
  // We expect to be notified that there are no IOCapabilities, as there is no
  // PairingDelegate to provide them
  EXPECT_EQ(std::nullopt, pairing_state.OnIoCapabilityRequest());
  // All callbacks should be notified of pairing failure
  EXPECT_EQ(1, owner_status_handler.call_count());
  EXPECT_EQ(1, initiator_status_handler.call_count());
  ASSERT_TRUE(initiator_status_handler.status());
  EXPECT_EQ(ToResult(HostError::kNotReady), *initiator_status_handler.status());
  EXPECT_EQ(initiator_status_handler.status(), owner_status_handler.status());
  EXPECT_FALSE(peer()->MutBrEdr().is_pairing());
}

TEST_F(PairingStateTest,
       ResponderPairingStateRejectsIoCapReqWithoutPairingDelegate) {
  TestStatusHandler status_handler;
  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      PairingDelegate::WeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  // Advance state machine to Responder Waiting IOCap Request
  pairing_state.OnIoCapabilityResponse(
      pw::bluetooth::emboss::IoCapability::DISPLAY_YES_NO);
  EXPECT_FALSE(pairing_state.initiator());
  EXPECT_EQ(0, status_handler.call_count());
  EXPECT_TRUE(peer()->MutBrEdr().is_pairing());

  // We expect to be notified that there are no IOCapabilities, as there is no
  // PairingDelegate to provide them.
  EXPECT_EQ(std::nullopt, pairing_state.OnIoCapabilityRequest());
  // All callbacks should be notified of pairing failure
  EXPECT_EQ(1, status_handler.call_count());
  ASSERT_TRUE(status_handler.status());
  EXPECT_EQ(ToResult(HostError::kNotReady), *status_handler.status());
  EXPECT_FALSE(peer()->MutBrEdr().is_pairing());
}

TEST_F(PairingStateTest, UnexpectedLinkKeyAuthenticationRaisesError) {
  NoOpPairingDelegate pairing_delegate(sm::IOCapability::kDisplayOnly);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  // Advance state machine.
  pairing_state.OnIoCapabilityResponse(IoCapability::DISPLAY_YES_NO);
  ASSERT_FALSE(pairing_state.initiator());
  static_cast<void>(pairing_state.OnIoCapabilityRequest());
  pairing_state.OnUserConfirmationRequest(kTestPasskey,
                                          NoOpUserConfirmationCallback);
  pairing_state.OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);

  // Provide an authenticated link key when this should have resulted in an
  // unauthenticated link key
  pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                      kTestAuthenticatedLinkKeyType192);

  EXPECT_EQ(1, status_handler.call_count());
  ASSERT_TRUE(status_handler.handle());
  EXPECT_EQ(kTestHandle, *status_handler.handle());
  ASSERT_TRUE(status_handler.status());
  EXPECT_EQ(ToResult(HostError::kInsufficientSecurity),
            *status_handler.status());
}

TEST_F(PairingStateTest, LegacyPairingLinkKeyRaisesError) {
  NoOpPairingDelegate pairing_delegate(sm::IOCapability::kNoInputNoOutput);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  // Advance state machine.
  pairing_state.OnIoCapabilityResponse(IoCapability::DISPLAY_YES_NO);
  ASSERT_FALSE(pairing_state.initiator());
  static_cast<void>(pairing_state.OnIoCapabilityRequest());
  pairing_state.OnUserConfirmationRequest(kTestPasskey,
                                          NoOpUserConfirmationCallback);
  pairing_state.OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);

  // Provide a legacy pairing link key type.
  pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                      kTestLegacyLinkKeyType);

  EXPECT_EQ(1, status_handler.call_count());
  ASSERT_TRUE(status_handler.handle());
  EXPECT_EQ(kTestHandle, *status_handler.handle());
  ASSERT_TRUE(status_handler.status());
  EXPECT_EQ(ToResult(HostError::kInsufficientSecurity),
            *status_handler.status());
}

TEST_F(PairingStateTest, PairingSetsConnectionLinkKey) {
  NoOpPairingDelegate pairing_delegate(sm::IOCapability::kNoInputNoOutput);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  // Advance state machine.
  pairing_state.OnIoCapabilityResponse(IoCapability::DISPLAY_YES_NO);
  ASSERT_FALSE(pairing_state.initiator());
  static_cast<void>(pairing_state.OnIoCapabilityRequest());
  pairing_state.OnUserConfirmationRequest(kTestPasskey,
                                          NoOpUserConfirmationCallback);
  pairing_state.OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);

  ASSERT_FALSE(connection()->ltk());
  pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                      kTestUnauthenticatedLinkKeyType192);
  ASSERT_TRUE(connection()->ltk());
  EXPECT_EQ(kTestLinkKeyValue, connection()->ltk()->value());

  EXPECT_EQ(0, status_handler.call_count());
}

TEST_F(PairingStateTest,
       SecureConnectionsRequiresSecureConnectionsLinkKeySuccess) {
  NoOpPairingDelegate pairing_delegate(sm::IOCapability::kDisplayOnly);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  // Set peer lmp_features: Secure Connections (Host Support)
  peer()->SetFeaturePage(
      1u,
      static_cast<uint64_t>(
          hci_spec::LMPFeature::kSecureConnectionsHostSupport));

  // Set peer lmp_features: Secure Connections (Controller Support)
  peer()->SetFeaturePage(
      2u,
      static_cast<uint64_t>(
          hci_spec::LMPFeature::kSecureConnectionsControllerSupport));

  // Advance state machine.
  pairing_state.OnIoCapabilityResponse(IoCapability::DISPLAY_YES_NO);
  ASSERT_FALSE(pairing_state.initiator());
  EXPECT_TRUE(peer()->MutBrEdr().is_pairing());
  static_cast<void>(pairing_state.OnIoCapabilityRequest());
  pairing_state.OnUserConfirmationRequest(kTestPasskey,
                                          NoOpUserConfirmationCallback);
  pairing_state.OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_TRUE(peer()->MutBrEdr().is_pairing());

  // Ensure that P-256 authenticated link key was provided
  pairing_state.OnLinkKeyNotification(
      kTestLinkKeyValue,
      kTestUnauthenticatedLinkKeyType256,
      /*local_secure_connections_supported=*/true);

  ASSERT_TRUE(connection()->ltk());
  EXPECT_EQ(kTestLinkKeyValue, connection()->ltk()->value());

  EXPECT_EQ(0, status_handler.call_count());
  EXPECT_TRUE(peer()->MutBrEdr().is_pairing());
}

TEST_F(PairingStateTest,
       SecureConnectionsRequiresSecureConnectionsLinkKeyFail) {
  NoOpPairingDelegate pairing_delegate(sm::IOCapability::kDisplayOnly);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  // Set peer lmp_features: Secure Connections (Host Support)
  peer()->SetFeaturePage(
      1u,
      static_cast<uint64_t>(
          hci_spec::LMPFeature::kSecureConnectionsHostSupport));

  // Set peer lmp_features: Secure Connections (Controller Support)
  peer()->SetFeaturePage(
      2u,
      static_cast<uint64_t>(
          hci_spec::LMPFeature::kSecureConnectionsControllerSupport));

  // Advance state machine.
  pairing_state.OnIoCapabilityResponse(IoCapability::DISPLAY_YES_NO);
  ASSERT_FALSE(pairing_state.initiator());
  static_cast<void>(pairing_state.OnIoCapabilityRequest());
  pairing_state.OnUserConfirmationRequest(kTestPasskey,
                                          NoOpUserConfirmationCallback);
  pairing_state.OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);

  // Provide P-192 authenticated link key when this should have resulted in an
  // P-256 link key
  pairing_state.OnLinkKeyNotification(
      kTestLinkKeyValue,
      kTestUnauthenticatedLinkKeyType192,
      /*local_secure_connections_supported=*/true);

  EXPECT_EQ(1, status_handler.call_count());
  ASSERT_TRUE(status_handler.handle());
  EXPECT_EQ(kTestHandle, *status_handler.handle());
  ASSERT_TRUE(status_handler.status());
  EXPECT_EQ(ToResult(HostError::kInsufficientSecurity),
            *status_handler.status());
}

TEST_F(PairingStateTest,
       NumericComparisonPairingComparesPasskeyOnInitiatorDisplayYesNoSide) {
  FakePairingDelegate pairing_delegate(kTestLocalIoCap);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  // Advance state machine.
  pairing_state.InitiatePairing(kNoSecurityRequirements,
                                status_handler.MakeStatusCallback());
  ASSERT_TRUE(pairing_state.initiator());
  static_cast<void>(pairing_state.OnLinkKeyRequest());
  EXPECT_EQ(IoCapability::DISPLAY_YES_NO,
            *pairing_state.OnIoCapabilityRequest());

  pairing_state.OnIoCapabilityResponse(IoCapability::DISPLAY_YES_NO);

  pairing_delegate.SetDisplayPasskeyCallback(
      [this](PeerId peer_id,
             uint32_t value,
             PairingDelegate::DisplayMethod method,
             auto cb) {
        EXPECT_EQ(peer()->identifier(), peer_id);
        EXPECT_EQ(kTestPasskey, value);
        EXPECT_EQ(PairingDelegate::DisplayMethod::kComparison, method);
        ASSERT_TRUE(cb);
        cb(true);
      });
  bool confirmed = false;
  pairing_state.OnUserConfirmationRequest(
      kTestPasskey, [&confirmed](bool confirm) { confirmed = confirm; });
  EXPECT_TRUE(confirmed);

  pairing_delegate.SetCompletePairingCallback(
      [this](PeerId peer_id, sm::Result<> status) {
        EXPECT_EQ(peer()->identifier(), peer_id);
        EXPECT_EQ(fit::ok(), status);
      });
  pairing_state.OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);

  EXPECT_EQ(0, status_handler.call_count());
}

TEST_F(PairingStateTest,
       NumericComparisonPairingComparesPasskeyOnResponderDisplayYesNoSide) {
  FakePairingDelegate pairing_delegate(kTestLocalIoCap);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  // Advance state machine.
  pairing_state.OnIoCapabilityResponse(IoCapability::DISPLAY_YES_NO);
  ASSERT_FALSE(pairing_state.initiator());
  EXPECT_EQ(IoCapability::DISPLAY_YES_NO,
            *pairing_state.OnIoCapabilityRequest());

  pairing_delegate.SetDisplayPasskeyCallback(
      [this](PeerId peer_id,
             uint32_t value,
             PairingDelegate::DisplayMethod method,
             auto cb) {
        EXPECT_EQ(peer()->identifier(), peer_id);
        EXPECT_EQ(kTestPasskey, value);
        EXPECT_EQ(PairingDelegate::DisplayMethod::kComparison, method);
        ASSERT_TRUE(cb);
        cb(true);
      });
  bool confirmed = false;
  pairing_state.OnUserConfirmationRequest(
      kTestPasskey, [&confirmed](bool confirm) { confirmed = confirm; });
  EXPECT_TRUE(confirmed);

  pairing_delegate.SetCompletePairingCallback(
      [this](PeerId peer_id, sm::Result<> status) {
        EXPECT_EQ(peer()->identifier(), peer_id);
        EXPECT_EQ(fit::ok(), status);
      });
  pairing_state.OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);

  EXPECT_EQ(0, status_handler.call_count());
}

// v5.0, Vol 3, Part C, Sec 5.2.2.6 call this "Numeric Comparison with automatic
// confirmation on device B only and Yes/No confirmation on whether to pair on
// device A. Device A does not show the confirmation value." and it should
// result in user consent.
TEST_F(PairingStateTest,
       NumericComparisonWithoutValueRequestsConsentFromDisplayYesNoSide) {
  FakePairingDelegate pairing_delegate(kTestLocalIoCap);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  // Advance state machine.
  pairing_state.OnIoCapabilityResponse(IoCapability::NO_INPUT_NO_OUTPUT);
  ASSERT_FALSE(pairing_state.initiator());
  EXPECT_EQ(IoCapability::DISPLAY_YES_NO,
            *pairing_state.OnIoCapabilityRequest());

  pairing_delegate.SetConfirmPairingCallback([this](PeerId peer_id, auto cb) {
    EXPECT_EQ(peer()->identifier(), peer_id);
    ASSERT_TRUE(cb);
    cb(true);
  });
  bool confirmed = false;
  pairing_state.OnUserConfirmationRequest(
      kTestPasskey, [&confirmed](bool confirm) { confirmed = confirm; });
  EXPECT_TRUE(confirmed);

  pairing_delegate.SetCompletePairingCallback(
      [this](PeerId peer_id, sm::Result<> status) {
        EXPECT_EQ(peer()->identifier(), peer_id);
        EXPECT_EQ(fit::ok(), status);
      });
  pairing_state.OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);

  EXPECT_EQ(0, status_handler.call_count());
}

TEST_F(PairingStateTest, PasskeyEntryPairingDisplaysPasskeyToDisplayOnlySide) {
  FakePairingDelegate pairing_delegate(sm::IOCapability::kDisplayOnly);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  // Advance state machine.
  pairing_state.OnIoCapabilityResponse(IoCapability::KEYBOARD_ONLY);
  ASSERT_FALSE(pairing_state.initiator());
  EXPECT_EQ(IoCapability::DISPLAY_ONLY, *pairing_state.OnIoCapabilityRequest());

  pairing_delegate.SetDisplayPasskeyCallback(
      [this](PeerId peer_id,
             uint32_t value,
             PairingDelegate::DisplayMethod method,
             auto cb) {
        EXPECT_EQ(peer()->identifier(), peer_id);
        EXPECT_EQ(kTestPasskey, value);
        EXPECT_EQ(PairingDelegate::DisplayMethod::kPeerEntry, method);
        EXPECT_TRUE(cb);
      });
  pairing_state.OnUserPasskeyNotification(kTestPasskey);

  pairing_delegate.SetCompletePairingCallback(
      [this](PeerId peer_id, sm::Result<> status) {
        EXPECT_EQ(peer()->identifier(), peer_id);
        EXPECT_EQ(fit::ok(), status);
      });
  pairing_state.OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);

  EXPECT_EQ(0, status_handler.call_count());
}

TEST_F(PairingStateTest,
       PasskeyEntryPairingRequestsPasskeyFromKeyboardOnlySide) {
  FakePairingDelegate pairing_delegate(sm::IOCapability::kKeyboardOnly);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  // Advance state machine.
  pairing_state.OnIoCapabilityResponse(IoCapability::DISPLAY_ONLY);
  ASSERT_FALSE(pairing_state.initiator());
  EXPECT_EQ(IoCapability::KEYBOARD_ONLY,
            *pairing_state.OnIoCapabilityRequest());

  pairing_delegate.SetRequestPasskeyCallback([this](PeerId peer_id, auto cb) {
    EXPECT_EQ(peer()->identifier(), peer_id);
    ASSERT_TRUE(cb);
    cb(kTestPasskey);
  });
  bool cb_called = false;
  std::optional<uint32_t> passkey;
  auto passkey_cb = [&cb_called,
                     &passkey](std::optional<uint32_t> pairing_state_passkey) {
    cb_called = true;
    passkey = pairing_state_passkey;
  };

  pairing_state.OnUserPasskeyRequest(std::move(passkey_cb));
  EXPECT_TRUE(cb_called);
  ASSERT_TRUE(passkey);
  EXPECT_EQ(kTestPasskey, *passkey);

  pairing_delegate.SetCompletePairingCallback(
      [this](PeerId peer_id, sm::Result<> status) {
        EXPECT_EQ(peer()->identifier(), peer_id);
        EXPECT_EQ(fit::ok(), status);
      });
  pairing_state.OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);

  EXPECT_EQ(0, status_handler.call_count());
}

TEST_F(PairingStateTest,
       JustWorksPairingOutgoingConnectDoesNotRequestUserActionInitiator) {
  FakePairingDelegate pairing_delegate(sm::IOCapability::kNoInputNoOutput);

  TestStatusHandler owner_status_handler;
  TestStatusHandler initiator_status_handler;
  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/true,
      MakeAuthRequestCallback(),
      owner_status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  // Advance state machine to Initiator Waiting IOCap Request
  pairing_state.InitiatePairing(kNoSecurityRequirements,
                                initiator_status_handler.MakeStatusCallback());
  EXPECT_TRUE(pairing_state.initiator());
  static_cast<void>(pairing_state.OnLinkKeyRequest());
  EXPECT_EQ(IoCapability::NO_INPUT_NO_OUTPUT,
            *pairing_state.OnIoCapabilityRequest());

  pairing_state.OnIoCapabilityResponse(IoCapability::NO_INPUT_NO_OUTPUT);
  bool confirmed = false;
  pairing_state.OnUserConfirmationRequest(
      kTestPasskey, [&confirmed](bool confirm) { confirmed = confirm; });
  EXPECT_TRUE(confirmed);

  pairing_delegate.SetCompletePairingCallback(
      [this](PeerId peer_id, sm::Result<> status) {
        EXPECT_EQ(peer()->identifier(), peer_id);
        EXPECT_EQ(fit::ok(), status);
      });
  pairing_state.OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);

  EXPECT_EQ(0, owner_status_handler.call_count());
  EXPECT_EQ(0, initiator_status_handler.call_count());
}

TEST_F(PairingStateTest,
       JustWorksPairingOutgoingConnectDoesNotRequestUserActionResponder) {
  FakePairingDelegate pairing_delegate(sm::IOCapability::kNoInputNoOutput);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/true,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  // Advance state machine.
  pairing_state.OnIoCapabilityResponse(IoCapability::NO_INPUT_NO_OUTPUT);
  ASSERT_FALSE(pairing_state.initiator());
  EXPECT_EQ(IoCapability::NO_INPUT_NO_OUTPUT,
            *pairing_state.OnIoCapabilityRequest());

  bool confirmed = false;
  pairing_state.OnUserConfirmationRequest(
      kTestPasskey, [&confirmed](bool confirm) { confirmed = confirm; });
  EXPECT_TRUE(confirmed);

  pairing_delegate.SetCompletePairingCallback(
      [this](PeerId peer_id, sm::Result<> status) {
        EXPECT_EQ(peer()->identifier(), peer_id);
        EXPECT_EQ(fit::ok(), status);
      });
  pairing_state.OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);

  EXPECT_EQ(0, status_handler.call_count());
}

TEST_F(PairingStateTest,
       JustWorksPairingIncomingConnectRequiresConfirmationRejectedResponder) {
  FakePairingDelegate pairing_delegate(sm::IOCapability::kNoInputNoOutput);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  // Advance state machine.
  pairing_state.OnIoCapabilityResponse(IoCapability::NO_INPUT_NO_OUTPUT);
  ASSERT_FALSE(pairing_state.initiator());
  EXPECT_EQ(IoCapability::NO_INPUT_NO_OUTPUT,
            *pairing_state.OnIoCapabilityRequest());

  pairing_delegate.SetConfirmPairingCallback([this](PeerId peer_id, auto cb) {
    EXPECT_EQ(peer()->identifier(), peer_id);
    ASSERT_TRUE(cb);
    cb(false);
  });
  bool confirmed = true;
  pairing_state.OnUserConfirmationRequest(
      kTestPasskey, [&confirmed](bool confirm) { confirmed = confirm; });
  EXPECT_FALSE(confirmed);

  // Eventually the controller sends a SimplePairingComplete indicating the
  // failure.
  pairing_delegate.SetCompletePairingCallback(
      [this](PeerId peer_id, sm::Result<> status) {
        EXPECT_EQ(peer()->identifier(), peer_id);
        EXPECT_TRUE(status.is_error());
      });
  pairing_state.OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::AUTHENTICATION_FAILURE);

  EXPECT_EQ(1, status_handler.call_count());
  ASSERT_TRUE(status_handler.status());
  EXPECT_EQ(ToResult(pw::bluetooth::emboss::StatusCode::AUTHENTICATION_FAILURE),
            *status_handler.status());
}

TEST_F(PairingStateTest,
       JustWorksPairingIncomingConnectRequiresConfirmationRejectedInitiator) {
  FakePairingDelegate pairing_delegate(sm::IOCapability::kNoInputNoOutput);

  TestStatusHandler owner_status_handler;
  TestStatusHandler initiator_status_handler;
  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      owner_status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  // Advance state machine to Initiator Waiting IOCap Request
  pairing_state.InitiatePairing(kNoSecurityRequirements,
                                initiator_status_handler.MakeStatusCallback());
  EXPECT_TRUE(pairing_state.initiator());
  static_cast<void>(pairing_state.OnLinkKeyRequest());
  EXPECT_EQ(IoCapability::NO_INPUT_NO_OUTPUT,
            *pairing_state.OnIoCapabilityRequest());

  pairing_state.OnIoCapabilityResponse(IoCapability::NO_INPUT_NO_OUTPUT);

  pairing_delegate.SetConfirmPairingCallback([this](PeerId peer_id, auto cb) {
    EXPECT_EQ(peer()->identifier(), peer_id);
    ASSERT_TRUE(cb);
    cb(false);
  });
  bool confirmed = true;
  pairing_state.OnUserConfirmationRequest(
      kTestPasskey, [&confirmed](bool confirm) { confirmed = confirm; });
  EXPECT_FALSE(confirmed);

  // Eventually the controller sends a SimplePairingComplete indicating the
  // failure.
  pairing_delegate.SetCompletePairingCallback(
      [this](PeerId peer_id, sm::Result<> status) {
        EXPECT_EQ(peer()->identifier(), peer_id);
        EXPECT_TRUE(status.is_error());
      });
  pairing_state.OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::AUTHENTICATION_FAILURE);

  EXPECT_EQ(1, owner_status_handler.call_count());
  ASSERT_TRUE(owner_status_handler.status());
  EXPECT_EQ(ToResult(pw::bluetooth::emboss::StatusCode::AUTHENTICATION_FAILURE),
            *owner_status_handler.status());
  EXPECT_EQ(1, initiator_status_handler.call_count());
  ASSERT_TRUE(initiator_status_handler.status());
  EXPECT_EQ(ToResult(pw::bluetooth::emboss::StatusCode::AUTHENTICATION_FAILURE),
            *initiator_status_handler.status());
}

TEST_F(PairingStateTest,
       JustWorksPairingIncomingConnectRequiresConfirmationAcceptedResponder) {
  FakePairingDelegate pairing_delegate(sm::IOCapability::kNoInputNoOutput);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  // Advance state machine.
  pairing_state.OnIoCapabilityResponse(IoCapability::NO_INPUT_NO_OUTPUT);
  ASSERT_FALSE(pairing_state.initiator());
  EXPECT_EQ(IoCapability::NO_INPUT_NO_OUTPUT,
            *pairing_state.OnIoCapabilityRequest());

  pairing_delegate.SetConfirmPairingCallback([this](PeerId peer_id, auto cb) {
    EXPECT_EQ(peer()->identifier(), peer_id);
    ASSERT_TRUE(cb);
    cb(true);
  });
  bool confirmed = false;
  pairing_state.OnUserConfirmationRequest(
      kTestPasskey, [&confirmed](bool confirm) { confirmed = confirm; });
  EXPECT_TRUE(confirmed);

  pairing_delegate.SetCompletePairingCallback(
      [this](PeerId peer_id, sm::Result<> status) {
        EXPECT_EQ(peer()->identifier(), peer_id);
        EXPECT_EQ(fit::ok(), status);
      });
  pairing_state.OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);

  EXPECT_EQ(0, status_handler.call_count());
}

TEST_F(PairingStateTest,
       JustWorksPairingIncomingConnectRequiresConfirmationAcceptedInitiator) {
  FakePairingDelegate pairing_delegate(sm::IOCapability::kNoInputNoOutput);

  TestStatusHandler owner_status_handler;
  TestStatusHandler initiator_status_handler;
  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      owner_status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  // Advance state machine to Initiator Waiting IOCap Request
  pairing_state.InitiatePairing(kNoSecurityRequirements,
                                initiator_status_handler.MakeStatusCallback());
  EXPECT_TRUE(pairing_state.initiator());
  static_cast<void>(pairing_state.OnLinkKeyRequest());
  EXPECT_EQ(IoCapability::NO_INPUT_NO_OUTPUT,
            *pairing_state.OnIoCapabilityRequest());

  pairing_state.OnIoCapabilityResponse(IoCapability::NO_INPUT_NO_OUTPUT);

  pairing_delegate.SetConfirmPairingCallback([this](PeerId peer_id, auto cb) {
    EXPECT_EQ(peer()->identifier(), peer_id);
    ASSERT_TRUE(cb);
    cb(true);
  });
  bool confirmed = false;
  pairing_state.OnUserConfirmationRequest(
      kTestPasskey, [&confirmed](bool confirm) { confirmed = confirm; });
  EXPECT_TRUE(confirmed);

  pairing_delegate.SetCompletePairingCallback(
      [this](PeerId peer_id, sm::Result<> status) {
        EXPECT_EQ(peer()->identifier(), peer_id);
        EXPECT_EQ(fit::ok(), status);
      });
  pairing_state.OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);

  EXPECT_EQ(0, owner_status_handler.call_count());
  EXPECT_EQ(0, initiator_status_handler.call_count());
}

// Event injectors. Return values are necessarily ignored in order to make types
// match, so don't use these functions to test return values. Likewise,
// arguments have been filled with test defaults for a successful pairing flow.
void LinkKeyRequest(SecureSimplePairingState* pairing_state) {
  static_cast<void>(pairing_state->OnLinkKeyRequest());
}
void IoCapabilityRequest(SecureSimplePairingState* pairing_state) {
  static_cast<void>(pairing_state->OnIoCapabilityRequest());
}
void IoCapabilityResponse(SecureSimplePairingState* pairing_state) {
  pairing_state->OnIoCapabilityResponse(kTestPeerIoCap);
}
void UserConfirmationRequest(SecureSimplePairingState* pairing_state) {
  pairing_state->OnUserConfirmationRequest(kTestPasskey,
                                           NoOpUserConfirmationCallback);
}
void UserPasskeyRequest(SecureSimplePairingState* pairing_state) {
  pairing_state->OnUserPasskeyRequest(NoOpUserPasskeyCallback);
}
void UserPasskeyNotification(SecureSimplePairingState* pairing_state) {
  pairing_state->OnUserPasskeyNotification(kTestPasskey);
}
void SimplePairingComplete(SecureSimplePairingState* pairing_state) {
  pairing_state->OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
}
void LinkKeyNotification(SecureSimplePairingState* pairing_state) {
  pairing_state->OnLinkKeyNotification(kTestLinkKeyValue,
                                       kTestUnauthenticatedLinkKeyType192);
}
void AuthenticationComplete(SecureSimplePairingState* pairing_state) {
  pairing_state->OnAuthenticationComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
}

// Test suite fixture that genericizes an injected pairing state event. The
// event being tested should be retrieved with the GetParam method, which
// returns a default event injector. For example:
//
//   SecureSimplePairingState pairing_state;
//   GetParam()(&pairing_state);
//
// This is named so that the instantiated test description looks correct:
//
//   PairingStateTest/HandlesEvent.<test case>/<index of event>
class HandlesEvent : public PairingStateTest,
                     public ::testing::WithParamInterface<void (*)(
                         SecureSimplePairingState*)> {
 public:
  void SetUp() override {
    PairingStateTest::SetUp();

    pairing_delegate_ = std::make_unique<NoOpPairingDelegate>(kTestLocalIoCap);
    pairing_state_ = std::make_unique<SecureSimplePairingState>(
        peer()->GetWeakPtr(),
        pairing_delegate_->GetWeakPtr(),
        connection()->GetWeakPtr(),
        /*outgoing_connection=*/false,
        MakeAuthRequestCallback(),
        status_handler_.MakeStatusCallback(),
        /*low_energy_address_delegate=*/this,
        /*controller_remote_public_key_validation_supported=*/true,
        sm_factory_func(),
        dispatcher());
  }

  void TearDown() override {
    pairing_state_.reset();
    PairingStateTest::TearDown();
  }

  const TestStatusHandler& status_handler() const { return status_handler_; }
  SecureSimplePairingState& pairing_state() { return *pairing_state_; }

  // Returns an event injector that was passed to INSTANTIATE_TEST_SUITE_P.
  auto* event() const { return GetParam(); }

  void InjectEvent() { event()(&pairing_state()); }

 private:
  TestStatusHandler status_handler_;
  std::unique_ptr<NoOpPairingDelegate> pairing_delegate_;
  std::unique_ptr<SecureSimplePairingState> pairing_state_;
};

// The tests here exercise that SecureSimplePairingState can be successfully
// advances through the expected pairing flow and generates errors when the
// pairing flow occurs out of order. This is intended to cover its internal
// state machine transitions and not the side effects.
INSTANTIATE_TEST_SUITE_P(PairingStateTest,
                         HandlesEvent,
                         ::testing::Values(LinkKeyRequest,
                                           IoCapabilityRequest,
                                           IoCapabilityResponse,
                                           UserConfirmationRequest,
                                           UserPasskeyRequest,
                                           UserPasskeyNotification,
                                           SimplePairingComplete,
                                           LinkKeyNotification,
                                           AuthenticationComplete));

TEST_P(HandlesEvent, InIdleState) {
  RETURN_IF_FATAL(InjectEvent());
  if (event() == LinkKeyRequest || event() == IoCapabilityResponse) {
    EXPECT_EQ(0, status_handler().call_count());
  } else {
    EXPECT_EQ(1, status_handler().call_count());
    ASSERT_TRUE(status_handler().handle());
    EXPECT_EQ(kTestHandle, *status_handler().handle());
    ASSERT_TRUE(status_handler().status());
    EXPECT_EQ(ToResult(HostError::kNotSupported), status_handler().status());
  }
}

TEST_P(HandlesEvent, InInitiatorWaitLinkKeyRequestState) {
  // Advance state machine.
  static_cast<void>(pairing_state().InitiatePairing(kNoSecurityRequirements,
                                                    NoOpStatusCallback));

  RETURN_IF_FATAL(InjectEvent());
  if (event() == LinkKeyRequest) {
    EXPECT_EQ(0, status_handler().call_count());
  } else {
    EXPECT_EQ(1, status_handler().call_count());
    ASSERT_TRUE(status_handler().status());
    EXPECT_EQ(ToResult(HostError::kNotSupported), status_handler().status());
  }
}

TEST_P(HandlesEvent, InInitiatorWaitIoCapRequest) {
  // Advance state machine.
  pairing_state().InitiatePairing(kNoSecurityRequirements, NoOpStatusCallback);
  static_cast<void>(pairing_state().OnLinkKeyRequest());

  RETURN_IF_FATAL(InjectEvent());
  if (event() == IoCapabilityRequest) {
    EXPECT_EQ(0, status_handler().call_count());
  } else {
    EXPECT_EQ(1, status_handler().call_count());
    ASSERT_TRUE(status_handler().status());
    EXPECT_EQ(ToResult(HostError::kNotSupported), status_handler().status());
  }
}

TEST_P(HandlesEvent, InInitiatorWaitAuthCompleteSkippingSimplePairing) {
  EXPECT_TRUE(peer()->MutBrEdr().SetBondData(
      sm::LTK(sm::SecurityProperties(kTestUnauthenticatedLinkKeyType192),
              kTestLinkKey)));

  // Advance state machine.
  pairing_state().InitiatePairing(kNoSecurityRequirements, NoOpStatusCallback);
  EXPECT_NE(std::nullopt, pairing_state().OnLinkKeyRequest());

  RETURN_IF_FATAL(InjectEvent());
  if (event() == AuthenticationComplete) {
    EXPECT_EQ(0, status_handler().call_count());
    EXPECT_EQ(1, connection()->start_encryption_count());
  } else {
    EXPECT_EQ(1, status_handler().call_count());
    ASSERT_TRUE(status_handler().status());
    EXPECT_EQ(ToResult(HostError::kNotSupported), status_handler().status());
  }
}

TEST_P(HandlesEvent, InInitiatorWaitIoCapResponseState) {
  // Advance state machine.
  static_cast<void>(pairing_state().InitiatePairing(kNoSecurityRequirements,
                                                    NoOpStatusCallback));
  static_cast<void>(pairing_state().OnLinkKeyRequest());
  static_cast<void>(pairing_state().OnIoCapabilityRequest());

  RETURN_IF_FATAL(InjectEvent());
  if (event() == IoCapabilityResponse) {
    EXPECT_EQ(0, status_handler().call_count());
  } else {
    EXPECT_EQ(1, status_handler().call_count());
    ASSERT_TRUE(status_handler().status());
    EXPECT_EQ(ToResult(HostError::kNotSupported), status_handler().status());
  }
}

TEST_P(HandlesEvent, InResponderWaitIoCapRequestState) {
  // Advance state machine.
  pairing_state().OnIoCapabilityResponse(kTestPeerIoCap);

  RETURN_IF_FATAL(InjectEvent());
  if (event() == IoCapabilityRequest) {
    EXPECT_EQ(0, status_handler().call_count());
  } else {
    EXPECT_EQ(1, status_handler().call_count());
    ASSERT_TRUE(status_handler().status());
    EXPECT_EQ(ToResult(HostError::kNotSupported), status_handler().status());
  }
}

TEST_P(HandlesEvent,
       InErrorStateAfterIoCapRequestRejectedWithoutPairingDelegate) {
  // Clear the default pairing delegate set by the fixture.
  pairing_state().SetPairingDelegate(PairingDelegate::WeakPtr());

  // Advance state machine.
  pairing_state().OnIoCapabilityResponse(kTestPeerIoCap);
  EXPECT_FALSE(pairing_state().OnIoCapabilityRequest());

  // SecureSimplePairingState no longer accepts events because being not ready
  // to pair has raised an error.
  RETURN_IF_FATAL(InjectEvent());
  EXPECT_LE(1, status_handler().call_count());
  ASSERT_TRUE(status_handler().status());
  if (event() == LinkKeyRequest || event() == IoCapabilityResponse) {
    // Peer attempted to pair again, which raises an additional "not ready"
    // error.
    EXPECT_EQ(ToResult(HostError::kNotReady), status_handler().status());
  } else {
    EXPECT_EQ(ToResult(HostError::kNotSupported), status_handler().status());
  }
}

TEST_P(HandlesEvent, InWaitUserConfirmationStateAsInitiator) {
  NoOpPairingDelegate pairing_delegate(sm::IOCapability::kDisplayOnly);
  pairing_state().SetPairingDelegate(pairing_delegate.GetWeakPtr());

  // Advance state machine.
  pairing_state().InitiatePairing(kNoSecurityRequirements, NoOpStatusCallback);
  static_cast<void>(pairing_state().OnLinkKeyRequest());
  static_cast<void>(pairing_state().OnIoCapabilityRequest());
  pairing_state().OnIoCapabilityResponse(IoCapability::DISPLAY_YES_NO);
  ASSERT_TRUE(pairing_state().initiator());

  RETURN_IF_FATAL(InjectEvent());
  if (event() == UserConfirmationRequest) {
    EXPECT_EQ(0, status_handler().call_count());
  } else {
    EXPECT_EQ(1, status_handler().call_count());
    ASSERT_TRUE(status_handler().status());
    EXPECT_EQ(ToResult(HostError::kNotSupported), status_handler().status());
  }
}

TEST_P(HandlesEvent, InWaitUserPasskeyRequestStateAsInitiator) {
  NoOpPairingDelegate pairing_delegate(sm::IOCapability::kKeyboardOnly);
  pairing_state().SetPairingDelegate(pairing_delegate.GetWeakPtr());

  // Advance state machine.
  pairing_state().InitiatePairing(kNoSecurityRequirements, NoOpStatusCallback);
  static_cast<void>(pairing_state().OnLinkKeyRequest());
  static_cast<void>(pairing_state().OnIoCapabilityRequest());
  pairing_state().OnIoCapabilityResponse(IoCapability::DISPLAY_ONLY);
  ASSERT_TRUE(pairing_state().initiator());

  RETURN_IF_FATAL(InjectEvent());
  if (event() == UserPasskeyRequest) {
    EXPECT_EQ(0, status_handler().call_count());
  } else {
    EXPECT_EQ(1, status_handler().call_count());
    ASSERT_TRUE(status_handler().status());
    EXPECT_EQ(ToResult(HostError::kNotSupported), status_handler().status());
  }
}

TEST_P(HandlesEvent, InWaitUserPasskeyNotificationStateAsInitiator) {
  NoOpPairingDelegate pairing_delegate(sm::IOCapability::kDisplayOnly);
  pairing_state().SetPairingDelegate(pairing_delegate.GetWeakPtr());

  // Advance state machine.
  pairing_state().InitiatePairing(kNoSecurityRequirements, NoOpStatusCallback);
  static_cast<void>(pairing_state().OnLinkKeyRequest());
  static_cast<void>(pairing_state().OnIoCapabilityRequest());
  pairing_state().OnIoCapabilityResponse(IoCapability::KEYBOARD_ONLY);
  ASSERT_TRUE(pairing_state().initiator());

  RETURN_IF_FATAL(InjectEvent());
  if (event() == UserPasskeyNotification) {
    EXPECT_EQ(0, status_handler().call_count());
  } else {
    EXPECT_EQ(1, status_handler().call_count());
    ASSERT_TRUE(status_handler().status());
    EXPECT_EQ(ToResult(HostError::kNotSupported), status_handler().status());
  }
}

// TODO(xow): Split into three tests depending on the pairing event expected.
TEST_P(HandlesEvent, InWaitUserConfirmationStateAsResponder) {
  NoOpPairingDelegate pairing_delegate(sm::IOCapability::kDisplayOnly);
  pairing_state().SetPairingDelegate(pairing_delegate.GetWeakPtr());

  // Advance state machine.
  pairing_state().OnIoCapabilityResponse(IoCapability::DISPLAY_YES_NO);
  static_cast<void>(pairing_state().OnIoCapabilityRequest());
  ASSERT_FALSE(pairing_state().initiator());

  RETURN_IF_FATAL(InjectEvent());
  if (event() == UserConfirmationRequest) {
    EXPECT_EQ(0, status_handler().call_count());
  } else {
    EXPECT_EQ(1, status_handler().call_count());
    ASSERT_TRUE(status_handler().status());
    EXPECT_EQ(ToResult(HostError::kNotSupported), status_handler().status());
  }
}

TEST_P(HandlesEvent, InWaitUserPasskeyRequestStateAsResponder) {
  NoOpPairingDelegate pairing_delegate(sm::IOCapability::kKeyboardOnly);
  pairing_state().SetPairingDelegate(pairing_delegate.GetWeakPtr());

  // Advance state machine.
  pairing_state().OnIoCapabilityResponse(IoCapability::DISPLAY_ONLY);
  static_cast<void>(pairing_state().OnIoCapabilityRequest());
  ASSERT_FALSE(pairing_state().initiator());

  RETURN_IF_FATAL(InjectEvent());
  if (event() == UserPasskeyRequest) {
    EXPECT_EQ(0, status_handler().call_count());
  } else {
    EXPECT_EQ(1, status_handler().call_count());
    ASSERT_TRUE(status_handler().status());
    EXPECT_EQ(ToResult(HostError::kNotSupported), status_handler().status());
  }
}

TEST_P(HandlesEvent, InWaitUserNotificationStateAsResponder) {
  NoOpPairingDelegate pairing_delegate(sm::IOCapability::kDisplayOnly);
  pairing_state().SetPairingDelegate(pairing_delegate.GetWeakPtr());

  // Advance state machine.
  pairing_state().OnIoCapabilityResponse(IoCapability::KEYBOARD_ONLY);
  static_cast<void>(pairing_state().OnIoCapabilityRequest());
  ASSERT_FALSE(pairing_state().initiator());

  RETURN_IF_FATAL(InjectEvent());
  if (event() == UserPasskeyNotification) {
    EXPECT_EQ(0, status_handler().call_count());
  } else {
    EXPECT_EQ(1, status_handler().call_count());
    ASSERT_TRUE(status_handler().status());
    EXPECT_EQ(ToResult(HostError::kNotSupported), status_handler().status());
  }
}

TEST_P(HandlesEvent, InWaitPairingCompleteState) {
  // Advance state machine.
  pairing_state().OnIoCapabilityResponse(kTestPeerIoCap);
  static_cast<void>(pairing_state().OnIoCapabilityRequest());
  pairing_state().OnUserConfirmationRequest(kTestPasskey,
                                            NoOpUserConfirmationCallback);

  RETURN_IF_FATAL(InjectEvent());
  if (event() == SimplePairingComplete) {
    EXPECT_EQ(0, status_handler().call_count());
  } else {
    EXPECT_EQ(1, status_handler().call_count());
    ASSERT_TRUE(status_handler().status());
    EXPECT_EQ(ToResult(HostError::kNotSupported), status_handler().status());
  }
}

TEST_P(HandlesEvent, InWaitLinkKeyState) {
  // Advance state machine.
  pairing_state().OnIoCapabilityResponse(kTestPeerIoCap);
  static_cast<void>(pairing_state().OnIoCapabilityRequest());
  pairing_state().OnUserConfirmationRequest(kTestPasskey,
                                            NoOpUserConfirmationCallback);
  pairing_state().OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_EQ(0, connection()->start_encryption_count());

  RETURN_IF_FATAL(InjectEvent());
  if (event() == LinkKeyNotification) {
    EXPECT_EQ(0, status_handler().call_count());
    EXPECT_EQ(1, connection()->start_encryption_count());
  } else {
    EXPECT_EQ(1, status_handler().call_count());
    ASSERT_TRUE(status_handler().status());
    EXPECT_EQ(ToResult(HostError::kNotSupported), status_handler().status());
  }
}

TEST_P(HandlesEvent, InInitiatorWaitAuthCompleteStateAfterSimplePairing) {
  // Advance state machine.
  pairing_state().InitiatePairing(kNoSecurityRequirements, NoOpStatusCallback);
  static_cast<void>(pairing_state().OnLinkKeyRequest());
  static_cast<void>(pairing_state().OnIoCapabilityRequest());
  pairing_state().OnIoCapabilityResponse(kTestPeerIoCap);
  pairing_state().OnUserConfirmationRequest(kTestPasskey,
                                            NoOpUserConfirmationCallback);
  pairing_state().OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  pairing_state().OnLinkKeyNotification(kTestLinkKeyValue,
                                        kTestUnauthenticatedLinkKeyType192);
  ASSERT_TRUE(pairing_state().initiator());
  EXPECT_EQ(0, connection()->start_encryption_count());

  RETURN_IF_FATAL(InjectEvent());
  if (event() == AuthenticationComplete) {
    EXPECT_EQ(0, status_handler().call_count());
    EXPECT_EQ(1, connection()->start_encryption_count());
  } else {
    EXPECT_EQ(1, status_handler().call_count());
    ASSERT_TRUE(status_handler().status());
    EXPECT_EQ(ToResult(HostError::kNotSupported), status_handler().status());
  }
}

TEST_P(HandlesEvent, InWaitEncryptionStateAsInitiator) {
  // Advance state machine.
  pairing_state().InitiatePairing(kNoSecurityRequirements, NoOpStatusCallback);
  static_cast<void>(pairing_state().OnLinkKeyRequest());
  static_cast<void>(pairing_state().OnIoCapabilityRequest());
  pairing_state().OnIoCapabilityResponse(kTestPeerIoCap);
  pairing_state().OnUserConfirmationRequest(kTestPasskey,
                                            NoOpUserConfirmationCallback);
  pairing_state().OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  pairing_state().OnLinkKeyNotification(kTestLinkKeyValue,
                                        kTestUnauthenticatedLinkKeyType192);
  pairing_state().OnAuthenticationComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  ASSERT_TRUE(pairing_state().initiator());

  RETURN_IF_FATAL(InjectEvent());

  if (event() == IoCapabilityResponse) {
    // Restarting the pairing is allowed in this state.
    EXPECT_EQ(0, status_handler().call_count());
  } else {
    // Should not receive anything else other than OnEncryptionChange.
    EXPECT_EQ(1, status_handler().call_count());
    ASSERT_TRUE(status_handler().status());
    EXPECT_EQ(ToResult(HostError::kNotSupported), status_handler().status());
  }
}

TEST_P(HandlesEvent, InWaitEncryptionStateAsResponder) {
  // Advance state machine.
  pairing_state().OnIoCapabilityResponse(kTestPeerIoCap);
  static_cast<void>(pairing_state().OnIoCapabilityRequest());
  pairing_state().OnUserConfirmationRequest(kTestPasskey,
                                            NoOpUserConfirmationCallback);
  pairing_state().OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  pairing_state().OnLinkKeyNotification(kTestLinkKeyValue,
                                        kTestUnauthenticatedLinkKeyType192);
  ASSERT_FALSE(pairing_state().initiator());

  RETURN_IF_FATAL(InjectEvent());

  if (event() == IoCapabilityResponse) {
    // Restarting the pairing is allowed in this state.
    EXPECT_EQ(0, status_handler().call_count());
  } else {
    // Should not receive anything else other than OnEncryptionChange.
    EXPECT_EQ(1, status_handler().call_count());
    ASSERT_TRUE(status_handler().status());
    EXPECT_EQ(ToResult(HostError::kNotSupported), status_handler().status());
  }
}

TEST_P(HandlesEvent, InWaitEncryptionStateAsResponderForBonded) {
  // We are previously bonded.
  auto existing_link_key = sm::LTK(
      sm::SecurityProperties(kTestUnauthenticatedLinkKeyType192), kTestLinkKey);
  EXPECT_TRUE(peer()->MutBrEdr().SetBondData(existing_link_key));

  // Advance state machine.
  static_cast<void>(pairing_state().OnLinkKeyRequest());
  ASSERT_FALSE(pairing_state().initiator());

  RETURN_IF_FATAL(InjectEvent());

  if (event() == IoCapabilityResponse) {
    // This re-starts the pairing as a responder.
    EXPECT_EQ(0, status_handler().call_count());
  } else {
    // Should not receive anything else other than OnEncryptionChange, receiving
    // anything else is a failure.
    EXPECT_EQ(1, status_handler().call_count());
    ASSERT_TRUE(status_handler().status());
    EXPECT_EQ(ToResult(HostError::kNotSupported), status_handler().status());
  }
}

TEST_P(HandlesEvent, InIdleStateAfterOnePairing) {
  // Advance state machine.
  static_cast<void>(pairing_state().InitiatePairing(kNoSecurityRequirements,
                                                    NoOpStatusCallback));
  static_cast<void>(pairing_state().OnLinkKeyRequest());
  static_cast<void>(pairing_state().OnIoCapabilityRequest());
  pairing_state().OnIoCapabilityResponse(kTestPeerIoCap);
  pairing_state().OnUserConfirmationRequest(kTestPasskey,
                                            NoOpUserConfirmationCallback);
  pairing_state().OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  pairing_state().OnLinkKeyNotification(kTestLinkKeyValue,
                                        kTestUnauthenticatedLinkKeyType192);
  pairing_state().OnAuthenticationComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  ASSERT_TRUE(pairing_state().initiator());

  // Successfully enabling encryption should allow pairing to start again.
  pairing_state().OnEncryptionChange(fit::ok(true));
  EXPECT_EQ(1, status_handler().call_count());
  ASSERT_TRUE(status_handler().status());
  EXPECT_EQ(fit::ok(), *status_handler().status());
  EXPECT_FALSE(pairing_state().initiator());

  RETURN_IF_FATAL(InjectEvent());
  if (event() == LinkKeyRequest || event() == IoCapabilityResponse) {
    EXPECT_EQ(1, status_handler().call_count());
  } else {
    EXPECT_EQ(2, status_handler().call_count());
    ASSERT_TRUE(status_handler().status());
    EXPECT_EQ(ToResult(HostError::kNotSupported), status_handler().status());
  }
}

TEST_P(HandlesEvent, InFailedStateAfterPairingFailed) {
  // Advance state machine.
  pairing_state().OnIoCapabilityResponse(kTestPeerIoCap);
  static_cast<void>(pairing_state().OnIoCapabilityRequest());
  pairing_state().OnUserConfirmationRequest(kTestPasskey,
                                            NoOpUserConfirmationCallback);

  // Inject failure status.
  pairing_state().OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::AUTHENTICATION_FAILURE);
  EXPECT_EQ(1, status_handler().call_count());
  ASSERT_TRUE(status_handler().status());
  EXPECT_FALSE(status_handler().status()->is_ok());

  RETURN_IF_FATAL(InjectEvent());
  EXPECT_EQ(2, status_handler().call_count());
  ASSERT_TRUE(status_handler().status());
  EXPECT_EQ(ToResult(HostError::kNotSupported), status_handler().status());
}

TEST_P(HandlesEvent, InFailedStateAfterAuthenticationFailed) {
  // Advance state machine.
  static_cast<void>(pairing_state().InitiatePairing(kNoSecurityRequirements,
                                                    NoOpStatusCallback));
  static_cast<void>(pairing_state().OnLinkKeyRequest());
  static_cast<void>(pairing_state().OnIoCapabilityRequest());
  pairing_state().OnIoCapabilityResponse(kTestPeerIoCap);
  pairing_state().OnUserConfirmationRequest(kTestPasskey,
                                            NoOpUserConfirmationCallback);
  pairing_state().OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  pairing_state().OnLinkKeyNotification(kTestLinkKeyValue,
                                        kTestUnauthenticatedLinkKeyType192);

  // Inject failure status.
  pairing_state().OnAuthenticationComplete(
      pw::bluetooth::emboss::StatusCode::AUTHENTICATION_FAILURE);
  EXPECT_EQ(1, status_handler().call_count());
  ASSERT_TRUE(status_handler().status());
  EXPECT_FALSE(status_handler().status()->is_ok());

  RETURN_IF_FATAL(InjectEvent());
  EXPECT_EQ(2, status_handler().call_count());
  ASSERT_TRUE(status_handler().status());
  EXPECT_EQ(ToResult(HostError::kNotSupported), status_handler().status());
}

// PairingAction expected answers are inferred from "device A" Authentication
// Stage 1 specs in v5.0 Vol 3, Part C, Sec 5.2.2.6, Table 5.7.
TEST_F(PairingStateTest, GetInitiatorPairingAction) {
  EXPECT_EQ(PairingAction::kAutomatic,
            GetInitiatorPairingAction(IoCapability::DISPLAY_ONLY,
                                      IoCapability::DISPLAY_ONLY));
  EXPECT_EQ(PairingAction::kDisplayPasskey,
            GetInitiatorPairingAction(IoCapability::DISPLAY_ONLY,
                                      IoCapability::DISPLAY_YES_NO));
  EXPECT_EQ(PairingAction::kDisplayPasskey,
            GetInitiatorPairingAction(IoCapability::DISPLAY_ONLY,
                                      IoCapability::KEYBOARD_ONLY));
  EXPECT_EQ(PairingAction::kAutomatic,
            GetInitiatorPairingAction(IoCapability::DISPLAY_ONLY,
                                      IoCapability::NO_INPUT_NO_OUTPUT));

  EXPECT_EQ(PairingAction::kComparePasskey,
            GetInitiatorPairingAction(IoCapability::DISPLAY_YES_NO,
                                      IoCapability::DISPLAY_ONLY));
  EXPECT_EQ(PairingAction::kDisplayPasskey,
            GetInitiatorPairingAction(IoCapability::DISPLAY_YES_NO,
                                      IoCapability::DISPLAY_YES_NO));
  EXPECT_EQ(PairingAction::kDisplayPasskey,
            GetInitiatorPairingAction(IoCapability::DISPLAY_YES_NO,
                                      IoCapability::KEYBOARD_ONLY));
  EXPECT_EQ(PairingAction::kGetConsent,
            GetInitiatorPairingAction(IoCapability::DISPLAY_YES_NO,
                                      IoCapability::NO_INPUT_NO_OUTPUT));

  EXPECT_EQ(PairingAction::kRequestPasskey,
            GetInitiatorPairingAction(IoCapability::KEYBOARD_ONLY,
                                      IoCapability::DISPLAY_ONLY));
  EXPECT_EQ(PairingAction::kRequestPasskey,
            GetInitiatorPairingAction(IoCapability::KEYBOARD_ONLY,
                                      IoCapability::DISPLAY_YES_NO));
  EXPECT_EQ(PairingAction::kRequestPasskey,
            GetInitiatorPairingAction(IoCapability::KEYBOARD_ONLY,
                                      IoCapability::KEYBOARD_ONLY));
  EXPECT_EQ(PairingAction::kAutomatic,
            GetInitiatorPairingAction(IoCapability::KEYBOARD_ONLY,
                                      IoCapability::NO_INPUT_NO_OUTPUT));

  EXPECT_EQ(PairingAction::kAutomatic,
            GetInitiatorPairingAction(IoCapability::NO_INPUT_NO_OUTPUT,
                                      IoCapability::DISPLAY_ONLY));
  EXPECT_EQ(PairingAction::kAutomatic,
            GetInitiatorPairingAction(IoCapability::NO_INPUT_NO_OUTPUT,
                                      IoCapability::DISPLAY_YES_NO));
  EXPECT_EQ(PairingAction::kAutomatic,
            GetInitiatorPairingAction(IoCapability::NO_INPUT_NO_OUTPUT,
                                      IoCapability::KEYBOARD_ONLY));
  EXPECT_EQ(PairingAction::kAutomatic,
            GetInitiatorPairingAction(IoCapability::NO_INPUT_NO_OUTPUT,
                                      IoCapability::NO_INPUT_NO_OUTPUT));
}

// Ibid., but for "device B."
TEST_F(PairingStateTest, GetResponderPairingAction) {
  EXPECT_EQ(PairingAction::kAutomatic,
            GetResponderPairingAction(IoCapability::DISPLAY_ONLY,
                                      IoCapability::DISPLAY_ONLY));
  EXPECT_EQ(PairingAction::kComparePasskey,
            GetResponderPairingAction(IoCapability::DISPLAY_ONLY,
                                      IoCapability::DISPLAY_YES_NO));
  EXPECT_EQ(PairingAction::kRequestPasskey,
            GetResponderPairingAction(IoCapability::DISPLAY_ONLY,
                                      IoCapability::KEYBOARD_ONLY));
  EXPECT_EQ(PairingAction::kAutomatic,
            GetResponderPairingAction(IoCapability::DISPLAY_ONLY,
                                      IoCapability::NO_INPUT_NO_OUTPUT));

  EXPECT_EQ(PairingAction::kDisplayPasskey,
            GetResponderPairingAction(IoCapability::DISPLAY_YES_NO,
                                      IoCapability::DISPLAY_ONLY));
  EXPECT_EQ(PairingAction::kComparePasskey,
            GetResponderPairingAction(IoCapability::DISPLAY_YES_NO,
                                      IoCapability::DISPLAY_YES_NO));
  EXPECT_EQ(PairingAction::kRequestPasskey,
            GetResponderPairingAction(IoCapability::DISPLAY_YES_NO,
                                      IoCapability::KEYBOARD_ONLY));
  EXPECT_EQ(PairingAction::kAutomatic,
            GetResponderPairingAction(IoCapability::DISPLAY_YES_NO,
                                      IoCapability::NO_INPUT_NO_OUTPUT));

  EXPECT_EQ(PairingAction::kDisplayPasskey,
            GetResponderPairingAction(IoCapability::KEYBOARD_ONLY,
                                      IoCapability::DISPLAY_ONLY));
  EXPECT_EQ(PairingAction::kDisplayPasskey,
            GetResponderPairingAction(IoCapability::KEYBOARD_ONLY,
                                      IoCapability::DISPLAY_YES_NO));
  EXPECT_EQ(PairingAction::kRequestPasskey,
            GetResponderPairingAction(IoCapability::KEYBOARD_ONLY,
                                      IoCapability::KEYBOARD_ONLY));
  EXPECT_EQ(PairingAction::kAutomatic,
            GetResponderPairingAction(IoCapability::KEYBOARD_ONLY,
                                      IoCapability::NO_INPUT_NO_OUTPUT));

  EXPECT_EQ(PairingAction::kAutomatic,
            GetResponderPairingAction(IoCapability::NO_INPUT_NO_OUTPUT,
                                      IoCapability::DISPLAY_ONLY));
  EXPECT_EQ(PairingAction::kGetConsent,
            GetResponderPairingAction(IoCapability::NO_INPUT_NO_OUTPUT,
                                      IoCapability::DISPLAY_YES_NO));
  EXPECT_EQ(PairingAction::kGetConsent,
            GetResponderPairingAction(IoCapability::NO_INPUT_NO_OUTPUT,
                                      IoCapability::KEYBOARD_ONLY));
  EXPECT_EQ(PairingAction::kAutomatic,
            GetResponderPairingAction(IoCapability::NO_INPUT_NO_OUTPUT,
                                      IoCapability::NO_INPUT_NO_OUTPUT));
}

// Events are obtained from ibid. association models, mapped to HCI sequences in
// v5.0 Vol 3, Vol 2, Part F, Sec 4.2.10–15.
TEST_F(PairingStateTest, GetExpectedEvent) {
  EXPECT_EQ(
      kUserConfirmationRequestEventCode,
      GetExpectedEvent(IoCapability::DISPLAY_ONLY, IoCapability::DISPLAY_ONLY));
  EXPECT_EQ(kUserConfirmationRequestEventCode,
            GetExpectedEvent(IoCapability::DISPLAY_ONLY,
                             IoCapability::DISPLAY_YES_NO));
  EXPECT_EQ(kUserPasskeyNotificationEventCode,
            GetExpectedEvent(IoCapability::DISPLAY_ONLY,
                             IoCapability::KEYBOARD_ONLY));
  EXPECT_EQ(kUserConfirmationRequestEventCode,
            GetExpectedEvent(IoCapability::DISPLAY_ONLY,
                             IoCapability::NO_INPUT_NO_OUTPUT));

  EXPECT_EQ(kUserConfirmationRequestEventCode,
            GetExpectedEvent(IoCapability::DISPLAY_YES_NO,
                             IoCapability::DISPLAY_ONLY));
  EXPECT_EQ(kUserConfirmationRequestEventCode,
            GetExpectedEvent(IoCapability::DISPLAY_YES_NO,
                             IoCapability::DISPLAY_YES_NO));
  EXPECT_EQ(kUserPasskeyNotificationEventCode,
            GetExpectedEvent(IoCapability::DISPLAY_YES_NO,
                             IoCapability::KEYBOARD_ONLY));
  EXPECT_EQ(kUserConfirmationRequestEventCode,
            GetExpectedEvent(IoCapability::DISPLAY_YES_NO,
                             IoCapability::NO_INPUT_NO_OUTPUT));

  EXPECT_EQ(kUserPasskeyRequestEventCode,
            GetExpectedEvent(IoCapability::KEYBOARD_ONLY,
                             IoCapability::DISPLAY_ONLY));
  EXPECT_EQ(kUserPasskeyRequestEventCode,
            GetExpectedEvent(IoCapability::KEYBOARD_ONLY,
                             IoCapability::DISPLAY_YES_NO));
  EXPECT_EQ(kUserPasskeyRequestEventCode,
            GetExpectedEvent(IoCapability::KEYBOARD_ONLY,
                             IoCapability::KEYBOARD_ONLY));
  EXPECT_EQ(kUserConfirmationRequestEventCode,
            GetExpectedEvent(IoCapability::KEYBOARD_ONLY,
                             IoCapability::NO_INPUT_NO_OUTPUT));

  EXPECT_EQ(kUserConfirmationRequestEventCode,
            GetExpectedEvent(IoCapability::NO_INPUT_NO_OUTPUT,
                             IoCapability::DISPLAY_ONLY));
  EXPECT_EQ(kUserConfirmationRequestEventCode,
            GetExpectedEvent(IoCapability::NO_INPUT_NO_OUTPUT,
                             IoCapability::DISPLAY_YES_NO));
  EXPECT_EQ(kUserConfirmationRequestEventCode,
            GetExpectedEvent(IoCapability::NO_INPUT_NO_OUTPUT,
                             IoCapability::KEYBOARD_ONLY));
  EXPECT_EQ(kUserConfirmationRequestEventCode,
            GetExpectedEvent(IoCapability::NO_INPUT_NO_OUTPUT,
                             IoCapability::NO_INPUT_NO_OUTPUT));
}

// Level of authentication from ibid. table.
TEST_F(PairingStateTest, IsPairingAuthenticated) {
  EXPECT_FALSE(IsPairingAuthenticated(IoCapability::DISPLAY_ONLY,
                                      IoCapability::DISPLAY_ONLY));
  EXPECT_FALSE(IsPairingAuthenticated(IoCapability::DISPLAY_ONLY,
                                      IoCapability::DISPLAY_YES_NO));
  EXPECT_TRUE(IsPairingAuthenticated(IoCapability::DISPLAY_ONLY,
                                     IoCapability::KEYBOARD_ONLY));
  EXPECT_FALSE(IsPairingAuthenticated(IoCapability::DISPLAY_ONLY,
                                      IoCapability::NO_INPUT_NO_OUTPUT));

  EXPECT_FALSE(IsPairingAuthenticated(IoCapability::DISPLAY_YES_NO,
                                      IoCapability::DISPLAY_ONLY));
  EXPECT_TRUE(IsPairingAuthenticated(IoCapability::DISPLAY_YES_NO,
                                     IoCapability::DISPLAY_YES_NO));
  EXPECT_TRUE(IsPairingAuthenticated(IoCapability::DISPLAY_YES_NO,
                                     IoCapability::KEYBOARD_ONLY));
  EXPECT_FALSE(IsPairingAuthenticated(IoCapability::DISPLAY_YES_NO,
                                      IoCapability::NO_INPUT_NO_OUTPUT));

  EXPECT_TRUE(IsPairingAuthenticated(IoCapability::KEYBOARD_ONLY,
                                     IoCapability::DISPLAY_ONLY));
  EXPECT_TRUE(IsPairingAuthenticated(IoCapability::KEYBOARD_ONLY,
                                     IoCapability::DISPLAY_YES_NO));
  EXPECT_TRUE(IsPairingAuthenticated(IoCapability::KEYBOARD_ONLY,
                                     IoCapability::KEYBOARD_ONLY));
  EXPECT_FALSE(IsPairingAuthenticated(IoCapability::KEYBOARD_ONLY,
                                      IoCapability::NO_INPUT_NO_OUTPUT));

  EXPECT_FALSE(IsPairingAuthenticated(IoCapability::NO_INPUT_NO_OUTPUT,
                                      IoCapability::DISPLAY_ONLY));
  EXPECT_FALSE(IsPairingAuthenticated(IoCapability::NO_INPUT_NO_OUTPUT,
                                      IoCapability::DISPLAY_YES_NO));
  EXPECT_FALSE(IsPairingAuthenticated(IoCapability::NO_INPUT_NO_OUTPUT,
                                      IoCapability::KEYBOARD_ONLY));
  EXPECT_FALSE(IsPairingAuthenticated(IoCapability::NO_INPUT_NO_OUTPUT,
                                      IoCapability::NO_INPUT_NO_OUTPUT));
}

TEST_F(PairingStateTest, GetInitiatorAuthenticationRequirements) {
  EXPECT_EQ(AuthenticationRequirements::MITM_GENERAL_BONDING,
            GetInitiatorAuthenticationRequirements(IoCapability::DISPLAY_ONLY));
  EXPECT_EQ(
      AuthenticationRequirements::MITM_GENERAL_BONDING,
      GetInitiatorAuthenticationRequirements(IoCapability::DISPLAY_YES_NO));
  EXPECT_EQ(
      AuthenticationRequirements::MITM_GENERAL_BONDING,
      GetInitiatorAuthenticationRequirements(IoCapability::KEYBOARD_ONLY));
  EXPECT_EQ(
      AuthenticationRequirements::GENERAL_BONDING,
      GetInitiatorAuthenticationRequirements(IoCapability::NO_INPUT_NO_OUTPUT));
}

TEST_F(PairingStateTest, GetResponderAuthenticationRequirements) {
  EXPECT_EQ(AuthenticationRequirements::GENERAL_BONDING,
            GetResponderAuthenticationRequirements(IoCapability::DISPLAY_ONLY,
                                                   IoCapability::DISPLAY_ONLY));
  EXPECT_EQ(AuthenticationRequirements::GENERAL_BONDING,
            GetResponderAuthenticationRequirements(
                IoCapability::DISPLAY_ONLY, IoCapability::DISPLAY_YES_NO));
  EXPECT_EQ(AuthenticationRequirements::MITM_GENERAL_BONDING,
            GetResponderAuthenticationRequirements(
                IoCapability::DISPLAY_ONLY, IoCapability::KEYBOARD_ONLY));
  EXPECT_EQ(AuthenticationRequirements::GENERAL_BONDING,
            GetResponderAuthenticationRequirements(
                IoCapability::DISPLAY_ONLY, IoCapability::NO_INPUT_NO_OUTPUT));

  EXPECT_EQ(AuthenticationRequirements::GENERAL_BONDING,
            GetResponderAuthenticationRequirements(IoCapability::DISPLAY_YES_NO,
                                                   IoCapability::DISPLAY_ONLY));
  EXPECT_EQ(AuthenticationRequirements::MITM_GENERAL_BONDING,
            GetResponderAuthenticationRequirements(
                IoCapability::DISPLAY_YES_NO, IoCapability::DISPLAY_YES_NO));
  EXPECT_EQ(AuthenticationRequirements::MITM_GENERAL_BONDING,
            GetResponderAuthenticationRequirements(
                IoCapability::DISPLAY_YES_NO, IoCapability::KEYBOARD_ONLY));
  EXPECT_EQ(
      AuthenticationRequirements::GENERAL_BONDING,
      GetResponderAuthenticationRequirements(IoCapability::DISPLAY_YES_NO,
                                             IoCapability::NO_INPUT_NO_OUTPUT));

  EXPECT_EQ(AuthenticationRequirements::MITM_GENERAL_BONDING,
            GetResponderAuthenticationRequirements(IoCapability::KEYBOARD_ONLY,
                                                   IoCapability::DISPLAY_ONLY));
  EXPECT_EQ(AuthenticationRequirements::MITM_GENERAL_BONDING,
            GetResponderAuthenticationRequirements(
                IoCapability::KEYBOARD_ONLY, IoCapability::DISPLAY_YES_NO));
  EXPECT_EQ(AuthenticationRequirements::MITM_GENERAL_BONDING,
            GetResponderAuthenticationRequirements(
                IoCapability::KEYBOARD_ONLY, IoCapability::KEYBOARD_ONLY));
  EXPECT_EQ(AuthenticationRequirements::GENERAL_BONDING,
            GetResponderAuthenticationRequirements(
                IoCapability::KEYBOARD_ONLY, IoCapability::NO_INPUT_NO_OUTPUT));

  EXPECT_EQ(AuthenticationRequirements::GENERAL_BONDING,
            GetResponderAuthenticationRequirements(
                IoCapability::NO_INPUT_NO_OUTPUT, IoCapability::DISPLAY_ONLY));
  EXPECT_EQ(
      AuthenticationRequirements::GENERAL_BONDING,
      GetResponderAuthenticationRequirements(IoCapability::NO_INPUT_NO_OUTPUT,
                                             IoCapability::DISPLAY_YES_NO));
  EXPECT_EQ(AuthenticationRequirements::GENERAL_BONDING,
            GetResponderAuthenticationRequirements(
                IoCapability::NO_INPUT_NO_OUTPUT, IoCapability::KEYBOARD_ONLY));
  EXPECT_EQ(
      AuthenticationRequirements::GENERAL_BONDING,
      GetResponderAuthenticationRequirements(IoCapability::NO_INPUT_NO_OUTPUT,
                                             IoCapability::NO_INPUT_NO_OUTPUT));
}

TEST_F(PairingStateTest, SkipPairingIfExistingKeyMeetsSecurityRequirements) {
  NoOpPairingDelegate pairing_delegate(sm::IOCapability::kNoInputNoOutput);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  connection()->set_link_key(kTestLinkKey, kTestAuthenticatedLinkKeyType192);

  constexpr BrEdrSecurityRequirements kSecurityRequirements{
      .authentication = true, .secure_connections = false};
  TestStatusHandler initiator_status_handler;
  pairing_state.InitiatePairing(kSecurityRequirements,
                                initiator_status_handler.MakeStatusCallback());
  EXPECT_EQ(0u, auth_request_count());
  EXPECT_FALSE(pairing_state.initiator());
  EXPECT_EQ(0, status_handler.call_count());
  ASSERT_EQ(1, initiator_status_handler.call_count());
  EXPECT_EQ(fit::ok(), *initiator_status_handler.status());
}

TEST_F(
    PairingStateTest,
    InitiatorAuthRequiredCausesOnLinkKeyRequestToReturnNullIfUnauthenticatedKeyExists) {
  NoOpPairingDelegate pairing_delegate(sm::IOCapability::kNoInputNoOutput);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  BrEdrSecurityRequirements security{.authentication = true,
                                     .secure_connections = false};
  pairing_state.InitiatePairing(security, status_handler.MakeStatusCallback());

  EXPECT_TRUE(peer()->MutBrEdr().SetBondData(
      sm::LTK(sm::SecurityProperties(kTestUnauthenticatedLinkKeyType192),
              kTestLinkKey)));

  EXPECT_EQ(std::nullopt, pairing_state.OnLinkKeyRequest());
  EXPECT_EQ(0, status_handler.call_count());
}

TEST_F(
    PairingStateTest,
    InitiatorNoSecurityRequirementsCausesOnLinkKeyRequestToReturnExistingKey) {
  NoOpPairingDelegate pairing_delegate(sm::IOCapability::kNoInputNoOutput);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  pairing_state.InitiatePairing(kNoSecurityRequirements, NoOpStatusCallback);

  EXPECT_TRUE(peer()->MutBrEdr().SetBondData(
      sm::LTK(sm::SecurityProperties(kTestUnauthenticatedLinkKeyType192),
              kTestLinkKey)));
  EXPECT_FALSE(connection()->ltk().has_value());

  auto reply_key = pairing_state.OnLinkKeyRequest();
  ASSERT_TRUE(reply_key.has_value());
  EXPECT_EQ(kTestLinkKey, reply_key.value());
  EXPECT_EQ(0, status_handler.call_count());
  EXPECT_TRUE(connection()->ltk().has_value());
}

TEST_F(PairingStateTest,
       InitiatorOnLinkKeyRequestReturnsNullIfBondDataDoesNotExist) {
  NoOpPairingDelegate pairing_delegate(sm::IOCapability::kNoInputNoOutput);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  pairing_state.InitiatePairing(kNoSecurityRequirements, NoOpStatusCallback);

  auto reply_key = pairing_state.OnLinkKeyRequest();
  EXPECT_FALSE(reply_key.has_value());
  EXPECT_EQ(0, status_handler.call_count());
}

TEST_F(PairingStateTest,
       IdleStateOnLinkKeyRequestReturnsLinkKeyWhenBondDataExists) {
  NoOpPairingDelegate pairing_delegate(sm::IOCapability::kNoInputNoOutput);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  EXPECT_TRUE(peer()->MutBrEdr().SetBondData(
      sm::LTK(sm::SecurityProperties(kTestUnauthenticatedLinkKeyType192),
              kTestLinkKey)));
  EXPECT_FALSE(connection()->ltk().has_value());

  auto reply_key = pairing_state.OnLinkKeyRequest();
  ASSERT_TRUE(reply_key.has_value());
  EXPECT_EQ(kTestLinkKey, reply_key.value());
  EXPECT_EQ(0, status_handler.call_count());
  EXPECT_TRUE(connection()->ltk().has_value());
}

TEST_F(PairingStateTest,
       IdleStateOnLinkKeyRequestReturnsNullWhenBondDataDoesNotExist) {
  NoOpPairingDelegate pairing_delegate(sm::IOCapability::kNoInputNoOutput);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  auto reply_key = pairing_state.OnLinkKeyRequest();
  EXPECT_FALSE(reply_key.has_value());
  EXPECT_EQ(0, status_handler.call_count());
}

TEST_F(PairingStateTest,
       SimplePairingCompleteWithErrorCodeReceivedEarlyFailsPairing) {
  NoOpPairingDelegate pairing_delegate(sm::IOCapability::kNoInputNoOutput);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  pairing_state.InitiatePairing(kNoSecurityRequirements, NoOpStatusCallback);

  EXPECT_EQ(std::nullopt, pairing_state.OnLinkKeyRequest());
  EXPECT_EQ(IoCapability::NO_INPUT_NO_OUTPUT,
            *pairing_state.OnIoCapabilityRequest());
  EXPECT_EQ(0, status_handler.call_count());

  const auto status_code =
      pw::bluetooth::emboss::StatusCode::PAIRING_NOT_ALLOWED;
  pairing_state.OnSimplePairingComplete(status_code);
  ASSERT_EQ(1, status_handler.call_count());
  EXPECT_EQ(ToResult(status_code), status_handler.status().value());
}

TEST_F(PairingStateDeathTest, OnLinkKeyRequestReceivedMissingPeerAsserts) {
  NoOpPairingDelegate pairing_delegate(sm::IOCapability::kNoInputNoOutput);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  pairing_state.InitiatePairing(kNoSecurityRequirements, NoOpStatusCallback);

  EXPECT_TRUE(peer_cache()->RemoveDisconnectedPeer(peer()->identifier()));

  ASSERT_DEATH_IF_SUPPORTED(
      { [[maybe_unused]] auto reply_key = pairing_state.OnLinkKeyRequest(); },
      ".*peer.*");
}

TEST_F(PairingStateTest,
       AuthenticationCompleteWithErrorCodeReceivedEarlyFailsPairing) {
  NoOpPairingDelegate pairing_delegate(sm::IOCapability::kNoInputNoOutput);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  pairing_state.InitiatePairing(kNoSecurityRequirements, NoOpStatusCallback);

  EXPECT_EQ(std::nullopt, pairing_state.OnLinkKeyRequest());
  EXPECT_EQ(0, status_handler.call_count());

  const auto status_code =
      pw::bluetooth::emboss::StatusCode::AUTHENTICATION_FAILURE;
  pairing_state.OnAuthenticationComplete(status_code);
  ASSERT_EQ(1, status_handler.call_count());
  EXPECT_EQ(ToResult(status_code), status_handler.status().value());
}

TEST_F(
    PairingStateTest,
    AuthenticationCompleteWithMissingKeyRetriesWithoutKeyAndDoesntAutoConfirmRejected) {
  FakePairingDelegate pairing_delegate(sm::IOCapability::kNoInputNoOutput);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/true,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  auto existing_link_key = sm::LTK(
      sm::SecurityProperties(kTestUnauthenticatedLinkKeyType192), kTestLinkKey);

  EXPECT_TRUE(peer()->MutBrEdr().SetBondData(existing_link_key));
  EXPECT_FALSE(connection()->ltk().has_value());

  pairing_state.InitiatePairing(kNoSecurityRequirements, NoOpStatusCallback);
  EXPECT_EQ(1u, auth_request_count());

  auto reply_key = pairing_state.OnLinkKeyRequest();
  ASSERT_TRUE(reply_key.has_value());
  EXPECT_EQ(kTestLinkKey, reply_key.value());
  EXPECT_EQ(0, status_handler.call_count());

  // Peer says that they don't have a key.
  pairing_state.OnAuthenticationComplete(
      pw::bluetooth::emboss::StatusCode::PIN_OR_KEY_MISSING);
  ASSERT_EQ(0, status_handler.call_count());
  // We should retry the authentication request, this time pretending we don't
  // have a key.
  EXPECT_EQ(2u, auth_request_count());

  EXPECT_EQ(std::nullopt, pairing_state.OnLinkKeyRequest());
  EXPECT_EQ(0, status_handler.call_count());

  static_cast<void>(pairing_state.OnIoCapabilityRequest());
  pairing_state.OnIoCapabilityResponse(IoCapability::NO_INPUT_NO_OUTPUT);
  pairing_delegate.SetConfirmPairingCallback([this](PeerId peer_id, auto cb) {
    EXPECT_EQ(peer()->identifier(), peer_id);
    ASSERT_TRUE(cb);
    cb(false);
  });
  bool confirmed = true;
  pairing_state.OnUserConfirmationRequest(
      kTestPasskey, [&confirmed](bool confirm) { confirmed = confirm; });

  EXPECT_FALSE(confirmed);

  pairing_delegate.SetCompletePairingCallback(
      [this](PeerId peer_id, sm::Result<> status) {
        EXPECT_EQ(peer()->identifier(), peer_id);
        EXPECT_TRUE(status.is_error());
      });

  // The controller sends a SimplePairingComplete indicating the failure after
  // we send a Negative Confirmation.
  const auto status_code =
      pw::bluetooth::emboss::StatusCode::AUTHENTICATION_FAILURE;
  pairing_state.OnSimplePairingComplete(status_code);

  // The bonding key should not have been touched.
  EXPECT_EQ(existing_link_key, peer()->bredr()->link_key());

  EXPECT_EQ(1, status_handler.call_count());
  ASSERT_TRUE(status_handler.status().has_value());
  EXPECT_EQ(ToResult(status_code), status_handler.status().value());
}

TEST_F(PairingStateTest, ResponderSignalsCompletionOfPairing) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());
  EXPECT_FALSE(pairing_state.initiator());
  EXPECT_FALSE(peer()->MutBrEdr().is_pairing());

  auto existing_link_key = sm::LTK(
      sm::SecurityProperties(kTestUnauthenticatedLinkKeyType192), kTestLinkKey);

  EXPECT_TRUE(peer()->MutBrEdr().SetBondData(existing_link_key));
  EXPECT_FALSE(connection()->ltk().has_value());

  auto reply_key = pairing_state.OnLinkKeyRequest();
  ASSERT_TRUE(reply_key.has_value());
  EXPECT_EQ(kTestLinkKey, reply_key.value());
  EXPECT_EQ(0, status_handler.call_count());
  EXPECT_TRUE(peer()->MutBrEdr().is_pairing());

  // If a pairing request comes in after the peer has already asked for the key,
  // we add it's completion to the queue.
  TestStatusHandler new_pairing_handler;
  pairing_state.InitiatePairing(kNoSecurityRequirements,
                                new_pairing_handler.MakeStatusCallback());

  connection()->TriggerEncryptionChangeCallback(fit::ok(true));

  auto expected_status = pw::bluetooth::emboss::StatusCode::SUCCESS;
  EXPECT_EQ(1, status_handler.call_count());
  ASSERT_TRUE(status_handler.status().has_value());
  EXPECT_EQ(ToResult(expected_status), status_handler.status().value());
  EXPECT_FALSE(peer()->MutBrEdr().is_pairing());

  // and the new pairing handler gets called back too
  EXPECT_EQ(1, new_pairing_handler.call_count());
  ASSERT_TRUE(new_pairing_handler.status().has_value());
  EXPECT_EQ(ToResult(expected_status), new_pairing_handler.status().value());

  // The link key should be stored in the connection now.
  EXPECT_EQ(kTestLinkKey, connection()->ltk());
}

TEST_F(
    PairingStateTest,
    AuthenticationCompleteWithMissingKeyRetriesWithoutKeyAndDoesntAutoConfirmAccepted) {
  FakePairingDelegate pairing_delegate(sm::IOCapability::kNoInputNoOutput);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/true,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  auto existing_link_key = sm::LTK(
      sm::SecurityProperties(kTestUnauthenticatedLinkKeyType192), kTestLinkKey);

  EXPECT_TRUE(peer()->MutBrEdr().SetBondData(existing_link_key));
  EXPECT_FALSE(connection()->ltk().has_value());

  pairing_state.InitiatePairing(kNoSecurityRequirements, NoOpStatusCallback);
  EXPECT_EQ(1u, auth_request_count());

  auto reply_key = pairing_state.OnLinkKeyRequest();
  ASSERT_TRUE(reply_key.has_value());
  EXPECT_EQ(kTestLinkKey, reply_key.value());
  EXPECT_EQ(0, status_handler.call_count());

  // Peer says that they don't have a key.
  pairing_state.OnAuthenticationComplete(
      pw::bluetooth::emboss::StatusCode::PIN_OR_KEY_MISSING);
  ASSERT_EQ(0, status_handler.call_count());
  // We should retry the authentication request, this time pretending we don't
  // have a key.
  EXPECT_EQ(2u, auth_request_count());

  EXPECT_EQ(std::nullopt, pairing_state.OnLinkKeyRequest());
  EXPECT_EQ(0, status_handler.call_count());

  static_cast<void>(pairing_state.OnIoCapabilityRequest());
  pairing_state.OnIoCapabilityResponse(IoCapability::NO_INPUT_NO_OUTPUT);
  pairing_delegate.SetConfirmPairingCallback([this](PeerId peer_id, auto cb) {
    EXPECT_EQ(peer()->identifier(), peer_id);
    ASSERT_TRUE(cb);
    cb(true);
  });
  bool confirmed = false;
  pairing_state.OnUserConfirmationRequest(
      kTestPasskey, [&confirmed](bool confirm) { confirmed = confirm; });

  EXPECT_TRUE(confirmed);

  pairing_delegate.SetCompletePairingCallback(
      [this](PeerId peer_id, sm::Result<> status) {
        EXPECT_EQ(peer()->identifier(), peer_id);
        EXPECT_EQ(fit::ok(), status);
      });

  // The controller sends a SimplePairingComplete indicating the success, then
  // the controller sends us the new link key, and Authentication Complete.
  // Negative Confirmation.
  auto status_code = pw::bluetooth::emboss::StatusCode::SUCCESS;
  pairing_state.OnSimplePairingComplete(status_code);

  const auto new_link_key_value = UInt128{0xC0,
                                          0xDE,
                                          0xFA,
                                          0xCE,
                                          0x00,
                                          0x00,
                                          0x00,
                                          0x00,
                                          0x00,
                                          0x00,
                                          0x00,
                                          0x00,
                                          0x00,
                                          0x00,
                                          0x00,
                                          0x04};

  pairing_state.OnLinkKeyNotification(new_link_key_value,
                                      kTestUnauthenticatedLinkKeyType192);
  pairing_state.OnAuthenticationComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  // then we request encryption, which when it finishes, completes pairing.
  ASSERT_EQ(1, connection()->start_encryption_count());
  connection()->TriggerEncryptionChangeCallback(fit::ok(true));

  EXPECT_EQ(1, status_handler.call_count());
  ASSERT_TRUE(status_handler.status().has_value());
  EXPECT_EQ(ToResult(status_code), status_handler.status().value());

  // The new link key should be stored in the connection now.
  auto new_link_key = hci_spec::LinkKey(new_link_key_value, 0, 0);
  EXPECT_EQ(new_link_key, connection()->ltk());
}

TEST_F(
    PairingStateTest,
    MultipleQueuedPairingRequestsWithSameSecurityRequirementsCompleteAtSameTimeWithSuccess) {
  NoOpPairingDelegate pairing_delegate(sm::IOCapability::kNoInputNoOutput);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  TestStatusHandler initiate_status_handler_0;
  pairing_state.InitiatePairing(kNoSecurityRequirements,
                                initiate_status_handler_0.MakeStatusCallback());
  EXPECT_EQ(1u, auth_request_count());

  TestStatusHandler initiate_status_handler_1;
  pairing_state.InitiatePairing(kNoSecurityRequirements,
                                initiate_status_handler_1.MakeStatusCallback());
  EXPECT_EQ(1u, auth_request_count());

  AdvanceToEncryptionAsInitiator(&pairing_state);
  EXPECT_EQ(0, status_handler.call_count());
  EXPECT_EQ(1, connection()->start_encryption_count());

  connection()->TriggerEncryptionChangeCallback(fit::ok(true));
  EXPECT_EQ(1, status_handler.call_count());
  ASSERT_TRUE(status_handler.status());
  EXPECT_EQ(fit::ok(), *status_handler.status());
  ASSERT_EQ(1, initiate_status_handler_0.call_count());
  EXPECT_EQ(fit::ok(), *initiate_status_handler_0.status());
  ASSERT_EQ(1, initiate_status_handler_1.call_count());
  EXPECT_EQ(fit::ok(), *initiate_status_handler_1.status());
}

TEST_F(
    PairingStateTest,
    MultipleQueuedPairingRequestsWithAuthSecurityRequirementsCompleteAtSameTimeWithInsufficientSecurityFailure) {
  NoOpPairingDelegate pairing_delegate(sm::IOCapability::kNoInputNoOutput);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  constexpr BrEdrSecurityRequirements kSecurityRequirements{
      .authentication = true, .secure_connections = false};

  TestStatusHandler initiate_status_handler_0;
  pairing_state.InitiatePairing(kSecurityRequirements,
                                initiate_status_handler_0.MakeStatusCallback());
  EXPECT_EQ(1u, auth_request_count());

  TestStatusHandler initiate_status_handler_1;
  pairing_state.InitiatePairing(kSecurityRequirements,
                                initiate_status_handler_1.MakeStatusCallback());
  EXPECT_EQ(1u, auth_request_count());

  // Pair with unauthenticated link key.
  AdvanceToEncryptionAsInitiator(&pairing_state);
  EXPECT_EQ(0, status_handler.call_count());
  EXPECT_EQ(1, connection()->start_encryption_count());

  connection()->TriggerEncryptionChangeCallback(fit::ok(true));
  EXPECT_EQ(1, status_handler.call_count());
  ASSERT_TRUE(status_handler.status());
  EXPECT_EQ(fit::ok(), *status_handler.status());
  ASSERT_EQ(1, initiate_status_handler_0.call_count());
  EXPECT_EQ(ToResult(HostError::kInsufficientSecurity),
            initiate_status_handler_0.status().value());
  ASSERT_EQ(1, initiate_status_handler_1.call_count());
  EXPECT_EQ(ToResult(HostError::kInsufficientSecurity),
            initiate_status_handler_1.status().value());
}

TEST_F(
    PairingStateTest,
    AuthPairingRequestDuringInitiatorNoAuthPairingFailsQueuedAuthPairingRequest) {
  NoOpPairingDelegate pairing_delegate(sm::IOCapability::kNoInputNoOutput);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  TestStatusHandler initiate_status_handler_0;
  pairing_state.InitiatePairing(kNoSecurityRequirements,
                                initiate_status_handler_0.MakeStatusCallback());

  TestStatusHandler initiate_status_handler_1;
  constexpr BrEdrSecurityRequirements kSecurityRequirements{
      .authentication = true, .secure_connections = false};
  pairing_state.InitiatePairing(kSecurityRequirements,
                                initiate_status_handler_1.MakeStatusCallback());

  // Pair with unauthenticated link key.
  AdvanceToEncryptionAsInitiator(&pairing_state);

  EXPECT_EQ(0, status_handler.call_count());
  EXPECT_EQ(1, connection()->start_encryption_count());

  FakePairingDelegate fake_pairing_delegate(kTestLocalIoCap);
  pairing_state.SetPairingDelegate(fake_pairing_delegate.GetWeakPtr());

  connection()->TriggerEncryptionChangeCallback(fit::ok(true));
  EXPECT_EQ(1, status_handler.call_count());
  ASSERT_TRUE(status_handler.status());
  EXPECT_EQ(fit::ok(), *status_handler.status());
  ASSERT_EQ(1, initiate_status_handler_0.call_count());
  EXPECT_EQ(fit::ok(), *initiate_status_handler_0.status());
  ASSERT_EQ(1, initiate_status_handler_1.call_count());
  EXPECT_EQ(ToResult(HostError::kInsufficientSecurity),
            initiate_status_handler_1.status().value());

  // Pairing for second request should not start.
  EXPECT_FALSE(pairing_state.initiator());
}

TEST_F(
    PairingStateTest,
    InitiatingPairingDuringAuthenticationWithExistingUnauthenticatedLinkKey) {
  FakePairingDelegate pairing_delegate(kTestLocalIoCap);

  TestStatusHandler status_handler;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  EXPECT_TRUE(peer()->MutBrEdr().SetBondData(
      sm::LTK(sm::SecurityProperties(kTestUnauthenticatedLinkKeyType192),
              kTestLinkKey)));

  TestStatusHandler initiator_status_handler_0;
  pairing_state.InitiatePairing(
      kNoSecurityRequirements, initiator_status_handler_0.MakeStatusCallback());
  EXPECT_EQ(1u, auth_request_count());

  TestStatusHandler initiator_status_handler_1;
  constexpr BrEdrSecurityRequirements kSecurityRequirements{
      .authentication = true, .secure_connections = false};
  pairing_state.InitiatePairing(
      kSecurityRequirements, initiator_status_handler_1.MakeStatusCallback());
  EXPECT_EQ(1u, auth_request_count());

  // Authenticate with link key.
  EXPECT_NE(std::nullopt, pairing_state.OnLinkKeyRequest());
  EXPECT_TRUE(connection()->ltk().has_value());
  pairing_state.OnAuthenticationComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);

  EXPECT_EQ(0, status_handler.call_count());
  EXPECT_EQ(1, connection()->start_encryption_count());

  connection()->TriggerEncryptionChangeCallback(fit::ok(true));
  ASSERT_EQ(1, status_handler.call_count());
  EXPECT_EQ(fit::ok(), *status_handler.status());
  ASSERT_EQ(1, initiator_status_handler_0.call_count());
  EXPECT_EQ(fit::ok(), *initiator_status_handler_0.status());
  EXPECT_EQ(0, initiator_status_handler_1.call_count());

  pairing_delegate.SetDisplayPasskeyCallback(
      [](PeerId, uint32_t, PairingDelegate::DisplayMethod, auto cb) {
        cb(true);
      });
  pairing_delegate.SetCompletePairingCallback(
      [](PeerId, sm::Result<> status) { EXPECT_EQ(fit::ok(), status); });

  // Pairing for second request should start.
  EXPECT_EQ(2u, auth_request_count());
  EXPECT_TRUE(pairing_state.initiator());
  EXPECT_EQ(std::nullopt, pairing_state.OnLinkKeyRequest());
  EXPECT_EQ(IoCapability::DISPLAY_YES_NO,
            *pairing_state.OnIoCapabilityRequest());
  pairing_state.OnIoCapabilityResponse(IoCapability::DISPLAY_YES_NO);

  bool confirmed = false;
  pairing_state.OnUserConfirmationRequest(
      kTestPasskey, [&confirmed](bool confirm) { confirmed = confirm; });
  EXPECT_TRUE(confirmed);

  pairing_state.OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                      kTestAuthenticatedLinkKeyType192);
  pairing_state.OnAuthenticationComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_EQ(2, connection()->start_encryption_count());

  connection()->TriggerEncryptionChangeCallback(fit::ok(true));
  ASSERT_EQ(2, status_handler.call_count());
  EXPECT_EQ(fit::ok(), *status_handler.status());
  EXPECT_EQ(1, initiator_status_handler_0.call_count());
  ASSERT_EQ(1, initiator_status_handler_1.call_count());
  EXPECT_EQ(fit::ok(), *initiator_status_handler_1.status());

  // No further pairing should occur.
  EXPECT_EQ(2u, auth_request_count());
  EXPECT_FALSE(pairing_state.initiator());
}

#ifndef NINSPECT
TEST_F(PairingStateTest, Inspect) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  TestStatusHandler status_handler;

  inspect::Inspector inspector;

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      status_handler.MakeStatusCallback(),
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  pairing_state.AttachInspect(inspector.GetRoot(), "pairing_state");

  auto security_properties_matcher =
      AllOf(NodeMatches(AllOf(NameMatches("security_properties"),
                              PropertyList(UnorderedElementsAre(
                                  StringIs("level", "not secure"),
                                  BoolIs("encrypted", false),
                                  BoolIs("secure_connections", false),
                                  BoolIs("authenticated", false),
                                  StringIs("key_type", "kCombination"))))));

  auto pairing_state_matcher =
      AllOf(NodeMatches(AllOf(
                NameMatches("pairing_state"),
                PropertyList(UnorderedElementsAre(
                    StringIs("encryption_status", "OFF"),
                    UintIs("central_lmp_transaction_collision", 0),
                    UintIs("peripheral_lmp_transaction_collision", 0),
                    UintIs("central_different_transaction_collision", 0),
                    UintIs("peripheral_different_transaction_collision", 0))))),
            ChildrenMatch(UnorderedElementsAre(security_properties_matcher)));

  inspect::Hierarchy hierarchy = bt::testing::ReadInspect(inspector);
  EXPECT_THAT(hierarchy, ChildrenMatch(ElementsAre(pairing_state_matcher)));
}
#endif  // NINSPECT

TEST_F(PairingStateTest,
       CentralInitiatorCrossTransportKeyDerivationSuccessFollowedByRepair) {
  ASSERT_EQ(connection()->role(),
            pw::bluetooth::emboss::ConnectionRole::CENTRAL);

  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);
  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      NoOpStatusCallback,
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  l2cap::testing::FakeChannel sm_channel(
      l2cap::kSMPChannelId, l2cap::kSMPChannelId, kTestHandle, LinkType::kACL);
  pairing_state.SetSecurityManagerChannel(sm_channel.GetWeakPtr());
  WeakSelf<sm::testing::TestSecurityManager>::WeakPtr security_manager =
      sm_factory()->GetTestSm(kTestHandle);
  ASSERT_TRUE(security_manager.is_alive());
  sm::PairingData pairing_data;
  pairing_data.local_ltk = kLtk;
  pairing_data.peer_ltk = kLtk;
  security_manager->set_pairing_data(pairing_data);

  TestStatusHandler status_handler_0;
  pairing_state.InitiatePairing(kNoSecurityRequirements,
                                status_handler_0.MakeStatusCallback());
  AdvanceToEncryptionAsInitiator(&pairing_state);
  EXPECT_TRUE(pairing_state.initiator());
  ASSERT_EQ(0, status_handler_0.call_count());
  EXPECT_FALSE(peer()->le());

  connection()->TriggerEncryptionChangeCallback(fit::ok(true));
  ASSERT_TRUE(status_handler_0.status());
  EXPECT_EQ(fit::ok(), *status_handler_0.status());
  ASSERT_TRUE(security_manager->last_identity_info().has_value());
  EXPECT_EQ(security_manager->last_identity_info()->irk, kIrk);
  EXPECT_EQ(security_manager->last_identity_info()->address,
            kLEIdentityAddress);
  ASSERT_TRUE(peer()->le());
  ASSERT_TRUE(peer()->le()->bond_data().has_value());
  EXPECT_EQ(peer()->le()->bond_data().value(), pairing_data);
}

// When re-connecting (i.e. no new link key generated, just authentication &
// encryption), no CTKD should take place.
TEST_F(PairingStateTest,
       CentralInitiatorSkipCrossTransportKeyDerivationIfLinkKeyAlreadyExists) {
  ASSERT_EQ(connection()->role(),
            pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  EXPECT_TRUE(peer()->MutBrEdr().SetBondData(
      sm::LTK(sm::SecurityProperties(kTestUnauthenticatedLinkKeyType256),
              kTestLinkKey)));

  EXPECT_FALSE(peer()->le());
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);
  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      NoOpStatusCallback,
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  l2cap::testing::FakeChannel sm_channel(
      l2cap::kSMPChannelId, l2cap::kSMPChannelId, kTestHandle, LinkType::kACL);
  pairing_state.SetSecurityManagerChannel(sm_channel.GetWeakPtr());
  WeakSelf<sm::testing::TestSecurityManager>::WeakPtr security_manager =
      sm_factory()->GetTestSm(kTestHandle);
  ASSERT_TRUE(security_manager.is_alive());
  sm::PairingData pairing_data;
  pairing_data.local_ltk = kLtk;
  pairing_data.peer_ltk = kLtk;
  security_manager->set_pairing_data(pairing_data);

  TestStatusHandler status_handler;
  pairing_state.InitiatePairing(kNoSecurityRequirements,
                                status_handler.MakeStatusCallback());
  EXPECT_NE(std::nullopt, pairing_state.OnLinkKeyRequest());
  pairing_state.OnAuthenticationComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_EQ(0, status_handler.call_count());
  connection()->TriggerEncryptionChangeCallback(fit::ok(true));
  EXPECT_EQ(1, status_handler.call_count());
  EXPECT_FALSE(security_manager->last_identity_info().has_value());
  EXPECT_FALSE(peer()->le());
}

TEST_F(PairingStateTest, CentralInitiatorCrossTransportKeyDerivationFailure) {
  ASSERT_EQ(connection()->role(),
            pw::bluetooth::emboss::ConnectionRole::CENTRAL);

  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);
  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      NoOpStatusCallback,
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  l2cap::testing::FakeChannel sm_channel(
      l2cap::kSMPChannelId, l2cap::kSMPChannelId, kTestHandle, LinkType::kACL);
  pairing_state.SetSecurityManagerChannel(sm_channel.GetWeakPtr());

  // Not setting the pairing data ensures that CTKD fails.
  WeakSelf<sm::testing::TestSecurityManager>::WeakPtr security_manager =
      sm_factory()->GetTestSm(kTestHandle);
  ASSERT_TRUE(security_manager.is_alive());

  TestStatusHandler status_handler;
  pairing_state.InitiatePairing(kNoSecurityRequirements,
                                status_handler.MakeStatusCallback());
  AdvanceToEncryptionAsInitiator(&pairing_state);
  EXPECT_TRUE(pairing_state.initiator());
  ASSERT_EQ(0, status_handler.call_count());
  EXPECT_FALSE(peer()->le());

  connection()->TriggerEncryptionChangeCallback(fit::ok(true));
  ASSERT_TRUE(status_handler.status());
  // The main pairing should still succeed.
  EXPECT_EQ(fit::ok(), *status_handler.status());
  EXPECT_FALSE(peer()->le());
  EXPECT_FALSE(security_manager->last_identity_info().has_value());
}

TEST_F(PairingStateTest, PeripheralCrossTransportKeyDerivationSuccess) {
  connection()->set_role(pw::bluetooth::emboss::ConnectionRole::PERIPHERAL);

  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);
  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      NoOpStatusCallback,
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  l2cap::testing::FakeChannel sm_channel(
      l2cap::kSMPChannelId, l2cap::kSMPChannelId, kTestHandle, LinkType::kACL);
  pairing_state.SetSecurityManagerChannel(sm_channel.GetWeakPtr());
  WeakSelf<sm::testing::TestSecurityManager>::WeakPtr security_manager =
      sm_factory()->GetTestSm(kTestHandle);
  ASSERT_TRUE(security_manager.is_alive());

  EXPECT_FALSE(peer()->le());

  sm::PairingData pairing_data;
  pairing_data.local_ltk = kLtk;
  pairing_data.peer_ltk = kLtk;
  security_manager->TriggerPairingComplete(pairing_data);

  ASSERT_TRUE(peer()->le());
  ASSERT_TRUE(peer()->le()->bond_data().has_value());
  EXPECT_EQ(peer()->le()->bond_data().value(), pairing_data);
  EXPECT_TRUE(security_manager->last_identity_info().has_value());
}

TEST_F(PairingStateTest, CentralResponderCrossTransportKeyDerivationSuccess) {
  ASSERT_EQ(connection()->role(),
            pw::bluetooth::emboss::ConnectionRole::CENTRAL);

  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      NoOpStatusCallback,
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  l2cap::testing::FakeChannel sm_channel(
      l2cap::kSMPChannelId, l2cap::kSMPChannelId, kTestHandle, LinkType::kACL);
  pairing_state.SetSecurityManagerChannel(sm_channel.GetWeakPtr());
  WeakSelf<sm::testing::TestSecurityManager>::WeakPtr security_manager =
      sm_factory()->GetTestSm(kTestHandle);
  ASSERT_TRUE(security_manager.is_alive());
  sm::PairingData pairing_data;
  pairing_data.local_ltk = kLtk;
  pairing_data.peer_ltk = kLtk;
  security_manager->set_pairing_data(pairing_data);

  // Start responder flow
  pairing_state.OnIoCapabilityResponse(kTestPeerIoCap);
  ASSERT_FALSE(pairing_state.initiator());
  static_cast<void>(pairing_state.OnIoCapabilityRequest());
  pairing_state.OnUserConfirmationRequest(kTestPasskey,
                                          NoOpUserConfirmationCallback);

  pairing_state.OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                      kTestUnauthenticatedLinkKeyType192);

  connection()->TriggerEncryptionChangeCallback(fit::ok(true));
  ASSERT_TRUE(security_manager->last_identity_info().has_value());
  ASSERT_TRUE(peer()->le());
  ASSERT_TRUE(peer()->le()->bond_data().has_value());
}

TEST_F(PairingStateTest, SetNullSecurityManagerChannelIgnored) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);
  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      NoOpStatusCallback,
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());
  pairing_state.SetSecurityManagerChannel(l2cap::Channel::WeakPtr());
}

TEST_F(PairingStateTest, InitiatorWaitForLEPairingToComplete) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);
  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      NoOpStatusCallback,
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  std::optional<Peer::PairingToken> pairing_token =
      peer()->MutLe().RegisterPairing();

  // Queue 2 requests to ensure that edge case is handled.
  TestStatusHandler status_handler_0;
  pairing_state.InitiatePairing(kNoSecurityRequirements,
                                status_handler_0.MakeStatusCallback());
  TestStatusHandler status_handler_1;
  pairing_state.InitiatePairing(kNoSecurityRequirements,
                                status_handler_1.MakeStatusCallback());
  RunUntilIdle();
  EXPECT_EQ(auth_request_count(), 0u);

  pairing_token.reset();
  EXPECT_EQ(auth_request_count(), 1u);

  AdvanceToEncryptionAsInitiator(&pairing_state);
  EXPECT_TRUE(pairing_state.initiator());
  ASSERT_EQ(0, status_handler_0.call_count());
  ASSERT_EQ(0, status_handler_1.call_count());

  connection()->TriggerEncryptionChangeCallback(fit::ok(true));
  ASSERT_TRUE(status_handler_0.status());
  EXPECT_EQ(fit::ok(), *status_handler_0.status());
  ASSERT_TRUE(status_handler_1.status());
  EXPECT_EQ(fit::ok(), *status_handler_1.status());
}

TEST_F(PairingStateTest,
       LinkKeyRequestWhileInitiatorWaitsForLEPairingToComplete) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);
  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      NoOpStatusCallback,
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  std::optional<Peer::PairingToken> pairing_token =
      peer()->MutLe().RegisterPairing();

  TestStatusHandler status_handler_0;
  pairing_state.InitiatePairing(kNoSecurityRequirements,
                                status_handler_0.MakeStatusCallback());
  RunUntilIdle();
  EXPECT_EQ(auth_request_count(), 0u);

  EXPECT_TRUE(peer()->MutBrEdr().SetBondData(
      sm::LTK(sm::SecurityProperties(kTestUnauthenticatedLinkKeyType192),
              kTestLinkKey)));

  static_cast<void>(pairing_state.OnLinkKeyRequest());
  ASSERT_EQ(0, status_handler_0.call_count());

  connection()->TriggerEncryptionChangeCallback(fit::ok(true));
  ASSERT_TRUE(status_handler_0.status());
  EXPECT_EQ(fit::ok(), *status_handler_0.status());

  // The end of LE pairing should be ignored now.
  pairing_token.reset();
  EXPECT_EQ(auth_request_count(), 0u);
}

TEST_F(PairingStateTest,
       IoCapabilityResponseWhileInitiatorWaitsForLEPairingToComplete) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);
  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      NoOpStatusCallback,
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  std::optional<Peer::PairingToken> pairing_token =
      peer()->MutLe().RegisterPairing();

  TestStatusHandler status_handler_0;
  pairing_state.InitiatePairing(kNoSecurityRequirements,
                                status_handler_0.MakeStatusCallback());
  RunUntilIdle();
  EXPECT_EQ(auth_request_count(), 0u);

  pairing_state.OnIoCapabilityResponse(kTestPeerIoCap);
  ASSERT_FALSE(pairing_state.initiator());
  static_cast<void>(pairing_state.OnIoCapabilityRequest());
  pairing_state.OnUserConfirmationRequest(kTestPasskey,
                                          NoOpUserConfirmationCallback);

  pairing_state.OnSimplePairingComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                      kTestUnauthenticatedLinkKeyType192);

  connection()->TriggerEncryptionChangeCallback(fit::ok(true));
  ASSERT_TRUE(status_handler_0.status());
  EXPECT_EQ(fit::ok(), *status_handler_0.status());

  // The end of LE pairing should be ignored now.
  pairing_token.reset();
  EXPECT_EQ(auth_request_count(), 0u);
}

TEST_F(PairingStateTest, TransactionCollision) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);
  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      NoOpStatusCallback,
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  TestStatusHandler status_handler;
  pairing_state.InitiatePairing(kNoSecurityRequirements,
                                status_handler.MakeStatusCallback());
  RunUntilIdle();
  EXPECT_TRUE(peer()->MutBrEdr().SetBondData(
      sm::LTK(sm::SecurityProperties(kTestUnauthenticatedLinkKeyType192),
              kTestLinkKey)));

  static_cast<void>(pairing_state.OnLinkKeyRequest());
  ASSERT_EQ(0, status_handler.call_count());

  pairing_state.OnAuthenticationComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  ASSERT_EQ(0, status_handler.call_count());

  auto tc_result = ToResult(
      pw::bluetooth::emboss::StatusCode::LMP_ERROR_TRANSACTION_COLLISION);
  hci::Result<bool> result(tc_result.take_error());
  connection()->TriggerEncryptionChangeCallback(result);
  ASSERT_EQ(0, status_handler.call_count());

  connection()->TriggerEncryptionChangeCallback(fit::ok(true));
  ASSERT_EQ(1, status_handler.call_count());
  ASSERT_TRUE(status_handler.status());
  EXPECT_EQ(fit::ok(), *status_handler.status());
}

TEST_F(PairingStateTest, DifferentTransactionCollisionAsCentral) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);
  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/true,
      MakeAuthRequestCallback(),
      NoOpStatusCallback,
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  TestStatusHandler status_handler;
  pairing_state.InitiatePairing(kNoSecurityRequirements,
                                status_handler.MakeStatusCallback());
  RunUntilIdle();
  EXPECT_TRUE(peer()->MutBrEdr().SetBondData(
      sm::LTK(sm::SecurityProperties(kTestUnauthenticatedLinkKeyType192),
              kTestLinkKey)));

  static_cast<void>(pairing_state.OnLinkKeyRequest());
  ASSERT_EQ(0, status_handler.call_count());

  pairing_state.OnAuthenticationComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  ASSERT_EQ(0, status_handler.call_count());
  ASSERT_EQ(1, connection()->start_encryption_count());

  auto tc_result = ToResult(
      pw::bluetooth::emboss::StatusCode::DIFFERENT_TRANSACTION_COLLISION);
  hci::Result<bool> result(tc_result.take_error());
  connection()->TriggerEncryptionChangeCallback(result);
  ASSERT_EQ(0, status_handler.call_count());

  // Ensure we don't retry as a Central
  RunFor(SecureSimplePairingState::kDelayRetryEnableEncryption);
  ASSERT_EQ(1, connection()->start_encryption_count());

  connection()->TriggerEncryptionChangeCallback(fit::ok(true));
  ASSERT_EQ(1, status_handler.call_count());
  ASSERT_TRUE(status_handler.status());
  EXPECT_EQ(fit::ok(), *status_handler.status());
}

TEST_F(PairingStateTest, DifferentTransactionCollisionAsPeripheral) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);
  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      NoOpStatusCallback,
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  TestStatusHandler status_handler;
  pairing_state.InitiatePairing(kNoSecurityRequirements,
                                status_handler.MakeStatusCallback());
  RunUntilIdle();
  EXPECT_TRUE(peer()->MutBrEdr().SetBondData(
      sm::LTK(sm::SecurityProperties(kTestUnauthenticatedLinkKeyType192),
              kTestLinkKey)));

  static_cast<void>(pairing_state.OnLinkKeyRequest());
  ASSERT_EQ(0, status_handler.call_count());

  pairing_state.OnAuthenticationComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  ASSERT_EQ(0, status_handler.call_count());
  ASSERT_EQ(1, connection()->start_encryption_count());

  auto tc_result = ToResult(
      pw::bluetooth::emboss::StatusCode::DIFFERENT_TRANSACTION_COLLISION);
  hci::Result<bool> result(tc_result.take_error());
  connection()->TriggerEncryptionChangeCallback(result);
  ASSERT_EQ(0, status_handler.call_count());

  // Ensure we retry after kDelayRetryEnableEncryption amount of time
  RunFor(SecureSimplePairingState::kDelayRetryEnableEncryption);
  ASSERT_EQ(2, connection()->start_encryption_count());

  connection()->TriggerEncryptionChangeCallback(fit::ok(true));
  ASSERT_EQ(1, status_handler.call_count());
  ASSERT_TRUE(status_handler.status());
  EXPECT_EQ(fit::ok(), *status_handler.status());
}

TEST_F(PairingStateTest,
       DifferentTransactionCollisionRetryNotCalledIfSucceeded) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);
  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/false,
      MakeAuthRequestCallback(),
      NoOpStatusCallback,
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  TestStatusHandler status_handler;
  pairing_state.InitiatePairing(kNoSecurityRequirements,
                                status_handler.MakeStatusCallback());
  RunUntilIdle();
  EXPECT_TRUE(peer()->MutBrEdr().SetBondData(
      sm::LTK(sm::SecurityProperties(kTestUnauthenticatedLinkKeyType192),
              kTestLinkKey)));

  static_cast<void>(pairing_state.OnLinkKeyRequest());
  ASSERT_EQ(0, status_handler.call_count());

  pairing_state.OnAuthenticationComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  ASSERT_EQ(0, status_handler.call_count());
  ASSERT_EQ(1, connection()->start_encryption_count());

  auto tc_result = ToResult(
      pw::bluetooth::emboss::StatusCode::DIFFERENT_TRANSACTION_COLLISION);
  hci::Result<bool> result(tc_result.take_error());
  connection()->TriggerEncryptionChangeCallback(result);
  ASSERT_EQ(0, status_handler.call_count());

  connection()->TriggerEncryptionChangeCallback(fit::ok(true));
  ASSERT_EQ(1, status_handler.call_count());
  ASSERT_TRUE(status_handler.status());
  EXPECT_EQ(fit::ok(), *status_handler.status());

  // Ensure we don't retry if we succeeded in enabling encryption before the
  // retry handler is called
  RunFor(SecureSimplePairingState::kDelayRetryEnableEncryption);
  ASSERT_EQ(1, connection()->start_encryption_count());
}

// Verify that receiving an unexpected event (like HCI_Authentication_Complete)
// while in kInitiatorWaitLEPairingComplete (waiting for an LE pairing to finish
// before starting BR/EDR pairing) does not crash the host and instead signals
// the queued pairing requests with an error.
TEST_F(PairingStateTest,
       UnexpectedEventWhileWaitingForLEPairingSignalsQueuedRequests) {
  NoOpPairingDelegate pairing_delegate(sm::IOCapability::kDisplayYesNo);

  SecureSimplePairingState pairing_state(
      peer()->GetWeakPtr(),
      pairing_delegate.GetWeakPtr(),
      connection()->GetWeakPtr(),
      /*outgoing_connection=*/true,
      /*auth_cb=*/[] {},
      NoOpStatusCallback,
      /*low_energy_address_delegate=*/this,
      /*controller_remote_public_key_validation_supported=*/true,
      sm_factory_func(),
      dispatcher());

  // Register an LE pairing to force the state machine to wait.
  std::optional<Peer::PairingToken> le_pairing_token =
      peer()->MutLe().RegisterPairing();
  ASSERT_TRUE(peer()->le() && peer()->le()->is_pairing());

  // Initiate BR/EDR pairing so that it is queued while LE pairing is active.
  TestStatusHandler status_handler;
  pairing_state.InitiatePairing(kNoSecurityRequirements,
                                status_handler.MakeStatusCallback());
  RunUntilIdle();

  pairing_state.OnAuthenticationComplete(
      pw::bluetooth::emboss::StatusCode::AUTHENTICATION_FAILURE);

  EXPECT_EQ(1, status_handler.call_count());
  ASSERT_TRUE(status_handler.status().has_value());
  EXPECT_EQ(kTestHandle, *status_handler.handle());
  EXPECT_EQ(ToResult(pw::bluetooth::emboss::StatusCode::AUTHENTICATION_FAILURE),
            *status_handler.status());
  le_pairing_token.reset();
}
}  // namespace
}  // namespace bt::gap
