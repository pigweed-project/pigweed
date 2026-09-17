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

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/low_energy_peripheral_server.h"

#include "fuchsia/bluetooth/cpp/fidl.h"
#include "fuchsia/bluetooth/le/cpp/fidl.h"
#include "pw_bluetooth_sapphire/fake_lease_provider.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/adapter_test_fixture.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/fake_adapter_test_fixture.h"
#include "pw_bluetooth_sapphire/internal/host/common/advertising_data.h"
#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_advertising_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_connection_manager.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_peer.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"

namespace bthost {
namespace {

bool IsChannelPeerClosed(const zx::channel& channel) {
  zx_signals_t ignored;
  return ZX_OK == channel.wait_one(/*signals=*/ZX_CHANNEL_PEER_CLOSED,
                                   /*deadline=*/zx::time(ZX_TIME_INFINITE_PAST),
                                   &ignored);
}

namespace fble = fuchsia::bluetooth::le;
const bt::DeviceAddress kTestAddr(bt::DeviceAddress::Type::kLEPublic,
                                  {0x01, 0, 0, 0, 0, 0});
const bt::DeviceAddress kTestAddr2(bt::DeviceAddress::Type::kLEPublic,
                                   {0x02, 0, 0, 0, 0, 0});

using bt::testing::FakePeer;
using FidlAdvHandle = fidl::InterfaceHandle<fble::AdvertisingHandle>;

class LowEnergyPeripheralServerTestFakeAdapter
    : public bt::fidl::testing::FakeAdapterTestFixture {
 public:
  LowEnergyPeripheralServerTestFakeAdapter() = default;
  ~LowEnergyPeripheralServerTestFakeAdapter() override = default;

  void SetUp() override {
    bt::fidl::testing::FakeAdapterTestFixture::SetUp();

    fake_gatt_ =
        std::make_unique<bt::gatt::testing::FakeLayer>(pw_dispatcher());

    // Create a LowEnergyPeripheralServer and bind it to a local client.
    fidl::InterfaceHandle<fble::Peripheral> handle;
    peripheral_server_ =
        std::make_unique<LowEnergyPeripheralServer>(adapter()->AsWeakPtr(),
                                                    fake_gatt_->GetWeakPtr(),
                                                    lease_provider_,
                                                    handle.NewRequest());
    peripheral_client_.Bind(std::move(handle));
  }

  void TearDown() override {
    RunLoopUntilIdle();

    peripheral_client_ = nullptr;
    peripheral_server_ = nullptr;
    bt::fidl::testing::FakeAdapterTestFixture::TearDown();
  }

  LowEnergyPeripheralServer* server() const { return peripheral_server_.get(); }

  void SetOnPeerConnectedCallback(
      fble::Peripheral::OnPeerConnectedCallback cb) {
    peripheral_client_.events().OnPeerConnected = std::move(cb);
  }

 private:
  pw::bluetooth_sapphire::testing::FakeLeaseProvider lease_provider_;
  std::unique_ptr<LowEnergyPeripheralServer> peripheral_server_;
  fble::PeripheralPtr peripheral_client_;
  std::unique_ptr<bt::gatt::testing::FakeLayer> fake_gatt_;
  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(
      LowEnergyPeripheralServerTestFakeAdapter);
};

class LowEnergyPrivilegedPeripheralServerTestFakeAdapter
    : public bt::fidl::testing::FakeAdapterTestFixture {
 public:
  LowEnergyPrivilegedPeripheralServerTestFakeAdapter() = default;
  ~LowEnergyPrivilegedPeripheralServerTestFakeAdapter() override = default;

  void SetUp() override {
    bt::fidl::testing::FakeAdapterTestFixture::SetUp();

    fake_gatt_ =
        std::make_unique<bt::gatt::testing::FakeLayer>(pw_dispatcher());

    // Create a LowEnergyPrivilegedPeripheralServer and bind it to a local
    // client.
    fidl::InterfaceHandle<fble::PrivilegedPeripheral> privileged_handle;
    privileged_peripheral_server_ =
        std::make_unique<LowEnergyPrivilegedPeripheralServer>(
            adapter()->AsWeakPtr(),
            fake_gatt_->GetWeakPtr(),
            lease_provider_,
            privileged_handle.NewRequest());
  }

  void TearDown() override {
    RunLoopUntilIdle();

    privileged_peripheral_server_ = nullptr;
    bt::fidl::testing::FakeAdapterTestFixture::TearDown();
  }

  LowEnergyPrivilegedPeripheralServer* privileged_server() const {
    return privileged_peripheral_server_.get();
  }

 private:
  pw::bluetooth_sapphire::testing::FakeLeaseProvider lease_provider_;
  std::unique_ptr<LowEnergyPrivilegedPeripheralServer>
      privileged_peripheral_server_;
  std::unique_ptr<bt::gatt::testing::FakeLayer> fake_gatt_;
  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(
      LowEnergyPrivilegedPeripheralServerTestFakeAdapter);
};

class LowEnergyPeripheralServerTest
    : public bthost::testing::AdapterTestFixture {
 public:
  LowEnergyPeripheralServerTest() = default;
  ~LowEnergyPeripheralServerTest() override = default;

  void SetUp() override {
    AdapterTestFixture::SetUp();

    fake_gatt_ =
        std::make_unique<bt::gatt::testing::FakeLayer>(pw_dispatcher());

    // Create a LowEnergyPeripheralServer and bind it to a local client.
    fidl::InterfaceHandle<fble::Peripheral> handle;
    server_ =
        std::make_unique<LowEnergyPeripheralServer>(adapter(),
                                                    fake_gatt_->GetWeakPtr(),
                                                    lease_provider_,
                                                    handle.NewRequest());
    peripheral_client_.Bind(std::move(handle));
  }

  void TearDown() override {
    RunLoopUntilIdle();

    peripheral_client_ = nullptr;
    server_ = nullptr;
    AdapterTestFixture::TearDown();
  }

  LowEnergyPeripheralServer* server() const { return server_.get(); }

  void SetOnPeerConnectedCallback(
      fble::Peripheral::OnPeerConnectedCallback cb) {
    peripheral_client_.events().OnPeerConnected = std::move(cb);
  }

 private:
  pw::bluetooth_sapphire::testing::FakeLeaseProvider lease_provider_;
  std::unique_ptr<LowEnergyPeripheralServer> server_;
  fble::PeripheralPtr peripheral_client_;
  std::unique_ptr<bt::gatt::testing::FakeLayer> fake_gatt_;
  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LowEnergyPeripheralServerTest);
};

class LowEnergyPrivilegedPeripheralServerTest
    : public bthost::testing::AdapterTestFixture {
 public:
  LowEnergyPrivilegedPeripheralServerTest() = default;
  ~LowEnergyPrivilegedPeripheralServerTest() override = default;

  void SetUp() override {
    AdapterTestFixture::SetUp();

    fake_gatt_ =
        std::make_unique<bt::gatt::testing::FakeLayer>(pw_dispatcher());

    // Create a LowEnergyPrivilegedPeripheralServer and bind it to a local
    // client.
    fidl::InterfaceHandle<fble::PrivilegedPeripheral> handle;
    server_ = std::make_unique<LowEnergyPrivilegedPeripheralServer>(
        adapter(),
        fake_gatt_->GetWeakPtr(),
        lease_provider_,
        handle.NewRequest());
    peripheral_client_.Bind(std::move(handle));
  }

  void TearDown() override {
    RunLoopUntilIdle();

    peripheral_client_ = nullptr;
    server_ = nullptr;
    AdapterTestFixture::TearDown();
  }

  LowEnergyPrivilegedPeripheralServer* server() const { return server_.get(); }

  void SetOnPeerConnectedCallback(
      fble::Peripheral::OnPeerConnectedCallback cb) {
    peripheral_client_.events().OnPeerConnected = std::move(cb);
  }

 private:
  pw::bluetooth_sapphire::testing::FakeLeaseProvider lease_provider_;
  std::unique_ptr<LowEnergyPrivilegedPeripheralServer> server_;
  fble::PrivilegedPeripheralPtr peripheral_client_;
  std::unique_ptr<bt::gatt::testing::FakeLayer> fake_gatt_;
  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(
      LowEnergyPrivilegedPeripheralServerTest);
};

class BoolParam : public LowEnergyPeripheralServerTest,
                  public ::testing::WithParamInterface<bool> {};

class FakeAdvertisedPeripheral : public ServerBase<fble::AdvertisedPeripheral> {
 public:
  struct Connection {
    fble::Peer peer;
    fble::ConnectionHandle connection;
    OnConnectedCallback callback;
  };

  FakeAdvertisedPeripheral(
      fidl::InterfaceRequest<fble::AdvertisedPeripheral> request)
      : ServerBase(this, std::move(request)) {}

  void Unbind() { binding()->Unbind(); }

  void OnConnected(fble::Peer peer,
                   fidl::InterfaceHandle<fble::Connection> connection,
                   OnConnectedCallback callback) override {
    connections_.push_back(
        {std::move(peer), std::move(connection), std::move(callback)});
  }

  std::optional<bt::PeerId> last_connected_peer() const {
    if (connections_.empty()) {
      return std::nullopt;
    }
    return bt::PeerId(connections_.back().peer.id().value);
  }

  std::vector<Connection>& connections() { return connections_; }

 private:
  std::vector<Connection> connections_;
};

// Tests that an unprivileged client's explicit request to advertise a random
// address type fails since privacy is not enabled.
TEST_F(LowEnergyPeripheralServerTest,
       AdvertiseRandomAddressWithoutPrivacyEnabledFails) {
  fble::AdvertisingParameters params;
  fble::AdvertisingData adv_data;
  params.set_data(std::move(adv_data));
  params.set_address_type(fuchsia::bluetooth::AddressType::RANDOM);

  fble::AdvertisedPeripheralHandle adv_peripheral_handle;
  FakeAdvertisedPeripheral adv_peripheral_server(
      adv_peripheral_handle.NewRequest());

  std::optional<fpromise::result<void, fble::PeripheralError>> result;
  server()->Advertise(std::move(params),
                      std::move(adv_peripheral_handle),
                      [&](auto cb_result) { result = std::move(cb_result); });
  RunLoopUntilIdle();

  EXPECT_TRUE(result->is_error());
  EXPECT_EQ(fble::PeripheralError::INVALID_PARAMETERS, result->error());
}

// Tests that a privileged client's explicit request to advertise a random
// address type fails since privacy is not enabled.
TEST_F(LowEnergyPrivilegedPeripheralServerTest,
       AdvertiseRandomAddressWithoutPrivacyEnabledFails) {
  fble::AdvertisingParameters params;
  fble::AdvertisingData adv_data;
  params.set_data(std::move(adv_data));
  params.set_address_type(fuchsia::bluetooth::AddressType::RANDOM);

  fble::AdvertisedPeripheralHandle adv_peripheral_handle;
  FakeAdvertisedPeripheral adv_peripheral_server(
      adv_peripheral_handle.NewRequest());

  std::optional<fpromise::result<void, fble::PeripheralError>> result;
  server()->Advertise(std::move(params),
                      std::move(adv_peripheral_handle),
                      [&](auto cb_result) { result = std::move(cb_result); });
  RunLoopUntilIdle();

  EXPECT_TRUE(result->is_error());
  EXPECT_EQ(fble::PeripheralError::INVALID_PARAMETERS, result->error());
}

// This test just starts advertising using the legacy interfaces, drops the
// AdvertisingHandle, then attempts to restart.
TEST_F(LowEnergyPeripheralServerTest, CanRestartAdvertisingAfterHandleDropped) {
  {
    fble::AdvertisingParameters params;
    FidlAdvHandle token;

    std::optional<fpromise::result<void, fble::PeripheralError>> result;
    server()->StartAdvertising(
        std::move(params), token.NewRequest(), [&](auto actual) {
          result = std::move(actual);
        });
    RunLoopUntilIdle();
    ASSERT_TRUE(result);
    EXPECT_TRUE(result->is_ok());
  }

  // Process the dropped handle
  RunLoopUntilIdle();

  {
    fble::AdvertisingParameters params;
    FidlAdvHandle token;

    std::optional<fpromise::result<void, fble::PeripheralError>> result;
    server()->StartAdvertising(
        std::move(params), token.NewRequest(), [&](auto actual) {
          result = std::move(actual);
        });
    RunLoopUntilIdle();
    ASSERT_TRUE(result);
    EXPECT_TRUE(result->is_ok());
  }
}

// Tests that aborting a StartAdvertising command sequence does not cause a
// crash in successive requests.
TEST_F(LowEnergyPeripheralServerTest,
       StartAdvertisingWhilePendingDoesNotCrash) {
  fble::AdvertisingParameters params1, params2, params3;
  FidlAdvHandle token1, token2, token3;

  std::optional<fpromise::result<void, fble::PeripheralError>> result1, result2,
      result3;
  server()->StartAdvertising(std::move(params1),
                             token1.NewRequest(),
                             [&](auto result) { result1 = std::move(result); });
  server()->StartAdvertising(std::move(params2),
                             token2.NewRequest(),
                             [&](auto result) { result2 = std::move(result); });
  server()->StartAdvertising(std::move(params3),
                             token3.NewRequest(),
                             [&](auto result) { result3 = std::move(result); });
  RunLoopUntilIdle();

  ASSERT_TRUE(result1);
  ASSERT_TRUE(result2);
  ASSERT_TRUE(result3);
  EXPECT_TRUE(result1->is_error());
  EXPECT_EQ(fble::PeripheralError::ABORTED, result1->error());
  EXPECT_TRUE(result2->is_error());
  EXPECT_EQ(fble::PeripheralError::ABORTED, result2->error());
  EXPECT_TRUE(result3->is_ok());
}

// Same as the test above but tests that an error status leaves the server in
// the expected state.
TEST_F(LowEnergyPeripheralServerTest,
       StartAdvertisingWhilePendingDoesNotCrashWithControllerError) {
  test_device()->SetDefaultResponseStatus(
      bt::hci_spec::kLESetAdvertisingEnable,
      pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED);
  fble::AdvertisingParameters params1, params2, params3, params4;
  FidlAdvHandle token1, token2, token3, token4;

  std::optional<fpromise::result<void, fble::PeripheralError>> result1, result2,
      result3, result4;
  server()->StartAdvertising(std::move(params1),
                             token1.NewRequest(),
                             [&](auto result) { result1 = std::move(result); });
  server()->StartAdvertising(std::move(params2),
                             token2.NewRequest(),
                             [&](auto result) { result2 = std::move(result); });
  server()->StartAdvertising(std::move(params3),
                             token3.NewRequest(),
                             [&](auto result) { result3 = std::move(result); });
  RunLoopUntilIdle();

  ASSERT_TRUE(result1);
  ASSERT_TRUE(result2);
  ASSERT_TRUE(result3);
  EXPECT_TRUE(result1->is_error());
  EXPECT_EQ(fble::PeripheralError::ABORTED, result1->error());
  EXPECT_TRUE(result2->is_error());
  EXPECT_EQ(fble::PeripheralError::ABORTED, result2->error());
  EXPECT_TRUE(result3->is_error());
  EXPECT_EQ(fble::PeripheralError::FAILED, result3->error());

  // The next request should succeed as normal.
  test_device()->ClearDefaultResponseStatus(
      bt::hci_spec::kLESetAdvertisingEnable);
  server()->StartAdvertising(std::move(params4),
                             token4.NewRequest(),
                             [&](auto result) { result4 = std::move(result); });
  RunLoopUntilIdle();

  ASSERT_TRUE(result4);
  EXPECT_TRUE(result4->is_ok());
}

TEST_F(LowEnergyPeripheralServerTest,
       AdvertiseWhilePendingDoesNotCrashWithControllerError) {
  test_device()->SetDefaultResponseStatus(
      bt::hci_spec::kLESetAdvertisingEnable,
      pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED);
  fble::AdvertisingParameters params1, params2, params3, params4;

  fble::AdvertisedPeripheralHandle adv_peripheral_handle_1;
  FakeAdvertisedPeripheral adv_peripheral_server_1(
      adv_peripheral_handle_1.NewRequest());
  fble::AdvertisedPeripheralHandle adv_peripheral_handle_2;
  FakeAdvertisedPeripheral adv_peripheral_server_2(
      adv_peripheral_handle_2.NewRequest());
  fble::AdvertisedPeripheralHandle adv_peripheral_handle_3;
  FakeAdvertisedPeripheral adv_peripheral_server_3(
      adv_peripheral_handle_3.NewRequest());

  std::optional<fpromise::result<void, fble::PeripheralError>> result1, result2,
      result3, result4;
  server()->Advertise(std::move(params1),
                      std::move(adv_peripheral_handle_1),
                      [&](auto result) { result1 = std::move(result); });
  server()->Advertise(std::move(params2),
                      std::move(adv_peripheral_handle_2),
                      [&](auto result) { result2 = std::move(result); });
  server()->Advertise(std::move(params3),
                      std::move(adv_peripheral_handle_3),
                      [&](auto result) { result3 = std::move(result); });
  RunLoopUntilIdle();
  ASSERT_TRUE(result1);
  ASSERT_TRUE(result2);
  ASSERT_TRUE(result3);
  EXPECT_TRUE(result1->is_error());
  EXPECT_EQ(fble::PeripheralError::FAILED, result1->error());
  EXPECT_TRUE(result2->is_error());
  EXPECT_EQ(fble::PeripheralError::NOT_SUPPORTED, result2->error());
  EXPECT_TRUE(result3->is_error());
  EXPECT_EQ(fble::PeripheralError::NOT_SUPPORTED, result3->error());

  // The next request should succeed as normal.
  test_device()->ClearDefaultResponseStatus(
      bt::hci_spec::kLESetAdvertisingEnable);

  fble::AdvertisedPeripheralHandle adv_peripheral_handle_4;
  FakeAdvertisedPeripheral adv_peripheral_server_4(
      adv_peripheral_handle_4.NewRequest());
  server()->Advertise(std::move(params4),
                      std::move(adv_peripheral_handle_4),
                      [&](auto result) { result4 = std::move(result); });
  RunLoopUntilIdle();
  EXPECT_FALSE(result4);
  adv_peripheral_server_4.Unbind();
  RunLoopUntilIdle();
  EXPECT_TRUE(result4);
}

TEST_F(LowEnergyPeripheralServerTest,
       StartAdvertisingNoConnectionRelatedParamsNoConnection) {
  fble::Peer peer;
  // `conn` is stored so the bondable mode of the connection resulting from
  // `OnPeerConnected` can be checked. The connection would otherwise be dropped
  // immediately after `ConnectLowEnergy`.
  fidl::InterfaceHandle<fble::Connection> conn;
  auto peer_connected_cb = [&](auto cb_peer, auto cb_conn) {
    peer = std::move(cb_peer);
    conn = std::move(cb_conn);
  };
  SetOnPeerConnectedCallback(peer_connected_cb);

  fble::AdvertisingParameters params;

  FidlAdvHandle token;

  std::optional<fpromise::result<void, fble::PeripheralError>> result;
  server()->StartAdvertising(
      std::move(params), token.NewRequest(), [&](auto cb_result) {
        result = std::move(cb_result);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(result.has_value());
  ASSERT_FALSE(result->is_error());

  test_device()->AddPeer(
      std::make_unique<FakePeer>(kTestAddr, pw_dispatcher()));
  test_device()->ConnectLowEnergy(kTestAddr);
  RunLoopUntilIdle();

  ASSERT_FALSE(peer.has_id());
  ASSERT_FALSE(conn.is_valid());
}

TEST_F(LowEnergyPeripheralServerTest,
       AdvertiseNoConnectionRelatedParamsNoConnection) {
  fble::AdvertisedPeripheralHandle adv_peripheral_handle;
  FakeAdvertisedPeripheral adv_peripheral_server(
      adv_peripheral_handle.NewRequest());
  fble::AdvertisingParameters params;
  std::optional<fpromise::result<void, fble::PeripheralError>> result;
  server()->Advertise(std::move(params),
                      std::move(adv_peripheral_handle),
                      [&](auto cb_result) { result = std::move(cb_result); });
  RunLoopUntilIdle();
  EXPECT_FALSE(result.has_value());

  test_device()->AddPeer(
      std::make_unique<FakePeer>(kTestAddr, pw_dispatcher()));
  test_device()->ConnectLowEnergy(kTestAddr);
  RunLoopUntilIdle();
  EXPECT_FALSE(adv_peripheral_server.last_connected_peer());
  adv_peripheral_server.Unbind();
  RunLoopUntilIdle();
  EXPECT_TRUE(result);
}

TEST_F(LowEnergyPeripheralServerTest,
       StartAdvertisingConnectableParameterTrueConnectsBondable) {
  fble::Peer peer;
  // `conn` is stored so the bondable mode of the connection resulting from
  // `OnPeerConnected` can be checked. The connection would otherwise be dropped
  // immediately after `ConnectLowEnergy`.
  fidl::InterfaceHandle<fble::Connection> conn;
  auto peer_connected_cb = [&](auto cb_peer, auto cb_conn) {
    peer = std::move(cb_peer);
    conn = std::move(cb_conn);
  };
  SetOnPeerConnectedCallback(peer_connected_cb);

  fble::AdvertisingParameters params;
  params.set_connectable(true);

  FidlAdvHandle token;

  std::optional<fpromise::result<void, fble::PeripheralError>> result;
  server()->StartAdvertising(
      std::move(params), token.NewRequest(), [&](auto cb_result) {
        result = std::move(cb_result);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(result.has_value());
  ASSERT_FALSE(result->is_error());

  test_device()->AddPeer(
      std::make_unique<FakePeer>(kTestAddr, pw_dispatcher()));
  test_device()->ConnectLowEnergy(kTestAddr);
  RunLoopUntilIdle();

  ASSERT_TRUE(peer.has_id());
  ASSERT_TRUE(conn.is_valid());

  auto connected_id = bt::PeerId(peer.id().value);
  const bt::gap::LowEnergyConnectionHandle* conn_handle =
      server()->FindConnectionForTesting(connected_id);

  ASSERT_TRUE(conn_handle);
  ASSERT_EQ(conn_handle->bondable_mode(), bt::sm::BondableMode::Bondable);
}

TEST_F(LowEnergyPeripheralServerTest,
       StartAdvertisingEmptyConnectionOptionsConnectsBondable) {
  fble::Peer peer;
  // `conn` is stored so the bondable mode of the connection resulting from
  // `OnPeerConnected` can be checked. The connection would otherwise be dropped
  // immediately after `ConnectLowEnergy`.
  fidl::InterfaceHandle<fble::Connection> conn;
  auto peer_connected_cb = [&](auto cb_peer, auto cb_conn) {
    peer = std::move(cb_peer);
    conn = std::move(cb_conn);
  };
  SetOnPeerConnectedCallback(peer_connected_cb);

  fble::AdvertisingParameters params;
  fble::ConnectionOptions conn_opts;
  params.set_connection_options(std::move(conn_opts));

  FidlAdvHandle token;

  std::optional<fpromise::result<void, fble::PeripheralError>> result;
  server()->StartAdvertising(
      std::move(params), token.NewRequest(), [&](auto cb_result) {
        result = std::move(cb_result);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(result.has_value());
  ASSERT_FALSE(result->is_error());

  test_device()->AddPeer(
      std::make_unique<FakePeer>(kTestAddr, pw_dispatcher()));
  test_device()->ConnectLowEnergy(kTestAddr);
  RunLoopUntilIdle();

  ASSERT_TRUE(peer.has_id());
  ASSERT_TRUE(conn.is_valid());

  auto connected_id = bt::PeerId(peer.id().value);
  const bt::gap::LowEnergyConnectionHandle* conn_handle =
      server()->FindConnectionForTesting(connected_id);

  ASSERT_TRUE(conn_handle);
  ASSERT_EQ(conn_handle->bondable_mode(), bt::sm::BondableMode::Bondable);
}

TEST_F(LowEnergyPeripheralServerTest,
       AdvertiseEmptyConnectionOptionsConnectsBondable) {
  fble::AdvertisedPeripheralHandle adv_peripheral_handle;
  FakeAdvertisedPeripheral adv_peripheral_server(
      adv_peripheral_handle.NewRequest());

  fble::AdvertisingParameters params;
  fble::ConnectionOptions conn_opts;
  params.set_connection_options(std::move(conn_opts));

  std::optional<fpromise::result<void, fble::PeripheralError>> result;
  server()->Advertise(std::move(params),
                      std::move(adv_peripheral_handle),
                      [&](auto cb_result) { result = std::move(cb_result); });
  RunLoopUntilIdle();
  EXPECT_FALSE(result.has_value());

  test_device()->AddPeer(
      std::make_unique<FakePeer>(kTestAddr, pw_dispatcher()));
  test_device()->ConnectLowEnergy(kTestAddr);
  RunLoopUntilIdle();
  auto connected_id = adv_peripheral_server.last_connected_peer();
  ASSERT_TRUE(connected_id);

  const bt::gap::LowEnergyConnectionHandle* conn_handle =
      server()->FindConnectionForTesting(*connected_id);
  ASSERT_TRUE(conn_handle);
  ASSERT_EQ(conn_handle->bondable_mode(), bt::sm::BondableMode::Bondable);

  adv_peripheral_server.Unbind();
  RunLoopUntilIdle();
  EXPECT_TRUE(result.has_value());
}

TEST_P(BoolParam, AdvertiseBondableOrNonBondableConnectsBondableOrNonBondable) {
  const bool bondable = GetParam();

  fble::AdvertisedPeripheralHandle adv_peripheral_handle;
  FakeAdvertisedPeripheral adv_peripheral_server(
      adv_peripheral_handle.NewRequest());

  fble::AdvertisingParameters params;
  fble::ConnectionOptions conn_opts;
  conn_opts.set_bondable_mode(bondable);
  params.set_connection_options(std::move(conn_opts));

  std::optional<fpromise::result<void, fble::PeripheralError>> result;
  server()->Advertise(std::move(params),
                      std::move(adv_peripheral_handle),
                      [&](auto cb_result) { result = std::move(cb_result); });
  RunLoopUntilIdle();
  EXPECT_FALSE(result.has_value());

  test_device()->AddPeer(
      std::make_unique<FakePeer>(kTestAddr, pw_dispatcher()));
  test_device()->ConnectLowEnergy(kTestAddr);
  RunLoopUntilIdle();
  auto connected_id = adv_peripheral_server.last_connected_peer();
  ASSERT_TRUE(connected_id);

  const bt::gap::LowEnergyConnectionHandle* conn_handle =
      server()->FindConnectionForTesting(*connected_id);
  ASSERT_TRUE(conn_handle);
  EXPECT_EQ(conn_handle->bondable_mode(),
            bondable ? bt::sm::BondableMode::Bondable
                     : bt::sm::BondableMode::NonBondable);

  adv_peripheral_server.Unbind();
  RunLoopUntilIdle();
}

TEST_P(BoolParam,
       StartAdvertisingBondableOrNonBondableConnectsBondableOrNonBondable) {
  const bool bondable = GetParam();

  fble::Peer peer;
  // `conn` is stored so the bondable mode of the connection resulting from
  // `OnPeerConnected` can be checked. The connection would otherwise be dropped
  // immediately after `ConnectLowEnergy`.
  fidl::InterfaceHandle<fble::Connection> conn;
  auto peer_connected_cb = [&](auto cb_peer, auto cb_conn) {
    peer = std::move(cb_peer);
    conn = std::move(cb_conn);
  };
  SetOnPeerConnectedCallback(peer_connected_cb);

  fble::AdvertisingParameters params;
  fble::ConnectionOptions conn_opts;
  conn_opts.set_bondable_mode(bondable);
  params.set_connection_options(std::move(conn_opts));

  FidlAdvHandle token;

  std::optional<fpromise::result<void, fble::PeripheralError>> result;
  server()->StartAdvertising(
      std::move(params), token.NewRequest(), [&](auto cb_result) {
        result = std::move(cb_result);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(result);
  ASSERT_FALSE(result->is_error());

  test_device()->AddPeer(
      std::make_unique<FakePeer>(kTestAddr, pw_dispatcher()));
  test_device()->ConnectLowEnergy(kTestAddr);
  RunLoopUntilIdle();

  ASSERT_TRUE(peer.has_id());
  ASSERT_TRUE(conn.is_valid());

  auto connected_id = bt::PeerId(peer.id().value);
  const bt::gap::LowEnergyConnectionHandle* conn_handle =
      server()->FindConnectionForTesting(connected_id);

  ASSERT_TRUE(conn_handle);
  EXPECT_EQ(conn_handle->bondable_mode(),
            bondable ? bt::sm::BondableMode::Bondable
                     : bt::sm::BondableMode::NonBondable);
}

TEST_F(LowEnergyPeripheralServerTest,
       RestartStartAdvertisingDuringInboundConnKeepsNewAdvAlive) {
  fble::Peer peer;
  // `conn` is stored so that the connection is not dropped immediately after
  // connection.
  fidl::InterfaceHandle<fble::Connection> conn;
  auto peer_connected_cb = [&](auto cb_peer, auto cb_conn) {
    peer = std::move(cb_peer);
    conn = std::move(cb_conn);
  };
  SetOnPeerConnectedCallback(peer_connected_cb);

  FidlAdvHandle first_token, second_token;

  fble::AdvertisingParameters params;
  params.set_connectable(true);
  std::optional<fpromise::result<void, fble::PeripheralError>> result;
  server()->StartAdvertising(
      std::move(params), first_token.NewRequest(), [&](auto cb_result) {
        result = std::move(cb_result);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->is_ok());

  fit::closure complete_interrogation;
  // Hang interrogation so we can control when the inbound connection procedure
  // completes.
  test_device()->pause_responses_for_opcode(
      bt::hci_spec::kReadRemoteVersionInfo, [&](fit::closure trigger) {
        complete_interrogation = std::move(trigger);
      });

  test_device()->AddPeer(
      std::make_unique<FakePeer>(kTestAddr, pw_dispatcher()));
  test_device()->ConnectLowEnergy(kTestAddr);
  RunLoopUntilIdle();

  EXPECT_FALSE(peer.has_id());
  EXPECT_FALSE(conn.is_valid());
  // test_device()->ConnectLowEnergy caused interrogation as part of the inbound
  // GAP connection process, so this closure should be filled in.
  ASSERT_TRUE(complete_interrogation);

  // Hang the SetAdvertisingParameters HCI command so we can invoke the
  // advertising status callback after connection completion.
  fit::closure complete_start_advertising;
  test_device()->pause_responses_for_opcode(
      bt::hci_spec::kLESetAdvertisingParameters, [&](fit::closure trigger) {
        complete_start_advertising = std::move(trigger);
      });

  // Restart advertising during inbound connection, simulating the race seen in
  // https://fxbug.dev/42152329.
  result = std::nullopt;
  server()->StartAdvertising(
      fble::AdvertisingParameters{},
      second_token.NewRequest(),
      [&](auto cb_result) { result = std::move(cb_result); });
  RunLoopUntilIdle();
  ASSERT_TRUE(complete_start_advertising);
  // Advertising shouldn't complete until we trigger the above closure
  EXPECT_FALSE(result.has_value());
  // The first AdvertisingHandle should be closed, as we have started a second
  // advertisement.
  EXPECT_TRUE(IsChannelPeerClosed(first_token.channel()));

  // Allow interrogation to complete, enabling the connection process to
  // proceed.
  complete_interrogation();
  RunLoopUntilIdle();
  // Connection should have been dropped after completing because first
  // advertisement was canceled.
  EXPECT_FALSE(peer.has_id());
  EXPECT_FALSE(conn.is_valid());

  // Allow second StartAdvertising to complete.
  complete_start_advertising();
  RunLoopUntilIdle();
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->is_ok());
  // The second advertising handle should still be active.
  EXPECT_FALSE(IsChannelPeerClosed(second_token.channel()));
}

// Ensures that a connection to a canceled advertisement received after the
// advertisement is canceled doesn't end or get sent to a new
// AdvertisedPeripheral.
TEST_F(LowEnergyPeripheralServerTest,
       RestartAdvertiseDuringInboundConnKeepsNewAdvAlive) {
  fble::AdvertisedPeripheralHandle adv_peripheral_handle_0;
  FakeAdvertisedPeripheral adv_peripheral_server_0(
      adv_peripheral_handle_0.NewRequest());

  fble::AdvertisingParameters params;
  params.set_connectable(true);
  std::optional<fpromise::result<void, fble::PeripheralError>> result;
  server()->Advertise(std::move(params),
                      std::move(adv_peripheral_handle_0),
                      [&](auto cb_result) { result = std::move(cb_result); });
  RunLoopUntilIdle();
  EXPECT_FALSE(result.has_value());

  fit::closure complete_interrogation;
  // Hang interrogation so we can control when the inbound connection procedure
  // completes.
  test_device()->pause_responses_for_opcode(
      bt::hci_spec::kReadRemoteVersionInfo, [&](fit::closure trigger) {
        complete_interrogation = std::move(trigger);
      });

  test_device()->AddPeer(
      std::make_unique<FakePeer>(kTestAddr, pw_dispatcher()));
  test_device()->ConnectLowEnergy(kTestAddr);
  RunLoopUntilIdle();
  EXPECT_FALSE(adv_peripheral_server_0.last_connected_peer());
  // test_device()->ConnectLowEnergy caused interrogation as part of the inbound
  // GAP connection process, so this closure should be filled in.
  ASSERT_TRUE(complete_interrogation);

  // Cancel the first advertisement.
  adv_peripheral_server_0.Unbind();
  RunLoopUntilIdle();
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->is_ok());

  // Hang the SetAdvertisingParameters HCI command so we can invoke the
  // advertising status callback of the second advertising request after
  // connection completion.
  fit::closure complete_start_advertising;
  test_device()->pause_responses_for_opcode(
      bt::hci_spec::kLESetAdvertisingParameters, [&](fit::closure trigger) {
        complete_start_advertising = std::move(trigger);
      });

  // Restart advertising during inbound connection, simulating the race seen in
  // https://fxbug.dev/42152329.
  fble::AdvertisedPeripheralHandle adv_peripheral_handle_1;
  FakeAdvertisedPeripheral adv_peripheral_server_1(
      adv_peripheral_handle_1.NewRequest());
  bool server_1_closed = false;
  adv_peripheral_server_1.set_error_handler(
      [&](auto) { server_1_closed = true; });
  result = std::nullopt;
  server()->Advertise(fble::AdvertisingParameters{},
                      std::move(adv_peripheral_handle_1),
                      [&](auto cb_result) { result = std::move(cb_result); });
  RunLoopUntilIdle();
  ASSERT_TRUE(complete_start_advertising);
  EXPECT_FALSE(result.has_value());

  // Allow interrogation to complete, enabling the connection process to
  // proceed.
  complete_interrogation();
  RunLoopUntilIdle();
  // The connection should have been dropped.
  EXPECT_FALSE(adv_peripheral_server_1.last_connected_peer());
  EXPECT_FALSE(adv_peripheral_server_0.last_connected_peer());

  // Allow second Advertise to complete.
  complete_start_advertising();
  RunLoopUntilIdle();
  EXPECT_FALSE(result.has_value());
  EXPECT_FALSE(server_1_closed);
  EXPECT_FALSE(adv_peripheral_server_1.last_connected_peer());

  adv_peripheral_server_1.Unbind();
  RunLoopUntilIdle();
  EXPECT_TRUE(result.has_value());
}

TEST_F(LowEnergyPeripheralServerTestFakeAdapter,
       StartAdvertisingWithIncludeTxPowerSetToTrue) {
  fble::AdvertisingParameters params;
  fble::AdvertisingData adv_data;
  adv_data.set_include_tx_power_level(true);
  params.set_data(std::move(adv_data));

  FidlAdvHandle token;

  server()->StartAdvertising(
      std::move(params), token.NewRequest(), [&](auto) {});
  RunLoopUntilIdle();
  ASSERT_EQ(adapter()->fake_le()->registered_advertisements().size(), 1u);
  EXPECT_TRUE(adapter()
                  ->fake_le()
                  ->registered_advertisements()
                  .begin()
                  ->second.include_tx_power_level);
}

TEST_F(LowEnergyPeripheralServerTest, StartAdvertisingDataTooLong) {
  // Legacy advertising PDU limit is 31 bytes (kMaxLEAdvertisingDataLength).
  // A name of kMaxLEAdvertisingDataLength requires 33 bytes in the AD structure
  // (1 byte length + 1 byte type + 31 bytes payload), exceeding the allowable
  // limit.
  fble::AdvertisingParameters params;
  fble::AdvertisingData adv_data;
  adv_data.set_name(
      std::string(bt::hci_spec::kMaxLEAdvertisingDataLength, 'a'));
  params.set_data(std::move(adv_data));

  FidlAdvHandle token;

  std::optional<fpromise::result<void, fble::PeripheralError>> result;
  server()->StartAdvertising(
      std::move(params), token.NewRequest(), [&](auto cb_result) {
        result = std::move(cb_result);
      });
  RunLoopUntilIdle();

  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->is_error());
  EXPECT_EQ(result->error(), fble::PeripheralError::ADVERTISING_DATA_TOO_LONG);
  EXPECT_TRUE(IsChannelPeerClosed(token.channel()));
}

TEST_F(LowEnergyPeripheralServerTest, StartAdvertisingScanResponseDataTooLong) {
  // Legacy scan response PDU limit is 31 bytes (kMaxLEAdvertisingDataLength).
  // A name of kMaxLEAdvertisingDataLength requires 33 bytes in the AD structure
  // (1 byte length + 1 byte type + 31 bytes payload), exceeding the allowable
  // limit.
  fble::AdvertisingParameters params;
  fble::AdvertisingData scan_rsp;
  scan_rsp.set_name(
      std::string(bt::hci_spec::kMaxLEAdvertisingDataLength, 'a'));
  params.set_scan_response(std::move(scan_rsp));

  FidlAdvHandle token;

  std::optional<fpromise::result<void, fble::PeripheralError>> result;
  server()->StartAdvertising(
      std::move(params), token.NewRequest(), [&](auto cb_result) {
        result = std::move(cb_result);
      });
  RunLoopUntilIdle();

  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->is_error());
  EXPECT_EQ(result->error(),
            fble::PeripheralError::SCAN_RESPONSE_DATA_TOO_LONG);
  EXPECT_TRUE(IsChannelPeerClosed(token.channel()));
}

TEST_F(LowEnergyPeripheralServerTestFakeAdapter,
       AdvertiseWithIncludeTxPowerSetToTrue) {
  fble::AdvertisingParameters params;
  fble::AdvertisingData adv_data;
  adv_data.set_include_tx_power_level(true);
  params.set_data(std::move(adv_data));

  fble::AdvertisedPeripheralHandle adv_peripheral_handle;
  FakeAdvertisedPeripheral adv_peripheral_server(
      adv_peripheral_handle.NewRequest());

  std::optional<fpromise::result<void, fble::PeripheralError>> result;
  server()->Advertise(std::move(params),
                      std::move(adv_peripheral_handle),
                      [&](auto cb_result) { result = std::move(cb_result); });
  RunLoopUntilIdle();
  ASSERT_EQ(adapter()->fake_le()->registered_advertisements().size(), 1u);
  EXPECT_TRUE(adapter()
                  ->fake_le()
                  ->registered_advertisements()
                  .begin()
                  ->second.include_tx_power_level);

  adv_peripheral_server.Unbind();
  RunLoopUntilIdle();
}

TEST_F(LowEnergyPeripheralServerTestFakeAdapter, AdvertiseInvalidAdvData) {
  fble::AdvertisingData adv_data;
  adv_data.set_name(std::string(bt::kMaxNameLength + 1, '*'));
  fble::AdvertisingParameters params;
  params.set_data(std::move(adv_data));

  fidl::InterfaceHandle<fble::AdvertisedPeripheral>
      advertised_peripheral_client;
  fidl::InterfaceRequest<fble::AdvertisedPeripheral>
      advertised_peripheral_server = advertised_peripheral_client.NewRequest();

  std::optional<fpromise::result<void, fble::PeripheralError>> adv_result;
  server()->Advertise(std::move(params),
                      std::move(advertised_peripheral_client),
                      [&](auto result) { adv_result = std::move(result); });
  RunLoopUntilIdle();
  EXPECT_EQ(adapter()->fake_le()->registered_advertisements().size(), 0u);
  ASSERT_TRUE(adv_result);
  EXPECT_TRUE(adv_result.value().is_error());
  EXPECT_EQ(adv_result->error(), fble::PeripheralError::INVALID_PARAMETERS);
}

TEST_F(LowEnergyPeripheralServerTestFakeAdapter,
       AdvertiseInvalidScanResponseData) {
  fble::AdvertisingData adv_data;
  adv_data.set_name(std::string(bt::kMaxNameLength + 1, '*'));
  fble::AdvertisingParameters params;
  params.set_scan_response(std::move(adv_data));

  fidl::InterfaceHandle<fble::AdvertisedPeripheral>
      advertised_peripheral_client;
  fidl::InterfaceRequest<fble::AdvertisedPeripheral>
      advertised_peripheral_server = advertised_peripheral_client.NewRequest();

  std::optional<fpromise::result<void, fble::PeripheralError>> adv_result;
  server()->Advertise(std::move(params),
                      std::move(advertised_peripheral_client),
                      [&](auto result) { adv_result = std::move(result); });
  RunLoopUntilIdle();
  EXPECT_EQ(adapter()->fake_le()->registered_advertisements().size(), 0u);
  ASSERT_TRUE(adv_result);
  EXPECT_TRUE(adv_result.value().is_error());
  EXPECT_EQ(adv_result->error(), fble::PeripheralError::INVALID_PARAMETERS);
}

// Tests that a privileged client's advertising request defaults to a random
// address type since privacy is enabled.
TEST_F(LowEnergyPrivilegedPeripheralServerTestFakeAdapter,
       AdvertiseRandomAddressWithPrivacyEnabled) {
  fble::AdvertisingParameters params;
  fble::AdvertisingData adv_data;
  params.set_data(std::move(adv_data));

  adapter()->fake_le()->EnablePrivacy(true);

  fble::AdvertisedPeripheralHandle adv_peripheral_handle;
  FakeAdvertisedPeripheral adv_peripheral_server(
      adv_peripheral_handle.NewRequest());

  std::optional<fpromise::result<void, fble::PeripheralError>> result;
  privileged_server()->Advertise(
      std::move(params), std::move(adv_peripheral_handle), [&](auto cb_result) {
        result = std::move(cb_result);
      });
  RunLoopUntilIdle();
  ASSERT_EQ(adapter()->fake_le()->registered_advertisements().size(), 1u);
  ASSERT_EQ(adapter()
                ->fake_le()
                ->registered_advertisements()
                .begin()
                ->second.addr_type,
            bt::DeviceAddress::Type::kLERandom);

  adv_peripheral_server.Unbind();
  RunLoopUntilIdle();
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->is_ok());
}

// Tests that a privileged client's advertising request defaults to a public
// address type since privacy is not enabled.
TEST_F(LowEnergyPrivilegedPeripheralServerTestFakeAdapter,
       AdvertisePublicAddressWithoutPrivacyEnabled) {
  fble::AdvertisingParameters params;
  fble::AdvertisingData adv_data;
  params.set_data(std::move(adv_data));

  fble::AdvertisedPeripheralHandle adv_peripheral_handle;
  FakeAdvertisedPeripheral adv_peripheral_server(
      adv_peripheral_handle.NewRequest());

  std::optional<fpromise::result<void, fble::PeripheralError>> result;
  privileged_server()->Advertise(
      std::move(params), std::move(adv_peripheral_handle), [&](auto cb_result) {
        result = std::move(cb_result);
      });
  RunLoopUntilIdle();
  ASSERT_EQ(adapter()->fake_le()->registered_advertisements().size(), 1u);
  ASSERT_EQ(adapter()
                ->fake_le()
                ->registered_advertisements()
                .begin()
                ->second.addr_type,
            bt::DeviceAddress::Type::kLEPublic);

  adv_peripheral_server.Unbind();
  RunLoopUntilIdle();
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->is_ok());
}

// Tests that a privileged client's explicit request to advertise a public
// address type does so, even when privacy is enabled.
TEST_F(LowEnergyPrivilegedPeripheralServerTestFakeAdapter,
       AdvertisePublicAddressWithPrivacyEnabled) {
  fble::AdvertisingParameters params;
  fble::AdvertisingData adv_data;
  params.set_data(std::move(adv_data));
  params.set_address_type(fuchsia::bluetooth::AddressType::PUBLIC);

  adapter()->fake_le()->EnablePrivacy(true);

  fble::AdvertisedPeripheralHandle adv_peripheral_handle;
  FakeAdvertisedPeripheral adv_peripheral_server(
      adv_peripheral_handle.NewRequest());

  std::optional<fpromise::result<void, fble::PeripheralError>> result;
  privileged_server()->Advertise(
      std::move(params), std::move(adv_peripheral_handle), [&](auto cb_result) {
        result = std::move(cb_result);
      });
  RunLoopUntilIdle();
  ASSERT_EQ(adapter()->fake_le()->registered_advertisements().size(), 1u);
  ASSERT_EQ(adapter()
                ->fake_le()
                ->registered_advertisements()
                .begin()
                ->second.addr_type,
            bt::DeviceAddress::Type::kLEPublic);

  adv_peripheral_server.Unbind();
  RunLoopUntilIdle();
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->is_ok());
}

// Tests that a privileged client's explicit request to advertise a random
// address type fails since privacy is not enabled.
TEST_F(LowEnergyPrivilegedPeripheralServerTestFakeAdapter,
       AdvertiseRandomAddressWithoutPrivacyEnabledFails) {
  fble::AdvertisingParameters params;
  fble::AdvertisingData adv_data;
  params.set_data(std::move(adv_data));
  params.set_address_type(fuchsia::bluetooth::AddressType::RANDOM);

  fble::AdvertisedPeripheralHandle adv_peripheral_handle;
  FakeAdvertisedPeripheral adv_peripheral_server(
      adv_peripheral_handle.NewRequest());

  std::optional<fpromise::result<void, fble::PeripheralError>> result;
  privileged_server()->Advertise(
      std::move(params), std::move(adv_peripheral_handle), [&](auto cb_result) {
        result = std::move(cb_result);
      });
  RunLoopUntilIdle();
  ASSERT_EQ(adapter()->fake_le()->registered_advertisements().size(), 0u);

  adv_peripheral_server.Unbind();
  RunLoopUntilIdle();
  ASSERT_TRUE(result.value().is_error());
  EXPECT_EQ(result->error(), fble::PeripheralError::INVALID_PARAMETERS);
}

// Tests that an unprivileged client's advertising request defaults to a random
// address type since privacy is enabled.
TEST_F(LowEnergyPeripheralServerTestFakeAdapter,
       AdvertiseRandomAddressWithPrivacyEnabled) {
  fble::AdvertisingParameters params;
  fble::AdvertisingData adv_data;
  params.set_data(std::move(adv_data));

  adapter()->fake_le()->EnablePrivacy(true);

  fble::AdvertisedPeripheralHandle adv_peripheral_handle;
  FakeAdvertisedPeripheral adv_peripheral_server(
      adv_peripheral_handle.NewRequest());

  std::optional<fpromise::result<void, fble::PeripheralError>> result;
  server()->Advertise(std::move(params),
                      std::move(adv_peripheral_handle),
                      [&](auto cb_result) { result = std::move(cb_result); });
  RunLoopUntilIdle();
  ASSERT_EQ(adapter()->fake_le()->registered_advertisements().size(), 1u);
  ASSERT_EQ(adapter()
                ->fake_le()
                ->registered_advertisements()
                .begin()
                ->second.addr_type,
            bt::DeviceAddress::Type::kLERandom);

  adv_peripheral_server.Unbind();
  RunLoopUntilIdle();
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->is_ok());
}

// Tests that an unprivileged client's advertising request defaults to a public
// address type since privacy is not enabled.
TEST_F(LowEnergyPeripheralServerTestFakeAdapter,
       AdvertisePublicAddressWithoutPrivacyEnabled) {
  fble::AdvertisingParameters params;
  fble::AdvertisingData adv_data;
  params.set_data(std::move(adv_data));

  fble::AdvertisedPeripheralHandle adv_peripheral_handle;
  FakeAdvertisedPeripheral adv_peripheral_server(
      adv_peripheral_handle.NewRequest());

  std::optional<fpromise::result<void, fble::PeripheralError>> result;
  server()->Advertise(std::move(params),
                      std::move(adv_peripheral_handle),
                      [&](auto cb_result) { result = std::move(cb_result); });
  RunLoopUntilIdle();
  ASSERT_EQ(adapter()->fake_le()->registered_advertisements().size(), 1u);
  ASSERT_EQ(adapter()
                ->fake_le()
                ->registered_advertisements()
                .begin()
                ->second.addr_type,
            bt::DeviceAddress::Type::kLEPublic);

  adv_peripheral_server.Unbind();
  RunLoopUntilIdle();
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->is_ok());
}

// Tests that an unprivileged client's explicit request to advertise a public
// address type fails.
TEST_F(LowEnergyPeripheralServerTestFakeAdapter,
       AdvertisePublicAddressWithPrivacyEnabledFails) {
  fble::AdvertisingParameters params;
  fble::AdvertisingData adv_data;
  params.set_data(std::move(adv_data));
  params.set_address_type(fuchsia::bluetooth::AddressType::PUBLIC);

  adapter()->fake_le()->EnablePrivacy(true);

  fble::AdvertisedPeripheralHandle adv_peripheral_handle;
  FakeAdvertisedPeripheral adv_peripheral_server(
      adv_peripheral_handle.NewRequest());

  std::optional<fpromise::result<void, fble::PeripheralError>> result;
  server()->Advertise(std::move(params),
                      std::move(adv_peripheral_handle),
                      [&](auto cb_result) { result = std::move(cb_result); });
  RunLoopUntilIdle();
  ASSERT_EQ(adapter()->fake_le()->registered_advertisements().size(), 0u);

  adv_peripheral_server.Unbind();
  RunLoopUntilIdle();
  ASSERT_TRUE(result.value().is_error());
  EXPECT_EQ(result->error(), fble::PeripheralError::INVALID_PARAMETERS);
}

// Tests that an unprivileged client's explicit request to advertise a random
// address type fails since privacy is not enabled.
TEST_F(LowEnergyPeripheralServerTestFakeAdapter,
       AdvertiseRandomAddressWithoutPrivacyEnabledFails) {
  fble::AdvertisingParameters params;
  fble::AdvertisingData adv_data;
  params.set_data(std::move(adv_data));
  params.set_address_type(fuchsia::bluetooth::AddressType::RANDOM);

  fble::AdvertisedPeripheralHandle adv_peripheral_handle;
  FakeAdvertisedPeripheral adv_peripheral_server(
      adv_peripheral_handle.NewRequest());

  std::optional<fpromise::result<void, fble::PeripheralError>> result;
  server()->Advertise(std::move(params),
                      std::move(adv_peripheral_handle),
                      [&](auto cb_result) { result = std::move(cb_result); });
  RunLoopUntilIdle();
  ASSERT_EQ(adapter()->fake_le()->registered_advertisements().size(), 0u);

  adv_peripheral_server.Unbind();
  RunLoopUntilIdle();
  ASSERT_TRUE(result.value().is_error());
  EXPECT_EQ(result->error(), fble::PeripheralError::INVALID_PARAMETERS);
}

TEST_F(LowEnergyPeripheralServerTest, AdvertiseAndReceiveTwoConnections) {
  fble::AdvertisedPeripheralHandle adv_peripheral_handle;
  FakeAdvertisedPeripheral adv_peripheral_server(
      adv_peripheral_handle.NewRequest());

  fble::AdvertisingParameters params;
  fble::ConnectionOptions conn_opts;
  params.set_connection_options(std::move(conn_opts));

  std::optional<fpromise::result<void, fble::PeripheralError>> adv_result;
  server()->Advertise(
      std::move(params), std::move(adv_peripheral_handle), [&](auto cb_result) {
        adv_result = std::move(cb_result);
      });
  RunLoopUntilIdle();
  EXPECT_FALSE(adv_result.has_value());

  test_device()->AddPeer(
      std::make_unique<FakePeer>(kTestAddr, pw_dispatcher()));
  test_device()->ConnectLowEnergy(kTestAddr);
  RunLoopUntilIdle();
  ASSERT_TRUE(adv_peripheral_server.last_connected_peer());

  // Sending response to first connection should restart advertising.
  adv_peripheral_server.connections()[0].callback();
  RunLoopUntilIdle();

  test_device()->AddPeer(
      std::make_unique<FakePeer>(kTestAddr2, pw_dispatcher()));
  test_device()->ConnectLowEnergy(kTestAddr2);
  RunLoopUntilIdle();
  ASSERT_EQ(adv_peripheral_server.connections().size(), 2u);

  adv_peripheral_server.Unbind();
  RunLoopUntilIdle();
  ASSERT_TRUE(adv_result.has_value());
  EXPECT_TRUE(adv_result->is_ok());
}

TEST_F(LowEnergyPeripheralServerTest,
       AdvertiseCanceledBeforeAdvertisingStarts) {
  fit::closure send_adv_enable_response;
  test_device()->pause_responses_for_opcode(
      bt::hci_spec::kLESetAdvertisingEnable, [&](fit::closure send_rsp) {
        send_adv_enable_response = std::move(send_rsp);
      });

  fble::AdvertisedPeripheralHandle adv_peripheral_handle;
  FakeAdvertisedPeripheral adv_peripheral_server(
      adv_peripheral_handle.NewRequest());

  fble::AdvertisingParameters params;
  std::optional<fpromise::result<void, fble::PeripheralError>> adv_result;
  server()->Advertise(
      std::move(params), std::move(adv_peripheral_handle), [&](auto cb_result) {
        adv_result = std::move(cb_result);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(send_adv_enable_response);

  adv_peripheral_server.Unbind();
  RunLoopUntilIdle();
  send_adv_enable_response();
  RunLoopUntilIdle();
  ASSERT_TRUE(adv_result.has_value());
  EXPECT_TRUE(adv_result->is_ok());
}

TEST_P(BoolParam, AdvertiseTwiceCausesSecondToFail) {
  fble::AdvertisedPeripheralHandle adv_peripheral_handle_0;
  FakeAdvertisedPeripheral adv_peripheral_server_0(
      adv_peripheral_handle_0.NewRequest());
  bool adv_peripheral_server_0_closed = false;
  adv_peripheral_server_0.set_error_handler(
      [&](auto) { adv_peripheral_server_0_closed = true; });

  fble::AdvertisingParameters params_0;
  fble::ConnectionOptions conn_opts;
  params_0.set_connection_options(std::move(conn_opts));

  std::optional<fpromise::result<void, fble::PeripheralError>> adv_result_0;
  server()->Advertise(
      std::move(params_0),
      std::move(adv_peripheral_handle_0),
      [&](auto cb_result) { adv_result_0 = std::move(cb_result); });

  // Test both with and without running the loop between Advertise requests.
  if (GetParam()) {
    RunLoopUntilIdle();
    EXPECT_FALSE(adv_result_0.has_value());
    EXPECT_FALSE(adv_peripheral_server_0_closed);
  }

  fble::AdvertisedPeripheralHandle adv_peripheral_handle_1;
  FakeAdvertisedPeripheral adv_peripheral_server_1(
      adv_peripheral_handle_1.NewRequest());
  bool adv_peripheral_server_1_closed = false;
  adv_peripheral_server_1.set_error_handler(
      [&](auto) { adv_peripheral_server_1_closed = true; });
  fble::AdvertisingParameters params_1;
  std::optional<fpromise::result<void, fble::PeripheralError>> adv_result_1;
  server()->Advertise(
      std::move(params_1),
      std::move(adv_peripheral_handle_1),
      [&](auto cb_result) { adv_result_1 = std::move(cb_result); });
  RunLoopUntilIdle();
  EXPECT_FALSE(adv_result_0.has_value());
  EXPECT_FALSE(adv_peripheral_server_0_closed);
  ASSERT_TRUE(adv_result_1.has_value());
  ASSERT_TRUE(adv_result_1->is_error());
  EXPECT_EQ(adv_result_1->error(), fble::PeripheralError::NOT_SUPPORTED);
  EXPECT_TRUE(adv_peripheral_server_1_closed);

  // Server 0 should still receive connections.
  test_device()->AddPeer(
      std::make_unique<FakePeer>(kTestAddr, pw_dispatcher()));
  test_device()->ConnectLowEnergy(kTestAddr);
  RunLoopUntilIdle();
  EXPECT_TRUE(adv_peripheral_server_0.last_connected_peer());

  adv_peripheral_server_0.Unbind();
  RunLoopUntilIdle();
  ASSERT_TRUE(adv_result_0.has_value());
  EXPECT_TRUE(adv_result_0->is_ok());
}

TEST_F(LowEnergyPeripheralServerTest,
       CallAdvertiseTwiceSequentiallyBothSucceed) {
  fble::AdvertisedPeripheralHandle adv_peripheral_handle_0;
  FakeAdvertisedPeripheral adv_peripheral_server_0(
      adv_peripheral_handle_0.NewRequest());
  fble::AdvertisingParameters params_0;
  std::optional<fpromise::result<void, fble::PeripheralError>> adv_result_0;
  server()->Advertise(
      std::move(params_0),
      std::move(adv_peripheral_handle_0),
      [&](auto cb_result) { adv_result_0 = std::move(cb_result); });
  RunLoopUntilIdle();
  EXPECT_FALSE(adv_result_0.has_value());

  adv_peripheral_server_0.Unbind();
  RunLoopUntilIdle();
  ASSERT_TRUE(adv_result_0.has_value());
  EXPECT_TRUE(adv_result_0->is_ok());

  fble::AdvertisedPeripheralHandle adv_peripheral_handle_1;
  FakeAdvertisedPeripheral adv_peripheral_server_1(
      adv_peripheral_handle_1.NewRequest());

  fble::AdvertisingParameters params_1;
  fble::ConnectionOptions conn_opts;
  params_1.set_connection_options(std::move(conn_opts));

  std::optional<fpromise::result<void, fble::PeripheralError>> adv_result_1;
  server()->Advertise(
      std::move(params_1),
      std::move(adv_peripheral_handle_1),
      [&](auto cb_result) { adv_result_1 = std::move(cb_result); });
  RunLoopUntilIdle();
  EXPECT_FALSE(adv_result_1.has_value());

  // Server 1 should receive connections.
  test_device()->AddPeer(
      std::make_unique<FakePeer>(kTestAddr, pw_dispatcher()));
  test_device()->ConnectLowEnergy(kTestAddr);
  RunLoopUntilIdle();
  EXPECT_TRUE(adv_peripheral_server_1.last_connected_peer());

  adv_peripheral_server_1.Unbind();
  RunLoopUntilIdle();
  ASSERT_TRUE(adv_result_1.has_value());
  EXPECT_TRUE(adv_result_1->is_ok());
}

TEST_F(LowEnergyPeripheralServerTest, PeerDisconnectClosesConnection) {
  fble::AdvertisedPeripheralHandle adv_peripheral_handle;
  FakeAdvertisedPeripheral adv_peripheral_server(
      adv_peripheral_handle.NewRequest());

  fble::AdvertisingParameters params;
  fble::ConnectionOptions conn_opts;
  params.set_connection_options(std::move(conn_opts));

  std::optional<fpromise::result<void, fble::PeripheralError>> adv_result;
  server()->Advertise(
      std::move(params), std::move(adv_peripheral_handle), [&](auto cb_result) {
        adv_result = std::move(cb_result);
      });
  RunLoopUntilIdle();
  EXPECT_FALSE(adv_result.has_value());

  test_device()->AddPeer(
      std::make_unique<FakePeer>(kTestAddr, pw_dispatcher()));
  test_device()->ConnectLowEnergy(kTestAddr);
  RunLoopUntilIdle();
  EXPECT_TRUE(adv_peripheral_server.last_connected_peer());
  fidl::InterfacePtr<fble::Connection> connection =
      adv_peripheral_server.connections()[0].connection.Bind();
  bool connection_closed = false;
  connection.set_error_handler([&](auto) { connection_closed = true; });
  EXPECT_FALSE(connection_closed);
  RunLoopUntilIdle();

  adv_peripheral_server.Unbind();
  RunLoopUntilIdle();
  ASSERT_TRUE(adv_result.has_value());
  EXPECT_TRUE(adv_result->is_ok());
  EXPECT_FALSE(connection_closed);

  test_device()->Disconnect(kTestAddr);
  RunLoopUntilIdle();
  EXPECT_TRUE(connection_closed);
}

TEST_F(LowEnergyPeripheralServerTest,
       IncomingConnectionFailureContinuesAdvertising) {
  fble::AdvertisedPeripheralHandle adv_peripheral_handle;
  FakeAdvertisedPeripheral adv_peripheral_server(
      adv_peripheral_handle.NewRequest());

  fble::AdvertisingParameters params;
  fble::ConnectionOptions conn_opts;
  params.set_connection_options(std::move(conn_opts));

  std::optional<fpromise::result<void, fble::PeripheralError>> adv_result;
  server()->Advertise(
      std::move(params), std::move(adv_peripheral_handle), [&](auto cb_result) {
        adv_result = std::move(cb_result);
      });
  RunLoopUntilIdle();
  EXPECT_FALSE(adv_result.has_value());

  // Cause peer interrogation to fail. This will result in a connection error
  // status to be received. Advertising should be immediately resumed, allowing
  // future connections.
  test_device()->SetDefaultCommandStatus(
      bt::hci_spec::kReadRemoteVersionInfo,
      pw::bluetooth::emboss::StatusCode::UNSUPPORTED_REMOTE_FEATURE);

  test_device()->AddPeer(
      std::make_unique<FakePeer>(kTestAddr, pw_dispatcher()));
  test_device()->ConnectLowEnergy(kTestAddr);
  RunLoopUntilIdle();
  EXPECT_FALSE(adv_peripheral_server.last_connected_peer());
  EXPECT_FALSE(adv_result.has_value());

  // Allow next interrogation to succeed.
  test_device()->ClearDefaultCommandStatus(
      bt::hci_spec::kReadRemoteVersionInfo);

  test_device()->AddPeer(
      std::make_unique<FakePeer>(kTestAddr, pw_dispatcher()));
  test_device()->ConnectLowEnergy(kTestAddr);
  RunLoopUntilIdle();
  EXPECT_TRUE(adv_peripheral_server.last_connected_peer());
  EXPECT_FALSE(adv_result.has_value());

  adv_peripheral_server.Unbind();
  RunLoopUntilIdle();
  EXPECT_TRUE(adv_result.has_value());
}

INSTANTIATE_TEST_SUITE_P(LowEnergyPeripheralServerTest,
                         BoolParam,
                         ::testing::Bool());

}  // namespace
}  // namespace bthost
