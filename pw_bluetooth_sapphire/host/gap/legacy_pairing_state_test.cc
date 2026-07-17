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

#include <gmock/gmock.h>

#include "pw_bluetooth_sapphire/internal/host/gap/fake_pairing_delegate.h"
#include "pw_bluetooth_sapphire/internal/host/gap/peer_cache.h"
#include "pw_bluetooth_sapphire/internal/host/hci/fake_bredr_connection.h"
#include "pw_bluetooth_sapphire/internal/host/sm/smp.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"
#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/gtest_helpers.h"
#include "pw_bluetooth_sapphire/internal/host/testing/inspect_util.h"
#include "pw_bluetooth_sapphire/internal/host/testing/mock_controller.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_packets.h"
#include "pw_bluetooth_sapphire/internal/host/transport/error.h"

namespace bt::gap {
namespace {

using namespace inspect::testing;

using hci::testing::FakeBrEdrConnection;

const hci_spec::ConnectionHandle kTestHandle(0x0A0B);
const DeviceAddress kLocalAddress(DeviceAddress::Type::kBREDR,
                                  {0x22, 0x11, 0x00, 0xCC, 0xBB, 0xAA});
const DeviceAddress kPeerAddress(DeviceAddress::Type::kBREDR,
                                 {0x99, 0x88, 0x77, 0xFF, 0xEE, 0xDD});
const sm::IOCapability kTestLocalIoCap = sm::IOCapability::kDisplayYesNo;
const uint16_t kTestDefaultPinCode = 0000;
const uint16_t kTestRandomPinCode = 9876;
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
const hci_spec::LinkKeyType kTestLegacyLinkKeyType =
    hci_spec::LinkKeyType::kCombination;
const auto kTestUnauthenticatedLinkKeyType192 =
    hci_spec::LinkKeyType::kUnauthenticatedCombination192;

void NoOpStatusCallback(hci_spec::ConnectionHandle, hci::Result<>) {}

class NoOpPairingDelegate final : public PairingDelegate {
 public:
  explicit NoOpPairingDelegate(sm::IOCapability io_capability)
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
class LegacyPairingStateTest : public TestBase {
 public:
  LegacyPairingStateTest() = default;
  ~LegacyPairingStateTest() override = default;

  void SetUp() override {
    TestBase::SetUp();
    InitializeACLDataChannel();

    peer_cache_ = std::make_unique<PeerCache>(dispatcher());
    peer_ = peer_cache_->NewPeer(kPeerAddress, /*connectable=*/true);

    auth_request_count_ = 0;
    send_auth_request_callback_ = [this]() { auth_request_count_++; };

    connection_ = std::make_unique<FakeBrEdrConnection>(
        kTestHandle,
        kLocalAddress,
        kPeerAddress,
        pw::bluetooth::emboss::ConnectionRole::CENTRAL,
        transport()->GetWeakPtr(),
        dispatcher());
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

  PeerCache* peer_cache() const { return peer_cache_.get(); }

  Peer* peer() const { return peer_; }

  // Incomplete connection used to create LegacyPairingState in
  // *BeforeAclConnectionCompletes tests
  FakeBrEdrConnection::WeakPtr incomplete_connection() const {
    return FakeBrEdrConnection::WeakPtr();
  }

  FakeBrEdrConnection* connection() const { return connection_.get(); }

  uint8_t auth_request_count() const { return auth_request_count_; }

 private:
  std::unique_ptr<PeerCache> peer_cache_;
  Peer* peer_;
  std::unique_ptr<FakeBrEdrConnection> connection_;
  uint8_t auth_request_count_;
  fit::closure send_auth_request_callback_;
};

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

  uint8_t call_count() const { return call_count_; }

  // Returns std::nullopt if |call_count_ < 1|, otherwise values from the most
  // recent callback invocation.
  auto& handle() const { return handle_; }
  auto& status() const { return status_; }

 private:
  uint8_t call_count_ = 0;
  std::optional<hci_spec::ConnectionHandle> handle_;
  std::optional<hci::Result<>> status_;
};

TEST_F(LegacyPairingStateTest,
       TestStatusHandlerCorrectlyTracksStatusCallbackInvocations) {
  TestStatusHandler handler;
  EXPECT_EQ(0, handler.call_count());
  EXPECT_FALSE(handler.status());

  LegacyPairingState::StatusCallback status_cb = handler.MakeStatusCallback();
  EXPECT_EQ(0, handler.call_count());
  EXPECT_FALSE(handler.status());

  status_cb(kTestHandle,
            ToResult(pw::bluetooth::emboss::StatusCode::PAIRING_NOT_ALLOWED));

  EXPECT_EQ(1, handler.call_count());
  ASSERT_TRUE(handler.handle());
  EXPECT_EQ(kTestHandle, *handler.handle());
  ASSERT_TRUE(handler.status());
  EXPECT_EQ(ToResult(pw::bluetooth::emboss::StatusCode::PAIRING_NOT_ALLOWED),
            *handler.status());
}

TEST_F(LegacyPairingStateTest, BuildEstablishedLink) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher());

  // |pairing_state|'s temporary |link_key_| is empty
  EXPECT_FALSE(pairing_state.link_key().has_value());

  EXPECT_TRUE(peer()->MutBrEdr().SetBondData(
      sm::LTK(sm::SecurityProperties(kTestLegacyLinkKeyType), kTestLinkKey)));

  std::optional<hci_spec::LinkKey> reply_key = pairing_state.OnLinkKeyRequest();
  ASSERT_TRUE(reply_key.has_value());
  EXPECT_EQ(kTestLinkKey, reply_key.value());

  // Connection not complete yet so link key is stored in LegacyPairingState and
  // not the connection
  EXPECT_FALSE(connection()->ltk().has_value());
  EXPECT_TRUE(pairing_state.link_key().has_value());
  EXPECT_EQ(kTestLinkKey, pairing_state.link_key().value());
  EXPECT_TRUE(peer()->MutBrEdr().is_pairing());

  // Authentication is done and connection gets made by BrEdrConnectionManager.
  // For testing, we manually set the link info using |connection()|
  pairing_state.BuildEstablishedLink(connection()->GetWeakPtr(),
                                     MakeAuthRequestCallback(),
                                     NoOpStatusCallback);

  EXPECT_TRUE(connection()->ltk().has_value());
  EXPECT_EQ(kTestLinkKeyValue, connection()->ltk()->value());
  EXPECT_EQ(kTestLinkKeyValue, pairing_state.link_ltk()->value());
  EXPECT_TRUE(peer()->MutBrEdr().is_pairing());
}

TEST_F(LegacyPairingStateTest, PairingStateStartsAsResponder) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   NoOpStatusCallback);
  EXPECT_FALSE(pairing_state.initiator());
}

TEST_F(LegacyPairingStateTest,
       NeverInitiateLegacyPairingBeforeAclConnectionCompletes) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher());
  EXPECT_FALSE(pairing_state.initiator());

  pairing_state.InitiatePairing(NoOpStatusCallback);

  // |auth_cb| is only called if initiation was successful
  EXPECT_EQ(0, auth_request_count());
  EXPECT_FALSE(pairing_state.initiator());
  EXPECT_FALSE(peer()->MutBrEdr().is_pairing());
}

TEST_F(LegacyPairingStateTest, NeverInitiateLegacyPairingWhenPeerSupportsSSP) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   NoOpStatusCallback);
  EXPECT_FALSE(pairing_state.initiator());

  // Set peer's feature bits to indicate support for SSP
  peer()->SetFeaturePage(
      0,
      static_cast<uint64_t>(
          hci_spec::LMPFeature::kSecureSimplePairingControllerSupport));
  peer()->SetFeaturePage(
      1,
      static_cast<uint64_t>(
          hci_spec::LMPFeature::kSecureSimplePairingHostSupport));

  pairing_state.InitiatePairing(NoOpStatusCallback);

  // |auth_cb| is only called if initiation was successful
  EXPECT_EQ(0, auth_request_count());
  EXPECT_FALSE(pairing_state.initiator());
}

TEST_F(LegacyPairingStateTest,
       SkipPairingIfExistingKeyMeetsSecurityRequirements) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   NoOpStatusCallback);
  EXPECT_FALSE(pairing_state.initiator());

  connection()->set_link_key(kTestLinkKey, kTestLegacyLinkKeyType);

  TestStatusHandler initiator_status_handler;
  pairing_state.InitiatePairing(initiator_status_handler.MakeStatusCallback());

  // |auth_cb| is only called if initiation was successful
  EXPECT_EQ(0, auth_request_count());
  EXPECT_FALSE(pairing_state.initiator());
  ASSERT_EQ(1, initiator_status_handler.call_count());
  EXPECT_EQ(fit::ok(), *initiator_status_handler.status());
  EXPECT_FALSE(peer()->MutBrEdr().is_pairing());
}

TEST_F(
    LegacyPairingStateTest,
    PairingResponderOnLinkKeyRequestReturnsLinkKeyWhenBondDataExistsBeforeAclConnectionCompletes) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher());
  EXPECT_FALSE(pairing_state.initiator());

  EXPECT_TRUE(peer()->MutBrEdr().SetBondData(
      sm::LTK(sm::SecurityProperties(kTestLegacyLinkKeyType), kTestLinkKey)));
  EXPECT_FALSE(connection()->ltk().has_value());

  std::optional<hci_spec::LinkKey> reply_key = pairing_state.OnLinkKeyRequest();
  ASSERT_TRUE(reply_key.has_value());
  EXPECT_EQ(kTestLinkKey, reply_key.value());

  // Connection not complete yet so link key is stored in LegacyPairingState and
  // not the connection
  EXPECT_FALSE(connection()->ltk().has_value());
  EXPECT_TRUE(pairing_state.link_key().has_value());
  EXPECT_EQ(kTestLinkKey, pairing_state.link_key().value());
  EXPECT_TRUE(peer()->MutBrEdr().is_pairing());
}

TEST_F(
    LegacyPairingStateTest,
    PairingResponderOnLinkKeyRequestReturnsLinkKeyWhenBondDataExistsAfterAclConnectionComplete) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   NoOpStatusCallback);
  EXPECT_FALSE(pairing_state.initiator());

  EXPECT_TRUE(peer()->MutBrEdr().SetBondData(
      sm::LTK(sm::SecurityProperties(kTestLegacyLinkKeyType), kTestLinkKey)));
  EXPECT_FALSE(connection()->ltk().has_value());

  std::optional<hci_spec::LinkKey> reply_key = pairing_state.OnLinkKeyRequest();
  ASSERT_TRUE(reply_key.has_value());
  EXPECT_EQ(kTestLinkKey, reply_key.value());

  // Connection was complete so link key was stored in the connection
  EXPECT_TRUE(connection()->ltk().has_value());
  EXPECT_EQ(kTestLinkKeyValue, connection()->ltk()->value());
  EXPECT_FALSE(pairing_state.link_key().has_value());
}

TEST_F(
    LegacyPairingStateTest,
    PairingInitiatorOnLinkKeyRequestReturnsReturnsLinkKeyWhenBondDataExists) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   NoOpStatusCallback);
  EXPECT_FALSE(pairing_state.initiator());

  EXPECT_TRUE(peer()->MutBrEdr().SetBondData(
      sm::LTK(sm::SecurityProperties(kTestLegacyLinkKeyType), kTestLinkKey)));
  EXPECT_FALSE(connection()->ltk().has_value());

  pairing_state.InitiatePairing(NoOpStatusCallback);
  EXPECT_EQ(1, auth_request_count());
  EXPECT_TRUE(pairing_state.initiator());

  std::optional<hci_spec::LinkKey> reply_key = pairing_state.OnLinkKeyRequest();
  ASSERT_TRUE(reply_key.has_value());
  EXPECT_EQ(kTestLinkKey, reply_key.value());

  // Connection was complete so link key was stored in the connection
  EXPECT_TRUE(connection()->ltk().has_value());
  EXPECT_EQ(kTestLinkKeyValue, connection()->ltk()->value());
  EXPECT_FALSE(pairing_state.link_key().has_value());
}

TEST_F(
    LegacyPairingStateTest,
    PairingResponderOnLinkKeyRequestReturnsNullWhenBondDataDoesNotExistBeforeAclComplete) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher());
  EXPECT_FALSE(pairing_state.initiator());

  std::optional<hci_spec::LinkKey> reply_key = pairing_state.OnLinkKeyRequest();
  EXPECT_FALSE(reply_key.has_value());
}

TEST_F(
    LegacyPairingStateTest,
    PairingResponderOnLinkKeyRequestReturnsNullWhenBondDataDoesNotExistAfterAclComplete) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   NoOpStatusCallback);
  EXPECT_FALSE(pairing_state.initiator());

  std::optional<hci_spec::LinkKey> reply_key = pairing_state.OnLinkKeyRequest();
  EXPECT_FALSE(reply_key.has_value());
}

TEST_F(LegacyPairingStateTest,
       PairingInitiatorOnLinkKeyRequestReturnsNullWhenBondDataDoesNotExist) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   NoOpStatusCallback);
  EXPECT_FALSE(pairing_state.initiator());

  pairing_state.InitiatePairing(NoOpStatusCallback);
  EXPECT_EQ(1, auth_request_count());
  EXPECT_TRUE(pairing_state.initiator());

  std::optional<hci_spec::LinkKey> reply_key = pairing_state.OnLinkKeyRequest();
  EXPECT_FALSE(reply_key.has_value());
}

TEST_F(LegacyPairingStateTest, OnLinkKeyRequestReceivedMissingPeerAsserts) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   NoOpStatusCallback);
  EXPECT_FALSE(pairing_state.initiator());

  pairing_state.InitiatePairing(NoOpStatusCallback);
  EXPECT_TRUE(pairing_state.initiator());

  EXPECT_TRUE(peer_cache()->RemoveDisconnectedPeer(peer()->identifier()));

  ASSERT_DEATH_IF_SUPPORTED(
      {
        [[maybe_unused]] std::optional<hci_spec::LinkKey> reply_key =
            pairing_state.OnLinkKeyRequest();
      },
      ".*peer.*");
}

TEST_F(LegacyPairingStateTest,
       NeverInitiateLegacyPairingWithNoNumericOutputCapability) {
  NoOpPairingDelegate pairing_delegate(sm::IOCapability::kNoInputNoOutput);

  TestStatusHandler status_handler;

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   status_handler.MakeStatusCallback());
  EXPECT_FALSE(pairing_state.initiator());

  pairing_state.InitiatePairing(NoOpStatusCallback);
  EXPECT_EQ(0, auth_request_count());
  EXPECT_FALSE(pairing_state.initiator());
  EXPECT_EQ(1, status_handler.call_count());
  ASSERT_TRUE(status_handler.handle());
  EXPECT_EQ(kTestHandle, *status_handler.handle());
  ASSERT_TRUE(status_handler.status());
  EXPECT_EQ(ToResult(HostError::kFailed), *status_handler.status());
  EXPECT_FALSE(peer()->MutBrEdr().is_pairing());
}

TEST_F(LegacyPairingStateTest, PairingInitiatorWithNoInputGeneratesRandomPin) {
  FakePairingDelegate pairing_delegate(sm::IOCapability::kDisplayOnly);

  TestStatusHandler status_handler;

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   status_handler.MakeStatusCallback());
  EXPECT_FALSE(pairing_state.initiator());

  pairing_delegate.SetDisplayPasskeyCallback(
      [](PeerId, uint32_t, PairingDelegate::DisplayMethod, auto cb) {
        cb(/*confirm=*/true);
      });

  pairing_state.InitiatePairing(NoOpStatusCallback);
  EXPECT_EQ(1, auth_request_count());
  EXPECT_TRUE(pairing_state.initiator());

  EXPECT_EQ(std::nullopt, pairing_state.OnLinkKeyRequest());
  EXPECT_EQ(0, status_handler.call_count());

  std::optional<uint16_t> pin_code;
  auto pin_code_cb = [&pin_code](std::optional<uint16_t> pin) {
    pin_code = pin;
  };
  pairing_state.OnPinCodeRequest(std::move(pin_code_cb));
  ASSERT_TRUE(pin_code.has_value());
  ASSERT_TRUE(pin_code.value() >= 0);
  ASSERT_TRUE(pin_code.value() <= 9999);

  pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                      kTestLegacyLinkKeyType);

  EXPECT_TRUE(connection()->ltk().has_value());
  EXPECT_EQ(kTestLinkKeyValue, connection()->ltk()->value());
  EXPECT_EQ(kTestLinkKeyValue, pairing_state.link_ltk()->value());
}

TEST_F(LegacyPairingStateTest,
       PairingInitiatorWithYesNoInputGeneratesRandomPin) {
  FakePairingDelegate pairing_delegate(kTestLocalIoCap);

  TestStatusHandler status_handler;

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   status_handler.MakeStatusCallback());
  EXPECT_FALSE(pairing_state.initiator());

  pairing_delegate.SetDisplayPasskeyCallback(
      [](PeerId, uint32_t, PairingDelegate::DisplayMethod, auto cb) {
        cb(/*confirm=*/true);
      });

  pairing_state.InitiatePairing(NoOpStatusCallback);
  EXPECT_EQ(1, auth_request_count());
  EXPECT_TRUE(pairing_state.initiator());

  EXPECT_EQ(std::nullopt, pairing_state.OnLinkKeyRequest());
  EXPECT_EQ(0, status_handler.call_count());

  std::optional<uint16_t> pin_code;
  auto pin_code_cb = [&pin_code](std::optional<uint16_t> pin) {
    pin_code = pin;
  };
  pairing_state.OnPinCodeRequest(std::move(pin_code_cb));
  ASSERT_TRUE(pin_code.has_value());
  ASSERT_TRUE(pin_code.value() >= 0);
  ASSERT_TRUE(pin_code.value() <= 9999);

  pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                      kTestLegacyLinkKeyType);

  EXPECT_TRUE(connection()->ltk().has_value());
  EXPECT_EQ(kTestLinkKeyValue, connection()->ltk()->value());
  EXPECT_EQ(kTestLinkKeyValue, pairing_state.link_ltk()->value());
}

TEST_F(LegacyPairingStateTest,
       PairingInitiatorWithKeyboardInputGeneratesRandomPin) {
  FakePairingDelegate pairing_delegate(sm::IOCapability::kKeyboardDisplay);

  TestStatusHandler status_handler;

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   status_handler.MakeStatusCallback());
  EXPECT_FALSE(pairing_state.initiator());

  pairing_delegate.SetDisplayPasskeyCallback(
      [](PeerId, uint32_t, PairingDelegate::DisplayMethod, auto cb) {
        cb(/*confirm=*/true);
      });

  pairing_state.InitiatePairing(NoOpStatusCallback);
  EXPECT_EQ(1, auth_request_count());
  EXPECT_TRUE(pairing_state.initiator());

  EXPECT_EQ(std::nullopt, pairing_state.OnLinkKeyRequest());
  EXPECT_EQ(0, status_handler.call_count());

  std::optional<uint16_t> pin_code;
  auto pin_code_cb = [&pin_code](std::optional<uint16_t> pin) {
    pin_code = pin;
  };
  pairing_state.OnPinCodeRequest(std::move(pin_code_cb));
  ASSERT_TRUE(pin_code.has_value());
  ASSERT_TRUE(pin_code.value() >= 0);
  ASSERT_TRUE(pin_code.value() <= 9999);

  pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                      kTestLegacyLinkKeyType);

  EXPECT_TRUE(connection()->ltk().has_value());
  EXPECT_EQ(kTestLinkKeyValue, connection()->ltk()->value());
  EXPECT_EQ(kTestLinkKeyValue, pairing_state.link_ltk()->value());
}

TEST_F(LegacyPairingStateTest, PairingResponderWithNoInputTriesCommonPins) {
  FakePairingDelegate pairing_delegate(sm::IOCapability::kDisplayOnly);

  TestStatusHandler status_handler;

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   status_handler.MakeStatusCallback());
  EXPECT_FALSE(pairing_state.initiator());

  EXPECT_EQ(std::nullopt, pairing_state.OnLinkKeyRequest());
  EXPECT_EQ(0, status_handler.call_count());

  pairing_delegate.SetRequestPasskeyCallback([this](PeerId peer_id, auto cb) {
    EXPECT_EQ(peer()->identifier(), peer_id);
    ASSERT_TRUE(cb);
    cb(kTestDefaultPinCode);
  });

  std::optional<uint16_t> pin_code;
  auto pin_code_cb = [&pin_code](std::optional<uint16_t> pin) {
    pin_code = pin;
  };
  pairing_state.OnPinCodeRequest(std::move(pin_code_cb));
  ASSERT_TRUE(pin_code.has_value());
  ASSERT_EQ(0, pin_code.value());

  pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                      kTestLegacyLinkKeyType);

  EXPECT_TRUE(connection()->ltk().has_value());
  EXPECT_EQ(kTestLinkKeyValue, connection()->ltk()->value());
  EXPECT_EQ(kTestLinkKeyValue, pairing_state.link_ltk()->value());
}

TEST_F(LegacyPairingStateTest, PairingResponderWithYesNoInputTriesCommonPins) {
  FakePairingDelegate pairing_delegate(kTestLocalIoCap);

  TestStatusHandler status_handler;

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   status_handler.MakeStatusCallback());
  EXPECT_FALSE(pairing_state.initiator());

  EXPECT_EQ(std::nullopt, pairing_state.OnLinkKeyRequest());
  EXPECT_EQ(0, status_handler.call_count());

  pairing_delegate.SetRequestPasskeyCallback([this](PeerId peer_id, auto cb) {
    EXPECT_EQ(peer()->identifier(), peer_id);
    ASSERT_TRUE(cb);
    cb(kTestDefaultPinCode);
  });

  std::optional<uint16_t> pin_code;
  auto pin_code_cb = [&pin_code](std::optional<uint16_t> pin) {
    pin_code = pin;
  };
  pairing_state.OnPinCodeRequest(std::move(pin_code_cb));
  ASSERT_TRUE(pin_code.has_value());
  ASSERT_EQ(0, pin_code.value());

  pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                      kTestLegacyLinkKeyType);

  EXPECT_TRUE(connection()->ltk().has_value());
  EXPECT_EQ(kTestLinkKeyValue, connection()->ltk()->value());
  EXPECT_EQ(kTestLinkKeyValue, pairing_state.link_ltk()->value());
}

TEST_F(LegacyPairingStateTest,
       PairingResponderWithKeyboardInputNoOutputRequestsUserPasskey) {
  FakePairingDelegate pairing_delegate(sm::IOCapability::kKeyboardOnly);

  TestStatusHandler status_handler;

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   status_handler.MakeStatusCallback());
  EXPECT_FALSE(pairing_state.initiator());

  EXPECT_EQ(std::nullopt, pairing_state.OnLinkKeyRequest());
  EXPECT_EQ(0, status_handler.call_count());

  pairing_delegate.SetRequestPasskeyCallback([this](PeerId peer_id, auto cb) {
    EXPECT_EQ(peer()->identifier(), peer_id);
    ASSERT_TRUE(cb);
    cb(kTestRandomPinCode);
  });

  std::optional<uint16_t> pin_code;
  auto pin_code_cb = [&pin_code](std::optional<uint16_t> pin) {
    pin_code = pin;
  };
  pairing_state.OnPinCodeRequest(std::move(pin_code_cb));
  ASSERT_TRUE(pin_code.has_value());
  ASSERT_EQ(kTestRandomPinCode, pin_code.value());

  pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                      kTestLegacyLinkKeyType);

  EXPECT_TRUE(connection()->ltk().has_value());
  EXPECT_EQ(kTestLinkKeyValue, connection()->ltk()->value());
  EXPECT_EQ(kTestLinkKeyValue, pairing_state.link_ltk()->value());
}

TEST_F(LegacyPairingStateTest,
       PairingResponderWithKeyboardInputDisplayOutputRequestsUserPasskey) {
  FakePairingDelegate pairing_delegate(sm::IOCapability::kKeyboardDisplay);

  TestStatusHandler status_handler;

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   status_handler.MakeStatusCallback());
  EXPECT_FALSE(pairing_state.initiator());

  EXPECT_EQ(std::nullopt, pairing_state.OnLinkKeyRequest());
  EXPECT_EQ(0, status_handler.call_count());

  pairing_delegate.SetRequestPasskeyCallback([this](PeerId peer_id, auto cb) {
    EXPECT_EQ(peer()->identifier(), peer_id);
    ASSERT_TRUE(cb);
    cb(kTestRandomPinCode);
  });

  std::optional<uint16_t> pin_code;
  auto pin_code_cb = [&pin_code](std::optional<uint16_t> pin) {
    pin_code = pin;
  };
  pairing_state.OnPinCodeRequest(std::move(pin_code_cb));
  ASSERT_TRUE(pin_code.has_value());
  ASSERT_EQ(kTestRandomPinCode, pin_code.value());

  pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                      kTestLegacyLinkKeyType);

  EXPECT_TRUE(connection()->ltk().has_value());
  EXPECT_EQ(kTestLinkKeyValue, connection()->ltk()->value());
  EXPECT_EQ(kTestLinkKeyValue, pairing_state.link_ltk()->value());
}

TEST_F(
    LegacyPairingStateTest,
    PairingInitiatorFailsPairingWhenAuthenticationCompleteWithErrorCodeReceivedEarly) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  TestStatusHandler status_handler;

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   status_handler.MakeStatusCallback());
  EXPECT_FALSE(pairing_state.initiator());

  pairing_state.InitiatePairing(NoOpStatusCallback);
  EXPECT_TRUE(pairing_state.initiator());

  EXPECT_EQ(std::nullopt, pairing_state.OnLinkKeyRequest());
  EXPECT_EQ(0, status_handler.call_count());

  pairing_state.OnAuthenticationComplete(
      pw::bluetooth::emboss::StatusCode::AUTHENTICATION_FAILURE);
  ASSERT_EQ(1, status_handler.call_count());
  EXPECT_EQ(ToResult(pw::bluetooth::emboss::StatusCode::AUTHENTICATION_FAILURE),
            status_handler.status().value());
  EXPECT_FALSE(peer()->MutBrEdr().is_pairing());
}

TEST_F(LegacyPairingStateTest,
       InitatorPairingStateSendsAuthenticationRequestOnceForDuplicateRequest) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   NoOpStatusCallback);
  EXPECT_FALSE(pairing_state.initiator());

  pairing_state.InitiatePairing(NoOpStatusCallback);
  EXPECT_EQ(1, auth_request_count());
  EXPECT_TRUE(pairing_state.initiator());

  pairing_state.InitiatePairing(NoOpStatusCallback);
  EXPECT_EQ(1, auth_request_count());
  EXPECT_TRUE(pairing_state.initiator());
}

TEST_F(LegacyPairingStateTest,
       PairingResponderSetsConnectionLinkKeyBeforeAclConnectionComplete) {
  FakePairingDelegate pairing_delegate(kTestLocalIoCap);

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher());
  EXPECT_FALSE(pairing_state.initiator());

  pairing_delegate.SetRequestPasskeyCallback([this](PeerId peer_id, auto cb) {
    EXPECT_EQ(peer()->identifier(), peer_id);
    ASSERT_TRUE(cb);
    cb(kTestRandomPinCode);
  });

  // Peer has invalid link key so we receive a PIN code request
  std::optional<uint16_t> pin_code;
  auto pin_code_cb = [&pin_code](std::optional<uint16_t> pin) {
    pin_code = pin;
  };
  pairing_state.OnPinCodeRequest(std::move(pin_code_cb));
  ASSERT_TRUE(pin_code.has_value());

  EXPECT_FALSE(connection()->ltk().has_value());
  pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                      kTestLegacyLinkKeyType);

  // Connection not complete yet so link key is stored in LegacyPairingState and
  // not the connection
  EXPECT_FALSE(connection()->ltk().has_value());
  EXPECT_TRUE(pairing_state.link_key().has_value());
  EXPECT_EQ(kTestLinkKey, pairing_state.link_key().value());
}

TEST_F(LegacyPairingStateTest,
       PairingResponderSetsConnectionLinkKeyAfterAclConnectionComplete) {
  FakePairingDelegate pairing_delegate(kTestLocalIoCap);

  TestStatusHandler status_handler;

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   status_handler.MakeStatusCallback());
  EXPECT_FALSE(pairing_state.initiator());

  pairing_delegate.SetRequestPasskeyCallback([this](PeerId peer_id, auto cb) {
    EXPECT_EQ(peer()->identifier(), peer_id);
    ASSERT_TRUE(cb);
    cb(kTestRandomPinCode);
  });

  // Peer has invalid link key so we receive a PIN code request
  std::optional<uint16_t> pin_code;
  auto pin_code_cb = [&pin_code](std::optional<uint16_t> pin) {
    pin_code = pin;
  };
  pairing_state.OnPinCodeRequest(std::move(pin_code_cb));
  ASSERT_TRUE(pin_code.has_value());

  EXPECT_FALSE(connection()->ltk().has_value());
  pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                      kTestLegacyLinkKeyType);
  ASSERT_TRUE(connection()->ltk());
  EXPECT_EQ(kTestLinkKeyValue, connection()->ltk()->value());

  EXPECT_EQ(0, status_handler.call_count());
}

TEST_F(LegacyPairingStateTest,
       PairingInitiatorSetsConnectionLinkKeyAfterAclConnectionComplete) {
  FakePairingDelegate pairing_delegate(kTestLocalIoCap);

  TestStatusHandler status_handler;

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   status_handler.MakeStatusCallback());
  EXPECT_FALSE(pairing_state.initiator());

  pairing_delegate.SetDisplayPasskeyCallback(
      [](PeerId, uint32_t, PairingDelegate::DisplayMethod, auto cb) {
        cb(/*confirm=*/true);
      });

  EXPECT_FALSE(connection()->ltk().has_value());

  pairing_state.InitiatePairing(NoOpStatusCallback);
  EXPECT_EQ(1, auth_request_count());
  EXPECT_TRUE(pairing_state.initiator());

  EXPECT_EQ(std::nullopt, pairing_state.OnLinkKeyRequest());
  EXPECT_EQ(0, status_handler.call_count());

  std::optional<uint16_t> pin_code;
  auto pin_code_cb = [&pin_code](std::optional<uint16_t> pin) {
    pin_code = pin;
  };
  pairing_state.OnPinCodeRequest(std::move(pin_code_cb));
  ASSERT_TRUE(pin_code.has_value());

  pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                      kTestLegacyLinkKeyType);

  EXPECT_TRUE(connection()->ltk().has_value());
  EXPECT_EQ(kTestLinkKeyValue, connection()->ltk()->value());
  EXPECT_EQ(kTestLinkKeyValue, pairing_state.link_ltk()->value());
}

void NoOpUserPinCodeCallback(std::optional<uint16_t>) {}

TEST_F(LegacyPairingStateTest, UnexpectedLinkKeyTypeRaisesError) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  TestStatusHandler status_handler;

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   status_handler.MakeStatusCallback());
  EXPECT_FALSE(pairing_state.initiator());

  // Advance state machine.
  pairing_state.OnPinCodeRequest(NoOpUserPinCodeCallback);

  // Provide an SSP link key when a combination link key was expected
  pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                      kTestUnauthenticatedLinkKeyType192);

  EXPECT_EQ(1, status_handler.call_count());
  ASSERT_TRUE(status_handler.handle());
  EXPECT_EQ(kTestHandle, *status_handler.handle());
  ASSERT_TRUE(status_handler.status());
  EXPECT_EQ(ToResult(HostError::kFailed), *status_handler.status());
}

TEST_F(LegacyPairingStateTest,
       UnexpectedEncryptionChangeDoesNotTriggerStatusCallback) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  TestStatusHandler status_handler;

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   status_handler.MakeStatusCallback());

  // Advance state machine.
  pairing_state.InitiatePairing(NoOpStatusCallback);
  static_cast<void>(pairing_state.OnLinkKeyRequest());
  pairing_state.OnPinCodeRequest(NoOpUserPinCodeCallback);

  ASSERT_EQ(0, connection()->start_encryption_count());
  ASSERT_EQ(0, status_handler.call_count());

  connection()->TriggerEncryptionChangeCallback(fit::ok(true));
  EXPECT_EQ(0, status_handler.call_count());
}

TEST_F(LegacyPairingStateTest,
       InitiatingPairingOnPairingResponderWaitsForPairingToFinish) {
  FakePairingDelegate pairing_delegate(kTestLocalIoCap);

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   NoOpStatusCallback);
  EXPECT_FALSE(pairing_state.initiator());

  pairing_delegate.SetRequestPasskeyCallback([this](PeerId peer_id, auto cb) {
    EXPECT_EQ(peer()->identifier(), peer_id);
    ASSERT_TRUE(cb);
    cb(kTestDefaultPinCode);
  });

  // Advance state machine as pairing responder.
  pairing_state.OnPinCodeRequest(NoOpUserPinCodeCallback);
  EXPECT_TRUE(peer()->MutBrEdr().is_pairing());

  // Try to initiate pairing while pairing is in progress.
  TestStatusHandler status_handler;
  pairing_state.InitiatePairing(status_handler.MakeStatusCallback());
  EXPECT_FALSE(pairing_state.initiator());

  // Keep advancing state machine.
  pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                      kTestLegacyLinkKeyType);

  // Connection was complete so link key was stored in the connection
  EXPECT_TRUE(connection()->ltk().has_value());

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
  EXPECT_EQ(1, status_handler.call_count());
}

TEST_F(
    LegacyPairingStateTest,
    PairingStateRemainsResponderIfPairingInitiatedWhileResponderPairingInProgress) {
  FakePairingDelegate pairing_delegate(kTestLocalIoCap);

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   NoOpStatusCallback);
  EXPECT_FALSE(pairing_state.initiator());

  pairing_delegate.SetRequestPasskeyCallback([this](PeerId peer_id, auto cb) {
    EXPECT_EQ(peer()->identifier(), peer_id);
    ASSERT_TRUE(cb);
    cb(kTestDefaultPinCode);
  });

  pairing_state.OnPinCodeRequest(NoOpUserPinCodeCallback);

  pairing_state.InitiatePairing(NoOpStatusCallback);
  EXPECT_EQ(0, auth_request_count());
  EXPECT_FALSE(pairing_state.initiator());
}

TEST_F(LegacyPairingStateTest,
       InitiatingPairingAfterErrorTriggersStatusCallbackWithError) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  TestStatusHandler link_status_handler;

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   link_status_handler.MakeStatusCallback());

  // Unexpected event should make status callback get called with an error
  pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                      kTestLegacyLinkKeyType);

  EXPECT_EQ(1, link_status_handler.call_count());
  ASSERT_TRUE(link_status_handler.handle());
  EXPECT_EQ(kTestHandle, *link_status_handler.handle());
  ASSERT_TRUE(link_status_handler.status());
  EXPECT_EQ(ToResult(HostError::kFailed), *link_status_handler.status());

  // Try to initiate pairing again.
  TestStatusHandler pairing_status_handler;
  pairing_state.InitiatePairing(pairing_status_handler.MakeStatusCallback());

  // The status callback for pairing attempts made after a pairing failure
  // should be rejected as canceled.
  EXPECT_EQ(1, pairing_status_handler.call_count());
  ASSERT_TRUE(pairing_status_handler.handle());
  EXPECT_EQ(kTestHandle, *pairing_status_handler.handle());
  ASSERT_TRUE(pairing_status_handler.status());
  EXPECT_EQ(ToResult(HostError::kCanceled), *pairing_status_handler.status());
}

TEST_F(LegacyPairingStateTest, UnresolvedPairingCallbackIsCalledOnDestruction) {
  TestStatusHandler overall_status, request_status;
  {
    FakePairingDelegate pairing_delegate(kTestLocalIoCap);

    LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                     pairing_delegate.GetWeakPtr(),
                                     connection()->GetWeakPtr(),
                                     /*outgoing_connection=*/false,
                                     &dispatcher(),
                                     MakeAuthRequestCallback(),
                                     overall_status.MakeStatusCallback());
    EXPECT_FALSE(pairing_state.initiator());

    pairing_delegate.SetRequestPasskeyCallback([this](PeerId peer_id, auto cb) {
      EXPECT_EQ(peer()->identifier(), peer_id);
      ASSERT_TRUE(cb);
      cb(kTestRandomPinCode);
    });

    // Advance state machine as pairing responder.
    pairing_state.OnPinCodeRequest(NoOpUserPinCodeCallback);

    // Try to initiate pairing while pairing is in progress.
    pairing_state.InitiatePairing(request_status.MakeStatusCallback());
    EXPECT_FALSE(pairing_state.initiator());

    // Keep advancing state machine.
    pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                        kTestLegacyLinkKeyType);

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

TEST_F(LegacyPairingStateTest, StatusCallbackMayDestroyPairingState) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  std::unique_ptr<LegacyPairingState> pairing_state;
  bool cb_called = false;
  auto status_cb = [&pairing_state, &cb_called](hci_spec::ConnectionHandle,
                                                hci::Result<> status) {
    EXPECT_TRUE(status.is_error());
    cb_called = true;

    // Note that this lambda is owned by the LegacyPairingState so its
    // captures are invalid after this.
    pairing_state = nullptr;
  };

  pairing_state =
      std::make_unique<LegacyPairingState>(peer()->GetWeakPtr(),
                                           pairing_delegate.GetWeakPtr(),
                                           connection()->GetWeakPtr(),
                                           /*outgoing_connection=*/false,
                                           &dispatcher(),
                                           MakeAuthRequestCallback(),
                                           status_cb);

  // Unexpected event should make status callback get called with an error
  pairing_state->OnLinkKeyNotification(kTestLinkKeyValue,
                                       kTestLegacyLinkKeyType);

  EXPECT_TRUE(cb_called);
}

TEST_F(LegacyPairingStateTest, PairingInitiatorCallbackMayDestroyPairingState) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  std::unique_ptr<LegacyPairingState> pairing_state =
      std::make_unique<LegacyPairingState>(peer()->GetWeakPtr(),
                                           pairing_delegate.GetWeakPtr(),
                                           connection()->GetWeakPtr(),
                                           /*outgoing_connection=*/false,
                                           &dispatcher(),
                                           MakeAuthRequestCallback(),
                                           NoOpStatusCallback);
  bool cb_called = false;
  auto status_cb = [&pairing_state, &cb_called](hci_spec::ConnectionHandle,
                                                hci::Result<> status) {
    EXPECT_TRUE(status.is_error());
    cb_called = true;

    // Note that this lambda is owned by the LegacyPairingState so its
    // captures are invalid after this.
    pairing_state = nullptr;
  };
  pairing_state->InitiatePairing(status_cb);

  // Unexpected event should make status callback get called with an error
  pairing_state->OnLinkKeyNotification(kTestLinkKeyValue,
                                       kTestLegacyLinkKeyType);

  EXPECT_TRUE(cb_called);
}

TEST_F(LegacyPairingStateTest,
       PinCodeRequestWithoutPairingDelegateRejectsRequest) {
  TestStatusHandler status_handler;

  // Construct LegacyPairingState with a dead pairing delegate.
  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   PairingDelegate::WeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   status_handler.MakeStatusCallback());

  std::optional<uint16_t> pin_code = 1234;
  bool cb_called = false;

  // An incoming PIN code request when no pairing delegate is registered should
  // be gracefully rejected.
  auto pin_code_cb = [&](std::optional<uint16_t> pin) {
    cb_called = true;
    pin_code = pin;
  };

  pairing_state.OnPinCodeRequest(std::move(pin_code_cb));

  EXPECT_TRUE(cb_called);
  EXPECT_EQ(std::nullopt, pin_code);

  // Pairing status listeners should be notified of pairing failure due to no
  // delegate.
  EXPECT_EQ(1, status_handler.call_count());
  ASSERT_TRUE(status_handler.status());
  EXPECT_EQ(ToResult(HostError::kNotReady), *status_handler.status());
}

TEST_F(LegacyPairingStateTest, TransactionCollision) {
  FakePairingDelegate pairing_delegate(kTestLocalIoCap);
  pairing_delegate.SetDisplayPasskeyCallback(
      [](PeerId, uint32_t, PairingDelegate::DisplayMethod, auto cb) {
        cb(/*confirm=*/true);
      });

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   NoOpStatusCallback);
  pairing_state.SetPairingDelegate(pairing_delegate.GetWeakPtr());

  bool cb_called = false;
  auto status_cb = [&cb_called](hci_spec::ConnectionHandle,
                                hci::Result<> status) {
    EXPECT_FALSE(status.is_error());
    cb_called = true;
  };

  pairing_state.InitiatePairing(status_cb);
  static_cast<void>(pairing_state.OnLinkKeyRequest());
  pairing_state.OnPinCodeRequest(NoOpUserPinCodeCallback);
  pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                      kTestLegacyLinkKeyType);
  pairing_state.OnAuthenticationComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);

  auto tc_result = ToResult(
      pw::bluetooth::emboss::StatusCode::LMP_ERROR_TRANSACTION_COLLISION);
  hci::Result<bool> result(tc_result.take_error());
  pairing_state.OnEncryptionChange(result);

  EXPECT_FALSE(cb_called);
  pairing_state.OnEncryptionChange(fit::ok(true));
  EXPECT_TRUE(cb_called);
}

TEST_F(LegacyPairingStateTest, DifferentTransactionCollisionAsCentral) {
  FakePairingDelegate pairing_delegate(kTestLocalIoCap);
  pairing_delegate.SetDisplayPasskeyCallback(
      [](PeerId, uint32_t, PairingDelegate::DisplayMethod, auto cb) {
        cb(/*confirm=*/true);
      });

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/true,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   NoOpStatusCallback);
  pairing_state.SetPairingDelegate(pairing_delegate.GetWeakPtr());

  bool cb_called = false;
  auto status_cb = [&cb_called](hci_spec::ConnectionHandle,
                                hci::Result<> status) {
    EXPECT_FALSE(status.is_error());
    cb_called = true;
  };

  pairing_state.InitiatePairing(status_cb);
  static_cast<void>(pairing_state.OnLinkKeyRequest());
  pairing_state.OnPinCodeRequest(NoOpUserPinCodeCallback);
  pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                      kTestLegacyLinkKeyType);
  pairing_state.OnAuthenticationComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  ASSERT_EQ(1, connection()->start_encryption_count());

  auto tc_result = ToResult(
      pw::bluetooth::emboss::StatusCode::DIFFERENT_TRANSACTION_COLLISION);
  hci::Result<bool> result(tc_result.take_error());
  pairing_state.OnEncryptionChange(result);
  EXPECT_FALSE(cb_called);

  // Ensure we don't retry as a Central
  RunFor(LegacyPairingState::kDelayRetryEnableEncryption);
  ASSERT_EQ(1, connection()->start_encryption_count());

  pairing_state.OnEncryptionChange(fit::ok(true));
  EXPECT_TRUE(cb_called);
}

TEST_F(LegacyPairingStateTest, DifferentTransactionCollisionAsPeripheral) {
  FakePairingDelegate pairing_delegate(kTestLocalIoCap);
  pairing_delegate.SetDisplayPasskeyCallback(
      [](PeerId, uint32_t, PairingDelegate::DisplayMethod, auto cb) {
        cb(/*confirm=*/true);
      });

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   NoOpStatusCallback);
  pairing_state.SetPairingDelegate(pairing_delegate.GetWeakPtr());

  bool cb_called = false;
  auto status_cb = [&cb_called](hci_spec::ConnectionHandle,
                                hci::Result<> status) {
    EXPECT_FALSE(status.is_error());
    cb_called = true;
  };

  pairing_state.InitiatePairing(status_cb);
  static_cast<void>(pairing_state.OnLinkKeyRequest());
  pairing_state.OnPinCodeRequest(NoOpUserPinCodeCallback);
  pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                      kTestLegacyLinkKeyType);
  pairing_state.OnAuthenticationComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  ASSERT_EQ(1, connection()->start_encryption_count());

  auto tc_result = ToResult(
      pw::bluetooth::emboss::StatusCode::DIFFERENT_TRANSACTION_COLLISION);
  hci::Result<bool> result(tc_result.take_error());
  pairing_state.OnEncryptionChange(result);
  EXPECT_FALSE(cb_called);

  // Ensure we retry after kDelayRetryEnableEncryption amount of time
  RunFor(LegacyPairingState::kDelayRetryEnableEncryption);
  ASSERT_EQ(2, connection()->start_encryption_count());

  pairing_state.OnEncryptionChange(fit::ok(true));
  EXPECT_TRUE(cb_called);
}

TEST_F(LegacyPairingStateTest,
       DifferentTransactionCollisionRetryNotCalledIfSucceeded) {
  FakePairingDelegate pairing_delegate(kTestLocalIoCap);
  pairing_delegate.SetDisplayPasskeyCallback(
      [](PeerId, uint32_t, PairingDelegate::DisplayMethod, auto cb) {
        cb(/*confirm=*/true);
      });

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   NoOpStatusCallback);
  pairing_state.SetPairingDelegate(pairing_delegate.GetWeakPtr());

  bool cb_called = false;
  auto status_cb = [&cb_called](hci_spec::ConnectionHandle,
                                hci::Result<> status) {
    EXPECT_FALSE(status.is_error());
    cb_called = true;
  };

  pairing_state.InitiatePairing(status_cb);
  static_cast<void>(pairing_state.OnLinkKeyRequest());
  pairing_state.OnPinCodeRequest(NoOpUserPinCodeCallback);
  pairing_state.OnLinkKeyNotification(kTestLinkKeyValue,
                                      kTestLegacyLinkKeyType);
  pairing_state.OnAuthenticationComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  ASSERT_EQ(1, connection()->start_encryption_count());

  auto tc_result = ToResult(
      pw::bluetooth::emboss::StatusCode::DIFFERENT_TRANSACTION_COLLISION);
  hci::Result<bool> result(tc_result.take_error());
  pairing_state.OnEncryptionChange(result);
  EXPECT_FALSE(cb_called);

  pairing_state.OnEncryptionChange(fit::ok(true));
  EXPECT_TRUE(cb_called);

  // Ensure we don't retry if we succeeded in enabling encryption before the
  // retry handler is called
  RunFor(LegacyPairingState::kDelayRetryEnableEncryption);
  ASSERT_EQ(1, connection()->start_encryption_count());
}

// Event injectors. Return values are necessarily ignored in order to make types
// match, so don't use these functions to test return values. Likewise,
// arguments have been filled with test defaults for a successful pairing flow.
void LinkKeyRequest(LegacyPairingState* pairing_state) {
  static_cast<void>(pairing_state->OnLinkKeyRequest());
}
void PinCodeRequest(LegacyPairingState* pairing_state) {
  pairing_state->OnPinCodeRequest(NoOpUserPinCodeCallback);
}
void LinkKeyNotification(LegacyPairingState* pairing_state) {
  pairing_state->OnLinkKeyNotification(kTestLinkKeyValue,
                                       kTestLegacyLinkKeyType);
}
void AuthenticationComplete(LegacyPairingState* pairing_state) {
  pairing_state->OnAuthenticationComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
}

// Test suite fixture that genericizes an injected pairing state event. The
// event being tested should be retrieved with the GetParam method, which
// returns a default event injector. For example:
//
//   LegacyPairingState pairing_state;
//   GetParam()(&pairing_state);
//
// This is named so that the instantiated test description looks correct:
//
//   LegacyPairingStateTest/HandlesLegacyEvent.<test case>/<index of event>
class HandlesLegacyEvent
    : public LegacyPairingStateTest,
      public ::testing::WithParamInterface<void (*)(LegacyPairingState*)> {
 public:
  void SetUp() override {
    LegacyPairingStateTest::SetUp();

    pairing_delegate_ = std::make_unique<NoOpPairingDelegate>(kTestLocalIoCap);
    pairing_state_ = std::make_unique<LegacyPairingState>(
        peer()->GetWeakPtr(),
        pairing_delegate_->GetWeakPtr(),
        connection()->GetWeakPtr(),
        /*outgoing_connection=*/false,
        &dispatcher(),
        MakeAuthRequestCallback(),
        status_handler_.MakeStatusCallback());
    pairing_state().SetPairingDelegate(pairing_delegate_->GetWeakPtr());
  }

  void TearDown() override {
    pairing_state_.reset();
    LegacyPairingStateTest::TearDown();
  }

  const TestStatusHandler& status_handler() const { return status_handler_; }
  LegacyPairingState& pairing_state() { return *pairing_state_; }

  // Returns an event injector that was passed to INSTANTIATE_TEST_SUITE_P.
  static auto* event() { return GetParam(); }

  void InjectEvent() { event()(&pairing_state()); }

 private:
  TestStatusHandler status_handler_;
  std::unique_ptr<NoOpPairingDelegate> pairing_delegate_;
  std::unique_ptr<LegacyPairingState> pairing_state_;
};

// The tests here and below exercise that LegacyPairingState successfully
// advances through the expected pairing flow and generates errors when the
// pairing flow occurs out of order. This is intended to cover its internal
// state machine transitions and not the side effects.
INSTANTIATE_TEST_SUITE_P(LegacyPairingStateTest,
                         HandlesLegacyEvent,
                         ::testing::Values(LinkKeyRequest,
                                           PinCodeRequest,
                                           LinkKeyNotification,
                                           AuthenticationComplete));

TEST_P(HandlesLegacyEvent, InIdleState) {
  RETURN_IF_FATAL(InjectEvent());
  if (event() == LinkKeyRequest || event() == PinCodeRequest) {
    EXPECT_EQ(0, status_handler().call_count());
  } else {
    EXPECT_EQ(1, status_handler().call_count());
    ASSERT_TRUE(status_handler().handle());
    EXPECT_EQ(kTestHandle, *status_handler().handle());
    ASSERT_TRUE(status_handler().status());
    EXPECT_EQ(ToResult(HostError::kFailed), status_handler().status());
  }
}

TEST_P(HandlesLegacyEvent, InInitiatorWaitLinkKeyRequestState) {
  // Advance state machine
  pairing_state().InitiatePairing(NoOpStatusCallback);

  RETURN_IF_FATAL(InjectEvent());
  if (event() == LinkKeyRequest) {
    EXPECT_EQ(0, status_handler().call_count());
  } else {
    EXPECT_EQ(1, status_handler().call_count());
    ASSERT_TRUE(status_handler().status());
    EXPECT_EQ(ToResult(HostError::kFailed), status_handler().status());
  }
}

TEST_P(HandlesLegacyEvent, InWaitPinCodeRequestState) {
  // Advance state machine.
  pairing_state().InitiatePairing(NoOpStatusCallback);
  EXPECT_EQ(std::nullopt, pairing_state().OnLinkKeyRequest());

  RETURN_IF_FATAL(InjectEvent());
  if (event() == PinCodeRequest) {
    EXPECT_EQ(0, status_handler().call_count());
  } else {
    EXPECT_EQ(1, status_handler().call_count());
    ASSERT_TRUE(status_handler().status());
    EXPECT_EQ(ToResult(HostError::kFailed), status_handler().status());
  }
}

TEST_P(HandlesLegacyEvent, InWaitLinkKeyState) {
  auto pairing_delegate =
      std::make_unique<FakePairingDelegate>(kTestLocalIoCap);
  pairing_state().SetPairingDelegate(pairing_delegate->GetWeakPtr());

  pairing_delegate->SetRequestPasskeyCallback([this](PeerId peer_id, auto cb) {
    EXPECT_EQ(peer()->identifier(), peer_id);
    ASSERT_TRUE(cb);
    cb(kTestDefaultPinCode);
  });

  // Advance state machine.
  pairing_state().OnPinCodeRequest(NoOpUserPinCodeCallback);
  EXPECT_EQ(0, connection()->start_encryption_count());

  RETURN_IF_FATAL(InjectEvent());
  if (event() == LinkKeyNotification) {
    EXPECT_EQ(0, status_handler().call_count());
    EXPECT_EQ(1, connection()->start_encryption_count());
  } else {
    EXPECT_EQ(1, status_handler().call_count());
    ASSERT_TRUE(status_handler().status());
    EXPECT_EQ(ToResult(HostError::kFailed), status_handler().status());
  }
}

TEST_P(HandlesLegacyEvent, InInitiatorWaitAuthCompleteSkippingLegacyPairing) {
  EXPECT_TRUE(peer()->MutBrEdr().SetBondData(
      sm::LTK(sm::SecurityProperties(kTestLegacyLinkKeyType), kTestLinkKey)));

  // Advance state machine
  pairing_state().InitiatePairing(NoOpStatusCallback);
  EXPECT_NE(std::nullopt, pairing_state().OnLinkKeyRequest());

  RETURN_IF_FATAL(InjectEvent());
  if (event() == AuthenticationComplete) {
    EXPECT_EQ(0, status_handler().call_count());
    EXPECT_EQ(1, connection()->start_encryption_count());
  } else {
    EXPECT_EQ(1, status_handler().call_count());
    ASSERT_TRUE(status_handler().status());
    EXPECT_EQ(ToResult(HostError::kFailed), status_handler().status());
  }
}

TEST_P(HandlesLegacyEvent, InInitiatorWaitAuthCompleteAfterLegacyPairing) {
  auto pairing_delegate =
      std::make_unique<FakePairingDelegate>(kTestLocalIoCap);
  pairing_state().SetPairingDelegate(pairing_delegate->GetWeakPtr());

  pairing_delegate->SetDisplayPasskeyCallback(
      [](PeerId, uint32_t, PairingDelegate::DisplayMethod, auto cb) {
        cb(/*confirm=*/true);
      });

  // Advance state machine.
  pairing_state().InitiatePairing(NoOpStatusCallback);
  static_cast<void>(pairing_state().OnLinkKeyRequest());
  pairing_state().OnPinCodeRequest(NoOpUserPinCodeCallback);
  pairing_state().OnLinkKeyNotification(kTestLinkKeyValue,
                                        kTestLegacyLinkKeyType);
  EXPECT_TRUE(pairing_state().initiator());

  RETURN_IF_FATAL(InjectEvent());
  if (event() == AuthenticationComplete) {
    EXPECT_EQ(0, status_handler().call_count());
    EXPECT_EQ(1, connection()->start_encryption_count());
  } else {
    EXPECT_EQ(1, status_handler().call_count());
    ASSERT_TRUE(status_handler().status());
    EXPECT_EQ(ToResult(HostError::kFailed), status_handler().status());
  }
}

TEST_P(HandlesLegacyEvent, InIdleStateAfterOnePairing) {
  auto pairing_delegate =
      std::make_unique<FakePairingDelegate>(kTestLocalIoCap);
  pairing_state().SetPairingDelegate(pairing_delegate->GetWeakPtr());

  pairing_delegate->SetDisplayPasskeyCallback(
      [](PeerId, uint32_t, PairingDelegate::DisplayMethod, auto cb) {
        cb(/*confirm=*/true);
      });

  if (event() == PinCodeRequest) {
    pairing_delegate->SetRequestPasskeyCallback(
        [this](PeerId peer_id, auto cb) {
          EXPECT_EQ(peer()->identifier(), peer_id);
          ASSERT_TRUE(cb);
          cb(kTestDefaultPinCode);
        });
  }

  // Advance state machine.
  pairing_state().InitiatePairing(NoOpStatusCallback);
  static_cast<void>(pairing_state().OnLinkKeyRequest());
  pairing_state().OnPinCodeRequest(NoOpUserPinCodeCallback);
  pairing_state().OnLinkKeyNotification(kTestLinkKeyValue,
                                        kTestLegacyLinkKeyType);
  pairing_state().OnAuthenticationComplete(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_TRUE(pairing_state().initiator());

  // Successfully enabling encryption should allow pairing to start again.
  pairing_state().OnEncryptionChange(fit::ok(true));
  EXPECT_EQ(1, status_handler().call_count());
  ASSERT_TRUE(status_handler().status());
  EXPECT_EQ(fit::ok(), *status_handler().status());
  EXPECT_FALSE(pairing_state().initiator());

  RETURN_IF_FATAL(InjectEvent());
  if (event() == LinkKeyRequest || event() == PinCodeRequest) {
    EXPECT_EQ(1, status_handler().call_count());
  } else {
    EXPECT_EQ(2, status_handler().call_count());
    ASSERT_TRUE(status_handler().status());
    EXPECT_EQ(ToResult(HostError::kFailed), status_handler().status());
  }
}

TEST_P(HandlesLegacyEvent, InFailedStateAfterAuthenticationFailed) {
  auto pairing_delegate =
      std::make_unique<FakePairingDelegate>(kTestLocalIoCap);
  pairing_state().SetPairingDelegate(pairing_delegate->GetWeakPtr());

  pairing_delegate->SetDisplayPasskeyCallback(
      [](PeerId, uint32_t, PairingDelegate::DisplayMethod, auto cb) {
        cb(/*confirm=*/true);
      });

  // Advance state machine.
  pairing_state().InitiatePairing(NoOpStatusCallback);
  static_cast<void>(pairing_state().OnLinkKeyRequest());
  pairing_state().OnPinCodeRequest(NoOpUserPinCodeCallback);
  pairing_state().OnLinkKeyNotification(kTestLinkKeyValue,
                                        kTestLegacyLinkKeyType);

  // Inject failure status.
  pairing_state().OnAuthenticationComplete(
      pw::bluetooth::emboss::StatusCode::AUTHENTICATION_FAILURE);
  EXPECT_EQ(1, status_handler().call_count());
  ASSERT_TRUE(status_handler().status());
  EXPECT_FALSE(status_handler().status()->is_ok());

  RETURN_IF_FATAL(InjectEvent());
  EXPECT_EQ(2, status_handler().call_count());
  ASSERT_TRUE(status_handler().status());
  EXPECT_EQ(ToResult(HostError::kFailed), status_handler().status());
}

#ifndef NINSPECT
TEST_F(LegacyPairingStateTest, Inspect) {
  NoOpPairingDelegate pairing_delegate(kTestLocalIoCap);

  TestStatusHandler status_handler;

  inspect::Inspector inspector;

  LegacyPairingState pairing_state(peer()->GetWeakPtr(),
                                   pairing_delegate.GetWeakPtr(),
                                   connection()->GetWeakPtr(),
                                   /*outgoing_connection=*/false,
                                   &dispatcher(),
                                   MakeAuthRequestCallback(),
                                   status_handler.MakeStatusCallback());

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

}  // namespace
}  // namespace bt::gap
