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

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/low_energy_central_server.h"

#include <cpp-string/string_printf.h>
#include <gmock/gmock.h>
#include <pw_assert/check.h>

#include <cstddef>

#include "fuchsia/bluetooth/gatt/cpp/fidl.h"
#include "fuchsia/bluetooth/le/cpp/fidl.h"
#include "lib/fidl/cpp/interface_request.h"
#include "pw_bluetooth_sapphire/fake_lease_provider.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/adapter_test_fixture.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/fake_adapter_test_fixture.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/helpers.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/measure_tape/hlcpp_measure_tape_for_peer.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_controller.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_peer.h"

namespace bthost {
namespace {

namespace fble = fuchsia::bluetooth::le;
namespace fgatt = fuchsia::bluetooth::gatt;

constexpr bt::PeerId kPeerId(2);

constexpr bt::hci::SyncId kSyncId(1);

constexpr uint8_t kAdvertisingSid = 3;

constexpr bt::gap::PeriodicAdvertisingSyncManager::SyncParameters
    kSyncParameters{
        .peer_id = kPeerId,
        .advertising_sid = kAdvertisingSid,
        .interval = 2,
        .phy = pw::bluetooth::emboss::LEPhy::LE_2M,
        .subevents_count = 3,
};

const bt::DeviceAddress kTestAddr(bt::DeviceAddress::Type::kLEPublic,
                                  {0x01, 0, 0, 0, 0, 0});
const size_t kLEMaxNumPackets = 10;
const bt::hci::DataBufferInfo kLEDataBufferInfo(
    bt::hci_spec::kMaxACLPayloadSize, kLEMaxNumPackets);

fble::ScanOptions ScanOptionsWithEmptyFilter() {
  fble::ScanOptions options;
  fble::Filter filter;
  std::vector<fble::Filter> filters;
  filters.emplace_back(std::move(filter));
  options.set_filters(std::move(filters));
  return options;
}

size_t MaxPeersPerScanResultWatcherChannel(const bt::gap::Peer& peer) {
  const size_t kPeerSize = measure_tape::fuchsia::bluetooth::le::Measure(
                               fidl_helpers::PeerToFidlLe(peer))
                               .num_bytes;
  const size_t kVectorOverhead =
      sizeof(fidl_message_header_t) + sizeof(fidl_vector_t);
  const size_t kMaxBytes = ZX_CHANNEL_MAX_MSG_BYTES - kVectorOverhead;
  return kMaxBytes / kPeerSize;
}

using TestingBase = bthost::testing::AdapterTestFixture;

class LowEnergyCentralServerTest : public TestingBase {
 public:
  LowEnergyCentralServerTest() = default;
  ~LowEnergyCentralServerTest() override = default;

  void SetUp() override {
    AdapterTestFixture::SetUp();

    // Create a LowEnergyCentralServer and bind it to a local client.
    fidl::InterfaceHandle<fble::Central> handle;
    gatt_ = take_gatt();
    server_ = std::make_unique<LowEnergyCentralServer>(adapter(),
                                                       handle.NewRequest(),
                                                       gatt_->GetWeakPtr(),
                                                       lease_provider_,
                                                       dispatcher());
    proxy_.Bind(std::move(handle));

    bt::testing::FakeController::Settings settings;
    settings.ApplyLegacyLEConfig();
    test_device()->set_settings(settings);
  }

  void TearDown() override {
    RunLoopUntilIdle();

    proxy_ = nullptr;
    server_ = nullptr;
    gatt_ = nullptr;

    RunLoopUntilIdle();
    AdapterTestFixture::TearDown();
  }

 protected:
  // Returns true if the given gatt.Client handle was closed after the event
  // loop finishes processing. Returns false if the handle was not closed.
  // Ownership of |handle| remains with the caller when this method returns.
  bool IsClientHandleClosedAfterLoop(
      fidl::InterfaceHandle<fgatt::Client>* handle) {
    PW_CHECK(handle);

    fgatt::ClientPtr proxy;
    proxy.Bind(std::move(*handle));

    bool closed = false;
    proxy.set_error_handler([&](zx_status_t s) {
      EXPECT_EQ(ZX_ERR_PEER_CLOSED, s);
      closed = true;
    });
    RunLoopUntilIdle();

    *handle = proxy.Unbind();
    return closed;
  }

  // Destroys the FIDL server. The le.Central proxy will be shut down and
  // subsequent calls to `server()` will return nullptr.
  void DestroyServer() { server_ = nullptr; }

  LowEnergyCentralServer* server() const { return server_.get(); }
  fuchsia::bluetooth::le::Central* central_proxy() const {
    return proxy_.get();
  }

 private:
  pw::bluetooth_sapphire::testing::FakeLeaseProvider lease_provider_;
  std::unique_ptr<LowEnergyCentralServer> server_;
  fble::CentralPtr proxy_;
  std::unique_ptr<bt::gatt::GATT> gatt_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LowEnergyCentralServerTest);
};

class LowEnergyCentralServerTestFakeAdapter
    : public bt::fidl::testing::FakeAdapterTestFixture {
 public:
  void SetUp() override {
    bt::fidl::testing::FakeAdapterTestFixture::SetUp();

    // Create a LowEnergyCentralServer and bind it to a local client.
    fidl::InterfaceHandle<fble::Central> handle;
    gatt_ = std::make_unique<bt::gatt::testing::FakeLayer>(pw_dispatcher());
    server_ = std::make_unique<LowEnergyCentralServer>(adapter()->AsWeakPtr(),
                                                       handle.NewRequest(),
                                                       gatt_->GetWeakPtr(),
                                                       lease_provider_,
                                                       dispatcher());
    proxy_.Bind(std::move(handle));
  }

  void TearDown() override {
    RunLoopUntilIdle();
    proxy_ = nullptr;
    server_ = nullptr;
    bt::fidl::testing::FakeAdapterTestFixture::TearDown();
  }

  fuchsia::bluetooth::le::Central* central_proxy() const {
    return proxy_.get();
  }

 private:
  pw::bluetooth_sapphire::testing::FakeLeaseProvider lease_provider_;
  std::unique_ptr<LowEnergyCentralServer> server_;
  fble::CentralPtr proxy_;
  std::unique_ptr<bt::gatt::GATT> gatt_;
};

class LowEnergyCentralServerTestFakeAdapterBoolParam
    : public LowEnergyCentralServerTestFakeAdapter,
      public ::testing::WithParamInterface<bool> {};

// Tests that connecting to a peripheral with
// LowEnergyConnectionOptions.bondable_mode unset results in a bondable
// connection ref being stored in LowEnergyConnectionManager
TEST_F(LowEnergyCentralServerTest, ConnectDefaultResultsBondableConnectionRef) {
  auto* const peer =
      adapter()->peer_cache()->NewPeer(kTestAddr, /*connectable=*/true);
  ASSERT_TRUE(peer);

  test_device()->AddPeer(
      std::make_unique<bt::testing::FakePeer>(kTestAddr, pw_dispatcher()));

  fble::ConnectionOptions options;

  fidl::InterfaceHandle<fuchsia::bluetooth::gatt::Client> gatt_client;
  fidl::InterfaceRequest<fuchsia::bluetooth::gatt::Client> gatt_client_req =
      gatt_client.NewRequest();

  auto status = fidl_helpers::NewFidlError(
      fuchsia::bluetooth::ErrorCode::BAD_STATE, "this should change");
  auto callback = [&status](::fuchsia::bluetooth::Status cb_status) {
    ASSERT_EQ(cb_status.error, nullptr);
    status = std::move(cb_status);
  };
  central_proxy()->ConnectPeripheral(peer->identifier().ToString(),
                                     std::move(options),
                                     std::move(gatt_client_req),
                                     callback);
  ASSERT_FALSE(server()->FindConnectionForTesting(peer->identifier()));
  RunLoopUntilIdle();
  auto conn_ref = server()->FindConnectionForTesting(peer->identifier());
  ASSERT_EQ(status.error, nullptr);
  ASSERT_TRUE(conn_ref.has_value());
  ASSERT_TRUE(conn_ref.value());
  ASSERT_EQ(conn_ref.value()->bondable_mode(), bt::sm::BondableMode::Bondable);
}

// Tests that setting LowEnergyConnectionOptions.bondable_mode to true and
// connecting to a peer in bondable mode results in a bondable connection ref
// being stored in LowEnergyConnectionManager
TEST_F(LowEnergyCentralServerTest,
       ConnectBondableResultsBondableConnectionRef) {
  auto* const peer =
      adapter()->peer_cache()->NewPeer(kTestAddr, /*connectable=*/true);
  ASSERT_TRUE(peer);

  test_device()->AddPeer(
      std::make_unique<bt::testing::FakePeer>(kTestAddr, pw_dispatcher()));

  fble::ConnectionOptions options;
  options.set_bondable_mode(true);

  fidl::InterfaceHandle<fuchsia::bluetooth::gatt::Client> gatt_client;
  fidl::InterfaceRequest<fuchsia::bluetooth::gatt::Client> gatt_client_req =
      gatt_client.NewRequest();

  auto status = fidl_helpers::NewFidlError(
      fuchsia::bluetooth::ErrorCode::BAD_STATE, "this should change");
  auto callback = [&status](::fuchsia::bluetooth::Status cb_status) {
    ASSERT_EQ(cb_status.error, nullptr);
    status = std::move(cb_status);
  };
  central_proxy()->ConnectPeripheral(peer->identifier().ToString(),
                                     std::move(options),
                                     std::move(gatt_client_req),
                                     callback);
  ASSERT_FALSE(server()->FindConnectionForTesting(peer->identifier()));
  RunLoopUntilIdle();
  auto conn_ref = server()->FindConnectionForTesting(peer->identifier());
  ASSERT_EQ(status.error, nullptr);
  ASSERT_TRUE(conn_ref.has_value());
  ASSERT_TRUE(conn_ref.value());
  ASSERT_EQ(conn_ref.value()->bondable_mode(), bt::sm::BondableMode::Bondable);
}

// Tests that setting LowEnergyConnectionOptions.bondable_mode to false and
// connecting to a peer results in a non-bondable connection ref being stored in
// LowEnergyConnectionManager.
TEST_F(LowEnergyCentralServerTest,
       ConnectNonBondableResultsNonBondableConnectionRef) {
  auto* const peer =
      adapter()->peer_cache()->NewPeer(kTestAddr, /*connectable=*/true);
  ASSERT_TRUE(peer);

  test_device()->AddPeer(
      std::make_unique<bt::testing::FakePeer>(kTestAddr, pw_dispatcher()));

  fble::ConnectionOptions options;
  options.set_bondable_mode(false);

  fidl::InterfaceHandle<fuchsia::bluetooth::gatt::Client> gatt_client;
  fidl::InterfaceRequest<fuchsia::bluetooth::gatt::Client> gatt_client_req =
      gatt_client.NewRequest();

  auto status = fidl_helpers::NewFidlError(
      fuchsia::bluetooth::ErrorCode::BAD_STATE, "this should change");
  auto callback = [&status](::fuchsia::bluetooth::Status cb_status) {
    ASSERT_EQ(cb_status.error, nullptr);
    status = std::move(cb_status);
  };
  central_proxy()->ConnectPeripheral(peer->identifier().ToString(),
                                     std::move(options),
                                     std::move(gatt_client_req),
                                     callback);
  ASSERT_FALSE(server()->FindConnectionForTesting(peer->identifier()));
  RunLoopUntilIdle();
  auto conn_ref = server()->FindConnectionForTesting(peer->identifier());
  ASSERT_EQ(status.error, nullptr);
  ASSERT_TRUE(conn_ref.has_value());
  ASSERT_TRUE(conn_ref.value());
  ASSERT_EQ(conn_ref.value()->bondable_mode(),
            bt::sm::BondableMode::NonBondable);
}

TEST_F(LowEnergyCentralServerTest,
       DisconnectUnconnectedPeripheralReturnsSuccess) {
  auto status = fidl_helpers::NewFidlError(
      fuchsia::bluetooth::ErrorCode::BAD_STATE, "this should change");
  auto callback = [&status](::fuchsia::bluetooth::Status cb_status) {
    status = std::move(cb_status);
  };
  central_proxy()->DisconnectPeripheral(bt::PeerId(1).ToString(),
                                        std::move(callback));
  RunLoopUntilIdle();
  EXPECT_EQ(status.error, nullptr);
}

TEST_F(LowEnergyCentralServerTest, FailedConnectionCleanedUp) {
  auto* const peer =
      adapter()->peer_cache()->NewPeer(kTestAddr, /*connectable=*/true);
  ASSERT_TRUE(peer);

  test_device()->AddPeer(
      std::make_unique<bt::testing::FakePeer>(kTestAddr, pw_dispatcher()));

  fble::ConnectionOptions options;

  fidl::InterfaceHandle<fuchsia::bluetooth::gatt::Client> gatt_client;
  fidl::InterfaceRequest<fuchsia::bluetooth::gatt::Client> gatt_client_req =
      gatt_client.NewRequest();

  fuchsia::bluetooth::Status status;
  auto callback = [&status](::fuchsia::bluetooth::Status cb_status) {
    status = std::move(cb_status);
  };

  test_device()->SetDefaultCommandStatus(
      bt::hci_spec::kReadRemoteVersionInfo,
      pw::bluetooth::emboss::StatusCode::CONNECTION_LIMIT_EXCEEDED);

  ASSERT_FALSE(
      server()->FindConnectionForTesting(peer->identifier()).has_value());
  central_proxy()->ConnectPeripheral(peer->identifier().ToString(),
                                     std::move(options),
                                     std::move(gatt_client_req),
                                     callback);
  RunLoopUntilIdle();
  auto conn = server()->FindConnectionForTesting(peer->identifier());
  EXPECT_NE(status.error, nullptr);
  EXPECT_FALSE(conn.has_value());
}

TEST_F(LowEnergyCentralServerTest, ConnectPeripheralAlreadyConnectedInLecm) {
  auto* const peer =
      adapter()->peer_cache()->NewPeer(kTestAddr, /*connectable=*/true);
  test_device()->AddPeer(
      std::make_unique<bt::testing::FakePeer>(kTestAddr, pw_dispatcher()));

  std::unique_ptr<bt::gap::LowEnergyConnectionHandle> le_conn;
  adapter()->le()->Connect(
      peer->identifier(),
      [&le_conn](auto result) {
        ASSERT_EQ(fit::ok(), result);
        le_conn = std::move(result).value();
      },
      bt::gap::LowEnergyConnectionOptions());
  RunLoopUntilIdle();
  ASSERT_TRUE(le_conn);
  ASSERT_FALSE(
      server()->FindConnectionForTesting(peer->identifier()).has_value());

  fuchsia::bluetooth::Status status;
  auto callback = [&status](::fuchsia::bluetooth::Status cb_status) {
    status = std::move(cb_status);
  };

  fble::ConnectionOptions options;
  fidl::InterfaceHandle<fuchsia::bluetooth::gatt::Client> gatt_client;
  fidl::InterfaceRequest<fuchsia::bluetooth::gatt::Client> gatt_client_req =
      gatt_client.NewRequest();
  central_proxy()->ConnectPeripheral(peer->identifier().ToString(),
                                     std::move(options),
                                     std::move(gatt_client_req),
                                     callback);
  RunLoopUntilIdle();
  EXPECT_EQ(status.error, nullptr);
  auto server_conn = server()->FindConnectionForTesting(peer->identifier());
  ASSERT_TRUE(server_conn.has_value());
  EXPECT_NE(server_conn.value(), nullptr);
}

TEST_F(LowEnergyCentralServerTest, ConnectPeripheralUnknownPeer) {
  fuchsia::bluetooth::Status status;
  auto callback = [&status](::fuchsia::bluetooth::Status cb_status) {
    status = std::move(cb_status);
  };

  const bt::PeerId peer_id(1);

  fble::ConnectionOptions options;
  fidl::InterfaceHandle<fuchsia::bluetooth::gatt::Client> gatt_client;
  fidl::InterfaceRequest<fuchsia::bluetooth::gatt::Client> gatt_client_req =
      gatt_client.NewRequest();
  central_proxy()->ConnectPeripheral(peer_id.ToString(),
                                     std::move(options),
                                     std::move(gatt_client_req),
                                     callback);
  RunLoopUntilIdle();
  ASSERT_TRUE(status.error);
  EXPECT_EQ(status.error->error_code, fuchsia::bluetooth::ErrorCode::NOT_FOUND);
  auto server_conn = server()->FindConnectionForTesting(peer_id);
  EXPECT_FALSE(server_conn.has_value());
}

TEST_F(LowEnergyCentralServerTest,
       DisconnectPeripheralClosesCorrectGattHandle) {
  const bt::DeviceAddress kAddr1 = kTestAddr;
  const bt::DeviceAddress kAddr2(bt::DeviceAddress::Type::kLEPublic,
                                 {2, 0, 0, 0, 0, 0});
  auto* const peer1 =
      adapter()->peer_cache()->NewPeer(kAddr1, /*connectable=*/true);
  auto* const peer2 =
      adapter()->peer_cache()->NewPeer(kAddr2, /*connectable=*/true);

  test_device()->AddPeer(
      std::make_unique<bt::testing::FakePeer>(kAddr1, pw_dispatcher()));
  test_device()->AddPeer(
      std::make_unique<bt::testing::FakePeer>(kAddr2, pw_dispatcher()));

  // Establish two connections.
  fidl::InterfaceHandle<fgatt::Client> handle1, handle2;
  central_proxy()->ConnectPeripheral(peer1->identifier().ToString(),
                                     fble::ConnectionOptions{},
                                     handle1.NewRequest(),
                                     [](auto) {});
  central_proxy()->ConnectPeripheral(peer2->identifier().ToString(),
                                     fble::ConnectionOptions{},
                                     handle2.NewRequest(),
                                     [](auto) {});
  RunLoopUntilIdle();
  ASSERT_TRUE(server()->FindConnectionForTesting(peer1->identifier()));
  ASSERT_TRUE(server()->FindConnectionForTesting(peer2->identifier()));
  EXPECT_FALSE(IsClientHandleClosedAfterLoop(&handle1));
  EXPECT_FALSE(IsClientHandleClosedAfterLoop(&handle2));

  // Disconnect peer1. Only its gatt.Client handle should close.
  central_proxy()->DisconnectPeripheral(peer1->identifier().ToString(),
                                        [](auto) {});
  EXPECT_TRUE(IsClientHandleClosedAfterLoop(&handle1));
  EXPECT_FALSE(IsClientHandleClosedAfterLoop(&handle2));

  // Disconnect peer2. Its handle should close now.
  central_proxy()->DisconnectPeripheral(peer2->identifier().ToString(),
                                        [](auto) {});
  EXPECT_TRUE(IsClientHandleClosedAfterLoop(&handle2));
}

TEST_F(LowEnergyCentralServerTest, PeerDisconnectClosesCorrectHandle) {
  const bt::DeviceAddress kAddr1 = kTestAddr;
  const bt::DeviceAddress kAddr2(bt::DeviceAddress::Type::kLEPublic,
                                 {2, 0, 0, 0, 0, 0});
  auto* const peer1 =
      adapter()->peer_cache()->NewPeer(kAddr1, /*connectable=*/true);
  auto* const peer2 =
      adapter()->peer_cache()->NewPeer(kAddr2, /*connectable=*/true);

  test_device()->AddPeer(
      std::make_unique<bt::testing::FakePeer>(kAddr1, pw_dispatcher()));
  test_device()->AddPeer(
      std::make_unique<bt::testing::FakePeer>(kAddr2, pw_dispatcher()));

  // Establish two connections.
  fidl::InterfaceHandle<fgatt::Client> handle1, handle2;
  central_proxy()->ConnectPeripheral(peer1->identifier().ToString(),
                                     fble::ConnectionOptions{},
                                     handle1.NewRequest(),
                                     [](auto) {});
  central_proxy()->ConnectPeripheral(peer2->identifier().ToString(),
                                     fble::ConnectionOptions{},
                                     handle2.NewRequest(),
                                     [](auto) {});
  RunLoopUntilIdle();
  ASSERT_TRUE(server()->FindConnectionForTesting(peer1->identifier()));
  ASSERT_TRUE(server()->FindConnectionForTesting(peer2->identifier()));
  EXPECT_FALSE(IsClientHandleClosedAfterLoop(&handle1));
  EXPECT_FALSE(IsClientHandleClosedAfterLoop(&handle2));

  // Disconnect peer1. Only its gatt.Client handle should close.
  test_device()->Disconnect(kAddr1);
  EXPECT_TRUE(IsClientHandleClosedAfterLoop(&handle1));
  EXPECT_FALSE(IsClientHandleClosedAfterLoop(&handle2));

  // Disconnect peer2. Its handle should close now.
  test_device()->Disconnect(kAddr2);
  EXPECT_TRUE(IsClientHandleClosedAfterLoop(&handle2));
}

TEST_F(LowEnergyCentralServerTest,
       ClosingCentralHandleClosesAssociatedGattClientHandles) {
  const bt::DeviceAddress kAddr1 = kTestAddr;
  const bt::DeviceAddress kAddr2(bt::DeviceAddress::Type::kLEPublic,
                                 {2, 0, 0, 0, 0, 0});
  auto* const peer1 =
      adapter()->peer_cache()->NewPeer(kAddr1, /*connectable=*/true);
  auto* const peer2 =
      adapter()->peer_cache()->NewPeer(kAddr2, /*connectable=*/true);

  test_device()->AddPeer(
      std::make_unique<bt::testing::FakePeer>(kAddr1, pw_dispatcher()));
  test_device()->AddPeer(
      std::make_unique<bt::testing::FakePeer>(kAddr2, pw_dispatcher()));

  // Establish two connections.
  fidl::InterfaceHandle<fgatt::Client> handle1, handle2;
  central_proxy()->ConnectPeripheral(peer1->identifier().ToString(),
                                     fble::ConnectionOptions{},
                                     handle1.NewRequest(),
                                     [](auto) {});
  central_proxy()->ConnectPeripheral(peer2->identifier().ToString(),
                                     fble::ConnectionOptions{},
                                     handle2.NewRequest(),
                                     [](auto) {});
  RunLoopUntilIdle();
  ASSERT_TRUE(server()->FindConnectionForTesting(peer1->identifier()));
  ASSERT_TRUE(server()->FindConnectionForTesting(peer2->identifier()));
  EXPECT_FALSE(IsClientHandleClosedAfterLoop(&handle1));
  EXPECT_FALSE(IsClientHandleClosedAfterLoop(&handle2));

  DestroyServer();
  EXPECT_TRUE(IsClientHandleClosedAfterLoop(&handle1));
  EXPECT_TRUE(IsClientHandleClosedAfterLoop(&handle2));
}

TEST_F(LowEnergyCentralServerTest, ScanWithEmptyScanOptionsFails) {
  fidl::InterfaceHandle<fble::ScanResultWatcher> result_watcher_handle;
  auto result_watcher_server = result_watcher_handle.NewRequest();

  auto result_watcher_client = result_watcher_handle.Bind();
  std::optional<zx_status_t> result_watcher_epitaph;
  result_watcher_client.set_error_handler(
      [&](zx_status_t epitaph) { result_watcher_epitaph = epitaph; });

  bool scan_stopped = false;
  central_proxy()->Scan(fble::ScanOptions(),
                        std::move(result_watcher_server),
                        [&]() { scan_stopped = true; });
  RunLoopUntilIdle();
  EXPECT_TRUE(scan_stopped);
  ASSERT_TRUE(result_watcher_epitaph.has_value());
  EXPECT_EQ(result_watcher_epitaph.value(), ZX_ERR_INVALID_ARGS);
}

TEST_F(LowEnergyCentralServerTest, ScanWithNoFiltersFails) {
  fble::ScanOptions options;
  std::vector<fble::Filter> filters;
  options.set_filters(std::move(filters));

  fidl::InterfaceHandle<fble::ScanResultWatcher> result_watcher_handle;
  auto result_watcher_server = result_watcher_handle.NewRequest();

  auto result_watcher_client = result_watcher_handle.Bind();
  std::optional<zx_status_t> result_watcher_epitaph;
  result_watcher_client.set_error_handler(
      [&](zx_status_t epitaph) { result_watcher_epitaph = epitaph; });

  bool scan_stopped = false;
  central_proxy()->Scan(std::move(options),
                        std::move(result_watcher_server),
                        [&]() { scan_stopped = true; });
  RunLoopUntilIdle();
  EXPECT_TRUE(scan_stopped);
  ASSERT_TRUE(result_watcher_epitaph.has_value());
  EXPECT_EQ(result_watcher_epitaph.value(), ZX_ERR_INVALID_ARGS);
}

TEST_F(LowEnergyCentralServerTest, ScanReceivesPeerPreviouslyAddedToPeerCache) {
  bt::gap::Peer* peer =
      adapter()->peer_cache()->NewPeer(kTestAddr, /*connectable=*/false);

  fidl::InterfaceHandle<fble::ScanResultWatcher> result_watcher_handle;
  auto result_watcher_server = result_watcher_handle.NewRequest();
  auto result_watcher_client = result_watcher_handle.Bind();
  std::optional<zx_status_t> epitaph;
  result_watcher_client.set_error_handler(
      [&](zx_status_t cb_epitaph) { epitaph = cb_epitaph; });

  bool scan_stopped = false;
  central_proxy()->Scan(ScanOptionsWithEmptyFilter(),
                        std::move(result_watcher_server),
                        [&]() { scan_stopped = true; });

  RunLoopUntilIdle();
  EXPECT_FALSE(scan_stopped);
  EXPECT_FALSE(epitaph);

  std::optional<std::vector<fble::Peer>> peers;
  result_watcher_client->Watch(
      [&](std::vector<fble::Peer> updated) { peers = std::move(updated); });
  RunLoopUntilIdle();
  ASSERT_TRUE(peers.has_value());
  ASSERT_EQ(peers->size(), 1u);
  ASSERT_TRUE(peers->front().has_id());
  EXPECT_EQ(peers->front().id().value, peer->identifier().value());

  result_watcher_client.Unbind();
  RunLoopUntilIdle();
  EXPECT_TRUE(scan_stopped);
}

TEST_F(LowEnergyCentralServerTest,
       ScanReceivesPeerAddedToPeerCacheAfterScanStart) {
  fidl::InterfaceHandle<fble::ScanResultWatcher> result_watcher_handle;
  auto result_watcher_server = result_watcher_handle.NewRequest();
  auto result_watcher_client = result_watcher_handle.Bind();
  std::optional<zx_status_t> epitaph;
  result_watcher_client.set_error_handler(
      [&](zx_status_t cb_epitaph) { epitaph = cb_epitaph; });

  bool scan_stopped = false;
  central_proxy()->Scan(ScanOptionsWithEmptyFilter(),
                        std::move(result_watcher_server),
                        [&]() { scan_stopped = true; });

  RunLoopUntilIdle();
  EXPECT_FALSE(scan_stopped);
  EXPECT_FALSE(epitaph);

  std::optional<std::vector<fble::Peer>> peers;
  result_watcher_client->Watch(
      [&](std::vector<fble::Peer> updated) { peers = std::move(updated); });
  RunLoopUntilIdle();
  ASSERT_FALSE(peers.has_value());

  bt::gap::Peer* peer =
      adapter()->peer_cache()->NewPeer(kTestAddr, /*connectable=*/false);
  RunLoopUntilIdle();
  ASSERT_TRUE(peers.has_value());
  ASSERT_EQ(peers->size(), 1u);
  ASSERT_TRUE(peers->front().has_id());
  EXPECT_EQ(peers->front().id().value, peer->identifier().value());

  result_watcher_client.Unbind();
  RunLoopUntilIdle();
  EXPECT_TRUE(scan_stopped);
}

TEST_F(LowEnergyCentralServerTest,
       PeerAddedToPeerCacheAfterScanEndDoesNotCrash) {
  fidl::InterfaceHandle<fble::ScanResultWatcher> result_watcher_handle;
  auto result_watcher_server = result_watcher_handle.NewRequest();
  auto result_watcher_client = result_watcher_handle.Bind();
  std::optional<zx_status_t> epitaph;
  result_watcher_client.set_error_handler(
      [&](zx_status_t cb_epitaph) { epitaph = cb_epitaph; });

  bool scan_stopped = false;
  central_proxy()->Scan(ScanOptionsWithEmptyFilter(),
                        std::move(result_watcher_server),
                        [&]() { scan_stopped = true; });

  RunLoopUntilIdle();
  EXPECT_FALSE(scan_stopped);
  EXPECT_FALSE(epitaph);

  RunLoopUntilIdle();

  result_watcher_client.Unbind();
  RunLoopUntilIdle();
  EXPECT_TRUE(scan_stopped);

  adapter()->peer_cache()->NewPeer(kTestAddr, /*connectable=*/false);
  RunLoopUntilIdle();
}

TEST_F(LowEnergyCentralServerTest, ConcurrentScansFail) {
  fidl::InterfaceHandle<fble::ScanResultWatcher> result_watcher_handle_0;
  auto result_watcher_server_0 = result_watcher_handle_0.NewRequest();
  auto result_watcher_client_0 = result_watcher_handle_0.Bind();
  bool scan_stopped_0 = false;
  central_proxy()->Scan(ScanOptionsWithEmptyFilter(),
                        std::move(result_watcher_server_0),
                        [&]() { scan_stopped_0 = true; });
  RunLoopUntilIdle();
  EXPECT_FALSE(scan_stopped_0);

  fidl::InterfaceHandle<fble::ScanResultWatcher> result_watcher_handle_1;
  auto result_watcher_server_1 = result_watcher_handle_1.NewRequest();
  auto result_watcher_client_1 = result_watcher_handle_1.Bind();
  std::optional<zx_status_t> epitaph_1;
  result_watcher_client_1.set_error_handler(
      [&](zx_status_t cb_epitaph) { epitaph_1 = cb_epitaph; });

  bool scan_stopped_1 = false;
  central_proxy()->Scan(ScanOptionsWithEmptyFilter(),
                        std::move(result_watcher_server_1),
                        [&]() { scan_stopped_1 = true; });
  RunLoopUntilIdle();
  EXPECT_FALSE(scan_stopped_0);
  EXPECT_TRUE(scan_stopped_1);
  ASSERT_TRUE(epitaph_1);
  EXPECT_EQ(epitaph_1.value(), ZX_ERR_ALREADY_EXISTS);

  result_watcher_client_0.Unbind();
  RunLoopUntilIdle();
  EXPECT_TRUE(scan_stopped_0);
}

TEST_F(LowEnergyCentralServerTest, SequentialScansSucceed) {
  fidl::InterfaceHandle<fble::ScanResultWatcher> result_watcher_handle_0;
  auto result_watcher_server_0 = result_watcher_handle_0.NewRequest();
  auto result_watcher_client_0 = result_watcher_handle_0.Bind();
  bool scan_stopped_0 = false;
  central_proxy()->Scan(ScanOptionsWithEmptyFilter(),
                        std::move(result_watcher_server_0),
                        [&]() { scan_stopped_0 = true; });
  RunLoopUntilIdle();
  EXPECT_FALSE(scan_stopped_0);

  result_watcher_client_0.Unbind();
  RunLoopUntilIdle();
  EXPECT_TRUE(scan_stopped_0);

  fidl::InterfaceHandle<fble::ScanResultWatcher> result_watcher_handle_1;
  auto result_watcher_server_1 = result_watcher_handle_1.NewRequest();
  auto result_watcher_client_1 = result_watcher_handle_1.Bind();
  bool scan_stopped_1 = false;
  central_proxy()->Scan(ScanOptionsWithEmptyFilter(),
                        std::move(result_watcher_server_1),
                        [&]() { scan_stopped_1 = true; });
  RunLoopUntilIdle();
  EXPECT_FALSE(scan_stopped_1);

  result_watcher_client_1.Unbind();
  RunLoopUntilIdle();
  EXPECT_TRUE(scan_stopped_1);
}

TEST_F(LowEnergyCentralServerTest, IgnorePeersThatDoNotMatchFilter) {
  fble::ScanOptions options;
  fble::Filter filter;
  filter.set_connectable(true);
  std::vector<fble::Filter> filters;
  filters.emplace_back(std::move(filter));
  options.set_filters(std::move(filters));

  fidl::InterfaceHandle<fble::ScanResultWatcher> result_watcher_handle;
  auto result_watcher_server = result_watcher_handle.NewRequest();
  auto result_watcher_client = result_watcher_handle.Bind();
  std::optional<zx_status_t> epitaph;
  result_watcher_client.set_error_handler(
      [&](zx_status_t cb_epitaph) { epitaph = cb_epitaph; });

  bool scan_stopped = false;
  central_proxy()->Scan(std::move(options),
                        std::move(result_watcher_server),
                        [&]() { scan_stopped = true; });

  RunLoopUntilIdle();
  EXPECT_FALSE(scan_stopped);
  EXPECT_FALSE(epitaph);

  std::optional<std::vector<fble::Peer>> peers;
  result_watcher_client->Watch(
      [&](std::vector<fble::Peer> updated) { peers = std::move(updated); });
  RunLoopUntilIdle();
  ASSERT_FALSE(peers.has_value());

  // Peer is not LE
  adapter()->peer_cache()->NewPeer(
      bt::DeviceAddress(bt::DeviceAddress::Type::kBREDR, {1, 0, 0, 0, 0, 0}),
      /*connectable=*/true);
  // Peer is not connectable
  adapter()->peer_cache()->NewPeer(
      bt::DeviceAddress(bt::DeviceAddress::Type::kLEPublic, {2, 0, 0, 0, 0, 0}),
      /*connectable=*/false);

  RunLoopUntilIdle();
  EXPECT_FALSE(peers.has_value());

  result_watcher_client.Unbind();
  RunLoopUntilIdle();
  EXPECT_TRUE(scan_stopped);
}

TEST_F(LowEnergyCentralServerTest,
       IgnorePeerThatDoesNotMatchServiceDataFilter) {
  fble::ScanOptions options;
  fble::Filter filter;
  const bt::UUID kServiceUuid(static_cast<uint16_t>(2));
  filter.set_connectable(true);
  filter.set_service_data_uuid(fuchsia::bluetooth::Uuid{kServiceUuid.value()});
  std::vector<fble::Filter> filters;
  filters.emplace_back(std::move(filter));
  options.set_filters(std::move(filters));

  fidl::InterfaceHandle<fble::ScanResultWatcher> result_watcher_handle;
  auto result_watcher_server = result_watcher_handle.NewRequest();
  auto result_watcher_client = result_watcher_handle.Bind();
  std::optional<zx_status_t> epitaph;
  result_watcher_client.set_error_handler(
      [&](zx_status_t cb_epitaph) { epitaph = cb_epitaph; });

  bool scan_stopped = false;
  central_proxy()->Scan(std::move(options),
                        std::move(result_watcher_server),
                        [&]() { scan_stopped = true; });

  RunLoopUntilIdle();
  EXPECT_FALSE(scan_stopped);
  EXPECT_FALSE(epitaph);

  std::optional<std::vector<fble::Peer>> peers;
  result_watcher_client->Watch(
      [&](std::vector<fble::Peer> updated) { peers = std::move(updated); });
  RunLoopUntilIdle();
  ASSERT_FALSE(peers.has_value());

  // Peer is connectable but doesn't have any service data.
  adapter()->peer_cache()->NewPeer(
      bt::DeviceAddress(bt::DeviceAddress::Type::kLEPublic, {2, 0, 0, 0, 0, 0}),
      /*connectable=*/true);

  RunLoopUntilIdle();
  EXPECT_FALSE(peers.has_value());

  result_watcher_client.Unbind();
  RunLoopUntilIdle();
  EXPECT_TRUE(scan_stopped);
}

TEST_F(LowEnergyCentralServerTest,
       DoNotNotifyResultWatcherWithPeerThatWasRemovedFromPeerCacheWhileQueued) {
  bt::gap::Peer* peer =
      adapter()->peer_cache()->NewPeer(kTestAddr, /*connectable=*/false);

  fidl::InterfaceHandle<fble::ScanResultWatcher> result_watcher_handle;
  auto result_watcher_server = result_watcher_handle.NewRequest();
  auto result_watcher_client = result_watcher_handle.Bind();
  std::optional<zx_status_t> epitaph;
  result_watcher_client.set_error_handler(
      [&](zx_status_t cb_epitaph) { epitaph = cb_epitaph; });

  bool scan_stopped = false;
  central_proxy()->Scan(ScanOptionsWithEmptyFilter(),
                        std::move(result_watcher_server),
                        [&]() { scan_stopped = true; });

  RunLoopUntilIdle();
  EXPECT_FALSE(scan_stopped);
  EXPECT_FALSE(epitaph);

  // Peer is in ScanResultWatcher queue. Remove it from PeerCache before Watch()
  // is called.
  EXPECT_TRUE(
      adapter()->peer_cache()->RemoveDisconnectedPeer(peer->identifier()));

  std::optional<std::vector<fble::Peer>> peers;
  result_watcher_client->Watch(
      [&](std::vector<fble::Peer> updated) { peers = std::move(updated); });
  RunLoopUntilIdle();
  EXPECT_FALSE(peers.has_value());

  result_watcher_client.Unbind();
  RunLoopUntilIdle();
  EXPECT_TRUE(scan_stopped);
}

TEST_F(LowEnergyCentralServerTest, MaxQueuedScanResultWatcherPeers) {
  // Create smallest possible peer
  bt::gap::Peer* peer_0 = adapter()->peer_cache()->NewPeer(
      bt::DeviceAddress(bt::DeviceAddress::Type::kLEPublic, {0, 0, 0, 0, 0, 0}),
      /*connectable=*/false);
  const size_t kMaxPeersPerChannel =
      MaxPeersPerScanResultWatcherChannel(*peer_0);
  ASSERT_GT(kMaxPeersPerChannel,
            LowEnergyCentralServer::kMaxPendingScanResultWatcherPeers);

  // Queue 1 more peer than queue size limit.
  ASSERT_LE(LowEnergyCentralServer::kMaxPendingScanResultWatcherPeers,
            std::numeric_limits<uint8_t>::max());
  for (size_t i = 1;
       i < LowEnergyCentralServer::kMaxPendingScanResultWatcherPeers + 1;
       i++) {
    SCOPED_TRACE(i);
    ASSERT_TRUE(adapter()->peer_cache()->NewPeer(
        bt::DeviceAddress(bt::DeviceAddress::Type::kLEPublic,
                          {static_cast<uint8_t>(i), 0, 0, 0, 0, 0}),
        /*connectable=*/false));
  }

  fidl::InterfaceHandle<fble::ScanResultWatcher> result_watcher_handle;
  auto result_watcher_server = result_watcher_handle.NewRequest();
  auto result_watcher_client = result_watcher_handle.Bind();
  std::optional<zx_status_t> epitaph;
  result_watcher_client.set_error_handler(
      [&](zx_status_t cb_epitaph) { epitaph = cb_epitaph; });

  bool scan_stopped = false;
  central_proxy()->Scan(ScanOptionsWithEmptyFilter(),
                        std::move(result_watcher_server),
                        [&]() { scan_stopped = true; });

  RunLoopUntilIdle();
  EXPECT_FALSE(scan_stopped);
  EXPECT_FALSE(epitaph);

  std::optional<std::vector<fble::Peer>> peers;
  result_watcher_client->Watch(
      [&](std::vector<fble::Peer> updated) { peers = std::move(updated); });
  RunLoopUntilIdle();
  ASSERT_TRUE(peers.has_value());
  EXPECT_EQ(peers->size(),
            LowEnergyCentralServer::kMaxPendingScanResultWatcherPeers);
  peers.reset();

  // Additional calls to Watch should hang
  result_watcher_client->Watch(
      [&](std::vector<fble::Peer> updated) { peers = std::move(updated); });
  RunLoopUntilIdle();
  EXPECT_FALSE(peers.has_value());

  result_watcher_client.Unbind();
  RunLoopUntilIdle();
  EXPECT_TRUE(scan_stopped);
}

TEST_F(LowEnergyCentralServerTest, ScanResultWatcherMeasureTape) {
  // Create a very large Peer
  bt::gap::Peer* peer_0 = adapter()->peer_cache()->NewPeer(
      bt::DeviceAddress(bt::DeviceAddress::Type::kLEPublic, {0, 0, 0, 0, 0, 0}),
      /*connectable=*/true);
  bt::AdvertisingData adv_data;
  for (int i = 0; i < 100; i++) {
    SCOPED_TRACE(i);
    ASSERT_TRUE(adv_data.AddUri(
        bt_lib_cpp_string::StringPrintf("uri:a-really-long-uri-%d", i)));
  }
  adv_data.CalculateBlockSize();
  bt::DynamicByteBuffer adv_buffer(adv_data.CalculateBlockSize());
  adv_data.WriteBlock(&adv_buffer, std::nullopt);
  peer_0->MutLe().SetAdvertisingData(
      /*rssi=*/0, adv_buffer, pw::chrono::SystemClock::time_point());

  const size_t kMaxPeersPerChannel =
      MaxPeersPerScanResultWatcherChannel(*peer_0);

  // Queue 1 more peer than will fit in the channel.
  // Start at i = 1 because peer_0 was created above.
  ASSERT_LE(kMaxPeersPerChannel, std::numeric_limits<uint8_t>::max());
  ASSERT_GT(LowEnergyCentralServer::kMaxPendingScanResultWatcherPeers,
            kMaxPeersPerChannel);
  for (size_t i = 1; i < kMaxPeersPerChannel + 1; i++) {
    SCOPED_TRACE(i);
    bt::gap::Peer* peer = adapter()->peer_cache()->NewPeer(
        bt::DeviceAddress(bt::DeviceAddress::Type::kLEPublic,
                          {static_cast<uint8_t>(i), 0, 0, 0, 0, 0}),
        /*connectable=*/false);
    ASSERT_TRUE(peer);
    peer->MutLe().SetAdvertisingData(
        /*rssi=*/0, adv_buffer, pw::chrono::SystemClock::time_point());
  }

  fidl::InterfaceHandle<fble::ScanResultWatcher> result_watcher_handle;
  auto result_watcher_server = result_watcher_handle.NewRequest();
  auto result_watcher_client = result_watcher_handle.Bind();
  std::optional<zx_status_t> epitaph;
  result_watcher_client.set_error_handler(
      [&](zx_status_t cb_epitaph) { epitaph = cb_epitaph; });

  bool scan_stopped = false;
  central_proxy()->Scan(ScanOptionsWithEmptyFilter(),
                        std::move(result_watcher_server),
                        [&]() { scan_stopped = true; });

  RunLoopUntilIdle();
  EXPECT_FALSE(scan_stopped);
  EXPECT_FALSE(epitaph);

  std::optional<std::vector<fble::Peer>> peers;
  result_watcher_client->Watch(
      [&](std::vector<fble::Peer> updated) { peers = std::move(updated); });
  RunLoopUntilIdle();
  ASSERT_TRUE(peers.has_value());
  EXPECT_EQ(peers->size(), kMaxPeersPerChannel);
  peers.reset();

  // Additional call to Watch should return the 1 peer that exceeded the channel
  // size limit.
  result_watcher_client->Watch(
      [&](std::vector<fble::Peer> updated) { peers = std::move(updated); });
  RunLoopUntilIdle();
  ASSERT_TRUE(peers.has_value());
  EXPECT_EQ(peers->size(), 1u);

  result_watcher_client.Unbind();
  RunLoopUntilIdle();
  EXPECT_TRUE(scan_stopped);
}

TEST_F(LowEnergyCentralServerTest, ScanResultsMatchPeerFromAnyFilter) {
  const int8_t kRssi = 0;
  // Peer that matches neither filter
  adapter()->peer_cache()->NewPeer(
      bt::DeviceAddress(bt::DeviceAddress::Type::kLEPublic, {0, 0, 0, 0, 0, 0}),
      /*connectable=*/false);

  // Peer that matches filter_0
  bt::gap::Peer* peer_0 = adapter()->peer_cache()->NewPeer(
      bt::DeviceAddress(bt::DeviceAddress::Type::kLEPublic, {1, 0, 0, 0, 0, 0}),
      /*connectable=*/true);
  ASSERT_TRUE(peer_0);
  const auto kAdvData0 =
      bt::StaticByteBuffer(0x02,  // Length
                           0x09,  // AD type: Complete Local Name
                           '0');
  peer_0->MutLe().SetAdvertisingData(
      kRssi, kAdvData0, pw::chrono::SystemClock::time_point());
  // Peer that matches filter_1
  bt::gap::Peer* peer_1 = adapter()->peer_cache()->NewPeer(
      bt::DeviceAddress(bt::DeviceAddress::Type::kLEPublic, {2, 0, 0, 0, 0, 0}),
      /*connectable=*/false);
  ASSERT_TRUE(peer_1);
  const auto kAdvData1 =
      bt::StaticByteBuffer(0x02,  // Length
                           0x09,  // AD type: Complete Local Name
                           '1');
  peer_1->MutLe().SetAdvertisingData(
      kRssi, kAdvData1, pw::chrono::SystemClock::time_point());

  fble::ScanOptions options;
  fble::Filter filter_0;
  filter_0.set_connectable(true);
  filter_0.set_name("0");
  fble::Filter filter_1;
  filter_1.set_connectable(false);
  filter_1.set_name("1");
  std::vector<fble::Filter> filters;
  filters.emplace_back(std::move(filter_0));
  filters.emplace_back(std::move(filter_1));
  options.set_filters(std::move(filters));

  fidl::InterfaceHandle<fble::ScanResultWatcher> result_watcher_handle;
  auto result_watcher_server = result_watcher_handle.NewRequest();
  auto result_watcher_client = result_watcher_handle.Bind();
  std::optional<zx_status_t> epitaph;
  result_watcher_client.set_error_handler(
      [&](zx_status_t cb_epitaph) { epitaph = cb_epitaph; });

  bool scan_stopped = false;
  central_proxy()->Scan(std::move(options),
                        std::move(result_watcher_server),
                        [&]() { scan_stopped = true; });

  RunLoopUntilIdle();
  EXPECT_FALSE(scan_stopped);
  EXPECT_FALSE(epitaph);

  std::optional<std::vector<bt::PeerId>> peers;
  result_watcher_client->Watch([&](std::vector<fble::Peer> updated) {
    peers = std::vector<bt::PeerId>();
    std::transform(updated.begin(),
                   updated.end(),
                   std::back_inserter(*peers),
                   [](auto& p) { return bt::PeerId(p.id().value); });
  });
  RunLoopUntilIdle();
  ASSERT_TRUE(peers.has_value());
  EXPECT_THAT(
      peers.value(),
      ::testing::UnorderedElementsAre(::testing::Eq(peer_0->identifier()),
                                      ::testing::Eq(peer_1->identifier())));
  result_watcher_client.Unbind();
  RunLoopUntilIdle();
  EXPECT_TRUE(scan_stopped);
}

TEST_F(LowEnergyCentralServerTest,
       DiscoveryStartJustAfterScanCanceledShouldBeIgnored) {
  // Pause discovery so that we can cancel scanning before resuming discovery.
  fit::closure start_discovery;
  test_device()->pause_responses_for_opcode(
      bt::hci_spec::kLESetScanEnable, [&](auto resume_set_scan_enable) {
        start_discovery = std::move(resume_set_scan_enable);
      });

  fidl::InterfaceHandle<fble::ScanResultWatcher> result_watcher_handle;
  auto result_watcher_server = result_watcher_handle.NewRequest();
  auto result_watcher_client = result_watcher_handle.Bind();

  bool scan_stopped = false;
  central_proxy()->Scan(ScanOptionsWithEmptyFilter(),
                        std::move(result_watcher_server),
                        [&]() { scan_stopped = true; });

  RunLoopUntilIdle();
  EXPECT_FALSE(scan_stopped);
  EXPECT_TRUE(start_discovery);

  result_watcher_client.Unbind();
  RunLoopUntilIdle();
  EXPECT_TRUE(scan_stopped);

  start_discovery();
  RunLoopUntilIdle();
}

TEST_F(LowEnergyCentralServerTest, ScanFailsToStart) {
  test_device()->SetDefaultResponseStatus(
      bt::hci_spec::kLESetScanEnable,
      pw::bluetooth::emboss::StatusCode::CONTROLLER_BUSY);

  fidl::InterfaceHandle<fble::ScanResultWatcher> result_watcher_handle;
  auto result_watcher_server = result_watcher_handle.NewRequest();
  auto result_watcher_client = result_watcher_handle.Bind();
  std::optional<zx_status_t> epitaph;
  result_watcher_client.set_error_handler(
      [&](zx_status_t cb_epitaph) { epitaph = cb_epitaph; });

  bool scan_stopped = false;
  central_proxy()->Scan(ScanOptionsWithEmptyFilter(),
                        std::move(result_watcher_server),
                        [&]() { scan_stopped = true; });

  RunLoopUntilIdle();
  EXPECT_TRUE(scan_stopped);
  ASSERT_TRUE(epitaph);
  EXPECT_EQ(*epitaph, ZX_ERR_INTERNAL);
}

TEST_F(LowEnergyCentralServerTest, ScanSessionErrorCancelsScan) {
  zx::duration kTestScanPeriod = zx::sec(1);
  pw::chrono::SystemClock::duration kPwTestScanPeriod = std::chrono::seconds(1);
  adapter()->le()->set_scan_period_for_testing(kPwTestScanPeriod);
  std::vector<bool> scan_states;
  test_device()->set_scan_state_callback([&](bool enabled) {
    scan_states.push_back(enabled);
    // Wait for 2 state transitions: -> enabled -> disabled.
    // Then disable restarting scanning, so that an error is sent to sessions.
    if (scan_states.size() == 2u) {
      EXPECT_FALSE(enabled);
      test_device()->SetDefaultResponseStatus(
          bt::hci_spec::kLESetScanEnable,
          pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED);
    }
  });

  fidl::InterfaceHandle<fble::ScanResultWatcher> result_watcher_handle;
  auto result_watcher_server = result_watcher_handle.NewRequest();
  auto result_watcher_client = result_watcher_handle.Bind();
  std::optional<zx_status_t> epitaph;
  result_watcher_client.set_error_handler(
      [&](zx_status_t cb_epitaph) { epitaph = cb_epitaph; });

  bool scan_stopped = false;
  central_proxy()->Scan(ScanOptionsWithEmptyFilter(),
                        std::move(result_watcher_server),
                        [&]() { scan_stopped = true; });
  RunLoopFor(kTestScanPeriod);
  EXPECT_TRUE(scan_stopped);
  ASSERT_TRUE(epitaph);
  EXPECT_EQ(*epitaph, ZX_ERR_INTERNAL);
}

TEST_F(LowEnergyCentralServerTest,
       ScanResultWatcherWatchCalledBeforePreviousWatchReceivedResponse) {
  fidl::InterfaceHandle<fble::ScanResultWatcher> result_watcher_handle;
  auto result_watcher_server = result_watcher_handle.NewRequest();
  auto result_watcher_client = result_watcher_handle.Bind();
  std::optional<zx_status_t> epitaph;
  result_watcher_client.set_error_handler(
      [&](zx_status_t cb_epitaph) { epitaph = cb_epitaph; });

  bool scan_stopped = false;
  central_proxy()->Scan(ScanOptionsWithEmptyFilter(),
                        std::move(result_watcher_server),
                        [&]() { scan_stopped = true; });
  bool watch_response_0 = false;
  result_watcher_client->Watch([&](auto) { watch_response_0 = true; });
  bool watch_response_1 = false;
  result_watcher_client->Watch([&](auto) { watch_response_1 = true; });
  RunLoopUntilIdle();
  EXPECT_FALSE(watch_response_0);
  EXPECT_FALSE(watch_response_1);
  EXPECT_TRUE(scan_stopped);
  ASSERT_TRUE(epitaph);
  EXPECT_EQ(*epitaph, ZX_ERR_CANCELED);
}

TEST_F(LowEnergyCentralServerTest, ConnectToAlreadyConnectedPeerFails) {
  auto* const peer =
      adapter()->peer_cache()->NewPeer(kTestAddr, /*connectable=*/true);
  ASSERT_TRUE(peer);
  test_device()->AddPeer(
      std::make_unique<bt::testing::FakePeer>(kTestAddr, pw_dispatcher()));

  fble::ConnectionPtr conn_client_0;
  std::optional<zx_status_t> epitaph_0;
  conn_client_0.set_error_handler(
      [&](zx_status_t cb_epitaph) { epitaph_0 = cb_epitaph; });

  const fuchsia::bluetooth::PeerId peer_id{peer->identifier().value()};
  fble::ConnectionOptions options_0;
  central_proxy()->Connect(
      peer_id, std::move(options_0), conn_client_0.NewRequest());
  RunLoopUntilIdle();
  EXPECT_FALSE(epitaph_0.has_value());

  fble::ConnectionPtr conn_client_1;
  std::optional<zx_status_t> epitaph_1;
  conn_client_1.set_error_handler(
      [&](zx_status_t cb_epitaph) { epitaph_1 = cb_epitaph; });

  fble::ConnectionOptions options_1;
  central_proxy()->Connect(
      peer_id, std::move(options_1), conn_client_1.NewRequest());
  RunLoopUntilIdle();
  EXPECT_FALSE(epitaph_0.has_value());
  ASSERT_TRUE(epitaph_1.has_value());
  EXPECT_EQ(epitaph_1.value(), ZX_ERR_ALREADY_BOUND);
}

TEST_F(LowEnergyCentralServerTest, ConnectToPeerWithRequestPending) {
  auto* const peer =
      adapter()->peer_cache()->NewPeer(kTestAddr, /*connectable=*/true);
  ASSERT_TRUE(peer);

  auto fake_peer =
      std::make_unique<bt::testing::FakePeer>(kTestAddr, pw_dispatcher());
  fake_peer->force_pending_connect();
  test_device()->AddPeer(std::move(fake_peer));

  fble::ConnectionPtr conn_client_0;
  std::optional<zx_status_t> epitaph_0;
  conn_client_0.set_error_handler(
      [&](zx_status_t cb_epitaph) { epitaph_0 = cb_epitaph; });

  const fuchsia::bluetooth::PeerId peer_id{peer->identifier().value()};
  fble::ConnectionOptions options_0;
  central_proxy()->Connect(
      peer_id, std::move(options_0), conn_client_0.NewRequest());
  RunLoopUntilIdle();
  EXPECT_FALSE(epitaph_0.has_value());

  fble::ConnectionPtr conn_client_1;
  std::optional<zx_status_t> epitaph_1;
  conn_client_1.set_error_handler(
      [&](zx_status_t cb_epitaph) { epitaph_1 = cb_epitaph; });

  fble::ConnectionOptions options_1;
  central_proxy()->Connect(
      peer_id, std::move(options_1), conn_client_1.NewRequest());
  RunLoopUntilIdle();
  EXPECT_FALSE(epitaph_0.has_value());
  ASSERT_TRUE(epitaph_1.has_value());
  EXPECT_EQ(epitaph_1.value(), ZX_ERR_ALREADY_BOUND);
}

TEST_F(LowEnergyCentralServerTest,
       ConnectToPeerAlreadyConnectedInLowEnergyConnectionManager) {
  auto* const peer =
      adapter()->peer_cache()->NewPeer(kTestAddr, /*connectable=*/true);
  ASSERT_TRUE(peer);
  test_device()->AddPeer(
      std::make_unique<bt::testing::FakePeer>(kTestAddr, pw_dispatcher()));

  std::unique_ptr<bt::gap::LowEnergyConnectionHandle> le_conn;
  adapter()->le()->Connect(
      peer->identifier(),
      [&le_conn](auto result) {
        ASSERT_EQ(fit::ok(), result);
        le_conn = std::move(result).value();
      },
      bt::gap::LowEnergyConnectionOptions());
  RunLoopUntilIdle();
  ASSERT_TRUE(le_conn);

  fble::ConnectionPtr conn_client1;
  std::optional<zx_status_t> epitaph1;
  conn_client1.set_error_handler(
      [&](zx_status_t cb_epitaph) { epitaph1 = cb_epitaph; });

  const fuchsia::bluetooth::PeerId kFidlPeerId{peer->identifier().value()};
  fble::ConnectionOptions options1;
  central_proxy()->Connect(
      kFidlPeerId, std::move(options1), conn_client1.NewRequest());
  RunLoopUntilIdle();
  EXPECT_FALSE(epitaph1.has_value());
}

TEST_F(LowEnergyCentralServerTest, ConnectThenPeerDisconnectThenReconnect) {
  auto* const peer =
      adapter()->peer_cache()->NewPeer(kTestAddr, /*connectable=*/true);
  ASSERT_TRUE(peer);
  const fuchsia::bluetooth::PeerId kFidlPeerId{peer->identifier().value()};

  std::unique_ptr<bt::testing::FakePeer> fake_peer =
      std::make_unique<bt::testing::FakePeer>(kTestAddr, pw_dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  fble::ConnectionPtr conn_client_0;
  std::optional<zx_status_t> epitaph_0;
  conn_client_0.set_error_handler(
      [&](zx_status_t cb_epitaph) { epitaph_0 = cb_epitaph; });

  fble::ConnectionOptions options_0;
  central_proxy()->Connect(
      kFidlPeerId, std::move(options_0), conn_client_0.NewRequest());
  RunLoopUntilIdle();
  EXPECT_FALSE(epitaph_0.has_value());

  test_device()->Disconnect(kTestAddr);
  RunLoopUntilIdle();
  EXPECT_TRUE(epitaph_0.has_value());

  fble::ConnectionPtr conn_client_1;
  std::optional<zx_status_t> epitaph_1;
  conn_client_1.set_error_handler(
      [&](zx_status_t cb_epitaph) { epitaph_1 = cb_epitaph; });

  fble::ConnectionOptions options_1;
  central_proxy()->Connect(
      kFidlPeerId, std::move(options_1), conn_client_1.NewRequest());
  RunLoopUntilIdle();
  EXPECT_FALSE(epitaph_1.has_value());
}

TEST_F(LowEnergyCentralServerTest,
       ConnectFailsDueToPeerNotConnectableThenConnectSuceeds) {
  bt::gap::Peer* peer =
      adapter()->peer_cache()->NewPeer(kTestAddr, /*connectable=*/false);
  ASSERT_TRUE(peer);
  auto fake_peer =
      std::make_unique<bt::testing::FakePeer>(kTestAddr, pw_dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  fble::ConnectionPtr conn_client_0;
  std::optional<zx_status_t> epitaph_0;
  conn_client_0.set_error_handler(
      [&](zx_status_t cb_epitaph) { epitaph_0 = cb_epitaph; });

  central_proxy()->Connect(
      fuchsia::bluetooth::PeerId{peer->identifier().value()},
      fble::ConnectionOptions{},
      conn_client_0.NewRequest());
  RunLoopUntilIdle();
  ASSERT_TRUE(epitaph_0.has_value());
  EXPECT_EQ(epitaph_0.value(), ZX_ERR_NOT_CONNECTED);

  // Connect to peer to verify connection state was cleaned up on previous
  // error.
  peer->set_connectable(true);

  fble::ConnectionPtr conn_client_1;
  std::optional<zx_status_t> epitaph_1;
  conn_client_1.set_error_handler(
      [&](zx_status_t cb_epitaph) { epitaph_1 = cb_epitaph; });

  central_proxy()->Connect(
      fuchsia::bluetooth::PeerId{peer->identifier().value()},
      fble::ConnectionOptions{},
      conn_client_1.NewRequest());
  RunLoopUntilIdle();
  EXPECT_FALSE(epitaph_1.has_value());
}

TEST_F(LowEnergyCentralServerTestFakeAdapter,
       ConnectWithConnectionOptionsNonBondableAndServiceFilter) {
  const bt::PeerId kPeerId(1);
  const bt::UUID kServiceUuid(static_cast<uint16_t>(2));

  fble::ConnectionPtr conn_client;
  std::optional<zx_status_t> epitaph;
  conn_client.set_error_handler(
      [&](zx_status_t cb_epitaph) { epitaph = cb_epitaph; });

  fble::ConnectionOptions options;
  options.set_bondable_mode(false);
  options.set_service_filter(fuchsia::bluetooth::Uuid{kServiceUuid.value()});
  central_proxy()->Connect(fuchsia::bluetooth::PeerId{kPeerId.value()},
                           std::move(options),
                           conn_client.NewRequest());
  RunLoopUntilIdle();
  EXPECT_FALSE(epitaph.has_value());

  auto& connections = adapter()->fake_le()->connections();
  auto conn_iter = connections.find(kPeerId);
  ASSERT_NE(conn_iter, connections.end());
  EXPECT_EQ(conn_iter->second.options.bondable_mode,
            bt::sm::BondableMode::NonBondable);
  ASSERT_TRUE(conn_iter->second.options.service_uuid.has_value());
  EXPECT_EQ(conn_iter->second.options.service_uuid, kServiceUuid);
  EXPECT_EQ(conn_iter->second.options.auto_connect, false);
}

TEST_P(LowEnergyCentralServerTestFakeAdapterBoolParam,
       ConnectConnectionOptionsBondable) {
  const bt::PeerId kPeerId(1);

  fble::ConnectionPtr conn_client;
  std::optional<zx_status_t> epitaph;
  conn_client.set_error_handler(
      [&](zx_status_t cb_epitaph) { epitaph = cb_epitaph; });

  fble::ConnectionOptions options;
  // Bondable mode option defaults to true, so behavior shouldn't change whether
  // or not it is explicitly set to true.
  if (GetParam()) {
    options.set_bondable_mode(true);
  }
  central_proxy()->Connect(fuchsia::bluetooth::PeerId{kPeerId.value()},
                           std::move(options),
                           conn_client.NewRequest());
  RunLoopUntilIdle();
  EXPECT_FALSE(epitaph.has_value());

  auto& connections = adapter()->fake_le()->connections();
  auto conn_iter = connections.find(kPeerId);
  ASSERT_NE(conn_iter, connections.end());
  EXPECT_EQ(conn_iter->second.options.bondable_mode,
            bt::sm::BondableMode::Bondable);
}

TEST_F(LowEnergyCentralServerTestFakeAdapter,
       SyncToPeriodicAdvertisementWithNoConfigSucceeds) {
  fble::CentralSyncToPeriodicAdvertisingRequest request;
  request.set_peer_id(::fuchsia::bluetooth::PeerId{kPeerId.value()});
  request.set_advertising_sid(kAdvertisingSid);
  fidl::InterfaceHandle<fble::PeriodicAdvertisingSync> sync_handle;
  request.set_sync(sync_handle.NewRequest());

  fidl::InterfacePtr<fble::PeriodicAdvertisingSync> sync_client =
      sync_handle.Bind();
  std::optional<zx_status_t> fidl_error;
  sync_client.set_error_handler(
      [&](zx_status_t status) { fidl_error = status; });
  int on_established_count = 0;
  sync_client.events().OnEstablished = [&](auto) { ++on_established_count; };
  int on_error_count = 0;
  sync_client.events().OnError = [&](auto) { ++on_error_count; };

  central_proxy()->SyncToPeriodicAdvertising(std::move(request));
  RunLoopUntilIdle();
  ASSERT_FALSE(fidl_error.has_value());
  EXPECT_EQ(on_established_count, 0);
  EXPECT_EQ(on_error_count, 0);

  ASSERT_EQ(adapter()->fake_le()->periodic_advertisement_syncs().size(), 1u);
  const bt::gap::testing::FakeAdapter::FakeLowEnergy::PeriodicAdvertisementSync&
      sync =
          adapter()->fake_le()->periodic_advertisement_syncs().begin()->second;
  EXPECT_EQ(sync.options.filter_duplicates, true);
  sync.delegate.OnSyncEstablished(kSyncId, kSyncParameters);
  RunLoopUntilIdle();
  EXPECT_EQ(on_established_count, 1);
  EXPECT_EQ(on_error_count, 0);

  sync_client.Unbind();
  RunLoopUntilIdle();
  EXPECT_EQ(adapter()->fake_le()->periodic_advertisement_syncs().size(), 0u);
}

TEST_P(LowEnergyCentralServerTestFakeAdapterBoolParam,
       SyncToPeriodicAdvertisementSuccessWithConfigSucceeds) {
  bool expected_filter_duplicates = GetParam();

  fble::CentralSyncToPeriodicAdvertisingRequest request;
  request.set_peer_id(::fuchsia::bluetooth::PeerId{kPeerId.value()});
  request.set_advertising_sid(kAdvertisingSid);
  fidl::InterfaceHandle<fble::PeriodicAdvertisingSync> sync_handle;
  request.set_sync(sync_handle.NewRequest());
  fble::PeriodicAdvertisingSyncConfiguration config;
  config.set_filter_duplicates(expected_filter_duplicates);
  request.set_config(std::move(config));

  fidl::InterfacePtr<fble::PeriodicAdvertisingSync> sync_client =
      sync_handle.Bind();
  std::optional<zx_status_t> fidl_error;
  sync_client.set_error_handler(
      [&](zx_status_t status) { fidl_error = status; });
  int on_established_count = 0;
  sync_client.events().OnEstablished = [&](auto) { ++on_established_count; };
  int on_error_count = 0;
  sync_client.events().OnError = [&](auto) { ++on_error_count; };

  central_proxy()->SyncToPeriodicAdvertising(std::move(request));
  RunLoopUntilIdle();
  ASSERT_FALSE(fidl_error.has_value());
  EXPECT_EQ(on_established_count, 0);
  EXPECT_EQ(on_error_count, 0);

  ASSERT_EQ(adapter()->fake_le()->periodic_advertisement_syncs().size(), 1u);
  const bt::gap::testing::FakeAdapter::FakeLowEnergy::PeriodicAdvertisementSync&
      sync =
          adapter()->fake_le()->periodic_advertisement_syncs().begin()->second;
  EXPECT_EQ(expected_filter_duplicates, sync.options.filter_duplicates);
  sync.delegate.OnSyncEstablished(kSyncId, kSyncParameters);
  RunLoopUntilIdle();
  EXPECT_EQ(on_established_count, 1);
  EXPECT_EQ(on_error_count, 0);

  sync_client.Unbind();
  RunLoopUntilIdle();
}

TEST_F(LowEnergyCentralServerTestFakeAdapter,
       SyncToPeriodicAdvertisementRequestMissingPeerId) {
  fble::CentralSyncToPeriodicAdvertisingRequest request;
  request.set_advertising_sid(kAdvertisingSid);
  fidl::InterfaceHandle<fble::PeriodicAdvertisingSync> sync_handle;
  request.set_sync(sync_handle.NewRequest());

  fidl::InterfacePtr<fble::PeriodicAdvertisingSync> sync_client =
      sync_handle.Bind();
  std::optional<zx_status_t> fidl_error;
  sync_client.set_error_handler(
      [&](zx_status_t status) { fidl_error = status; });

  central_proxy()->SyncToPeriodicAdvertising(std::move(request));
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_error.has_value());
  EXPECT_EQ(fidl_error.value(), ZX_ERR_INVALID_ARGS);
}

TEST_F(LowEnergyCentralServerTestFakeAdapter,
       SyncToPeriodicAdvertisementRequestMissingSid) {
  fble::CentralSyncToPeriodicAdvertisingRequest request;
  request.set_peer_id(::fuchsia::bluetooth::PeerId{kPeerId.value()});
  fidl::InterfaceHandle<fble::PeriodicAdvertisingSync> sync_handle;
  request.set_sync(sync_handle.NewRequest());

  fidl::InterfacePtr<fble::PeriodicAdvertisingSync> sync_client =
      sync_handle.Bind();
  std::optional<zx_status_t> fidl_error;
  sync_client.set_error_handler(
      [&](zx_status_t status) { fidl_error = status; });

  central_proxy()->SyncToPeriodicAdvertising(std::move(request));
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_error.has_value());
  EXPECT_EQ(fidl_error.value(), ZX_ERR_INVALID_ARGS);
}

TEST_F(LowEnergyCentralServerTestFakeAdapter,
       SyncToPeriodicAdvertisementRequestMissingSync) {
  fble::CentralSyncToPeriodicAdvertisingRequest request;
  request.set_peer_id(::fuchsia::bluetooth::PeerId{kPeerId.value()});
  request.set_advertising_sid(kAdvertisingSid);
  central_proxy()->SyncToPeriodicAdvertising(std::move(request));
  RunLoopUntilIdle();
}

TEST_F(LowEnergyCentralServerTestFakeAdapter,
       SyncToPeriodicAdvertisementFailsOnServerCreation) {
  adapter()->fake_le()->set_sync_to_periodic_advertisement_error(
      bt::hci::Error(bt::HostError::kNotSupported));

  fble::CentralSyncToPeriodicAdvertisingRequest request;
  request.set_peer_id(::fuchsia::bluetooth::PeerId{kPeerId.value()});
  request.set_advertising_sid(kAdvertisingSid);
  fidl::InterfaceHandle<fble::PeriodicAdvertisingSync> sync_handle;
  request.set_sync(sync_handle.NewRequest());

  fidl::InterfacePtr<fble::PeriodicAdvertisingSync> sync_client =
      sync_handle.Bind();
  std::optional<zx_status_t> fidl_error;
  sync_client.set_error_handler(
      [&](zx_status_t status) { fidl_error = status; });
  int on_established_count = 0;
  sync_client.events().OnEstablished = [&](auto) { ++on_established_count; };
  std::optional<fble::PeriodicAdvertisingSyncError> sync_error;
  sync_client.events().OnError = [&](fble::PeriodicAdvertisingSyncError error) {
    sync_error = error;
  };

  central_proxy()->SyncToPeriodicAdvertising(std::move(request));
  RunLoopUntilIdle();
  EXPECT_EQ(on_established_count, 0);
  ASSERT_TRUE(sync_error.has_value());
  EXPECT_EQ(sync_error.value(),
            fble::PeriodicAdvertisingSyncError::NOT_SUPPORTED_LOCAL);
  ASSERT_TRUE(fidl_error.has_value());
  EXPECT_EQ(fidl_error.value(), ZX_ERR_NOT_SUPPORTED);
}

INSTANTIATE_TEST_SUITE_P(LowEnergyCentralServerTestFakeAdapterBoolParamTests,
                         LowEnergyCentralServerTestFakeAdapterBoolParam,
                         ::testing::Bool());

}  // namespace
}  // namespace bthost
