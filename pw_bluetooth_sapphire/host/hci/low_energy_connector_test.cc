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

#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_connector.h"

#include <vector>

#include "pw_async/heap_dispatcher.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/defaults.h"
#include "pw_bluetooth_sapphire/internal/host/hci/fake_local_address_delegate.h"
#include "pw_bluetooth_sapphire/internal/host/hci/fake_low_energy_connection.h"
#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_controller.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_peer.h"

namespace bt::hci {
namespace {

using bt::testing::FakeController;
using bt::testing::FakePeer;
using TestingBase = bt::testing::FakeDispatcherControllerTest<FakeController>;

const DeviceAddress kLocalAddress(DeviceAddress::Type::kLEPublic, {0xFF});
const DeviceAddress kRandomAddress(DeviceAddress::Type::kLERandom, {0xFE});
const DeviceAddress kTestAddress(DeviceAddress::Type::kLEPublic, {1});
const hci_spec::LEPreferredConnectionParameters kTestParams(6, 6, 1, 10);
constexpr pw::chrono::SystemClock::duration kPwConnectTimeout =
    std::chrono::seconds(10);

class LowEnergyConnectorTest : public TestingBase,
                               public ::testing::WithParamInterface<bool> {
 public:
  LowEnergyConnectorTest() = default;
  ~LowEnergyConnectorTest() override = default;

 protected:
  void SetUp() override {
    TestingBase::SetUp();
    InitializeACLDataChannel();

    FakeController::Settings settings;
    settings.ApplyLegacyLEConfig();
    test_device()->set_settings(settings);

    fake_address_delegate_.set_local_address(kLocalAddress);

    bool use_extended_operations = GetParam();
    connector_ = std::make_unique<LowEnergyConnector>(
        transport()->GetWeakPtr(),
        &fake_address_delegate_,
        dispatcher(),
        fit::bind_member<&LowEnergyConnectorTest::OnIncomingConnectionCreated>(
            this),
        use_extended_operations);

    test_device()->set_connection_state_callback(
        fit::bind_member<&LowEnergyConnectorTest::OnConnectionStateChanged>(
            this));
  }

  void TearDown() override {
    connector_ = nullptr;
    in_connections_.clear();
    test_device()->Stop();
    TestingBase::TearDown();
  }

  EventPacket CreateConnectionCompleteSubevent(
      hci_spec::ConnectionHandle conn_handle,
      const DeviceAddress& peer_address) const {
    auto packet = hci::EventPacket::New<
        pw::bluetooth::emboss::LEEnhancedConnectionCompleteSubeventV1Writer>(
        hci_spec::kLEMetaEventCode);
    auto params = packet.view_t();

    params.le_meta_event().subevent_code().Write(
        hci_spec::kLEEnhancedConnectionCompleteSubeventCode);
    params.status().Write(pw::bluetooth::emboss::StatusCode::SUCCESS);
    params.connection_handle().Write(conn_handle);
    params.role().Write(pw::bluetooth::emboss::ConnectionRole::PERIPHERAL);
    params.peer_address_type().Write(
        pw::bluetooth::emboss::LEAddressType::PUBLIC);
    params.peer_address().CopyFrom(peer_address.value().view());
    params.connection_interval().Write(
        hci_spec::defaults::kLEConnectionIntervalMin);
    params.peripheral_latency().Write(0);
    params.supervision_timeout().Write(10);
    params.central_clock_accuracy().Write(
        pw::bluetooth::emboss::LEClockAccuracy::PPM_20);

    // there are also local_resolvable_private_address and
    // peer_resolvable_private_address fields available within
    // LEEnhancedConnectionCompleteSubeventV1. However, these fields are only
    // valid when we use either the identity addresses in
    // HCI_LE_Enhanced_Create_Connection for either the local device or peer
    // device. Our tests mainly use public or random addresses, as if they
    // have already resolved the addresses. We ignore setting these fields
    // here to prevent confusion.

    return packet;
  }

  EventPacket CreateConnectionCompleteSubeventWithInvalidAddressType(
      hci_spec::ConnectionHandle conn_handle,
      const DeviceAddress& peer_address,
      bool use_extended_operations) const {
    if (use_extended_operations) {
      EventPacket packet =
          CreateConnectionCompleteSubevent(conn_handle, peer_address);
      auto params =
          packet.view<pw::bluetooth::emboss::
                          LEEnhancedConnectionCompleteSubeventV1Writer>();

      params.peer_address_type().Write(
          static_cast<pw::bluetooth::emboss::LEAddressType>(0x04));  // Invalid!

      return packet;
    } else {
      auto packet = hci::EventPacket::New<
          pw::bluetooth::emboss::LEConnectionCompleteSubeventWriter>(
          hci_spec::kLEMetaEventCode);
      auto params = packet.view_t();

      params.le_meta_event().subevent_code().Write(
          hci_spec::kLEConnectionCompleteSubeventCode);
      params.status().Write(pw::bluetooth::emboss::StatusCode::SUCCESS);
      params.connection_handle().Write(conn_handle);
      params.role().Write(pw::bluetooth::emboss::ConnectionRole::PERIPHERAL);
      params.peer_address().CopyFrom(peer_address.value().view());
      params.peer_address_type().Write(
          static_cast<pw::bluetooth::emboss::LEPeerAddressTypeNoAnon>(
              0x04));  // Invalid!
      params.connection_interval().Write(
          hci_spec::defaults::kLEConnectionIntervalMin);
      params.peripheral_latency().Write(0);
      params.supervision_timeout().Write(10);

      return packet;
    }
  }

  pw::async::HeapDispatcher& heap_dispatcher() { return heap_dispatcher_; }
  void DeleteConnector() { connector_ = nullptr; }
  bool request_canceled() const { return request_canceled_; }
  LowEnergyConnector* connector() const { return connector_.get(); }

  const std::vector<std::unique_ptr<LowEnergyConnection>>& in_connections()
      const {
    return in_connections_;
  }

  FakeLocalAddressDelegate* fake_address_delegate() {
    return &fake_address_delegate_;
  }

 private:
  void OnIncomingConnectionCreated(hci_spec::ConnectionHandle handle,
                                   pw::bluetooth::emboss::ConnectionRole role,
                                   const DeviceAddress& peer_address,
                                   const hci_spec::LEConnectionParameters&) {
    in_connections_.push_back(
        std::make_unique<testing::FakeLowEnergyConnection>(
            handle,
            kLocalAddress,
            peer_address,
            role,
            transport()->GetWeakPtr(),
            dispatcher()));
  }

  void OnConnectionStateChanged(const DeviceAddress&,
                                hci_spec::ConnectionHandle,
                                bool,
                                bool canceled) {
    request_canceled_ = canceled;
  }

  bool request_canceled_ = false;
  FakeLocalAddressDelegate fake_address_delegate_{dispatcher()};
  std::unique_ptr<LowEnergyConnector> connector_;
  std::vector<std::unique_ptr<LowEnergyConnection>> in_connections_;
  pw::async::HeapDispatcher heap_dispatcher_{dispatcher()};

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LowEnergyConnectorTest);
};

INSTANTIATE_TEST_SUITE_P(LowEnergyConnectorTest,
                         LowEnergyConnectorTest,
                         ::testing::Bool());

TEST_P(LowEnergyConnectorTest, CreateConnection) {
  auto fake_peer = std::make_unique<FakePeer>(kTestAddress, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));
  // Public address would be used if privacy was disabled
  fake_address_delegate()->EnablePrivacy(true);

  EXPECT_FALSE(connector()->request_pending());
  EXPECT_FALSE(connector()->pending_peer_address());

  Result<> status = fit::ok();
  std::unique_ptr<LowEnergyConnection> conn;
  bool callback_called = false;

  auto callback = [&](auto cb_status, auto cb_conn) {
    status = cb_status;
    conn = std::move(cb_conn);
    callback_called = true;
  };

  bool ret = connector()->CreateConnection(
      /*use_accept_list=*/false,
      kTestAddress,
      hci_spec::defaults::kLEScanInterval,
      hci_spec::defaults::kLEScanWindow,
      kTestParams,
      callback,
      kPwConnectTimeout);
  EXPECT_TRUE(ret);
  EXPECT_TRUE(connector()->request_pending());
  EXPECT_EQ(connector()->pending_peer_address().value(), kTestAddress);

  ret = connector()->CreateConnection(
      /*use_accept_list=*/false,
      kTestAddress,
      hci_spec::defaults::kLEScanInterval,
      hci_spec::defaults::kLEScanWindow,
      kTestParams,
      callback,
      kPwConnectTimeout);
  EXPECT_FALSE(ret);

  RunUntilIdle();

  EXPECT_FALSE(connector()->request_pending());
  EXPECT_FALSE(connector()->pending_peer_address());
  EXPECT_TRUE(callback_called);
  EXPECT_EQ(fit::ok(), status);
  EXPECT_TRUE(in_connections().empty());

  ASSERT_TRUE(conn);
  EXPECT_EQ(1u, conn->handle());
  EXPECT_EQ(kLocalAddress, conn->local_address());
  EXPECT_EQ(kTestAddress, conn->peer_address());
  conn->Disconnect(
      pw::bluetooth::emboss::StatusCode::REMOTE_USER_TERMINATED_CONNECTION);
}

// Controller reports error from HCI Command Status event.
TEST_P(LowEnergyConnectorTest, CreateConnectionStatusError) {
  auto fake_peer = std::make_unique<FakePeer>(kTestAddress, dispatcher());
  fake_peer->set_connect_status(
      pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED);
  test_device()->AddPeer(std::move(fake_peer));

  EXPECT_FALSE(connector()->request_pending());

  Result<> status = fit::ok();
  std::unique_ptr<LowEnergyConnection> conn;
  bool callback_called = false;

  auto callback = [&](auto cb_status, auto cb_conn) {
    status = cb_status;
    conn = std::move(cb_conn);
    callback_called = true;
  };

  bool ret = connector()->CreateConnection(
      /*use_accept_list=*/false,
      kTestAddress,
      hci_spec::defaults::kLEScanInterval,
      hci_spec::defaults::kLEScanWindow,
      kTestParams,
      callback,
      kPwConnectTimeout);
  EXPECT_TRUE(ret);
  EXPECT_TRUE(connector()->request_pending());

  RunUntilIdle();

  EXPECT_FALSE(connector()->request_pending());
  EXPECT_TRUE(callback_called);
  EXPECT_EQ(ToResult(pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED),
            status);
  EXPECT_FALSE(conn);
  EXPECT_TRUE(in_connections().empty());
}

// Controller reports error from HCI LE Connection Complete event
TEST_P(LowEnergyConnectorTest, CreateConnectionEventError) {
  auto fake_peer = std::make_unique<FakePeer>(kTestAddress, dispatcher());
  fake_peer->set_connect_response(
      pw::bluetooth::emboss::StatusCode::CONNECTION_REJECTED_SECURITY);
  test_device()->AddPeer(std::move(fake_peer));

  EXPECT_FALSE(connector()->request_pending());

  Result<> status = fit::ok();
  std::unique_ptr<LowEnergyConnection> conn;
  bool callback_called = false;

  auto callback = [&](auto cb_status, auto cb_conn) {
    status = cb_status;
    callback_called = true;
    conn = std::move(cb_conn);
  };

  bool ret = connector()->CreateConnection(
      /*use_accept_list=*/false,
      kTestAddress,
      hci_spec::defaults::kLEScanInterval,
      hci_spec::defaults::kLEScanWindow,
      kTestParams,
      callback,
      kPwConnectTimeout);
  EXPECT_TRUE(ret);
  EXPECT_TRUE(connector()->request_pending());

  RunUntilIdle();

  EXPECT_FALSE(connector()->request_pending());
  EXPECT_TRUE(callback_called);
  EXPECT_EQ(
      ToResult(pw::bluetooth::emboss::StatusCode::CONNECTION_REJECTED_SECURITY),
      status);
  EXPECT_TRUE(in_connections().empty());
  EXPECT_FALSE(conn);
}

TEST_P(LowEnergyConnectorTest, CreateConnectionEventInvalidAddressType) {
  auto fake_peer = std::make_unique<FakePeer>(kTestAddress, dispatcher());

  // Make sure the pending connect remains pending.
  fake_peer->set_force_pending_connect(true);
  test_device()->AddPeer(std::move(fake_peer));

  Result<> status = fit::ok();
  bool callback_called = false;

  auto callback = [&](auto cb_status, auto /*cb_conn*/) {
    status = cb_status;
    callback_called = true;
  };

  connector()->CreateConnection(
      /*use_accept_list=*/false,
      kTestAddress,
      hci_spec::defaults::kLEScanInterval,
      hci_spec::defaults::kLEScanWindow,
      kTestParams,
      callback,
      kPwConnectTimeout);

  RunUntilIdle();
  EXPECT_TRUE(connector()->request_pending());

  bool use_extended_operations = GetParam();
  EventPacket packet = CreateConnectionCompleteSubeventWithInvalidAddressType(
      0x0001, kTestAddress, use_extended_operations);

  test_device()->SendCommandChannelPacket(packet.data());
  RunUntilIdle();

  EXPECT_FALSE(connector()->request_pending());
  EXPECT_TRUE(callback_called);
  EXPECT_TRUE(status.is_error());
  EXPECT_EQ(ToResult(HostError::kPacketMalformed), status);
}

// Controller reports error from HCI LE Connection Complete event
TEST_P(LowEnergyConnectorTest, Cancel) {
  auto fake_peer = std::make_unique<FakePeer>(kTestAddress, dispatcher());

  // Make sure the pending connect remains pending.
  fake_peer->set_force_pending_connect(true);
  test_device()->AddPeer(std::move(fake_peer));

  hci::Result<> status = fit::ok();
  std::unique_ptr<LowEnergyConnection> conn;
  bool callback_called = false;

  auto callback = [&](auto cb_status, auto cb_conn) {
    status = cb_status;
    callback_called = true;
    conn = std::move(cb_conn);
  };

  bool ret = connector()->CreateConnection(
      /*use_accept_list=*/false,
      kTestAddress,
      hci_spec::defaults::kLEScanInterval,
      hci_spec::defaults::kLEScanWindow,
      kTestParams,
      callback,
      kPwConnectTimeout);
  EXPECT_TRUE(ret);
  EXPECT_TRUE(connector()->request_pending());

  ASSERT_FALSE(request_canceled());

  connector()->Cancel();
  EXPECT_TRUE(connector()->request_pending());

  // The request timeout should be canceled regardless of whether it was posted
  // before.
  EXPECT_FALSE(connector()->timeout_posted());

  RunUntilIdle();

  EXPECT_FALSE(connector()->timeout_posted());
  EXPECT_FALSE(connector()->request_pending());
  EXPECT_TRUE(callback_called);
  EXPECT_TRUE(request_canceled());
  EXPECT_EQ(ToResult(HostError::kCanceled), status);
  EXPECT_TRUE(in_connections().empty());
  EXPECT_FALSE(conn);
}

TEST_P(LowEnergyConnectorTest, IncomingConnect) {
  EXPECT_TRUE(in_connections().empty());
  EXPECT_FALSE(connector()->request_pending());

  EventPacket packet = CreateConnectionCompleteSubevent(1, kTestAddress);
  test_device()->SendCommandChannelPacket(packet.data());

  RunUntilIdle();
  ASSERT_EQ(1u, in_connections().size());

  auto conn = in_connections()[0].get();
  EXPECT_EQ(1u, conn->handle());
  EXPECT_EQ(kLocalAddress, conn->local_address());
  EXPECT_EQ(kTestAddress, conn->peer_address());
  conn->Disconnect(
      pw::bluetooth::emboss::StatusCode::REMOTE_USER_TERMINATED_CONNECTION);
}

TEST_P(LowEnergyConnectorTest, IncomingConnectDuringConnectionRequest) {
  const DeviceAddress kIncomingAddress(DeviceAddress::Type::kLEPublic, {2});

  EXPECT_TRUE(in_connections().empty());
  EXPECT_FALSE(connector()->request_pending());

  auto fake_peer = std::make_unique<FakePeer>(kTestAddress, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  Result<> status = fit::ok();
  std::unique_ptr<LowEnergyConnection> conn;
  unsigned int callback_count = 0;

  auto callback = [&](auto cb_status, auto cb_conn) {
    status = cb_status;
    callback_count++;
    conn = std::move(cb_conn);
  };

  connector()->CreateConnection(
      /*use_accept_list=*/false,
      kTestAddress,
      hci_spec::defaults::kLEScanInterval,
      hci_spec::defaults::kLEScanWindow,
      kTestParams,
      callback,
      kPwConnectTimeout);

  (void)heap_dispatcher().Post(
      [kIncomingAddress, this](pw::async::Context /*ctx*/, pw::Status status) {
        if (!status.ok()) {
          return;
        }

        EventPacket packet =
            CreateConnectionCompleteSubevent(2, kIncomingAddress);
        test_device()->SendCommandChannelPacket(packet.data());
      });

  RunUntilIdle();

  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(1u, callback_count);
  ASSERT_EQ(1u, in_connections().size());

  const auto& in_conn = in_connections().front();

  EXPECT_EQ(1u, conn->handle());
  EXPECT_EQ(2u, in_conn->handle());
  EXPECT_EQ(kTestAddress, conn->peer_address());
  EXPECT_EQ(kIncomingAddress, in_conn->peer_address());

  conn->Disconnect(
      pw::bluetooth::emboss::StatusCode::REMOTE_USER_TERMINATED_CONNECTION);
  in_conn->Disconnect(
      pw::bluetooth::emboss::StatusCode::REMOTE_USER_TERMINATED_CONNECTION);
}

TEST_P(LowEnergyConnectorTest, CreateConnectionTimeout) {
  // We do not set up any fake devices. This will cause the request to time out.
  EXPECT_FALSE(connector()->request_pending());

  Result<> status = fit::ok();
  std::unique_ptr<LowEnergyConnection> conn;
  bool callback_called = false;

  auto callback = [&](auto cb_status, auto cb_conn) {
    status = cb_status;
    callback_called = true;
    conn = std::move(cb_conn);
  };

  connector()->CreateConnection(
      /*use_accept_list=*/false,
      kTestAddress,
      hci_spec::defaults::kLEScanInterval,
      hci_spec::defaults::kLEScanWindow,
      kTestParams,
      callback,
      kPwConnectTimeout);
  EXPECT_TRUE(connector()->request_pending());

  EXPECT_FALSE(request_canceled());

  // Make the connection attempt time out.
  RunFor(kPwConnectTimeout);

  EXPECT_FALSE(connector()->request_pending());
  EXPECT_TRUE(callback_called);
  EXPECT_TRUE(request_canceled());
  EXPECT_EQ(ToResult(HostError::kTimedOut), status);
  EXPECT_TRUE(in_connections().empty());
  EXPECT_FALSE(conn);
}

TEST_P(LowEnergyConnectorTest, SendRequestAndDelete) {
  auto fake_peer = std::make_unique<FakePeer>(kTestAddress, dispatcher());

  // Make sure the pending connect remains pending.
  fake_peer->set_force_pending_connect(true);
  test_device()->AddPeer(std::move(fake_peer));

  bool ret = connector()->CreateConnection(
      /*use_accept_list=*/
      false,
      kTestAddress,
      hci_spec::defaults::kLEScanInterval,
      hci_spec::defaults::kLEScanWindow,
      kTestParams,
      [](auto, auto) {},
      kPwConnectTimeout);
  EXPECT_TRUE(ret);
  EXPECT_TRUE(connector()->request_pending());

  DeleteConnector();
  RunUntilIdle();

  EXPECT_TRUE(request_canceled());
  EXPECT_TRUE(in_connections().empty());
}

TEST_P(LowEnergyConnectorTest, AllowsRandomAddressChange) {
  EXPECT_TRUE(connector()->AllowsRandomAddressChange());

  auto fake_peer = std::make_unique<FakePeer>(kTestAddress, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  // Address change should not be allowed while the procedure is pending.
  connector()->CreateConnection(
      /*use_accept_list=*/
      false,
      kTestAddress,
      hci_spec::defaults::kLEScanInterval,
      hci_spec::defaults::kLEScanWindow,
      kTestParams,
      [](auto, auto) {},
      kPwConnectTimeout);
  EXPECT_TRUE(connector()->request_pending());
  EXPECT_FALSE(connector()->AllowsRandomAddressChange());

  RunUntilIdle();
  EXPECT_TRUE(connector()->AllowsRandomAddressChange());
}

TEST_P(LowEnergyConnectorTest,
       AllowsRandomAddressChangeWhileRequestingLocalAddress) {
  // Make the local address delegate report its result asynchronously.
  fake_address_delegate()->set_async(true);

  // The connector should be in the "request pending" state without initiating
  // controller procedures that would prevent a local address change.
  connector()->CreateConnection(
      /*use_accept_list=*/
      false,
      kTestAddress,
      hci_spec::defaults::kLEScanInterval,
      hci_spec::defaults::kLEScanWindow,
      kTestParams,
      [](auto, auto) {},
      kPwConnectTimeout);
  EXPECT_TRUE(connector()->request_pending());
  EXPECT_TRUE(connector()->AllowsRandomAddressChange());

  // Initiating a new connection should fail in this state.
  bool result = connector()->CreateConnection(
      /*use_accept_list=*/
      false,
      kTestAddress,
      hci_spec::defaults::kLEScanInterval,
      hci_spec::defaults::kLEScanWindow,
      kTestParams,
      [](auto, auto) {},
      kPwConnectTimeout);
  EXPECT_FALSE(result);

  // After the loop runs the request should remain pending (since we added no
  // fake device, the request would eventually timeout) but address change
  // should no longer be allowed.
  RunUntilIdle();
  EXPECT_TRUE(connector()->request_pending());
  EXPECT_FALSE(connector()->AllowsRandomAddressChange());
}

TEST_P(LowEnergyConnectorTest, ConnectUsingAcceptList) {
  auto fake_peer = std::make_unique<FakePeer>(kTestAddress, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));
  connector()->CreateConnection(
      /*use_accept_list=*/
      true,
      kTestAddress,
      hci_spec::defaults::kLEScanInterval,
      hci_spec::defaults::kLEScanWindow,
      kTestParams,
      [](auto, auto) {},
      kPwConnectTimeout);
  RunUntilIdle();
  ASSERT_TRUE(test_device()->le_connect_params());
  EXPECT_EQ(pw::bluetooth::emboss::LEOwnAddressType::PUBLIC,
            test_device()->le_connect_params()->own_address_type);
}

TEST_P(LowEnergyConnectorTest, ConnectUsingPublicAddress) {
  auto fake_peer = std::make_unique<FakePeer>(kTestAddress, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));
  connector()->CreateConnection(
      /*use_accept_list=*/
      false,
      kTestAddress,
      hci_spec::defaults::kLEScanInterval,
      hci_spec::defaults::kLEScanWindow,
      kTestParams,
      [](auto, auto) {},
      kPwConnectTimeout);
  RunUntilIdle();
  ASSERT_TRUE(test_device()->le_connect_params());
  EXPECT_EQ(pw::bluetooth::emboss::LEOwnAddressType::PUBLIC,
            test_device()->le_connect_params()->own_address_type);
}

TEST_P(LowEnergyConnectorTest, ConnectUsingRandomAddress) {
  fake_address_delegate()->set_local_address(kRandomAddress);
  fake_address_delegate()->EnablePrivacy(true);

  auto fake_peer = std::make_unique<FakePeer>(kTestAddress, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));
  connector()->CreateConnection(
      /*use_accept_list=*/
      false,
      kTestAddress,
      hci_spec::defaults::kLEScanInterval,
      hci_spec::defaults::kLEScanWindow,
      kTestParams,
      [](auto, auto) {},
      kPwConnectTimeout);
  RunUntilIdle();
  ASSERT_TRUE(test_device()->le_connect_params());
  EXPECT_EQ(pw::bluetooth::emboss::LEOwnAddressType::RANDOM,
            test_device()->le_connect_params()->own_address_type);
}

TEST_P(LowEnergyConnectorTest, ConnectUsingRandomPeerAddress) {
  auto fake_peer = std::make_unique<FakePeer>(kRandomAddress, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));
  connector()->CreateConnection(
      /*use_accept_list=*/
      false,
      kRandomAddress,
      hci_spec::defaults::kLEScanInterval,
      hci_spec::defaults::kLEScanWindow,
      kTestParams,
      [](auto, auto) {},
      kPwConnectTimeout);
  RunUntilIdle();
  ASSERT_TRUE(test_device()->le_connect_params());
  EXPECT_EQ(DeviceAddress::Type::kLERandom,
            test_device()->le_connect_params()->peer_address.type());
}

TEST_P(LowEnergyConnectorTest, CancelConnectWhileWaitingForLocalAddress) {
  Result<> status = fit::ok();
  std::unique_ptr<LowEnergyConnection> conn;
  auto callback = [&](auto s, auto c) {
    status = s;
    conn = std::move(c);
  };
  fake_address_delegate()->set_async(true);
  connector()->CreateConnection(
      /*use_accept_list=*/false,
      kTestAddress,
      hci_spec::defaults::kLEScanInterval,
      hci_spec::defaults::kLEScanWindow,
      kTestParams,
      std::move(callback),
      kPwConnectTimeout);

  // Should be waiting for the address.
  EXPECT_TRUE(connector()->request_pending());
  EXPECT_TRUE(connector()->AllowsRandomAddressChange());

  connector()->Cancel();
  RunUntilIdle();
  EXPECT_FALSE(connector()->request_pending());
  EXPECT_TRUE(connector()->AllowsRandomAddressChange());

  // The controller should have received no command from us.
  EXPECT_FALSE(test_device()->le_connect_params());
  EXPECT_FALSE(request_canceled());

  // Our request should have resulted in an error.
  EXPECT_EQ(ToResult(HostError::kCanceled), status);
  EXPECT_FALSE(conn);
}

TEST_P(LowEnergyConnectorTest, UseLocalIdentityAddress) {
  // Public identity address and a random current local address.
  fake_address_delegate()->set_identity_address(kLocalAddress);
  fake_address_delegate()->set_local_address(kRandomAddress);

  connector()->UseLocalIdentityAddress();

  auto fake_peer = std::make_unique<FakePeer>(kTestAddress, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));
  connector()->CreateConnection(
      /*use_accept_list=*/
      false,
      kTestAddress,
      hci_spec::defaults::kLEScanInterval,
      hci_spec::defaults::kLEScanWindow,
      kTestParams,
      [](auto, auto) {},
      kPwConnectTimeout);
  RunUntilIdle();
  ASSERT_TRUE(test_device()->le_connect_params());

  // The public address should have been used.
  EXPECT_EQ(pw::bluetooth::emboss::LEOwnAddressType::PUBLIC,
            test_device()->le_connect_params()->own_address_type);
}

TEST_P(LowEnergyConnectorTest, ExtendedCreateConnectionUsesSameParameters) {
  bool use_extended_operations = GetParam();
  if (!use_extended_operations) {
    // For this specific test, we want to test only the LE Extended Create
    // Connection functionality. Since this is only a single test, skipping the
    // LE Create Connection option seemed simpler. If tests for LE Extended
    // Create Connection grow in number, we should create a separate file, test
    // suite, etc.
    return;
  }

  using LEConnectParams = FakeController::LEConnectParams;
  using InitiatingPHYs = LEConnectParams::InitiatingPHYs;

  auto fake_peer = std::make_unique<FakePeer>(kTestAddress, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  connector()->CreateConnection(
      /*use_accept_list=*/
      true,
      kTestAddress,
      hci_spec::defaults::kLEScanInterval,
      hci_spec::defaults::kLEScanWindow,
      kTestParams,
      [](auto, auto) {},
      kPwConnectTimeout);
  RunUntilIdle();

  const std::optional<LEConnectParams>& params =
      test_device()->le_connect_params();
  ASSERT_EQ(3u, params->phy_conn_params.size());
  const auto& le_1m =
      params->phy_conn_params.find(InitiatingPHYs::kLE_1M)->second;
  const auto& le_2m =
      params->phy_conn_params.find(InitiatingPHYs::kLE_2M)->second;
  const auto& le_coded =
      params->phy_conn_params.find(InitiatingPHYs::kLE_Coded)->second;

  ASSERT_EQ(le_1m.scan_interval, le_2m.scan_interval);
  ASSERT_EQ(le_1m.scan_window, le_2m.scan_window);
  ASSERT_EQ(le_1m.connection_interval_min, le_2m.connection_interval_min);
  ASSERT_EQ(le_1m.connection_interval_max, le_2m.connection_interval_max);
  ASSERT_EQ(le_1m.max_latency, le_2m.max_latency);
  ASSERT_EQ(le_1m.supervision_timeout, le_2m.supervision_timeout);
  ASSERT_EQ(le_1m.min_ce_length, le_2m.min_ce_length);
  ASSERT_EQ(le_1m.max_ce_length, le_2m.max_ce_length);

  ASSERT_EQ(le_2m.scan_interval, le_coded.scan_interval);
  ASSERT_EQ(le_2m.scan_window, le_coded.scan_window);
  ASSERT_EQ(le_2m.connection_interval_min, le_coded.connection_interval_min);
  ASSERT_EQ(le_2m.connection_interval_max, le_coded.connection_interval_max);
  ASSERT_EQ(le_2m.max_latency, le_coded.max_latency);
  ASSERT_EQ(le_2m.supervision_timeout, le_coded.supervision_timeout);
  ASSERT_EQ(le_2m.min_ce_length, le_coded.min_ce_length);
  ASSERT_EQ(le_2m.max_ce_length, le_coded.max_ce_length);
}

}  // namespace
}  // namespace bt::hci
