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

#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_connection_manager.h"

#include <gmock/gmock.h>
#include <lib/fit/function.h>
#include <pw_assert/check.h>

#include <cstddef>
#include <limits>
#include <memory>
#include <vector>

#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/common/device_address.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/common/random.h"
#include "pw_bluetooth_sapphire/internal/host/gap/gap.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_address_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gap/peer.h"
#include "pw_bluetooth_sapphire/internal/host/gap/peer_cache.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/fake_layer.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/defaults.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/util.h"
#include "pw_bluetooth_sapphire/internal/host/hci/advertising_packet_filter.h"
#include "pw_bluetooth_sapphire/internal/host/hci/fake_local_address_delegate.h"
#include "pw_bluetooth_sapphire/internal/host/hci/legacy_low_energy_scanner.h"
#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_connection.h"
#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_connector.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/fake_channel.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/fake_l2cap.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/types.h"
#include "pw_bluetooth_sapphire/internal/host/sm/test_security_manager.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"
#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_controller.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_peer.h"
#include "pw_bluetooth_sapphire/internal/host/testing/inspect.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_packets.h"
#include "pw_bluetooth_sapphire/internal/host/transport/fake_acl_connection.h"

namespace bt::gap {
namespace {

using namespace inspect::testing;

using bt::sm::BondableMode;
using bt::testing::FakeController;
using bt::testing::FakePeer;

using TestingBase = bt::testing::FakeDispatcherControllerTest<FakeController>;
using TestSm = sm::testing::TestSecurityManager;
using TestSmFactory = sm::testing::TestSecurityManagerFactory;
using ConnectionResult = LowEnergyConnectionManager::ConnectionResult;

const bt::sm::LTK kLTK;

const DeviceAddress kAddress0(DeviceAddress::Type::kLEPublic, {1});
const DeviceAddress kAddrAlias0(DeviceAddress::Type::kBREDR, kAddress0.value());
const DeviceAddress kAddress1(DeviceAddress::Type::kLERandom, {2});
const DeviceAddress kAddress2(DeviceAddress::Type::kBREDR, {3});
const DeviceAddress kAddress3(DeviceAddress::Type::kLEPublic, {4});
const DeviceAddress kAdapterAddress(DeviceAddress::Type::kLEPublic, {9});

const size_t kLEMaxNumPackets = 10;
const hci::DataBufferInfo kLEDataBufferInfo(hci_spec::kMaxACLPayloadSize,
                                            kLEMaxNumPackets);

constexpr std::array kConnectDelays = {
    std::chrono::seconds(0), std::chrono::seconds(2), std::chrono::seconds(4)};

const LowEnergyConnectionOptions kConnectionOptions{};

class LowEnergyConnectionManagerTest : public TestingBase {
 public:
  LowEnergyConnectionManagerTest() = default;
  ~LowEnergyConnectionManagerTest() override = default;

 protected:
  void SetUp() override {
    TestingBase::SetUp();

    // Initialize with LE buffers only.
    TestingBase::InitializeACLDataChannel(hci::DataBufferInfo(),
                                          kLEDataBufferInfo);

    FakeController::Settings settings;
    settings.ApplyLegacyLEConfig();
    test_device()->set_settings(settings);

    peer_cache_ = std::make_unique<PeerCache>(dispatcher());
    l2cap_ = std::make_unique<l2cap::testing::FakeL2cap>(dispatcher());

    const hci::CommandChannel::WeakPtr cmd_weak = cmd_channel()->AsWeakPtr();

    connector_ = std::make_unique<hci::LowEnergyConnector>(
        transport()->GetWeakPtr(),
        &addr_delegate_,
        dispatcher(),
        fit::bind_member<&LowEnergyConnectionManagerTest::OnIncomingConnection>(
            this));

    gatt_ = std::make_unique<gatt::testing::FakeLayer>(dispatcher());
    sm_factory_ = std::make_unique<TestSmFactory>();

    hci::AdvertisingPacketFilter::Config packet_filter_config(
        false,
        0,
        hci::AdvertisingPacketFilter::Config::DeliveryMode::kImmediate);

    address_manager_ = std::make_unique<LowEnergyAddressManager>(
        kAdapterAddress,
        /*delegate=*/[] { return false; },
        cmd_weak,
        dispatcher());
    scanner_ =
        std::make_unique<hci::LegacyLowEnergyScanner>(address_manager_.get(),
                                                      packet_filter_config,
                                                      transport()->GetWeakPtr(),
                                                      dispatcher());
    discovery_manager_ = std::make_unique<LowEnergyDiscoveryManager>(
        scanner_.get(), peer_cache_.get(), packet_filter_config, dispatcher());
    conn_mgr_ = std::make_unique<LowEnergyConnectionManager>(
        transport()->GetWeakPtr(),
        &addr_delegate_,
        connector_.get(),
        peer_cache_.get(),
        l2cap_.get(),
        gatt_->GetWeakPtr(),
        discovery_manager_->GetWeakPtr(),
        fit::bind_member<&TestSmFactory::CreateSm>(sm_factory_.get()),
        adapter_state_,
        dispatcher(),
        lease_provider());

    test_device()->set_connection_state_callback(
        fit::bind_member<
            &LowEnergyConnectionManagerTest::OnConnectionStateChanged>(this));
  }

  void TearDown() override {
    if (conn_mgr_) {
      conn_mgr_ = nullptr;
    }
    discovery_manager_ = nullptr;
    scanner_ = nullptr;
    address_manager_ = nullptr;
    gatt_ = nullptr;
    connector_ = nullptr;
    peer_cache_ = nullptr;

    l2cap_ = nullptr;

    TestingBase::TearDown();
  }

  // Deletes |conn_mgr_|.
  void DeleteConnMgr() { conn_mgr_ = nullptr; }

  PeerCache* peer_cache() const { return peer_cache_.get(); }
  LowEnergyConnectionManager* conn_mgr() const { return conn_mgr_.get(); }
  l2cap::testing::FakeL2cap* fake_l2cap() const { return l2cap_.get(); }
  gatt::testing::FakeLayer* fake_gatt() { return gatt_.get(); }
  LowEnergyDiscoveryManager* discovery_mgr() {
    return discovery_manager_.get();
  }

  // Addresses of currently connected fake peers.
  using PeerList = std::unordered_set<DeviceAddress>;
  const PeerList& connected_peers() const { return connected_peers_; }

  // Addresses of peers with a canceled connection attempt.
  const PeerList& canceled_peers() const { return canceled_peers_; }

  std::unique_ptr<hci::LowEnergyConnection> MoveLastRemoteInitiated() {
    return std::move(last_remote_initiated_);
  }

  TestSm::WeakPtr TestSmByHandle(hci_spec::ConnectionHandle handle) {
    return sm_factory_->GetTestSm(handle);
  }

  AdapterState& adapter_state() { return adapter_state_; }

 private:
  // Called by |connector_| when a new remote initiated connection is received.
  void OnIncomingConnection(
      hci_spec::ConnectionHandle handle,
      pw::bluetooth::emboss::ConnectionRole role,
      const DeviceAddress& peer_address,
      const hci_spec::LEConnectionParameters& conn_params) {
    DeviceAddress local_address(DeviceAddress::Type::kLEPublic,
                                {3, 2, 1, 1, 2, 3});

    // Create a production connection object that can interact with the fake
    // controller.
    last_remote_initiated_ =
        std::make_unique<hci::LowEnergyConnection>(handle,
                                                   local_address,
                                                   peer_address,
                                                   conn_params,
                                                   role,
                                                   transport()->GetWeakPtr());
  }

  // Called by FakeController on connection events.
  void OnConnectionStateChanged(const DeviceAddress& address,
                                hci_spec::ConnectionHandle handle,
                                bool connected,
                                bool canceled) {
    bt_log(DEBUG,
           "gap-test",
           "OnConnectionStateChanged: %s (handle: %#.4x) (connected: %s) "
           "(canceled: %s):\n",
           address.ToString().c_str(),
           handle,
           (connected ? "true" : "false"),
           (canceled ? "true" : "false"));
    if (canceled) {
      canceled_peers_.insert(address);
    } else if (connected) {
      PW_DCHECK(connected_peers_.find(address) == connected_peers_.end());
      connected_peers_.insert(address);
    } else {
      PW_DCHECK(connected_peers_.find(address) != connected_peers_.end());
      connected_peers_.erase(address);
    }
  }

  std::unique_ptr<l2cap::testing::FakeL2cap> l2cap_;
  hci::FakeLocalAddressDelegate addr_delegate_{dispatcher()};
  std::unique_ptr<PeerCache> peer_cache_;
  std::unique_ptr<hci::LowEnergyConnector> connector_;
  std::unique_ptr<gatt::testing::FakeLayer> gatt_;
  std::unique_ptr<TestSmFactory> sm_factory_;
  std::unique_ptr<hci::LegacyLowEnergyScanner> scanner_;
  std::unique_ptr<LowEnergyAddressManager> address_manager_;
  std::unique_ptr<LowEnergyDiscoveryManager> discovery_manager_;
  std::unique_ptr<LowEnergyConnectionManager> conn_mgr_;

  AdapterState adapter_state_ = {};

  // The most recent remote-initiated connection reported by |connector_|.
  std::unique_ptr<hci::LowEnergyConnection> last_remote_initiated_;

  PeerList connected_peers_;
  PeerList canceled_peers_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LowEnergyConnectionManagerTest);
};

using GAP_LowEnergyConnectionManagerTest = LowEnergyConnectionManagerTest;

TEST_F(LowEnergyConnectionManagerTest, ConnectUnknownPeer) {
  constexpr PeerId kUnknownId(1);
  ConnectionResult result = fit::ok(nullptr);
  conn_mgr()->Connect(
      kUnknownId,
      [&result](auto res) { result = std::move(res); },
      kConnectionOptions);
  ASSERT_TRUE(result.is_error());
  EXPECT_EQ(HostError::kNotFound, result.error_value());
}

TEST_F(LowEnergyConnectionManagerTest, ConnectClassicPeer) {
  auto* peer = peer_cache()->NewPeer(kAddress2, /*connectable=*/true);
  ConnectionResult result = fit::ok(nullptr);
  conn_mgr()->Connect(
      peer->identifier(),
      [&result](auto res) { result = std::move(res); },
      kConnectionOptions);
  ASSERT_TRUE(result.is_error());
  EXPECT_EQ(HostError::kNotFound, result.error_value());
}

TEST_F(LowEnergyConnectionManagerTest, ConnectNonConnectablePeer) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/false);
  ConnectionResult result = fit::ok(nullptr);
  conn_mgr()->Connect(
      peer->identifier(),
      [&result](auto res) { result = std::move(res); },
      kConnectionOptions);
  ASSERT_TRUE(result.is_error());
  EXPECT_EQ(HostError::kNotFound, result.error_value());
}

// An error is received via the HCI Command cb_status event
TEST_F(LowEnergyConnectionManagerTest, ConnectSinglePeerErrorStatus) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  fake_peer->set_connect_status(
      pw::bluetooth::emboss::StatusCode::CONNECTION_FAILED_TO_BE_ESTABLISHED);
  test_device()->AddPeer(std::move(fake_peer));

  ASSERT_TRUE(peer->le());
  EXPECT_EQ(Peer::ConnectionState::kNotConnected,
            peer->le()->connection_state());

  ConnectionResult result = fit::ok(nullptr);
  auto callback = [&result](auto res) { result = std::move(res); };

  conn_mgr()->Connect(peer->identifier(), callback, kConnectionOptions);
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer->le()->connection_state());

  RunUntilIdle();

  ASSERT_TRUE(result.is_error());
  EXPECT_EQ(HostError::kFailed, result.error_value());
  EXPECT_EQ(Peer::ConnectionState::kNotConnected,
            peer->le()->connection_state());
}

// LE Connection Complete event reports error
TEST_F(LowEnergyConnectionManagerTest, ConnectSinglePeerFailure) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  fake_peer->set_connect_response(
      pw::bluetooth::emboss::StatusCode::CONNECTION_FAILED_TO_BE_ESTABLISHED);
  test_device()->AddPeer(std::move(fake_peer));

  ConnectionResult result = fit::ok(nullptr);
  auto callback = [&result](auto res) { result = std::move(res); };

  conn_mgr()->Connect(peer->identifier(), callback, kConnectionOptions);
  ASSERT_TRUE(peer->le());
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer->le()->connection_state());

  RunUntilIdle();

  ASSERT_TRUE(result.is_error());
  EXPECT_EQ(HostError::kFailed, result.error_value());
  EXPECT_EQ(Peer::ConnectionState::kNotConnected,
            peer->le()->connection_state());
}

TEST_F(LowEnergyConnectionManagerTest, ConnectSinglePeerScanTimeout) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);

  // We add no fake peers to cause the scan to time out.

  ConnectionResult result = fit::ok(nullptr);
  auto callback = [&result](auto res) { result = std::move(res); };

  conn_mgr()->Connect(peer->identifier(), callback, kConnectionOptions);
  ASSERT_TRUE(peer->le());
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer->le()->connection_state());

  RunFor(kLEGeneralCepScanTimeout);

  ASSERT_TRUE(result.is_error());
  EXPECT_EQ(HostError::kTimedOut, result.error_value());
  EXPECT_EQ(Peer::ConnectionState::kNotConnected,
            peer->le()->connection_state());
}

TEST_F(LowEnergyConnectionManagerTest, ConnectSinglePeerAlreadyInScanCache) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  // Ensure peer is in scan cache by doing active discovery.
  LowEnergyDiscoverySessionPtr session;
  discovery_mgr()->StartDiscovery(
      /*active=*/true, {}, [&session](auto cb_session) {
        session = std::move(cb_session);
      });
  RunUntilIdle();

  ConnectionResult result = fit::ok(nullptr);
  auto callback = [&result](auto res) { result = std::move(res); };

  conn_mgr()->Connect(peer->identifier(), callback, kConnectionOptions);
  RunUntilIdle();
  ASSERT_EQ(fit::ok(), result);
}

TEST_F(LowEnergyConnectionManagerTest, ConnectSinglePeerRequestTimeout) {
  constexpr pw::chrono::SystemClock::duration kTestRequestTimeout =
      std::chrono::seconds(20);

  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);

  // Add a fake peer so that scan succeeds but connect stalls.
  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  fake_peer->set_force_pending_connect(true);
  test_device()->AddPeer(std::move(fake_peer));

  ConnectionResult result = fit::ok(nullptr);
  auto callback = [&result](auto res) { result = std::move(res); };

  conn_mgr()->set_request_timeout_for_testing(kTestRequestTimeout);
  conn_mgr()->Connect(peer->identifier(), callback, kConnectionOptions);
  ASSERT_TRUE(peer->le());
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer->le()->connection_state());

  RunFor(kTestRequestTimeout);
  RunUntilIdle();

  ASSERT_TRUE(result.is_error());
  EXPECT_EQ(HostError::kTimedOut, result.error_value());
  EXPECT_EQ(Peer::ConnectionState::kNotConnected,
            peer->le()->connection_state());
}

// Tests that an entry in the cache does not expire while a connection attempt
// is pending.
TEST_F(LowEnergyConnectionManagerTest, PeerDoesNotExpireDuringTimeout) {
  // Set a connection timeout that is longer than the PeerCache expiry
  // timeout.
  // TODO(fxbug.dev/42087236): Consider configuring the cache timeout explicitly
  // rather than relying on the kCacheTimeout constant.
  constexpr pw::chrono::SystemClock::duration kTestRequestTimeout =
      kCacheTimeout + std::chrono::seconds(1);
  conn_mgr()->set_request_timeout_for_testing(kTestRequestTimeout);

  // Note: Use a random address so that the peer becomes temporary upon failure.
  auto* peer = peer_cache()->NewPeer(kAddress1, /*connectable=*/true);
  EXPECT_TRUE(peer->temporary());

  ConnectionResult result = fit::ok(nullptr);
  auto callback = [&result](auto res) { result = std::move(res); };

  conn_mgr()->Connect(peer->identifier(), callback, kConnectionOptions);
  ASSERT_TRUE(peer->le());
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer->le()->connection_state());
  EXPECT_FALSE(peer->temporary());

  RunFor(kTestRequestTimeout);
  ASSERT_TRUE(result.is_error());
  EXPECT_EQ(HostError::kTimedOut, result.error_value());
  EXPECT_EQ(peer, peer_cache()->FindByAddress(kAddress1));
  EXPECT_EQ(Peer::ConnectionState::kNotConnected,
            peer->le()->connection_state());
  EXPECT_TRUE(peer->temporary());
}

TEST_F(LowEnergyConnectionManagerTest, PeerDoesNotExpireDuringDelayedConnect) {
  // Make the connection resolve after a delay that is longer than the cache
  // timeout.
  constexpr pw::chrono::SystemClock::duration kConnectionDelay =
      kCacheTimeout + std::chrono::seconds(1);
  FakeController::Settings settings;
  settings.ApplyLegacyLEConfig();
  settings.le_connection_delay = kConnectionDelay;
  test_device()->set_settings(settings);

  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto id = peer->identifier();
  EXPECT_TRUE(peer->temporary());

  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  // Make sure the connection request doesn't time out while waiting for a
  // response.
  conn_mgr()->set_request_timeout_for_testing(kConnectionDelay +
                                              std::chrono::seconds(1));

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto callback = [&conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
    ASSERT_TRUE(conn_handle);
    EXPECT_TRUE(conn_handle->active());
  };
  conn_mgr()->Connect(id, callback, kConnectionOptions);
  ASSERT_TRUE(peer->le());
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer->le()->connection_state());

  RunFor(kConnectionDelay);
  ASSERT_TRUE(conn_handle);

  // The peer should not have expired during this time.
  peer = peer_cache()->FindByAddress(kAddress0);
  ASSERT_TRUE(peer);
  EXPECT_EQ(id, peer->identifier());
  EXPECT_TRUE(peer->connected());
  EXPECT_FALSE(peer->temporary());
}

// Successful connection to single peer
TEST_F(LowEnergyConnectionManagerTest, ConnectSinglePeer) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  EXPECT_TRUE(peer->temporary());

  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  // Use a StaticPacket so that the packet is copied.
  std::optional<
      StaticPacket<pw::bluetooth::emboss::LECreateConnectionCommandWriter>>
      connect_params;
  test_device()->set_le_create_connection_command_callback(
      [&](pw::bluetooth::emboss::LECreateConnectionCommandView params) {
        connect_params.emplace(params);
      });

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto callback = [&conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
    EXPECT_TRUE(conn_handle->active());
  };

  EXPECT_TRUE(connected_peers().empty());
  conn_mgr()->Connect(peer->identifier(), callback, kConnectionOptions);
  ASSERT_TRUE(peer->le());
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer->le()->connection_state());

  RunUntilIdle();

  EXPECT_EQ(1u, connected_peers().size());
  EXPECT_EQ(1u, connected_peers().count(kAddress0));

  ASSERT_TRUE(conn_handle);
  EXPECT_TRUE(conn_handle->active());
  EXPECT_EQ(peer->identifier(), conn_handle->peer_identifier());
  EXPECT_FALSE(peer->temporary());
  EXPECT_EQ(Peer::ConnectionState::kConnected, peer->le()->connection_state());
  ASSERT_TRUE(connect_params);
  EXPECT_EQ(connect_params->view().le_scan_interval().Read(),
            kLEScanFastInterval);
  EXPECT_EQ(connect_params->view().le_scan_window().Read(), kLEScanFastWindow);
}

struct TestObject final {
  explicit TestObject(bool* d) : deleted(d) {
    PW_DCHECK(deleted);
    *deleted = false;
  }

  ~TestObject() { *deleted = true; }

  bool* deleted;
};

TEST_F(LowEnergyConnectionManagerTest, DeleteRefInClosedCallback) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));

  bool deleted = false;
  auto obj = std::make_shared<TestObject>(&deleted);
  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  int closed_count = 0;
  auto closed_cb = [&, obj = std::move(obj)] {
    closed_count++;
    conn_handle = nullptr;

    // The object should remain alive for the duration of this callback.
    EXPECT_FALSE(deleted);
  };

  auto success_cb = [&conn_handle, &closed_cb](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
    conn_handle->set_closed_callback(std::move(closed_cb));
  };

  conn_mgr()->Connect(peer->identifier(), success_cb, kConnectionOptions);
  RunUntilIdle();

  ASSERT_TRUE(conn_handle);
  ASSERT_TRUE(conn_handle->active());

  // This will trigger the closed callback.
  EXPECT_TRUE(conn_mgr()->Disconnect(peer->identifier()));
  RunUntilIdle();

  EXPECT_EQ(1, closed_count);
  EXPECT_TRUE(connected_peers().empty());
  EXPECT_FALSE(conn_handle);

  // The object should be deleted.
  EXPECT_TRUE(deleted);
}

TEST_F(LowEnergyConnectionManagerTest, ReleaseRef) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto callback = [&conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
    EXPECT_TRUE(conn_handle->active());
  };

  EXPECT_TRUE(connected_peers().empty());
  conn_mgr()->Connect(peer->identifier(), callback, kConnectionOptions);

  RunUntilIdle();

  EXPECT_EQ(1u, connected_peers().size());
  ASSERT_TRUE(peer->le());
  EXPECT_EQ(Peer::ConnectionState::kConnected, peer->le()->connection_state());

  ASSERT_TRUE(conn_handle);
  conn_handle = nullptr;

  RunUntilIdle();

  EXPECT_TRUE(connected_peers().empty());
  EXPECT_EQ(Peer::ConnectionState::kNotConnected,
            peer->le()->connection_state());
}

TEST_F(LowEnergyConnectionManagerTest, OnePeerTwoPendingRequestsBothFail) {
  constexpr size_t kRequestCount = 2;

  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  fake_peer->set_connect_response(
      pw::bluetooth::emboss::StatusCode::CONNECTION_FAILED_TO_BE_ESTABLISHED);
  test_device()->AddPeer(std::move(fake_peer));

  std::vector<ConnectionResult> results;

  auto callback = [&results](auto result) {
    results.push_back(std::move(result));
  };

  for (size_t i = 0; i < kRequestCount; ++i) {
    conn_mgr()->Connect(peer->identifier(), callback, kConnectionOptions);
  }

  RunUntilIdle();

  EXPECT_EQ(kRequestCount, results.size());
  for (size_t i = 0; i < results.size(); ++i) {
    ASSERT_TRUE(results.at(i).is_error());
    EXPECT_EQ(HostError::kFailed, results.at(i).error_value())
        << "request count: " << i + 1;
  }
}

TEST_F(LowEnergyConnectionManagerTest, OnePeerManyPendingRequests) {
  constexpr size_t kRequestCount = 50;

  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  std::vector<std::unique_ptr<LowEnergyConnectionHandle>> conn_handles;
  auto callback = [&conn_handles](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handles.emplace_back(std::move(result).value());
  };

  for (size_t i = 0; i < kRequestCount; ++i) {
    conn_mgr()->Connect(peer->identifier(), callback, kConnectionOptions);
  }

  RunUntilIdle();

  EXPECT_EQ(1u, connected_peers().size());
  EXPECT_EQ(1u, connected_peers().count(kAddress0));

  EXPECT_EQ(kRequestCount, conn_handles.size());
  for (size_t i = 0; i < kRequestCount; ++i) {
    ASSERT_TRUE(conn_handles[i]);
    EXPECT_TRUE(conn_handles[i]->active());
    EXPECT_EQ(peer->identifier(), conn_handles[i]->peer_identifier());
  }

  // Release one reference. The rest should be active.
  conn_handles[0] = nullptr;
  for (size_t i = 1; i < kRequestCount; ++i)
    EXPECT_TRUE(conn_handles[i]->active());

  // Release all but one reference.
  for (size_t i = 1; i < kRequestCount - 1; ++i)
    conn_handles[i] = nullptr;
  EXPECT_TRUE(conn_handles[kRequestCount - 1]->active());

  // Drop the last reference.
  conn_handles[kRequestCount - 1] = nullptr;

  RunUntilIdle();

  EXPECT_TRUE(connected_peers().empty());
}

TEST_F(LowEnergyConnectionManagerTest, AddRefAfterConnection) {
  constexpr size_t kRefCount = 50;

  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  std::vector<std::unique_ptr<LowEnergyConnectionHandle>> conn_handles;
  auto callback = [&conn_handles](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handles.emplace_back(std::move(result).value());
  };

  conn_mgr()->Connect(peer->identifier(), callback, kConnectionOptions);

  RunUntilIdle();

  EXPECT_EQ(1u, connected_peers().size());
  EXPECT_EQ(1u, connected_peers().count(kAddress0));
  EXPECT_EQ(1u, conn_handles.size());

  // Add new references.
  for (size_t i = 1; i < kRefCount; ++i) {
    conn_mgr()->Connect(peer->identifier(), callback, kConnectionOptions);
    RunUntilIdle();
  }

  EXPECT_EQ(1u, connected_peers().size());
  EXPECT_EQ(1u, connected_peers().count(kAddress0));
  EXPECT_EQ(kRefCount, conn_handles.size());

  // Disconnect.
  conn_handles.clear();

  RunUntilIdle();

  EXPECT_TRUE(connected_peers().empty());
}

TEST_F(LowEnergyConnectionManagerTest, PendingRequestsOnTwoPeers) {
  auto* peer0 = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto* peer1 = peer_cache()->NewPeer(kAddress1, /*connectable=*/true);

  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress1, dispatcher()));

  std::vector<std::unique_ptr<LowEnergyConnectionHandle>> conn_handles;
  auto callback = [&conn_handles](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handles.emplace_back(std::move(result).value());
  };

  conn_mgr()->Connect(peer0->identifier(), callback, kConnectionOptions);
  conn_mgr()->Connect(peer1->identifier(), callback, kConnectionOptions);

  RunUntilIdle();

  EXPECT_EQ(2u, connected_peers().size());
  EXPECT_EQ(1u, connected_peers().count(kAddress0));
  EXPECT_EQ(1u, connected_peers().count(kAddress1));

  ASSERT_EQ(2u, conn_handles.size());
  ASSERT_TRUE(conn_handles[0]);
  ASSERT_TRUE(conn_handles[1]);
  EXPECT_EQ(peer0->identifier(), conn_handles[0]->peer_identifier());
  EXPECT_EQ(peer1->identifier(), conn_handles[1]->peer_identifier());

  // |peer1| should disconnect first.
  conn_handles[1] = nullptr;

  RunUntilIdle();

  EXPECT_EQ(1u, connected_peers().size());
  EXPECT_EQ(1u, connected_peers().count(kAddress0));

  conn_handles.clear();

  RunUntilIdle();
  EXPECT_TRUE(connected_peers().empty());
}

TEST_F(LowEnergyConnectionManagerTest, PendingRequestsOnTwoPeersOneFails) {
  auto* peer0 = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto* peer1 = peer_cache()->NewPeer(kAddress1, /*connectable=*/true);

  auto fake_peer0 = std::make_unique<FakePeer>(kAddress0, dispatcher());
  fake_peer0->set_connect_response(
      pw::bluetooth::emboss::StatusCode::CONNECTION_FAILED_TO_BE_ESTABLISHED);
  test_device()->AddPeer(std::move(fake_peer0));
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress1, dispatcher()));

  std::vector<ConnectionResult> conn_results;
  auto callback = [&conn_results](auto result) {
    conn_results.emplace_back(std::move(result));
  };

  conn_mgr()->Connect(peer0->identifier(), callback, kConnectionOptions);
  conn_mgr()->Connect(peer1->identifier(), callback, kConnectionOptions);

  RunUntilIdle();

  EXPECT_EQ(1u, connected_peers().size());
  EXPECT_EQ(1u, connected_peers().count(kAddress1));

  ASSERT_EQ(2u, conn_results.size());
  EXPECT_TRUE(conn_results[0].is_error());
  ASSERT_EQ(fit::ok(), conn_results[1]);
  EXPECT_EQ(peer1->identifier(), conn_results[1].value()->peer_identifier());

  // Both connections should disconnect.
  conn_results.clear();

  RunUntilIdle();
  EXPECT_TRUE(connected_peers().empty());
}

TEST_F(LowEnergyConnectionManagerTest, Destructor) {
  auto* peer0 = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto* peer1 = peer_cache()->NewPeer(kAddress1, /*connectable=*/true);

  // Connecting to this peer will succeed.
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));

  // Connecting to this peer will remain pending.
  auto pending_peer = std::make_unique<FakePeer>(kAddress1, dispatcher());
  pending_peer->set_force_pending_connect(true);
  test_device()->AddPeer(std::move(pending_peer));

  // Below we create one connection and one pending request to have at the time
  // of destruction.

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto success_cb = [&conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
  };

  conn_mgr()->Connect(peer0->identifier(), success_cb, kConnectionOptions);
  RunUntilIdle();

  ASSERT_TRUE(conn_handle);
  bool conn_closed = false;
  conn_handle->set_closed_callback([&conn_closed] { conn_closed = true; });

  bool error_cb_called = false;
  auto error_cb = [&error_cb_called](auto result) {
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(HostError::kCanceled, result.error_value());
    error_cb_called = true;
  };

  // This will send an HCI command to the fake controller. We delete the
  // connection manager before a connection event gets received which should
  // cancel the connection.
  conn_mgr()->Connect(peer1->identifier(), error_cb, kConnectionOptions);
  RunUntilIdle();
  EXPECT_FALSE(error_cb_called);

  DeleteConnMgr();

  RunUntilIdle();

  EXPECT_TRUE(error_cb_called);
  EXPECT_TRUE(conn_closed);
  EXPECT_EQ(1u, canceled_peers().size());
  EXPECT_EQ(1u, canceled_peers().count(kAddress1));
}

TEST_F(LowEnergyConnectionManagerTest,
       DisconnectPendingConnectionWhileAwaitingScanStart) {
  auto peer_0 = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto peer_1 = peer_cache()->NewPeer(kAddress1, /*connectable=*/true);
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress1, dispatcher()));

  int conn_cb_0_count = 0;
  auto conn_cb_0 = [&](auto result) {
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(HostError::kCanceled, result.error_value());
    EXPECT_EQ(peer_0->le()->connection_state(),
              Peer::ConnectionState::kNotConnected);
    conn_cb_0_count++;
  };

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto conn_cb_1 = [&](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
  };

  conn_mgr()->Connect(peer_0->identifier(), conn_cb_0, kConnectionOptions);
  conn_mgr()->Connect(peer_1->identifier(), conn_cb_1, kConnectionOptions);
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer_0->le()->connection_state());
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer_1->le()->connection_state());

  // Do NOT wait for scanning to start asynchronously before calling Disconnect
  // synchronously. After peer_0's connection request is cancelled, peer_1's
  // connection request should succeed.
  EXPECT_TRUE(conn_mgr()->Disconnect(peer_0->identifier()));
  RunUntilIdle();
  EXPECT_EQ(conn_cb_0_count, 1);
  ASSERT_TRUE(conn_handle);
  EXPECT_EQ(conn_handle->peer_identifier(), peer_1->identifier());
  EXPECT_EQ(Peer::ConnectionState::kNotConnected,
            peer_0->le()->connection_state());
  EXPECT_EQ(Peer::ConnectionState::kConnected,
            peer_1->le()->connection_state());
}

TEST_F(LowEnergyConnectionManagerTest, DisconnectPendingConnectionDuringScan) {
  // Don't add FakePeer for peer_0 in order to stall during scanning.
  auto peer_0 = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto peer_1 = peer_cache()->NewPeer(kAddress1, /*connectable=*/true);
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress1, dispatcher()));

  int conn_cb_0_count = 0;
  auto conn_cb_0 = [&](auto result) {
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(HostError::kCanceled, result.error_value());
    EXPECT_EQ(peer_0->le()->connection_state(),
              Peer::ConnectionState::kNotConnected);
    conn_cb_0_count++;
  };

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto conn_cb_1 = [&](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
  };

  conn_mgr()->Connect(peer_0->identifier(), conn_cb_0, kConnectionOptions);
  conn_mgr()->Connect(peer_1->identifier(), conn_cb_1, kConnectionOptions);

  // Wait for scanning to start & OnScanStart callback to be called.
  RunUntilIdle();
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer_0->le()->connection_state());
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer_1->le()->connection_state());

  // After peer_0's connection request is cancelled, peer_1's connection request
  // should succeed.
  EXPECT_TRUE(conn_mgr()->Disconnect(peer_0->identifier()));
  RunUntilIdle();
  EXPECT_EQ(conn_cb_0_count, 1);
  ASSERT_TRUE(conn_handle);
  EXPECT_EQ(conn_handle->peer_identifier(), peer_1->identifier());
  EXPECT_EQ(Peer::ConnectionState::kNotConnected,
            peer_0->le()->connection_state());
  EXPECT_EQ(Peer::ConnectionState::kConnected,
            peer_1->le()->connection_state());
}

TEST_F(LowEnergyConnectionManagerTest, LocalDisconnectWhileConnectorPending) {
  auto peer_0 = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto fake_peer_0 = std::make_unique<FakePeer>(kAddress0, dispatcher());
  fake_peer_0->set_force_pending_connect(true);
  test_device()->AddPeer(std::move(fake_peer_0));

  auto peer_1 = peer_cache()->NewPeer(kAddress1, /*connectable=*/true);
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress1, dispatcher()));

  int conn_cb_0_count = 0;
  auto conn_cb_0 = [&](auto result) {
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(HostError::kCanceled, result.error_value());
    conn_cb_0_count++;
  };

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto conn_cb_1 = [&](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
  };

  conn_mgr()->Connect(peer_0->identifier(), conn_cb_0, kConnectionOptions);
  conn_mgr()->Connect(peer_1->identifier(), conn_cb_1, kConnectionOptions);
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer_0->le()->connection_state());
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer_1->le()->connection_state());

  // Wait for peer_0 scanning to complete and kLECreateConnection command to be
  // sent.
  RunUntilIdle();

  // After peer_0's connection request is cancelled, peer_1's connection request
  // should succeed.
  EXPECT_TRUE(conn_mgr()->Disconnect(peer_0->identifier()));
  RunUntilIdle();
  EXPECT_EQ(conn_cb_0_count, 1);
  ASSERT_TRUE(conn_handle);
  EXPECT_EQ(conn_handle->peer_identifier(), peer_1->identifier());
  EXPECT_EQ(Peer::ConnectionState::kNotConnected,
            peer_0->le()->connection_state());
  EXPECT_EQ(Peer::ConnectionState::kConnected,
            peer_1->le()->connection_state());
}

TEST_F(
    LowEnergyConnectionManagerTest,
    DisconnectQueuedPendingConnectionAndThenPendingConnectionWithPendingConnector) {
  auto peer_0 = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto fake_peer_0 = std::make_unique<FakePeer>(kAddress0, dispatcher());
  fake_peer_0->set_force_pending_connect(true);
  test_device()->AddPeer(std::move(fake_peer_0));

  auto peer_1 = peer_cache()->NewPeer(kAddress1, /*connectable=*/true);
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress1, dispatcher()));

  int conn_cb_0_count = 0;
  auto conn_cb_0 = [&](auto result) {
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(HostError::kCanceled, result.error_value());
    EXPECT_EQ(peer_0->le()->connection_state(),
              Peer::ConnectionState::kNotConnected);
    conn_cb_0_count++;
  };

  int conn_cb_1_count = 0;
  auto conn_cb_1 = [&](auto result) {
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(HostError::kCanceled, result.error_value());
    EXPECT_EQ(peer_1->le()->connection_state(),
              Peer::ConnectionState::kNotConnected);
    conn_cb_1_count++;
  };

  conn_mgr()->Connect(peer_0->identifier(), conn_cb_0, kConnectionOptions);
  conn_mgr()->Connect(peer_1->identifier(), conn_cb_1, kConnectionOptions);
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer_0->le()->connection_state());
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer_1->le()->connection_state());

  EXPECT_TRUE(conn_mgr()->Disconnect(peer_1->identifier()));
  RunUntilIdle();
  EXPECT_EQ(conn_cb_0_count, 0);
  EXPECT_EQ(conn_cb_1_count, 1);
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer_0->le()->connection_state());
  EXPECT_EQ(Peer::ConnectionState::kNotConnected,
            peer_1->le()->connection_state());

  EXPECT_TRUE(conn_mgr()->Disconnect(peer_0->identifier()));
  RunUntilIdle();
  EXPECT_EQ(conn_cb_0_count, 1);
  EXPECT_EQ(Peer::ConnectionState::kNotConnected,
            peer_0->le()->connection_state());
  EXPECT_EQ(Peer::ConnectionState::kNotConnected,
            peer_1->le()->connection_state());
}

TEST_F(LowEnergyConnectionManagerTest, DisconnectUnknownPeer) {
  // Unknown peers are inherently "not connected."
  EXPECT_TRUE(conn_mgr()->Disconnect(PeerId(999)));
}

TEST_F(LowEnergyConnectionManagerTest, DisconnectUnconnectedPeer) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));

  // This returns true so long the peer is not connected.
  EXPECT_TRUE(conn_mgr()->Disconnect(peer->identifier()));
}

TEST_F(LowEnergyConnectionManagerTest, Disconnect) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));

  int closed_count = 0;
  auto closed_cb = [&closed_count] { closed_count++; };

  std::vector<std::unique_ptr<LowEnergyConnectionHandle>> conn_handles;
  auto success_cb = [&conn_handles, &closed_cb](auto result) {
    ASSERT_EQ(fit::ok(), result);
    auto conn_handle = std::move(result).value();
    conn_handle->set_closed_callback(closed_cb);
    conn_handles.push_back(std::move(conn_handle));
  };

  // Issue two connection refs.
  conn_mgr()->Connect(peer->identifier(), success_cb, kConnectionOptions);
  conn_mgr()->Connect(peer->identifier(), success_cb, kConnectionOptions);

  RunUntilIdle();

  ASSERT_EQ(2u, conn_handles.size());

  EXPECT_TRUE(conn_mgr()->Disconnect(peer->identifier()));

  bool peer_removed = peer_cache()->RemoveDisconnectedPeer(peer->identifier());
  EXPECT_TRUE(peer_removed);

  RunUntilIdle();

  EXPECT_EQ(2, closed_count);
  EXPECT_TRUE(connected_peers().empty());
  EXPECT_TRUE(canceled_peers().empty());

  // The central pause timeout handler should not run.
  RunFor(kLEConnectionPauseCentral);
}

TEST_F(LowEnergyConnectionManagerTest,
       IntentionalDisconnectDisablesAutoConnectBehavior) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));

  std::vector<std::unique_ptr<LowEnergyConnectionHandle>> conn_handles;
  auto success_cb = [&conn_handles](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handles.push_back(std::move(result).value());
  };

  sm::PairingData data;
  data.peer_ltk = sm::LTK();
  data.local_ltk = sm::LTK();
  EXPECT_TRUE(peer_cache()->StoreLowEnergyBond(peer->identifier(), data));

  // Issue connection ref.
  conn_mgr()->Connect(peer->identifier(), success_cb, kConnectionOptions);
  RunUntilIdle();

  // Bonded peer should have auto-connection enabled.
  EXPECT_TRUE(peer->le()->should_auto_connect());

  // Explicit disconnect should disable the auto-connection property.
  EXPECT_TRUE(conn_mgr()->Disconnect(peer->identifier()));
  RunUntilIdle();
  EXPECT_FALSE(peer->le()->should_auto_connect());

  // Intentional re-connection should re-enable the auto-connection property.
  conn_mgr()->Connect(peer->identifier(), success_cb, kConnectionOptions);
  RunUntilIdle();
  EXPECT_TRUE(peer->le()->should_auto_connect());
}

TEST_F(LowEnergyConnectionManagerTest,
       IncidentalDisconnectDoesNotAffectAutoConnectBehavior) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));

  std::vector<std::unique_ptr<LowEnergyConnectionHandle>> conn_handles;
  auto success_cb = [&conn_handles](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handles.push_back(std::move(result).value());
  };

  sm::PairingData data;
  data.peer_ltk = sm::LTK();
  data.local_ltk = sm::LTK();
  EXPECT_TRUE(peer_cache()->StoreLowEnergyBond(peer->identifier(), data));

  // Issue connection ref.
  conn_mgr()->Connect(peer->identifier(), success_cb, kConnectionOptions);
  RunUntilIdle();

  // Bonded peer should have auto-connection enabled.
  EXPECT_TRUE(peer->le()->should_auto_connect());

  // Incidental disconnect should NOT disable the auto-connection property.
  ASSERT_TRUE(conn_handles.size());
  conn_handles[0] = nullptr;
  RunUntilIdle();
  EXPECT_TRUE(peer->le()->should_auto_connect());
}

TEST_F(LowEnergyConnectionManagerTest, DisconnectThrice) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));

  int closed_count = 0;
  auto closed_cb = [&closed_count] { closed_count++; };

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto success_cb = [&closed_cb, &conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
    ASSERT_TRUE(conn_handle);
    conn_handle->set_closed_callback(closed_cb);
  };

  conn_mgr()->Connect(peer->identifier(), success_cb, kConnectionOptions);

  RunUntilIdle();

  EXPECT_TRUE(conn_mgr()->Disconnect(peer->identifier()));

  // Try to disconnect again while the first disconnection is in progress.
  EXPECT_TRUE(conn_mgr()->Disconnect(peer->identifier()));

  RunUntilIdle();

  // The single ref should get only one "closed" call.
  EXPECT_EQ(1, closed_count);
  EXPECT_TRUE(connected_peers().empty());
  EXPECT_TRUE(canceled_peers().empty());

  // Try to disconnect once more, now that the link is gone.
  EXPECT_TRUE(conn_mgr()->Disconnect(peer->identifier()));
}

// Tests when a link is lost without explicitly disconnecting
TEST_F(LowEnergyConnectionManagerTest, DisconnectEvent) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);

  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));

  int closed_count = 0;
  auto closed_cb = [&closed_count] { closed_count++; };

  std::vector<std::unique_ptr<LowEnergyConnectionHandle>> conn_handles;
  auto success_cb = [&conn_handles, &closed_cb](auto result) {
    ASSERT_EQ(fit::ok(), result);
    auto conn_handle = std::move(result).value();
    conn_handle->set_closed_callback(closed_cb);
    conn_handles.push_back(std::move(conn_handle));
  };

  // Issue two connection refs.
  conn_mgr()->Connect(peer->identifier(), success_cb, kConnectionOptions);
  conn_mgr()->Connect(peer->identifier(), success_cb, kConnectionOptions);

  RunUntilIdle();

  ASSERT_EQ(2u, conn_handles.size());

  // This makes FakeController send us HCI Disconnection Complete events.
  test_device()->Disconnect(kAddress0);

  RunUntilIdle();

  EXPECT_EQ(2, closed_count);
}

TEST_F(LowEnergyConnectionManagerTest, DisconnectAfterRefsReleased) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto success_cb = [&conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
  };

  conn_mgr()->Connect(peer->identifier(), success_cb, kConnectionOptions);

  RunUntilIdle();

  ASSERT_TRUE(conn_handle);
  conn_handle.reset();

  // Try to disconnect while the zero-refs connection is being disconnected.
  EXPECT_TRUE(conn_mgr()->Disconnect(peer->identifier()));

  RunUntilIdle();

  EXPECT_TRUE(connected_peers().empty());
  EXPECT_TRUE(canceled_peers().empty());
}

TEST_F(LowEnergyConnectionManagerTest,
       DisconnectAfterSecondConnectionRequestInvalidatesRefs) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle_0;
  auto success_cb = [&conn_handle_0](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle_0 = std::move(result).value();
    ASSERT_TRUE(conn_handle_0);
    EXPECT_TRUE(conn_handle_0->active());
  };

  conn_mgr()->Connect(peer->identifier(), success_cb, kConnectionOptions);
  RunUntilIdle();
  ASSERT_TRUE(conn_handle_0);
  EXPECT_TRUE(conn_handle_0->active());

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle_1;
  auto ref_cb = [&conn_handle_1](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle_1 = std::move(result).value();
  };

  // Callback should be run synchronously with success status because connection
  // already exists.
  conn_mgr()->Connect(peer->identifier(), ref_cb, kConnectionOptions);
  EXPECT_TRUE(conn_handle_1);
  EXPECT_TRUE(conn_handle_1->active());

  // This should invalidate the refs.
  EXPECT_TRUE(conn_mgr()->Disconnect(peer->identifier()));
  EXPECT_FALSE(conn_handle_1->active());
  EXPECT_FALSE(conn_handle_0->active());

  RunUntilIdle();
}

// This tests that a connection reference callback succeeds if a HCI
// Disconnection Complete event is received for the corresponding ACL link
// immediately after the callback gets run.
TEST_F(LowEnergyConnectionManagerTest, DisconnectCompleteEventAfterConnect) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto success_cb = [&conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
    EXPECT_TRUE(conn_handle->active());
  };

  conn_mgr()->Connect(peer->identifier(), success_cb, kConnectionOptions);
  RunUntilIdle();
  ASSERT_TRUE(conn_handle);

  // Request a new reference. Disconnect the link before the reference is
  // received.
  size_t ref_cb_count = 0;
  auto ref_cb = [&ref_cb_count](auto result) {
    ref_cb_count++;
    EXPECT_EQ(fit::ok(), result);
  };

  size_t disconn_cb_count = 0;
  auto disconn_cb =
      [this, ref_cb, peer, &disconn_cb_count, &ref_cb_count](auto) {
        disconn_cb_count++;
        // The link is gone but conn_mgr() hasn't updated the connection state
        // yet. The request to connect will attempt to add a new reference which
        // will succeed because ref_cb is called synchronously.
        EXPECT_EQ(0u, ref_cb_count);
        conn_mgr()->Connect(peer->identifier(), ref_cb, kConnectionOptions);
        EXPECT_EQ(1u, ref_cb_count);
      };
  conn_mgr()->SetDisconnectCallbackForTesting(disconn_cb);

  test_device()->SendDisconnectionCompleteEvent(conn_handle->handle());

  RunUntilIdle();

  EXPECT_EQ(1u, ref_cb_count);
  EXPECT_EQ(1u, disconn_cb_count);
}

TEST_F(LowEnergyConnectionManagerTest,
       RemovePeerFromPeerCacheDuringDisconnection) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto success_cb = [&conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
    EXPECT_TRUE(conn_handle->active());
  };

  conn_mgr()->Connect(peer->identifier(), success_cb, kConnectionOptions);
  RunUntilIdle();
  ASSERT_TRUE(conn_handle);

  // This should invalidate the ref that was bound to |ref_cb|.
  const PeerId id = peer->identifier();
  EXPECT_TRUE(conn_mgr()->Disconnect(id));
  ASSERT_FALSE(peer->le()->connected());
  EXPECT_FALSE(conn_handle->active());

  EXPECT_TRUE(peer_cache()->RemoveDisconnectedPeer(id));

  RunUntilIdle();

  EXPECT_FALSE(peer_cache()->FindById(id));
  EXPECT_FALSE(peer_cache()->FindByAddress(kAddress0));
}

// Listener receives remote initiated connection ref.
TEST_F(LowEnergyConnectionManagerTest, RegisterRemoteInitiatedLink) {
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));

  // First create a fake incoming connection.
  test_device()->ConnectLowEnergy(kAddress0);

  RunUntilIdle();

  auto link = MoveLastRemoteInitiated();
  ASSERT_TRUE(link);

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  conn_mgr()->RegisterRemoteInitiatedLink(
      std::move(link), BondableMode::Bondable, [&](auto result) {
        ASSERT_EQ(fit::ok(), result);
        conn_handle = std::move(result).value();
      });
  // A Peer should now exist in the cache.
  auto* peer = peer_cache()->FindByAddress(kAddress0);
  EXPECT_EQ(peer->le()->connection_state(),
            Peer::ConnectionState::kInitializing);

  RunUntilIdle();

  ASSERT_TRUE(conn_handle);
  EXPECT_TRUE(conn_handle->active());
  ASSERT_TRUE(peer);
  EXPECT_EQ(peer->identifier(), conn_handle->peer_identifier());
  EXPECT_TRUE(peer->connected());
  EXPECT_TRUE(peer->le()->connected());
  EXPECT_TRUE(peer->version().has_value());
  EXPECT_TRUE(peer->le()->features().has_value());

  conn_handle = nullptr;

  RunUntilIdle();
  EXPECT_TRUE(connected_peers().empty());
}

TEST_F(LowEnergyConnectionManagerTest,
       RegisterRemoteInitiatedLinkDuringLocalInitiatedLinkConnecting) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  fake_peer->set_force_pending_connect(true);
  test_device()->AddPeer(std::move(fake_peer));

  // Create a fake incoming connection.
  test_device()->ConnectLowEnergy(kAddress0);
  RunUntilIdle();
  auto link = MoveLastRemoteInitiated();
  ASSERT_TRUE(link);

  // Create a pending outgoing connection.
  ConnectionResult result = fit::ok(nullptr);
  auto callback = [&result](auto res) { result = std::move(res); };
  conn_mgr()->Connect(peer->identifier(), callback, kConnectionOptions);

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  conn_mgr()->RegisterRemoteInitiatedLink(
      std::move(link), BondableMode::Bondable, [&](auto result) {
        ASSERT_EQ(fit::ok(), result);
        conn_handle = std::move(result).value();
      });
  RunUntilIdle();
  ASSERT_TRUE(conn_handle);

  // Local connector result handler should not crash when it finds that
  // connection to peer already exists.
  RunFor(kLECreateConnectionTimeout);
  // An error should be returned if the connection complete was incorrectly not
  // matched to the pending connection request (see fxbug.dev/42148050). In the
  // future it may make sense to return success because a link to the peer
  // already exists.
  ASSERT_TRUE(result.is_error());
  EXPECT_TRUE(peer->le()->connected());
}

TEST_F(LowEnergyConnectionManagerTest,
       RegisterRemoteInitiatedLinkDuringLocalInitiatedConnectionScanning) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  fake_peer->set_advertising_enabled(false);
  test_device()->AddPeer(std::move(fake_peer));

  // Create a fake incoming connection.
  test_device()->ConnectLowEnergy(kAddress0);
  RunUntilIdle();
  auto link = MoveLastRemoteInitiated();
  ASSERT_TRUE(link);

  // Create a pending outgoing connection.
  ConnectionResult result = fit::ok(nullptr);
  auto callback = [&result](auto res) { result = std::move(res); };
  conn_mgr()->Connect(peer->identifier(), callback, kConnectionOptions);

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  conn_mgr()->RegisterRemoteInitiatedLink(
      std::move(link), BondableMode::Bondable, [&](auto result) {
        ASSERT_EQ(fit::ok(), result);
        conn_handle = std::move(result).value();
      });
  RunUntilIdle();
  ASSERT_TRUE(conn_handle);

  // Local connector result handler should not crash when it finds that
  // connection to peer already exists.
  RunFor(kLEGeneralCepScanTimeout);
  ASSERT_TRUE(result.is_error());
  EXPECT_TRUE(peer->le()->connected());
}

// Listener receives remote initiated connection ref for a known peer with the
// same BR/EDR address.
TEST_F(LowEnergyConnectionManagerTest,
       IncomingConnectionUpgradesKnownBrEdrPeerToDualMode) {
  Peer* peer = peer_cache()->NewPeer(kAddrAlias0, /*connectable=*/true);
  ASSERT_TRUE(peer);
  ASSERT_EQ(peer, peer_cache()->FindByAddress(kAddress0));
  ASSERT_EQ(TechnologyType::kClassic, peer->technology());

  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));

  // First create a fake incoming connection.
  test_device()->ConnectLowEnergy(kAddress0);

  RunUntilIdle();

  auto link = MoveLastRemoteInitiated();
  ASSERT_TRUE(link);

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  conn_mgr()->RegisterRemoteInitiatedLink(
      std::move(link), BondableMode::Bondable, [&conn_handle](auto result) {
        ASSERT_EQ(fit::ok(), result);
        conn_handle = std::move(result).value();
      });
  RunUntilIdle();
  ASSERT_TRUE(conn_handle);

  EXPECT_EQ(peer->identifier(), conn_handle->peer_identifier());
  EXPECT_EQ(TechnologyType::kDualMode, peer->technology());
}

// Successful connection to a peer whose address type is kBREDR.
// TODO(fxbug.dev/42102158): This test will likely become obsolete when LE
// connections are based on the presence of LowEnergyData in a Peer and no
// address type enum exists.
TEST_F(LowEnergyConnectionManagerTest,
       ConnectAndDisconnectDualModeDeviceWithBrEdrAddress) {
  Peer* peer = peer_cache()->NewPeer(kAddrAlias0, /*connectable=*/true);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(peer->bredr());

  peer->MutLe();
  ASSERT_EQ(TechnologyType::kDualMode, peer->technology());
  ASSERT_EQ(peer, peer_cache()->FindByAddress(kAddress0));
  ASSERT_EQ(DeviceAddress::Type::kBREDR, peer->address().type());

  // Only the LE transport connects in this test, so only add an LE FakePeer to
  // FakeController.
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto callback = [&conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
  };

  EXPECT_TRUE(connected_peers().empty());
  conn_mgr()->Connect(peer->identifier(), callback, kConnectionOptions);
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer->le()->connection_state());

  RunUntilIdle();

  EXPECT_EQ(1u, connected_peers().size());
  EXPECT_EQ(1u, connected_peers().count(kAddress0));

  ASSERT_TRUE(conn_handle);
  EXPECT_TRUE(conn_handle->active());
  EXPECT_EQ(peer->identifier(), conn_handle->peer_identifier());
  EXPECT_FALSE(peer->temporary());
  EXPECT_EQ(Peer::ConnectionState::kConnected, peer->le()->connection_state());

  conn_handle = nullptr;
  RunUntilIdle();
  EXPECT_EQ(0u, connected_peers().size());
}

// Tests that the central accepts the connection parameters that are sent from
// a fake peripheral and eventually applies them to the link.
TEST_F(LowEnergyConnectionManagerTest,
       CentralAppliesL2capConnectionParameterUpdateRequestParams) {
  // Set up a fake peer and a connection over which to process the L2CAP
  // request.
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  ASSERT_TRUE(peer);

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto conn_cb = [&conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
  };
  conn_mgr()->Connect(peer->identifier(), conn_cb, kConnectionOptions);

  RunUntilIdle();
  ASSERT_TRUE(conn_handle);

  hci_spec::LEPreferredConnectionParameters preferred(
      hci_spec::kLEConnectionIntervalMin,
      hci_spec::kLEConnectionIntervalMax,
      hci_spec::kLEConnectionLatencyMax,
      hci_spec::kLEConnectionSupervisionTimeoutMax);

  std::optional<hci_spec::LEConnectionParameters> actual;

  auto conn_params_updated_cb = [&](const auto&, const auto& params) {
    actual = params;
  };
  test_device()->set_le_connection_parameters_callback(conn_params_updated_cb);

  fake_l2cap()->TriggerLEConnectionParameterUpdate(conn_handle->handle(),
                                                   preferred);

  // These connection update events for the wrong handle should be ignored.
  // Send twice: once before the parameter request is processed, and once after
  // the request has been processed.
  hci_spec::LEConnectionParameters wrong_handle_conn_params(0, 1, 2);
  test_device()->SendLEConnectionUpdateCompleteSubevent(
      conn_handle->handle() + 1, wrong_handle_conn_params);
  RunUntilIdle();

  test_device()->SendLEConnectionUpdateCompleteSubevent(
      conn_handle->handle() + 1, wrong_handle_conn_params);

  RunUntilIdle();

  ASSERT_TRUE(actual.has_value());
  ASSERT_TRUE(peer->le());
  EXPECT_EQ(preferred, *peer->le()->preferred_connection_parameters());
  EXPECT_EQ(actual.value(), *peer->le()->connection_parameters());
}

TEST_F(LowEnergyConnectionManagerTest, L2CAPSignalLinkError) {
  // Set up a fake peer and a connection over which to process the L2CAP
  // request.
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  ASSERT_TRUE(peer);

  l2cap::testing::FakeChannel::WeakPtr smp_chan;
  auto l2cap_chan_cb = [&smp_chan](auto chan) { smp_chan = chan; };
  fake_l2cap()->set_channel_callback(l2cap_chan_cb);

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto conn_cb = [&conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
  };
  conn_mgr()->Connect(peer->identifier(), conn_cb, kConnectionOptions);

  RunUntilIdle();
  ASSERT_TRUE(conn_handle);
  ASSERT_TRUE(smp_chan.is_alive());
  ASSERT_EQ(1u, connected_peers().size());

  // Signaling a link error through the channel should disconnect the link.
  smp_chan->SignalLinkError();

  RunUntilIdle();
  EXPECT_TRUE(connected_peers().empty());
}

TEST_F(LowEnergyConnectionManagerTest, AttBearerSignalsLinkError) {
  // Set up a fake peer and a connection over which to process the L2CAP
  // request.
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  ASSERT_TRUE(peer);

  l2cap::testing::FakeChannel::WeakPtr att_chan;
  auto l2cap_chan_cb = [&att_chan](l2cap::testing::FakeChannel::WeakPtr chan) {
    if (chan->id() == l2cap::kATTChannelId) {
      att_chan = std::move(chan);
    }
  };
  fake_l2cap()->set_channel_callback(l2cap_chan_cb);

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto conn_cb = [&conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
  };
  conn_mgr()->Connect(peer->identifier(), conn_cb, kConnectionOptions);

  RunUntilIdle();
  ASSERT_TRUE(conn_handle);
  ASSERT_TRUE(att_chan.is_alive());
  ASSERT_EQ(1u, connected_peers().size());

  // Receiving an invalid SDU should cause att::Bearer to signal a link error.
  DynamicByteBuffer too_large_att_sdu(att::kLEMaxMTU + 1);
  too_large_att_sdu.Fill(0x00);
  att_chan->Receive(too_large_att_sdu);

  RunUntilIdle();
  ASSERT_FALSE(att_chan.is_alive());
  EXPECT_TRUE(connected_peers().empty());
}

TEST_F(LowEnergyConnectionManagerTest, OutboundConnectATTChannelActivateFails) {
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  ASSERT_TRUE(peer);

  std::optional<l2cap::testing::FakeChannel::WeakPtr> att_chan;
  auto l2cap_chan_cb = [&att_chan](l2cap::testing::FakeChannel::WeakPtr chan) {
    if (chan->id() == l2cap::kATTChannelId) {
      // Cause att::Bearer construction/activation to fail.
      chan->set_activate_fails(true);
      att_chan = std::move(chan);
    }
  };
  fake_l2cap()->set_channel_callback(l2cap_chan_cb);

  std::optional<LowEnergyConnectionManager::ConnectionResult> result;
  auto conn_cb = [&](LowEnergyConnectionManager::ConnectionResult cb_result) {
    result = std::move(cb_result);
  };
  conn_mgr()->Connect(peer->identifier(), conn_cb, kConnectionOptions);

  RunUntilIdle();
  ASSERT_TRUE(att_chan.has_value());
  // The link should have been closed due to the error, invalidating the
  // channel.
  EXPECT_FALSE(att_chan.value().is_alive());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(HostError::kFailed, result->error_value());
  EXPECT_TRUE(connected_peers().empty());
}

TEST_F(LowEnergyConnectionManagerTest,
       InboundConnectionATTChannelActivateFails) {
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  ASSERT_TRUE(peer);

  std::optional<l2cap::testing::FakeChannel::WeakPtr> att_chan;
  auto l2cap_chan_cb = [&att_chan](l2cap::testing::FakeChannel::WeakPtr chan) {
    if (chan->id() == l2cap::kATTChannelId) {
      // Cause att::Bearer construction/activation to fail.
      chan->set_activate_fails(true);
      att_chan = std::move(chan);
    }
  };
  fake_l2cap()->set_channel_callback(l2cap_chan_cb);

  std::optional<LowEnergyConnectionManager::ConnectionResult> result;
  auto conn_cb = [&](LowEnergyConnectionManager::ConnectionResult cb_result) {
    result = std::move(cb_result);
  };
  test_device()->ConnectLowEnergy(kAddress0);
  RunUntilIdle();
  auto link = MoveLastRemoteInitiated();
  ASSERT_TRUE(link);
  conn_mgr()->RegisterRemoteInitiatedLink(
      std::move(link), BondableMode::Bondable, std::move(conn_cb));

  RunUntilIdle();
  ASSERT_TRUE(att_chan.has_value());
  // The link should have been closed due to the error, invalidating the
  // channel.
  EXPECT_FALSE(att_chan.value().is_alive());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(HostError::kFailed, result->error_value());
  EXPECT_TRUE(connected_peers().empty());
}

TEST_F(LowEnergyConnectionManagerTest, LinkErrorDuringInterrogation) {
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  ASSERT_TRUE(peer);

  // Get an arbitrary channel in order to signal a link error.
  l2cap::testing::FakeChannel::WeakPtr chan;
  auto l2cap_chan_cb = [&chan](l2cap::testing::FakeChannel::WeakPtr cb_chan) {
    chan = std::move(cb_chan);
  };
  fake_l2cap()->set_channel_callback(l2cap_chan_cb);

  // Cause interrogation to stall so that we can simulate a link error.
  fit::closure send_read_remote_features_rsp;
  test_device()->pause_responses_for_opcode(
      hci_spec::kLEReadRemoteFeatures, [&](fit::closure unpause) {
        send_read_remote_features_rsp = std::move(unpause);
      });

  std::optional<LowEnergyConnectionManager::ConnectionResult> result;
  auto conn_cb = [&](LowEnergyConnectionManager::ConnectionResult cb_result) {
    result = std::move(cb_result);
  };
  conn_mgr()->Connect(peer->identifier(), conn_cb, kConnectionOptions);

  RunUntilIdle();
  ASSERT_TRUE(chan.is_alive());
  fake_l2cap()->TriggerLinkError(chan->link_handle());

  send_read_remote_features_rsp();

  RunUntilIdle();
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->is_error());
  EXPECT_EQ(HostError::kFailed, result->error_value());
  EXPECT_TRUE(connected_peers().empty());
}

TEST_F(LowEnergyConnectionManagerTest, PairUnconnectedPeer) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  EXPECT_TRUE(peer->temporary());
  ASSERT_EQ(peer_cache()->count(), 1u);
  uint count_cb_called = 0;
  auto cb = [&count_cb_called](sm::Result<> status) {
    EXPECT_EQ(ToResult(bt::HostError::kNotFound), status);
    count_cb_called++;
  };
  conn_mgr()->Pair(peer->identifier(),
                   sm::SecurityLevel::kEncrypted,
                   sm::BondableMode::Bondable,
                   cb);
  ASSERT_EQ(count_cb_called, 1u);
}

TEST_F(LowEnergyConnectionManagerTest, PairWithBondableModes) {
  // clang-format on
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  EXPECT_TRUE(peer->temporary());

  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto callback = [&conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
    EXPECT_TRUE(conn_handle);
    EXPECT_TRUE(conn_handle->active());
  };

  conn_mgr()->Connect(peer->identifier(), callback, kConnectionOptions);
  ASSERT_TRUE(peer->le());

  RunUntilIdle();
  TestSm::WeakPtr mock_sm = TestSmByHandle(conn_handle->handle());
  ASSERT_TRUE(mock_sm.is_alive());

  ASSERT_EQ(Peer::ConnectionState::kConnected, peer->le()->connection_state());

  EXPECT_FALSE(mock_sm->last_requested_upgrade().has_value());
  conn_mgr()->Pair(peer->identifier(),
                   sm::SecurityLevel::kEncrypted,
                   sm::BondableMode::Bondable,
                   [](sm::Result<>) {});
  RunUntilIdle();

  EXPECT_EQ(BondableMode::Bondable, mock_sm->bondable_mode());
  EXPECT_EQ(sm::SecurityLevel::kEncrypted, mock_sm->last_requested_upgrade());

  conn_mgr()->Pair(peer->identifier(),
                   sm::SecurityLevel::kAuthenticated,
                   sm::BondableMode::NonBondable,
                   [](sm::Result<>) {});
  RunUntilIdle();

  EXPECT_EQ(BondableMode::NonBondable, mock_sm->bondable_mode());
  EXPECT_EQ(sm::SecurityLevel::kAuthenticated,
            mock_sm->last_requested_upgrade());
}

TEST_F(LowEnergyConnectionManagerTest, ConnectAndDiscoverByServiceWithoutUUID) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);

  bool cb_called = false;
  auto expect_uuids = [&cb_called](PeerId, auto uuids) {
    ASSERT_TRUE(uuids.empty());
    cb_called = true;
  };
  fake_gatt()->SetInitializeClientCallback(expect_uuids);

  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto callback = [&conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
    EXPECT_TRUE(conn_handle);
    EXPECT_TRUE(conn_handle->active());
  };

  LowEnergyConnectionOptions connection_options{.service_uuid = std::nullopt};
  conn_mgr()->Connect(peer->identifier(), callback, connection_options);

  RunUntilIdle();

  ASSERT_TRUE(cb_called);
}

TEST_F(LowEnergyConnectionManagerTest, ConnectAndDiscoverByServiceUuid) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);

  UUID kConnectUuid({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});
  std::array<UUID, 2> expected_uuids = {kConnectUuid, kGenericAccessService};

  bool cb_called = false;
  auto expect_uuid = [&cb_called, expected_uuids](PeerId, auto uuids) {
    EXPECT_THAT(uuids, ::testing::UnorderedElementsAreArray(expected_uuids));
    cb_called = true;
  };
  fake_gatt()->SetInitializeClientCallback(expect_uuid);

  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto callback = [&conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
    ASSERT_TRUE(conn_handle);
    EXPECT_TRUE(conn_handle->active());
  };

  LowEnergyConnectionOptions connection_options{
      .service_uuid = std::optional(kConnectUuid)};
  conn_mgr()->Connect(peer->identifier(), callback, connection_options);

  RunUntilIdle();

  ASSERT_TRUE(cb_called);
}

class ReadDeviceNameParameterizedFixture
    : public LowEnergyConnectionManagerTest,
      public ::testing::WithParamInterface<DynamicByteBuffer> {};

TEST_P(ReadDeviceNameParameterizedFixture, ReadDeviceNameParameterized) {
  Peer* peer = peer_cache()->NewPeer(kAddress0, true);
  std::unique_ptr<FakePeer> fake_peer =
      std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  // Set up GAP service
  gatt::ServiceData service_data(gatt::ServiceKind::PRIMARY,
                                 /*start=*/0x0001,
                                 /*end=*/0x0009,
                                 kGenericAccessService);
  auto [remote_svc, service_client] =
      fake_gatt()->AddPeerService(peer->identifier(), service_data);

  // Set up preferred connection parameters characteristic.
  att::Handle char_handle = 0x0002;
  att::Handle char_value_handle = 0x0003;
  gatt::CharacteristicData char_data(gatt::kRead,
                                     /*ext_props=*/std::nullopt,
                                     char_handle,
                                     char_value_handle,
                                     kDeviceNameCharacteristic);
  service_client->set_characteristics({char_data});

  DynamicByteBuffer char_value = GetParam();
  service_client->set_read_request_callback(
      [char_value_handle, char_value](att::Handle handle, auto read_cb) {
        if (handle == char_value_handle) {
          read_cb(fit::ok(), char_value, /*maybe_truncated=*/false);
        }
      });

  std::unique_ptr<LowEnergyConnectionHandle> conn_ref;
  auto callback =
      [&conn_ref](
          fit::result<HostError, std::unique_ptr<LowEnergyConnectionHandle>>
              result) {
        ASSERT_EQ(fit::ok(), result);
        conn_ref = std::move(result).value();
      };

  conn_mgr()->Connect(
      peer->identifier(), callback, LowEnergyConnectionOptions());

  RunUntilIdle();
  EXPECT_TRUE(conn_ref);
  ASSERT_TRUE(peer->name());
  EXPECT_EQ(peer->name_source(), Peer::NameSource::kGenericAccessService);
  std::string device_name = peer->name().value();
  EXPECT_EQ(device_name, "abc");
}

StaticByteBuffer<3> b1{'a', 'b', 'c'};
StaticByteBuffer<5> b2{'a', 'b', 'c', '\0', 'x'};
INSTANTIATE_TEST_SUITE_P(ReadDeviceNameTest,
                         ReadDeviceNameParameterizedFixture,
                         ::testing::Values(DynamicByteBuffer(b1),
                                           DynamicByteBuffer(b2)));

TEST_F(LowEnergyConnectionManagerTest, ReadDeviceNameLong) {
  Peer* peer = peer_cache()->NewPeer(kAddress0, true);
  std::unique_ptr<FakePeer> fake_peer =
      std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  // Set up GAP service
  gatt::ServiceData service_data(gatt::ServiceKind::PRIMARY,
                                 /*start=*/0x0001,
                                 /*end=*/0x0009,
                                 kGenericAccessService);
  auto [remote_svc, service_client] =
      fake_gatt()->AddPeerService(peer->identifier(), service_data);

  // Set up preferred connection parameters characteristic.
  att::Handle char_handle = 0x0002;
  att::Handle char_value_handle = 0x0003;
  gatt::CharacteristicData char_data(gatt::kRead,
                                     /*ext_props=*/std::nullopt,
                                     char_handle,
                                     char_value_handle,
                                     kDeviceNameCharacteristic);
  service_client->set_characteristics({char_data});

  // Max length read
  StaticByteBuffer<att::kMaxAttributeValueLength> char_value;
  char_value.Fill('a');
  service_client->set_read_request_callback(
      [char_value_handle, char_value](att::Handle handle, auto read_cb) {
        if (handle == char_value_handle) {
          read_cb(fit::ok(), char_value, /*maybe_truncated=*/false);
        }
      });

  std::unique_ptr<LowEnergyConnectionHandle> conn_ref;
  auto callback =
      [&conn_ref](
          fit::result<HostError, std::unique_ptr<LowEnergyConnectionHandle>>
              result) {
        ASSERT_EQ(fit::ok(), result);
        conn_ref = std::move(result).value();
      };

  conn_mgr()->Connect(
      peer->identifier(), callback, LowEnergyConnectionOptions());

  RunUntilIdle();
  EXPECT_TRUE(conn_ref);
  ASSERT_TRUE(peer->name());
  EXPECT_EQ(peer->name_source(), Peer::NameSource::kGenericAccessService);
  std::string device_name = peer->name().value();
  EXPECT_EQ(device_name, std::string(att::kMaxAttributeValueLength, 'a'));
}

TEST_F(LowEnergyConnectionManagerTest, ReadAppearance) {
  Peer* peer = peer_cache()->NewPeer(kAddress0, true);
  std::unique_ptr<FakePeer> fake_peer =
      std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  // Set up GAP service
  gatt::ServiceData service_data(gatt::ServiceKind::PRIMARY,
                                 /*start=*/0x0001,
                                 /*end=*/0x0009,
                                 kGenericAccessService);
  auto [remote_svc, service_client] =
      fake_gatt()->AddPeerService(peer->identifier(), service_data);

  // Set up preferred connection parameters characteristic.
  att::Handle char_handle = 0x0002;
  att::Handle char_value_handle = 0x0003;
  gatt::CharacteristicData char_data(gatt::kRead,
                                     /*ext_props=*/std::nullopt,
                                     char_handle,
                                     char_value_handle,
                                     kAppearanceCharacteristic);
  service_client->set_characteristics({char_data});
  StaticByteBuffer char_value(0x01, 0x00);
  service_client->set_read_request_callback(
      [char_value_handle, char_value](att::Handle handle, auto read_cb) {
        if (handle == char_value_handle) {
          read_cb(fit::ok(), char_value, /*maybe_truncated=*/false);
        }
      });

  std::unique_ptr<LowEnergyConnectionHandle> conn_ref;
  auto callback =
      [&conn_ref](
          fit::result<HostError, std::unique_ptr<LowEnergyConnectionHandle>>
              result) {
        ASSERT_EQ(fit::ok(), result);
        conn_ref = std::move(result).value();
      };

  conn_mgr()->Connect(
      peer->identifier(), callback, LowEnergyConnectionOptions());

  RunUntilIdle();
  EXPECT_TRUE(conn_ref);
  ASSERT_TRUE(peer->appearance());
  uint16_t device_appearance = peer->appearance().value();
  EXPECT_EQ(device_appearance, 1u);
}

TEST_F(LowEnergyConnectionManagerTest, ReadAppearanceInvalidSize) {
  Peer* peer = peer_cache()->NewPeer(kAddress0, true);
  std::unique_ptr<FakePeer> fake_peer =
      std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  // Set up GAP service
  gatt::ServiceData service_data(gatt::ServiceKind::PRIMARY,
                                 /*start=*/0x0001,
                                 /*end=*/0x0009,
                                 kGenericAccessService);
  auto [remote_svc, service_client] =
      fake_gatt()->AddPeerService(peer->identifier(), service_data);

  // Set up preferred connection parameters characteristic.
  att::Handle char_handle = 0x0002;
  att::Handle char_value_handle = 0x0003;
  gatt::CharacteristicData char_data(gatt::kRead,
                                     /*ext_props=*/std::nullopt,
                                     char_handle,
                                     char_value_handle,
                                     kAppearanceCharacteristic);
  service_client->set_characteristics({char_data});
  StaticByteBuffer invalid_char_value(0x01);  // too small
  service_client->set_read_request_callback(
      [char_value_handle, invalid_char_value](att::Handle handle,
                                              auto read_cb) {
        if (handle == char_value_handle) {
          read_cb(fit::ok(), invalid_char_value, /*maybe_truncated=*/false);
        }
      });

  std::unique_ptr<LowEnergyConnectionHandle> conn_ref;
  auto callback =
      [&conn_ref](
          fit::result<HostError, std::unique_ptr<LowEnergyConnectionHandle>>
              result) {
        ASSERT_EQ(fit::ok(), result);
        conn_ref = std::move(result).value();
      };

  conn_mgr()->Connect(
      peer->identifier(), callback, LowEnergyConnectionOptions());

  RunUntilIdle();
  EXPECT_TRUE(conn_ref);
  EXPECT_FALSE(peer->appearance());
}

TEST_F(
    LowEnergyConnectionManagerTest,
    ReadPeripheralPreferredConnectionParametersCharacteristicAndUpdateConnectionParameters) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  // Set up GAP service
  gatt::ServiceData service_data(gatt::ServiceKind::PRIMARY,
                                 /*start=*/0x0001,
                                 /*end=*/0x0009,
                                 kGenericAccessService);
  auto [remote_svc, service_client] =
      fake_gatt()->AddPeerService(peer->identifier(), service_data);

  // Set up preferred connection parameters characteristic.
  att::Handle char_handle = 0x0002;
  att::Handle char_value_handle = 0x0003;
  gatt::CharacteristicData char_data(
      gatt::kRead,
      /*ext_props=*/std::nullopt,
      char_handle,
      char_value_handle,
      kPeripheralPreferredConnectionParametersCharacteristic);
  service_client->set_characteristics({char_data});

  // TODO(fxbug.dev/42074287): These parameters are invalid, but this test
  // passes because we fail to validate them before sending them to the
  // controller.
  StaticByteBuffer char_value(0x01,
                              0x00,  // min interval
                              0x02,
                              0x00,  // max interval
                              0x03,
                              0x00,  // max latency
                              0x04,
                              0x00);  // supervision timeout
  service_client->set_read_request_callback(
      [char_value_handle, char_value](att::Handle handle, auto read_cb) {
        if (handle == char_value_handle) {
          read_cb(fit::ok(), char_value, /*maybe_truncated=*/false);
        }
      });

  std::unique_ptr<LowEnergyConnectionHandle> conn_ref;
  auto callback = [&conn_ref](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_ref = std::move(result).value();
  };

  conn_mgr()->Connect(
      peer->identifier(), callback, LowEnergyConnectionOptions());

  RunUntilIdle();
  EXPECT_TRUE(conn_ref);
  ASSERT_TRUE(peer->le()->preferred_connection_parameters());
  auto params = peer->le()->preferred_connection_parameters().value();
  EXPECT_EQ(params.min_interval(), 1u);
  EXPECT_EQ(params.max_interval(), 2u);
  EXPECT_EQ(params.max_latency(), 3u);
  EXPECT_EQ(params.supervision_timeout(), 4u);

  std::optional<hci_spec::LEConnectionParameters> conn_params;
  test_device()->set_le_connection_parameters_callback(
      [&](auto, auto parameters) { conn_params = parameters; });

  RunFor(kLEConnectionPauseCentral);
  ASSERT_TRUE(conn_params.has_value());
  EXPECT_EQ(conn_params->interval(),
            1u);  // FakeController will use min interval
  EXPECT_EQ(conn_params->latency(), 3u);
  EXPECT_EQ(conn_params->supervision_timeout(), 4u);
}

TEST_F(
    LowEnergyConnectionManagerTest,
    ReadPeripheralPreferredConnectionParametersCharacteristicInvalidValueSize) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  // Set up GAP service
  gatt::ServiceData service_data(gatt::ServiceKind::PRIMARY,
                                 /*start=*/0x0001,
                                 /*end=*/0x0003,
                                 kGenericAccessService);
  auto [remote_svc, service_client] =
      fake_gatt()->AddPeerService(peer->identifier(), service_data);

  // Set up preferred connection parameters characteristic.
  att::Handle char_handle = 0x0002;
  att::Handle char_value_handle = 0x0003;
  gatt::CharacteristicData char_data(
      gatt::kRead,
      /*ext_props=*/std::nullopt,
      char_handle,
      char_value_handle,
      kPeripheralPreferredConnectionParametersCharacteristic);
  service_client->set_characteristics({char_data});
  StaticByteBuffer invalid_char_value(0x01);  // too small
  service_client->set_read_request_callback(
      [char_value_handle, invalid_char_value](auto handle, auto read_cb) {
        if (handle == char_value_handle) {
          read_cb(fit::ok(), invalid_char_value, /*maybe_truncated=*/false);
        }
      });

  std::unique_ptr<LowEnergyConnectionHandle> conn_ref;
  auto callback = [&conn_ref](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_ref = std::move(result).value();
  };

  conn_mgr()->Connect(
      peer->identifier(), callback, LowEnergyConnectionOptions());

  RunUntilIdle();
  EXPECT_TRUE(conn_ref);
  EXPECT_FALSE(peer->le()->preferred_connection_parameters());
}

TEST_F(LowEnergyConnectionManagerTest, GapServiceCharacteristicDiscoveryError) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  // Set up GAP service
  gatt::ServiceData service_data(gatt::ServiceKind::PRIMARY,
                                 /*start=*/0x0001,
                                 /*end=*/0x0003,
                                 kGenericAccessService);
  auto [remote_svc, service_client] =
      fake_gatt()->AddPeerService(peer->identifier(), service_data);

  // Set up preferred connection parameters characteristic.
  att::Handle char_handle = 0x0002;
  att::Handle char_value_handle = 0x0003;
  gatt::CharacteristicData char_data(
      gatt::kRead,
      /*ext_props=*/std::nullopt,
      char_handle,
      char_value_handle,
      kPeripheralPreferredConnectionParametersCharacteristic);
  service_client->set_characteristic_discovery_status(
      ToResult(att::ErrorCode::kReadNotPermitted));

  std::unique_ptr<LowEnergyConnectionHandle> conn_ref;
  auto callback = [&conn_ref](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_ref = std::move(result).value();
  };

  conn_mgr()->Connect(
      peer->identifier(), callback, LowEnergyConnectionOptions());

  RunUntilIdle();
  EXPECT_TRUE(conn_ref);
  EXPECT_FALSE(peer->le()->preferred_connection_parameters());
}

TEST_F(LowEnergyConnectionManagerTest, GapServiceListServicesError) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  fake_gatt()->set_list_services_status(ToResult(HostError::kFailed));

  std::unique_ptr<LowEnergyConnectionHandle> conn_ref;
  auto callback = [&conn_ref](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_ref = std::move(result).value();
  };

  conn_mgr()->Connect(
      peer->identifier(), callback, LowEnergyConnectionOptions());

  RunUntilIdle();
  EXPECT_TRUE(conn_ref);
  EXPECT_FALSE(peer->le()->preferred_connection_parameters());
}

TEST_F(LowEnergyConnectionManagerTest,
       PeerGapServiceMissingConnectionParameterCharacteristic) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  // Set up GAP service
  gatt::ServiceData service_data(gatt::ServiceKind::PRIMARY,
                                 /*start=*/0x0001,
                                 /*end=*/0x0003,
                                 kGenericAccessService);
  auto [remote_svc, service_client] =
      fake_gatt()->AddPeerService(peer->identifier(), service_data);

  std::unique_ptr<LowEnergyConnectionHandle> conn_ref;
  auto callback = [&conn_ref](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_ref = std::move(result).value();
  };

  conn_mgr()->Connect(
      peer->identifier(), callback, LowEnergyConnectionOptions());

  RunUntilIdle();
  EXPECT_TRUE(conn_ref);
  EXPECT_FALSE(peer->le()->preferred_connection_parameters());
}

// Listener receives remote initiated connection ref.
TEST_F(LowEnergyConnectionManagerTest, PassBondableThroughRemoteInitiatedLink) {
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));

  // First create a fake incoming connection.
  test_device()->ConnectLowEnergy(kAddress0);

  RunUntilIdle();

  auto link = MoveLastRemoteInitiated();
  ASSERT_TRUE(link);

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  conn_mgr()->RegisterRemoteInitiatedLink(
      std::move(link), BondableMode::Bondable, [&conn_handle](auto result) {
        ASSERT_EQ(fit::ok(), result);
        conn_handle = std::move(result).value();
      });
  RunUntilIdle();

  ASSERT_TRUE(conn_handle);
  EXPECT_TRUE(conn_handle->active());
  EXPECT_EQ(conn_handle->bondable_mode(), BondableMode::Bondable);
}

TEST_F(LowEnergyConnectionManagerTest,
       PassNonBondableThroughRemoteInitiatedLink) {
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));

  // First create a fake incoming connection.
  test_device()->ConnectLowEnergy(kAddress0);

  RunUntilIdle();

  auto link = MoveLastRemoteInitiated();
  ASSERT_TRUE(link);

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  conn_mgr()->RegisterRemoteInitiatedLink(
      std::move(link), BondableMode::NonBondable, [&conn_handle](auto result) {
        ASSERT_EQ(fit::ok(), result);
        conn_handle = std::move(result).value();
      });
  RunUntilIdle();

  ASSERT_TRUE(conn_handle);
  EXPECT_TRUE(conn_handle->active());
  EXPECT_EQ(conn_handle->bondable_mode(), BondableMode::NonBondable);
}

// Successful connection to single peer
TEST_F(LowEnergyConnectionManagerTest, PassBondableThroughConnect) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  EXPECT_TRUE(peer->temporary());

  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto callback = [&conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
    ASSERT_TRUE(conn_handle);
    EXPECT_TRUE(conn_handle->active());
  };

  EXPECT_TRUE(connected_peers().empty());
  conn_mgr()->Connect(
      peer->identifier(), callback, {.bondable_mode = BondableMode::Bondable});

  RunUntilIdle();

  ASSERT_TRUE(conn_handle);
  EXPECT_EQ(conn_handle->bondable_mode(), BondableMode::Bondable);
}

// Successful connection to single peer
TEST_F(LowEnergyConnectionManagerTest, PassNonBondableThroughConnect) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  EXPECT_TRUE(peer->temporary());

  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto callback = [&conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
    ASSERT_TRUE(conn_handle);
    EXPECT_TRUE(conn_handle->active());
  };

  EXPECT_TRUE(connected_peers().empty());
  conn_mgr()->Connect(peer->identifier(),
                      callback,
                      {.bondable_mode = BondableMode::NonBondable});

  RunUntilIdle();

  ASSERT_TRUE(conn_handle);
  EXPECT_EQ(conn_handle->bondable_mode(), BondableMode::NonBondable);
}

// Tests that the connection manager cleans up its connection map correctly
// following a disconnection due to encryption failure.
TEST_F(LowEnergyConnectionManagerTest,
       ConnectionCleanUpFollowingEncryptionFailure) {
  // Set up a connection.
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  EXPECT_TRUE(peer->temporary());

  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  std::unique_ptr<LowEnergyConnectionHandle> conn;
  conn_mgr()->Connect(
      peer->identifier(),
      [&](auto result) {
        ASSERT_EQ(fit::ok(), result);
        conn = std::move(result).value();
      },
      kConnectionOptions);
  RunUntilIdle();
  ASSERT_TRUE(conn);

  hci_spec::ConnectionHandle handle = conn->handle();
  bool ref_cleaned_up = false;
  bool disconnected = false;
  conn->set_closed_callback([&] { ref_cleaned_up = true; });
  conn_mgr()->SetDisconnectCallbackForTesting(
      [&](hci_spec::ConnectionHandle cb_handle) {
        EXPECT_EQ(handle, cb_handle);
        disconnected = true;
      });

  test_device()->SendEncryptionChangeEvent(
      handle,
      pw::bluetooth::emboss::StatusCode::CONNECTION_TERMINATED_MIC_FAILURE,
      pw::bluetooth::emboss::EncryptionStatus::OFF);
  test_device()->SendDisconnectionCompleteEvent(handle);
  RunUntilIdle();

  EXPECT_TRUE(ref_cleaned_up);
  EXPECT_TRUE(disconnected);
}

TEST_F(LowEnergyConnectionManagerTest,
       SuccessfulInterrogationSetsPeerVersionAndFeatures) {
  constexpr hci_spec::LESupportedFeatures kLEFeatures{static_cast<uint64_t>(
      hci_spec::LESupportedFeature::kConnectionParametersRequestProcedure)};

  // Set up a connection.
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  ASSERT_TRUE(peer->le());

  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  fake_peer->set_le_features(kLEFeatures);
  test_device()->AddPeer(std::move(fake_peer));

  std::unique_ptr<LowEnergyConnectionHandle> conn;
  conn_mgr()->Connect(
      peer->identifier(),
      [&](auto result) {
        ASSERT_EQ(fit::ok(), result);
        conn = std::move(result).value();
      },
      kConnectionOptions);

  EXPECT_FALSE(peer->version().has_value());
  EXPECT_FALSE(peer->le()->features().has_value());
  RunUntilIdle();
  EXPECT_TRUE(conn);
  EXPECT_TRUE(peer->version().has_value());
  EXPECT_TRUE(peer->le()->features().has_value());
  EXPECT_EQ(kLEFeatures, peer->le()->features());
  EXPECT_FALSE(peer->temporary());
}

TEST_F(LowEnergyConnectionManagerTest, ConnectInterrogationFailure) {
  // Set up a connection.
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  ASSERT_TRUE(peer->le());

  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  std::optional<HostError> error;
  conn_mgr()->Connect(
      peer->identifier(),
      [&](auto result) {
        ASSERT_TRUE(result.is_error());
        error = result.error_value();
      },
      kConnectionOptions);
  ASSERT_FALSE(peer->le()->features().has_value());

  // Remove fake peer so LE Read Remote Features command fails during
  // interrogation.
  test_device()->set_le_read_remote_features_callback(
      [this]() { test_device()->RemovePeer(kAddress0); });

  RunUntilIdle();
  ASSERT_TRUE(error.has_value());
  EXPECT_FALSE(peer->connected());
  EXPECT_FALSE(peer->le()->connected());
  EXPECT_FALSE(peer->temporary());
}

TEST_F(LowEnergyConnectionManagerTest,
       RemoteInitiatedLinkInterrogationFailure) {
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));

  // First create a fake incoming connection.
  test_device()->ConnectLowEnergy(kAddress0);

  RunUntilIdle();

  auto link = MoveLastRemoteInitiated();
  ASSERT_TRUE(link);

  std::optional<HostError> error;
  conn_mgr()->RegisterRemoteInitiatedLink(
      std::move(link), BondableMode::Bondable, [&](auto result) {
        ASSERT_TRUE(result.is_error());
        error = result.error_value();
      });

  // Remove fake peer so LE Read Remote Features command fails during
  // interrogation.
  test_device()->set_le_read_remote_features_callback(
      [this]() { test_device()->RemovePeer(kAddress0); });

  RunUntilIdle();
  ASSERT_TRUE(error.has_value());

  // A Peer should now exist in the cache.
  auto* peer = peer_cache()->FindByAddress(kAddress0);
  ASSERT_TRUE(peer);
  EXPECT_FALSE(peer->connected());
  EXPECT_FALSE(peer->le()->connected());
  EXPECT_FALSE(peer->temporary());
}

TEST_F(LowEnergyConnectionManagerTest,
       L2capRequestConnParamUpdateAfterInterrogation) {
  const hci_spec::LEPreferredConnectionParameters kConnParams(
      hci_spec::defaults::kLEConnectionIntervalMin,
      hci_spec::defaults::kLEConnectionIntervalMax,
      /*max_latency=*/0,
      hci_spec::defaults::kLESupervisionTimeout);

  // Connection Parameter Update procedure NOT supported.
  constexpr hci_spec::LESupportedFeatures kLEFeatures{0};
  auto peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  peer->set_le_features(kLEFeatures);
  test_device()->AddPeer(std::move(peer));

  // First create a fake incoming connection as peripheral.
  test_device()->ConnectLowEnergy(
      kAddress0, pw::bluetooth::emboss::ConnectionRole::PERIPHERAL);

  RunUntilIdle();

  auto link = MoveLastRemoteInitiated();
  ASSERT_TRUE(link);

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  conn_mgr()->RegisterRemoteInitiatedLink(
      std::move(link), BondableMode::Bondable, [&](auto result) {
        ASSERT_EQ(fit::ok(), result);
        conn_handle = std::move(result).value();
      });

  size_t l2cap_conn_param_update_count = 0;
  fake_l2cap()->set_connection_parameter_update_request_responder(
      [&](auto, auto params) {
        EXPECT_EQ(kConnParams, params);
        l2cap_conn_param_update_count++;
        return true;
      });

  size_t hci_update_conn_param_count = 0;
  test_device()->set_le_connection_parameters_callback(
      [&](auto, auto) { hci_update_conn_param_count++; });

  RunUntilIdle();
  ASSERT_TRUE(conn_handle);
  EXPECT_TRUE(conn_handle->active());
  EXPECT_EQ(0u, l2cap_conn_param_update_count);
  EXPECT_EQ(0u, hci_update_conn_param_count);

  RunFor(kLEConnectionPausePeripheral);
  EXPECT_EQ(1u, l2cap_conn_param_update_count);
  EXPECT_EQ(0u, hci_update_conn_param_count);
}

// Based on PTS L2CAP/LE/CPU/BV-01-C, in which the LE feature mask indicates
// support for the Connection Parameter Request Procedure, but sending the
// request results in a kUnsupportedRemoteFeature event status. PTS expects the
// host to retry with a L2cap connection parameter request.
//
// Test that this behavior is followed for 2 concurrent connections in order to
// ensure correct command/event handling.
TEST_F(LowEnergyConnectionManagerTest,
       PeripheralsRetryLLConnectionUpdateWithL2capRequest) {
  auto peer0 = std::make_unique<FakePeer>(kAddress0, dispatcher());
  auto peer1 = std::make_unique<FakePeer>(kAddress1, dispatcher());

  // Connection Parameter Update procedure supported by controller.
  constexpr hci_spec::LESupportedFeatures kLEFeatures{static_cast<uint64_t>(
      hci_spec::LESupportedFeature::kConnectionParametersRequestProcedure)};

  peer0->set_le_features(kLEFeatures);
  peer1->set_le_features(kLEFeatures);

  // Simulate host rejection by causing FakeController to set LE Connection
  // Update Complete status to kUnsupportedRemoteFeature, as PTS does.
  peer0->set_supports_ll_conn_update_procedure(false);
  peer1->set_supports_ll_conn_update_procedure(false);

  test_device()->AddPeer(std::move(peer0));
  test_device()->AddPeer(std::move(peer1));

  // First create fake incoming connections with local host as peripheral.
  test_device()->ConnectLowEnergy(
      kAddress0, pw::bluetooth::emboss::ConnectionRole::PERIPHERAL);
  RunUntilIdle();
  auto link0 = MoveLastRemoteInitiated();
  ASSERT_TRUE(link0);

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle0;
  conn_mgr()->RegisterRemoteInitiatedLink(
      std::move(link0), BondableMode::Bondable, [&](auto result) {
        ASSERT_EQ(fit::ok(), result);
        conn_handle0 = std::move(result).value();
      });

  test_device()->ConnectLowEnergy(
      kAddress1, pw::bluetooth::emboss::ConnectionRole::PERIPHERAL);
  RunUntilIdle();
  auto link1 = MoveLastRemoteInitiated();
  ASSERT_TRUE(link1);

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle1;
  conn_mgr()->RegisterRemoteInitiatedLink(
      std::move(link1), BondableMode::Bondable, [&](auto result) {
        ASSERT_EQ(fit::ok(), result);
        conn_handle1 = std::move(result).value();
      });

  size_t l2cap_conn_param_update_count0 = 0;
  size_t l2cap_conn_param_update_count1 = 0;
  size_t hci_update_conn_param_count0 = 0;
  size_t hci_update_conn_param_count1 = 0;

  fake_l2cap()->set_connection_parameter_update_request_responder(
      [&](auto handle, auto) {
        if (handle == conn_handle0->handle()) {
          l2cap_conn_param_update_count0++;
          // connection update commands should be sent before l2cap requests
          EXPECT_EQ(hci_update_conn_param_count0, 1u);
        } else if (handle == conn_handle1->handle()) {
          l2cap_conn_param_update_count1++;
          EXPECT_EQ(hci_update_conn_param_count1, 1u);
        } else {
          ADD_FAILURE();
        }
        return true;
      });

  test_device()->set_le_connection_parameters_callback([&](auto address, auto) {
    if (address == kAddress0) {
      hci_update_conn_param_count0++;
      // l2cap requests should not be sent until after failed HCI connection
      // update commands
      EXPECT_EQ(l2cap_conn_param_update_count0, 0u);
    } else if (address == kAddress1) {
      hci_update_conn_param_count1++;
      EXPECT_EQ(l2cap_conn_param_update_count1, 0u);
    } else {
      ADD_FAILURE();
    }
  });

  RunFor(kLEConnectionPausePeripheral);
  ASSERT_TRUE(conn_handle0);
  EXPECT_TRUE(conn_handle0->active());
  ASSERT_TRUE(conn_handle1);
  EXPECT_TRUE(conn_handle1->active());

  EXPECT_EQ(conn_handle0->role(),
            pw::bluetooth::emboss::ConnectionRole::PERIPHERAL);
  EXPECT_EQ(conn_handle1->role(),
            pw::bluetooth::emboss::ConnectionRole::PERIPHERAL);

  EXPECT_EQ(1u, hci_update_conn_param_count0);
  EXPECT_EQ(1u, l2cap_conn_param_update_count0);
  EXPECT_EQ(1u, hci_update_conn_param_count1);
  EXPECT_EQ(1u, l2cap_conn_param_update_count1);

  // l2cap requests should not be sent on subsequent events
  test_device()->SendLEConnectionUpdateCompleteSubevent(
      conn_handle1->handle(),
      hci_spec::LEConnectionParameters(),
      pw::bluetooth::emboss::StatusCode::UNSUPPORTED_REMOTE_FEATURE);
  RunUntilIdle();
  EXPECT_EQ(1u, l2cap_conn_param_update_count0);
  EXPECT_EQ(1u, l2cap_conn_param_update_count1);
}

// Based on PTS L2CAP/LE/CPU/BV-01-C. When run twice, the controller caches the
// LE Connection Update Complete kUnsupportedRemoteFeature status and returns it
// directly in future LE Connection Update Command Status events. The host
// should retry with the L2CAP Connection Parameter Update Request after
// receiving this kUnsupportedRemoteFeature command status.
TEST_F(
    LowEnergyConnectionManagerTest,
    PeripheralSendsL2capConnParamReqAfterConnUpdateCommandStatusUnsupportedRemoteFeature) {
  auto peer = std::make_unique<FakePeer>(kAddress0, dispatcher());

  // Connection Parameter Update procedure supported by controller.
  constexpr hci_spec::LESupportedFeatures kLEFeatures{static_cast<uint64_t>(
      hci_spec::LESupportedFeature::kConnectionParametersRequestProcedure)};
  peer->set_le_features(kLEFeatures);
  test_device()->AddPeer(std::move(peer));

  // First create a fake incoming connection with local host as peripheral.
  test_device()->ConnectLowEnergy(
      kAddress0, pw::bluetooth::emboss::ConnectionRole::PERIPHERAL);
  RunUntilIdle();

  auto link = MoveLastRemoteInitiated();
  ASSERT_TRUE(link);

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  conn_mgr()->RegisterRemoteInitiatedLink(
      std::move(link), BondableMode::Bondable, [&](auto result) {
        ASSERT_EQ(fit::ok(), result);
        conn_handle = std::move(result).value();
      });

  size_t l2cap_conn_param_update_count = 0;
  size_t hci_update_conn_param_count = 0;

  fake_l2cap()->set_connection_parameter_update_request_responder(
      [&](auto, auto) {
        l2cap_conn_param_update_count++;
        return true;
      });

  test_device()->set_le_connection_parameters_callback(
      [&](auto, auto) { hci_update_conn_param_count++; });

  test_device()->SetDefaultCommandStatus(
      hci_spec::kLEConnectionUpdate,
      pw::bluetooth::emboss::StatusCode::UNSUPPORTED_REMOTE_FEATURE);

  RunFor(kLEConnectionPausePeripheral);
  ASSERT_TRUE(conn_handle);
  EXPECT_TRUE(conn_handle->active());
  EXPECT_EQ(0u, hci_update_conn_param_count);
  EXPECT_EQ(1u, l2cap_conn_param_update_count);

  test_device()->ClearDefaultCommandStatus(hci_spec::kLEConnectionUpdate);

  // l2cap request should not be called on subsequent events
  test_device()->SendLEConnectionUpdateCompleteSubevent(
      conn_handle->handle(),
      hci_spec::LEConnectionParameters(),
      pw::bluetooth::emboss::StatusCode::UNSUPPORTED_REMOTE_FEATURE);

  RunUntilIdle();
  EXPECT_EQ(1u, l2cap_conn_param_update_count);
}

// A peripheral should not attempt to handle the next LE Connection Update
// Complete event if the status of the LE Connection Update command is not
// success.
TEST_F(
    LowEnergyConnectionManagerTest,
    PeripheralDoesNotSendL2capConnParamReqAfterConnUpdateCommandStatusError) {
  auto peer = std::make_unique<FakePeer>(kAddress0, dispatcher());

  // Connection Parameter Update procedure supported by controller.
  constexpr hci_spec::LESupportedFeatures kLEFeatures{static_cast<uint64_t>(
      hci_spec::LESupportedFeature::kConnectionParametersRequestProcedure)};
  peer->set_le_features(kLEFeatures);
  test_device()->AddPeer(std::move(peer));

  // First create a fake incoming connection with local host as peripheral.
  test_device()->ConnectLowEnergy(
      kAddress0, pw::bluetooth::emboss::ConnectionRole::PERIPHERAL);
  RunUntilIdle();

  auto link = MoveLastRemoteInitiated();
  ASSERT_TRUE(link);

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  conn_mgr()->RegisterRemoteInitiatedLink(
      std::move(link), BondableMode::Bondable, [&](auto result) {
        ASSERT_EQ(fit::ok(), result);
        conn_handle = std::move(result).value();
      });

  size_t l2cap_conn_param_update_count = 0;
  size_t hci_update_conn_param_count = 0;

  fake_l2cap()->set_connection_parameter_update_request_responder(
      [&](auto, auto) {
        l2cap_conn_param_update_count++;
        return true;
      });

  test_device()->set_le_connection_parameters_callback(
      [&](auto, auto) { hci_update_conn_param_count++; });

  test_device()->SetDefaultCommandStatus(
      hci_spec::kLEConnectionUpdate,
      pw::bluetooth::emboss::StatusCode::UNSPECIFIED_ERROR);

  RunFor(kLEConnectionPausePeripheral);
  ASSERT_TRUE(conn_handle);
  EXPECT_TRUE(conn_handle->active());
  EXPECT_EQ(conn_handle->role(),
            pw::bluetooth::emboss::ConnectionRole::PERIPHERAL);
  EXPECT_EQ(0u, hci_update_conn_param_count);
  EXPECT_EQ(0u, l2cap_conn_param_update_count);

  test_device()->ClearDefaultCommandStatus(hci_spec::kLEConnectionUpdate);

  // l2cap request should not be called on subsequent events
  test_device()->SendLEConnectionUpdateCompleteSubevent(
      conn_handle->handle(),
      hci_spec::LEConnectionParameters(),
      pw::bluetooth::emboss::StatusCode::UNSUPPORTED_REMOTE_FEATURE);

  RunUntilIdle();
  EXPECT_EQ(0u, l2cap_conn_param_update_count);
}

TEST_F(LowEnergyConnectionManagerTest, HciUpdateConnParamsAfterInterrogation) {
  constexpr hci_spec::LESupportedFeatures kLEFeatures{static_cast<uint64_t>(
      hci_spec::LESupportedFeature::kConnectionParametersRequestProcedure)};

  auto peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  peer->set_le_features(kLEFeatures);
  test_device()->AddPeer(std::move(peer));

  // First create a fake incoming connection.
  test_device()->ConnectLowEnergy(
      kAddress0, pw::bluetooth::emboss::ConnectionRole::PERIPHERAL);

  RunUntilIdle();

  auto link = MoveLastRemoteInitiated();
  ASSERT_TRUE(link);

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  conn_mgr()->RegisterRemoteInitiatedLink(
      std::move(link), BondableMode::Bondable, [&](auto result) {
        ASSERT_EQ(fit::ok(), result);
        conn_handle = std::move(result).value();
      });

  size_t l2cap_conn_param_update_count = 0;
  fake_l2cap()->set_connection_parameter_update_request_responder(
      [&](auto, const auto) {
        l2cap_conn_param_update_count++;
        return true;
      });

  size_t hci_update_conn_param_count = 0;
  test_device()->set_le_connection_parameters_callback(
      [&](auto, const hci_spec::LEConnectionParameters& params) {
        // FakeController will pick an interval between min and max interval.
        EXPECT_TRUE(
            params.interval() >= hci_spec::defaults::kLEConnectionIntervalMin &&
            params.interval() <= hci_spec::defaults::kLEConnectionIntervalMax);
        EXPECT_EQ(0u, params.latency());
        EXPECT_EQ(hci_spec::defaults::kLESupervisionTimeout,
                  params.supervision_timeout());
        hci_update_conn_param_count++;
      });

  RunUntilIdle();
  ASSERT_TRUE(conn_handle);
  EXPECT_TRUE(conn_handle->active());
  EXPECT_EQ(conn_handle->role(),
            pw::bluetooth::emboss::ConnectionRole::PERIPHERAL);
  EXPECT_EQ(0u, l2cap_conn_param_update_count);
  EXPECT_EQ(0u, hci_update_conn_param_count);

  RunFor(kLEConnectionPausePeripheral);
  EXPECT_EQ(0u, l2cap_conn_param_update_count);
  EXPECT_EQ(1u, hci_update_conn_param_count);
}

TEST_F(LowEnergyConnectionManagerTest,
       CentralUpdatesConnectionParametersToDefaultsAfterInitialization) {
  // Set up a connection.
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  ASSERT_TRUE(peer->le());

  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));

  size_t hci_update_conn_param_count = 0;
  test_device()->set_le_connection_parameters_callback(
      [&](auto, const hci_spec::LEConnectionParameters& params) {
        // FakeController will pick an interval between min and max interval.
        EXPECT_TRUE(
            params.interval() >= hci_spec::defaults::kLEConnectionIntervalMin &&
            params.interval() <= hci_spec::defaults::kLEConnectionIntervalMax);
        EXPECT_EQ(0u, params.latency());
        EXPECT_EQ(hci_spec::defaults::kLESupervisionTimeout,
                  params.supervision_timeout());
        hci_update_conn_param_count++;
      });

  std::unique_ptr<LowEnergyConnectionHandle> conn;
  conn_mgr()->Connect(
      peer->identifier(),
      [&](auto result) {
        ASSERT_EQ(fit::ok(), result);
        conn = std::move(result).value();
      },
      kConnectionOptions);

  RunUntilIdle();
  EXPECT_EQ(0u, hci_update_conn_param_count);

  RunFor(kLEConnectionPauseCentral);
  EXPECT_EQ(1u, hci_update_conn_param_count);
  EXPECT_TRUE(conn);
}

TEST_F(LowEnergyConnectionManagerTest, ConnectCalledForPeerBeingInterrogated) {
  // Set up a connection.
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  ASSERT_TRUE(peer->le());

  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  // Prevent remote features event from being received.
  test_device()->SetDefaultCommandStatus(
      hci_spec::kLEReadRemoteFeatures,
      pw::bluetooth::emboss::StatusCode::SUCCESS);

  conn_mgr()->Connect(
      peer->identifier(),
      [&](auto result) { ASSERT_TRUE(result.is_error()); },
      kConnectionOptions);

  RunUntilIdle();
  // Interrogation should not complete.
  EXPECT_FALSE(peer->le()->features().has_value());

  // Connect to same peer again, before interrogation has completed.
  // No asserts should fail.
  conn_mgr()->Connect(
      peer->identifier(),
      [&](auto result) { ASSERT_TRUE(result.is_error()); },
      kConnectionOptions);
  RunUntilIdle();
}

LowEnergyConnectionManager::ConnectionResultCallback
MakeConnectionResultCallback(
    std::unique_ptr<LowEnergyConnectionHandle>& conn_handle) {
  return [&conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
    EXPECT_TRUE(conn_handle);
    EXPECT_TRUE(conn_handle->active());
  };
}

// Test that active connections not meeting the requirements for Secure
// Connections Only mode are disconnected when the security mode is changed to
// SC Only.
TEST_F(LowEnergyConnectionManagerTest,
       SecureConnectionsOnlyDisconnectsInsufficientSecurity) {
  Peer* encrypted_peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  Peer* unencrypted_peer =
      peer_cache()->NewPeer(kAddress1, /*connectable=*/true);
  Peer* secure_authenticated_peer =
      peer_cache()->NewPeer(kAddress3, /*connectable=*/true);
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress1, dispatcher()));
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress3, dispatcher()));

  std::unique_ptr<LowEnergyConnectionHandle> unencrypted_conn_handle,
      encrypted_conn_handle, secure_authenticated_conn_handle;
  EXPECT_TRUE(connected_peers().empty());
  conn_mgr()->Connect(unencrypted_peer->identifier(),
                      MakeConnectionResultCallback(unencrypted_conn_handle),
                      kConnectionOptions);
  conn_mgr()->Connect(encrypted_peer->identifier(),
                      MakeConnectionResultCallback(encrypted_conn_handle),
                      kConnectionOptions);
  conn_mgr()->Connect(
      secure_authenticated_peer->identifier(),
      MakeConnectionResultCallback(secure_authenticated_conn_handle),
      kConnectionOptions);
  RunUntilIdle();
  std::function<void(sm::Result<>)> pair_cb = [](sm::Result<> s) {
    EXPECT_EQ(fit::ok(), s);
  };
  EXPECT_EQ(3u, connected_peers().size());
  ASSERT_TRUE(unencrypted_conn_handle);
  ASSERT_TRUE(encrypted_conn_handle);
  ASSERT_TRUE(secure_authenticated_conn_handle);
  EXPECT_TRUE(unencrypted_conn_handle->active());
  EXPECT_TRUE(secure_authenticated_conn_handle->active());
  EXPECT_TRUE(encrypted_conn_handle->active());

  // "Pair" to the encrypted peers to get to the correct security level.
  conn_mgr()->Pair(encrypted_peer->identifier(),
                   sm::SecurityLevel::kEncrypted,
                   sm::BondableMode::Bondable,
                   pair_cb);
  conn_mgr()->Pair(secure_authenticated_peer->identifier(),
                   sm::SecurityLevel::kSecureAuthenticated,
                   sm::BondableMode::Bondable,
                   pair_cb);
  RunUntilIdle();
  EXPECT_EQ(sm::SecurityLevel::kNoSecurity,
            unencrypted_conn_handle->security().level());
  EXPECT_EQ(sm::SecurityLevel::kEncrypted,
            encrypted_conn_handle->security().level());
  EXPECT_EQ(sm::SecurityLevel::kSecureAuthenticated,
            secure_authenticated_conn_handle->security().level());

  // Setting Secure Connections Only mode causes connections not allowed under
  // this mode to be disconnected (in this case, `encrypted_peer` is encrypted,
  // SC-generated, and with max encryption key size, but not authenticated).
  conn_mgr()->SetSecurityMode(LESecurityMode::SecureConnectionsOnly);
  RunUntilIdle();
  EXPECT_EQ(LESecurityMode::SecureConnectionsOnly, conn_mgr()->security_mode());
  EXPECT_EQ(2u, connected_peers().size());
  EXPECT_TRUE(unencrypted_conn_handle->active());
  EXPECT_TRUE(secure_authenticated_conn_handle->active());
  EXPECT_FALSE(encrypted_conn_handle->active());
}

// Test that both existing and new peers pick up on a change to Secure
// Connections Only mode.
TEST_F(LowEnergyConnectionManagerTest, SetSecureConnectionsOnlyModeWorks) {
  // LE Connection Manager defaults to Mode 1.
  EXPECT_EQ(LESecurityMode::Mode1, conn_mgr()->security_mode());

  // This peer will already be connected when we set LE Secure Connections Only
  // mode.
  Peer* existing_peer = peer_cache()->NewPeer(kAddress1, /*connectable=*/true);
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress1, dispatcher()));
  std::unique_ptr<LowEnergyConnectionHandle> existing_conn_handle;
  RunUntilIdle();

  conn_mgr()->Connect(existing_peer->identifier(),
                      MakeConnectionResultCallback(existing_conn_handle),
                      kConnectionOptions);
  RunUntilIdle();
  TestSm::WeakPtr existing_peer_sm =
      TestSmByHandle(existing_conn_handle->handle());
  ASSERT_TRUE(existing_peer_sm.is_alive());
  EXPECT_EQ(LESecurityMode::Mode1, existing_peer_sm->security_mode());
  EXPECT_EQ(1u, connected_peers().size());

  conn_mgr()->SetSecurityMode(LESecurityMode::SecureConnectionsOnly);
  RunUntilIdle();

  EXPECT_EQ(LESecurityMode::SecureConnectionsOnly,
            existing_peer_sm->security_mode());

  // This peer is connected after setting LE Secure Connections Only mode.
  Peer* new_peer = peer_cache()->NewPeer(kAddress3, /*connectable=*/true);
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress3, dispatcher()));
  std::unique_ptr<LowEnergyConnectionHandle> new_conn_handle;

  conn_mgr()->Connect(new_peer->identifier(),
                      MakeConnectionResultCallback(new_conn_handle),
                      kConnectionOptions);
  RunUntilIdle();
  TestSm::WeakPtr new_peer_sm = TestSmByHandle(new_conn_handle->handle());
  ASSERT_TRUE(new_peer_sm.is_alive());
  EXPECT_EQ(2u, connected_peers().size());

  EXPECT_EQ(LESecurityMode::SecureConnectionsOnly,
            new_peer_sm->security_mode());
}

TEST_F(LowEnergyConnectionManagerTest,
       ConnectAndInterrogateSecondPeerDuringInterrogationOfFirstPeer) {
  auto* peer_0 = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  ASSERT_TRUE(peer_0->le());

  auto fake_peer_0 = std::make_unique<FakePeer>(kAddress0, dispatcher());
  auto fake_peer_0_ptr = fake_peer_0.get();
  test_device()->AddPeer(std::move(fake_peer_0));

  // Prevent remote features event from being received.
  test_device()->SetDefaultCommandStatus(
      hci_spec::kLEReadRemoteFeatures,
      pw::bluetooth::emboss::StatusCode::SUCCESS);

  std::unique_ptr<LowEnergyConnectionHandle> conn_0;
  conn_mgr()->Connect(
      peer_0->identifier(),
      [&conn_0](auto result) {
        ASSERT_EQ(fit::ok(), result);
        conn_0 = std::move(result).value();
        ASSERT_TRUE(conn_0);
      },
      kConnectionOptions);

  RunUntilIdle();
  // Interrogation should not complete.
  EXPECT_FALSE(peer_0->le()->connected());
  EXPECT_FALSE(conn_0);

  auto* peer_1 = peer_cache()->NewPeer(kAddress1, /*connectable=*/true);
  ASSERT_TRUE(peer_1->le());

  auto fake_peer_1 = std::make_unique<FakePeer>(kAddress1, dispatcher());
  auto fake_peer_1_ptr = fake_peer_1.get();
  test_device()->AddPeer(std::move(fake_peer_1));

  // Connect to different peer, before interrogation has completed.
  std::unique_ptr<LowEnergyConnectionHandle> conn_1;
  conn_mgr()->Connect(
      peer_1->identifier(),
      [&conn_1](auto result) {
        ASSERT_EQ(fit::ok(), result);
        conn_1 = std::move(result).value();
        ASSERT_TRUE(conn_1);
      },
      kConnectionOptions);
  RunUntilIdle();

  // Complete interrogation of peer_0
  ASSERT_FALSE(fake_peer_0_ptr->logical_links().empty());
  auto handle_0 = *fake_peer_0_ptr->logical_links().begin();

  auto response = hci::EventPacket::New<
      pw::bluetooth::emboss::LEReadRemoteFeaturesCompleteSubeventWriter>(
      hci_spec::kLEMetaEventCode);
  auto view = response.view_t();
  view.le_meta_event().subevent_code().Write(
      hci_spec::kLEReadRemoteFeaturesCompleteSubeventCode);
  view.connection_handle().Write(handle_0);
  view.status().Write(pw::bluetooth::emboss::StatusCode::SUCCESS);
  view.le_features().BackingStorage().WriteUInt(0u);
  test_device()->SendCommandChannelPacket(response.data());
  RunUntilIdle();
  EXPECT_TRUE(conn_0);
  EXPECT_TRUE(peer_0->le()->connected());

  // Complete interrogation of peer_1
  ASSERT_FALSE(fake_peer_1_ptr->logical_links().empty());
  auto handle_1 = *fake_peer_0_ptr->logical_links().begin();
  view.connection_handle().Write(handle_1);
  test_device()->SendCommandChannelPacket(response.data());
  RunUntilIdle();
  EXPECT_TRUE(conn_1);
  EXPECT_TRUE(peer_1->le()->connected());
}

TEST_F(LowEnergyConnectionManagerTest,
       ConnectSecondPeerDuringInterrogationOfFirstPeer) {
  auto* peer_0 = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  ASSERT_TRUE(peer_0->le());

  auto fake_peer_0 = std::make_unique<FakePeer>(kAddress0, dispatcher());
  auto fake_peer_0_ptr = fake_peer_0.get();
  test_device()->AddPeer(std::move(fake_peer_0));

  // Prevent remote features event from being received.
  test_device()->SetDefaultCommandStatus(
      hci_spec::kLEReadRemoteFeatures,
      pw::bluetooth::emboss::StatusCode::SUCCESS);

  std::unique_ptr<LowEnergyConnectionHandle> conn_0;
  conn_mgr()->Connect(
      peer_0->identifier(),
      [&](auto result) {
        ASSERT_EQ(fit::ok(), result);
        conn_0 = std::move(result).value();
      },
      kConnectionOptions);

  RunUntilIdle();
  // Interrogation should not complete.
  EXPECT_FALSE(peer_0->le()->connected());
  EXPECT_FALSE(conn_0);

  test_device()->ClearDefaultCommandStatus(hci_spec::kLEReadRemoteFeatures);
  // Stall connection complete for peer 1.
  test_device()->SetDefaultCommandStatus(
      hci_spec::kLECreateConnection,
      pw::bluetooth::emboss::StatusCode::SUCCESS);

  auto* peer_1 = peer_cache()->NewPeer(kAddress1, /*connectable=*/true);
  ASSERT_TRUE(peer_1->le());

  auto fake_peer_1 = std::make_unique<FakePeer>(kAddress1, dispatcher());
  test_device()->AddPeer(std::move(fake_peer_1));

  // Connect to different peer, before interrogation has completed.
  conn_mgr()->Connect(
      peer_1->identifier(),
      [&](auto result) { EXPECT_TRUE(result.is_error()); },
      kConnectionOptions);
  RunUntilIdle();

  // Complete interrogation of peer_0. No asserts should fail.
  ASSERT_FALSE(fake_peer_0_ptr->logical_links().empty());
  auto handle_0 = *fake_peer_0_ptr->logical_links().begin();
  auto response = hci::EventPacket::New<
      pw::bluetooth::emboss::LEReadRemoteFeaturesCompleteSubeventWriter>(
      hci_spec::kLEMetaEventCode);
  auto view = response.view_t();
  view.le_meta_event().subevent_code().Write(
      hci_spec::kLEReadRemoteFeaturesCompleteSubeventCode);
  view.connection_handle().Write(handle_0);
  view.status().Write(pw::bluetooth::emboss::StatusCode::SUCCESS);
  view.le_features().BackingStorage().WriteUInt(0u);
  test_device()->SendCommandChannelPacket(response.data());
  RunUntilIdle();
  EXPECT_TRUE(conn_0);
  EXPECT_TRUE(peer_0->le()->connected());
}

TEST_F(LowEnergyConnectionManagerTest,
       SynchonousInterrogationAndNoCallbackRetainsConnectionRef) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  ASSERT_TRUE(peer->le());

  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  std::unique_ptr<LowEnergyConnectionHandle> conn;
  conn_mgr()->Connect(
      peer->identifier(),
      [&](auto result) {
        ASSERT_EQ(fit::ok(), result);
        conn = std::move(result).value();
      },
      kConnectionOptions);

  RunUntilIdle();
  EXPECT_TRUE(peer->le()->connected());
  EXPECT_TRUE(conn);

  // Disconnect
  conn = nullptr;
  RunUntilIdle();

  // Second interrogation will complete synchronously because peer has already
  // been interrogated.
  bool conn_cb_called = false;
  conn_mgr()->Connect(
      peer->identifier(),
      [&](auto result) {
        conn_cb_called = true;
        EXPECT_EQ(fit::ok(), result);
        // Don't retain ref.
      },
      kConnectionOptions);
  // Wait for connect complete event.
  RunUntilIdle();
  EXPECT_TRUE(conn_cb_called);
}

TEST_F(LowEnergyConnectionManagerTest, AutoConnectSkipsScanning) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  EXPECT_TRUE(peer->temporary());

  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  size_t scan_cb_count = 0;
  test_device()->set_scan_state_callback(
      [&scan_cb_count](bool) { scan_cb_count++; });

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto callback = [&conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
    EXPECT_TRUE(conn_handle->active());
  };

  EXPECT_TRUE(connected_peers().empty());
  LowEnergyConnectionOptions options{.auto_connect = true};
  conn_mgr()->Connect(peer->identifier(), callback, options);
  ASSERT_TRUE(peer->le());
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer->le()->connection_state());

  RunUntilIdle();

  EXPECT_EQ(1u, connected_peers().size());
  EXPECT_EQ(1u, connected_peers().count(kAddress0));

  ASSERT_TRUE(conn_handle);
  EXPECT_TRUE(conn_handle->active());
  EXPECT_EQ(peer->identifier(), conn_handle->peer_identifier());
  EXPECT_FALSE(peer->temporary());
  EXPECT_EQ(Peer::ConnectionState::kConnected, peer->le()->connection_state());
  EXPECT_EQ(scan_cb_count, 0u);
}

TEST_F(LowEnergyConnectionManagerTest,
       PeerDisconnectBeforeInterrogationCompletes) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  EXPECT_TRUE(peer->temporary());

  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  auto fake_peer_ptr = fake_peer.get();
  test_device()->AddPeer(std::move(fake_peer));

  // Cause interrogation to stall by not responding with a Read Remote Version
  // complete event.
  test_device()->SetDefaultCommandStatus(
      hci_spec::kReadRemoteVersionInfo,
      pw::bluetooth::emboss::StatusCode::SUCCESS);

  int connect_count = 0;
  auto callback = [&connect_count](auto result) {
    ASSERT_TRUE(result.is_error());
    connect_count++;
  };

  EXPECT_TRUE(connected_peers().empty());
  conn_mgr()->Connect(peer->identifier(), callback, kConnectionOptions);
  ASSERT_TRUE(peer->le());
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer->le()->connection_state());

  RunUntilIdle();

  ASSERT_FALSE(fake_peer_ptr->logical_links().empty());
  auto handle = *fake_peer_ptr->logical_links().begin();

  test_device()->Disconnect(peer->address());

  RunUntilIdle();

  // Complete interrogation so that callback gets called.
  auto response = hci::EventPacket::New<
      pw::bluetooth::emboss::ReadRemoteVersionInfoCompleteEventWriter>(
      hci_spec::kReadRemoteVersionInfoCompleteEventCode);
  auto view = response.view_t();
  view.status().Write(pw::bluetooth::emboss::StatusCode::SUCCESS);
  view.connection_handle().Write(handle);
  test_device()->SendCommandChannelPacket(response.data());

  RunUntilIdle();
  EXPECT_EQ(0u, connected_peers().size());
  EXPECT_EQ(1, connect_count);
  EXPECT_EQ(Peer::ConnectionState::kNotConnected,
            peer->le()->connection_state());
}

TEST_F(LowEnergyConnectionManagerTest,
       LocalDisconnectBeforeInterrogationCompletes) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  EXPECT_TRUE(peer->temporary());

  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  auto fake_peer_ptr = fake_peer.get();
  test_device()->AddPeer(std::move(fake_peer));

  // Cause interrogation to stall by not responding with a Read Remote Version
  // complete event.
  test_device()->SetDefaultCommandStatus(
      hci_spec::kReadRemoteVersionInfo,
      pw::bluetooth::emboss::StatusCode::SUCCESS);

  int connect_count = 0;
  auto callback = [&connect_count](auto result) {
    ASSERT_TRUE(result.is_error());
    connect_count++;
  };

  EXPECT_TRUE(connected_peers().empty());
  conn_mgr()->Connect(peer->identifier(), callback, kConnectionOptions);
  ASSERT_TRUE(peer->le());
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer->le()->connection_state());

  RunUntilIdle();

  ASSERT_FALSE(fake_peer_ptr->logical_links().empty());
  auto handle = *fake_peer_ptr->logical_links().begin();

  conn_mgr()->Disconnect(peer->identifier());

  RunUntilIdle();

  // Complete interrogation so that callback gets called.
  auto response = hci::EventPacket::New<
      pw::bluetooth::emboss::ReadRemoteVersionInfoCompleteEventWriter>(
      hci_spec::kReadRemoteVersionInfoCompleteEventCode);
  auto view = response.view_t();
  view.status().Write(pw::bluetooth::emboss::StatusCode::SUCCESS);
  view.connection_handle().Write(handle);
  test_device()->SendCommandChannelPacket(response.data());

  RunUntilIdle();
  EXPECT_EQ(0u, connected_peers().size());
  EXPECT_EQ(1, connect_count);
  EXPECT_EQ(Peer::ConnectionState::kNotConnected,
            peer->le()->connection_state());
}

TEST_F(LowEnergyConnectionManagerTest,
       ConnectionFailedToBeEstablishedRetriesTwiceAndFails) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  EXPECT_TRUE(peer->temporary());

  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  size_t connected_count = 0;
  test_device()->set_connection_state_callback(
      [&](auto, auto, bool connected, bool) {
        if (connected) {
          connected_count++;
        }
      });

  int connect_cb_count = 0;
  auto callback = [&connect_cb_count](auto result) {
    ASSERT_TRUE(result.is_error());
    connect_cb_count++;
  };

  EXPECT_TRUE(connected_peers().empty());

  // Cause interrogation to fail.
  test_device()->SetDefaultCommandStatus(
      hci_spec::kReadRemoteVersionInfo,
      pw::bluetooth::emboss::StatusCode::CONNECTION_FAILED_TO_BE_ESTABLISHED);

  conn_mgr()->Connect(peer->identifier(), callback, kConnectionOptions);
  ASSERT_TRUE(peer->le());
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer->le()->connection_state());

  // Exhaust retries and cause connection to fail.
  for (size_t i = 0; i < kConnectDelays.size(); i++) {
    SCOPED_TRACE(i);
    if (i != 0) {
      RunFor(kConnectDelays[i] - std::chrono::nanoseconds(1));
      EXPECT_EQ(connected_count, i);
      RunFor(std::chrono::nanoseconds(1));
    } else {
      RunFor(kConnectDelays[i]);
    }
    EXPECT_EQ(connected_count, i + 1);
    EXPECT_EQ(Peer::ConnectionState::kInitializing,
              peer->le()->connection_state());

    test_device()->Disconnect(
        kAddress0,
        pw::bluetooth::emboss::StatusCode::CONNECTION_FAILED_TO_BE_ESTABLISHED);
    RunUntilIdle();
    EXPECT_EQ(connected_count, i + 1);
    // A connect command should be sent in connect_delays[i+1]
  }

  RunUntilIdle();
  EXPECT_TRUE(connected_peers().empty());
  EXPECT_EQ(connect_cb_count, 1);
  EXPECT_FALSE(peer->temporary());
  EXPECT_EQ(Peer::ConnectionState::kNotConnected,
            peer->le()->connection_state());
}

TEST_F(LowEnergyConnectionManagerTest,
       ConnectionFailedToBeEstablishedRetriesAndSucceeds) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  EXPECT_TRUE(peer->temporary());

  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto callback = [&conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
    EXPECT_TRUE(conn_handle->active());
  };

  EXPECT_TRUE(connected_peers().empty());

  // Cause interrogation to fail.
  test_device()->SetDefaultCommandStatus(
      hci_spec::kReadRemoteVersionInfo,
      pw::bluetooth::emboss::StatusCode::CONNECTION_FAILED_TO_BE_ESTABLISHED);

  conn_mgr()->Connect(peer->identifier(), callback, kConnectionOptions);
  ASSERT_TRUE(peer->le());
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer->le()->connection_state());

  RunUntilIdle();
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer->le()->connection_state());
  EXPECT_FALSE(conn_handle);

  // Allow the next interrogation to succeed.
  test_device()->ClearDefaultCommandStatus(hci_spec::kReadRemoteVersionInfo);

  // Disconnect should initiate retry #2 after a pause.
  test_device()->Disconnect(
      kAddress0,
      pw::bluetooth::emboss::StatusCode::CONNECTION_FAILED_TO_BE_ESTABLISHED);
  RunFor(std::chrono::seconds(2));
  EXPECT_EQ(1u, connected_peers().size());
  EXPECT_EQ(1u, connected_peers().count(kAddress0));
  ASSERT_TRUE(conn_handle);
  EXPECT_TRUE(conn_handle->active());
  EXPECT_FALSE(peer->temporary());
  EXPECT_EQ(Peer::ConnectionState::kConnected, peer->le()->connection_state());
}

TEST_F(LowEnergyConnectionManagerTest,
       ConnectionFailedToBeEstablishedAndDisconnectDuringRetryPauseTimeout) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  EXPECT_TRUE(peer->temporary());

  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  int connect_cb_count = 0;
  auto callback = [&](auto result) {
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(HostError::kCanceled, result.error_value());
    connect_cb_count++;
  };

  EXPECT_TRUE(connected_peers().empty());

  // Cause interrogation to fail.
  test_device()->SetDefaultCommandStatus(
      hci_spec::kReadRemoteVersionInfo,
      pw::bluetooth::emboss::StatusCode::CONNECTION_FAILED_TO_BE_ESTABLISHED);

  conn_mgr()->Connect(peer->identifier(), callback, kConnectionOptions);
  ASSERT_TRUE(peer->le());
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer->le()->connection_state());

  RunUntilIdle();
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer->le()->connection_state());
  EXPECT_EQ(connect_cb_count, 0);

  // Allow the next interrogation to succeed (even though it shouldn't happen).
  test_device()->ClearDefaultCommandStatus(hci_spec::kReadRemoteVersionInfo);

  // Peer disconnection during interrogation should also cause retry (after a
  // pause)
  test_device()->Disconnect(
      kAddress0,
      pw::bluetooth::emboss::StatusCode::CONNECTION_FAILED_TO_BE_ESTABLISHED);
  RunUntilIdle();
  // Disconnect will cancel request.
  conn_mgr()->Disconnect(peer->identifier());
  // Ensure timer is canceled.
  // TODO(saeedali): run repeatedly?
  // RunLoopRepeatedlyFor(std::chrono::seconds(1));
  RunFor(std::chrono::seconds(1));
  EXPECT_EQ(connect_cb_count, 1);
  EXPECT_EQ(0u, connected_peers().size());
  EXPECT_FALSE(peer->temporary());
  EXPECT_EQ(Peer::ConnectionState::kNotConnected,
            peer->le()->connection_state());
}

// Tests that receiving a peer kConnectionFailedToBeEstablished disconnect event
// before interrogation fails does not crash.
TEST_F(LowEnergyConnectionManagerTest,
       ConnectionFailedToBeEstablishedDisconnectionBeforeInterrogationFails) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  EXPECT_TRUE(peer->temporary());

  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  int connect_cb_count = 0;
  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto callback = [&](auto result) {
    ASSERT_EQ(fit::ok(), result);
    connect_cb_count++;
    conn_handle = std::move(result).value();
  };

  EXPECT_TRUE(connected_peers().empty());

  // Cause interrogation to stall waiting for command complete event.
  test_device()->SetDefaultCommandStatus(
      hci_spec::kReadRemoteVersionInfo,
      pw::bluetooth::emboss::StatusCode::SUCCESS);

  conn_mgr()->Connect(peer->identifier(), callback, kConnectionOptions);
  ASSERT_TRUE(peer->le());
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer->le()->connection_state());

  RunUntilIdle();
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer->le()->connection_state());
  EXPECT_EQ(connect_cb_count, 0);

  // Let retries succeed.
  test_device()->ClearDefaultCommandStatus(hci_spec::kReadRemoteVersionInfo);

  // Peer disconnection during interrogation should also cause retry (after a
  // pause).
  test_device()->Disconnect(
      kAddress0,
      pw::bluetooth::emboss::StatusCode::CONNECTION_FAILED_TO_BE_ESTABLISHED);
  RunUntilIdle();

  // Complete interrogation with an error that will be received after the
  // disconnect event. Event params other than status will be ignored because
  // status is an error.
  auto response = hci::EventPacket::New<
      pw::bluetooth::emboss::ReadRemoteVersionInfoCompleteEventWriter>(
      hci_spec::kReadRemoteVersionInfoCompleteEventCode);
  auto view = response.view_t();
  view.status().Write(pw::bluetooth::emboss::StatusCode::UNKNOWN_CONNECTION_ID);
  test_device()->SendCommandChannelPacket(response.data());

  RunUntilIdle();
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer->le()->connection_state());
  EXPECT_EQ(connect_cb_count, 0);

  // Wait for retry.
  RunFor(kConnectDelays[1]);
  EXPECT_EQ(connect_cb_count, 1);
  EXPECT_TRUE(conn_handle);
  EXPECT_EQ(1u, connected_peers().size());
  EXPECT_FALSE(peer->temporary());
  EXPECT_EQ(Peer::ConnectionState::kConnected, peer->le()->connection_state());
}

// Behavior verified in this test:
// 1. After a successful connection + bond to establish auto-connect for a peer,
// an auto-connect-
//    initiated connection attempt to that peer that fails with any of
//    `statuses_that_disable_ autoconnect` disables auto-connect to that peer.
// 2. After a successful ..., NON-autoconnect-inititated connection attempts
// (inbound or outbound)
//    to that peer that fail with any of `statuses_that_disable_autoconnect` do
//    NOT disable auto- connect to that peer.
TEST_F(LowEnergyConnectionManagerTest,
       ConnectSucceedsThenAutoConnectFailsDisablesAutoConnect) {
  // If an auto-connect attempt fails with any of these status codes, we disable
  // the auto-connect behavior until the next successful connection to avoid
  // looping.
  // clang-format off
  std::array statuses_that_disable_autoconnect = {
      pw::bluetooth::emboss::StatusCode::CONNECTION_TIMEOUT,
      pw::bluetooth::emboss::StatusCode::CONNECTION_REJECTED_SECURITY,
      pw::bluetooth::emboss::StatusCode::CONNECTION_ACCEPT_TIMEOUT_EXCEEDED,
      pw::bluetooth::emboss::StatusCode::CONNECTION_TERMINATED_BY_LOCAL_HOST,
      pw::bluetooth::emboss::StatusCode::CONNECTION_FAILED_TO_BE_ESTABLISHED
  };
  // clang-format on
  // Validate that looping with a uint8_t is safe, it makes the rest of the code
  // simpler.
  static_assert(statuses_that_disable_autoconnect.size() <
                std::numeric_limits<uint8_t>::max());
  for (uint8_t i = 0;
       i < static_cast<uint8_t>(statuses_that_disable_autoconnect.size());
       ++i) {
    SCOPED_TRACE(
        hci_spec::StatusCodeToString(statuses_that_disable_autoconnect[i]));
    const DeviceAddress kAddressI(DeviceAddress::Type::kLEPublic, {i});
    auto* peer = peer_cache()->NewPeer(kAddressI, /*connectable=*/true);
    auto fake_peer = std::make_unique<FakePeer>(kAddressI, dispatcher());
    test_device()->AddPeer(std::move(fake_peer));

    std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
    auto success_cb = [&conn_handle](auto result) {
      ASSERT_EQ(fit::ok(), result);
      conn_handle = std::move(result).value();
      EXPECT_TRUE(conn_handle->active());
    };

    conn_mgr()->Connect(peer->identifier(), success_cb, kConnectionOptions);
    RunUntilIdle();
    // Peer needs to be bonded to set auto connect
    peer->MutLe().SetBondData(sm::PairingData{});

    EXPECT_EQ(1u, connected_peers().count(kAddressI));
    ASSERT_TRUE(conn_handle);
    EXPECT_EQ(peer->identifier(), conn_handle->peer_identifier());
    EXPECT_TRUE(peer->le()->should_auto_connect());
    EXPECT_EQ(Peer::ConnectionState::kConnected,
              peer->le()->connection_state());

    // Disconnect has to be initiated by the "remote" device - locally initiated
    // disconnects will unset auto connect behavior.
    test_device()->Disconnect(peer->address());

    RunUntilIdle();

    EXPECT_EQ(Peer::ConnectionState::kNotConnected,
              peer->le()->connection_state());
    EXPECT_TRUE(peer->le()->should_auto_connect());

    // Causes interrogation to fail, so inbound connections will fail to
    // establish. This complexity is needed because inbound connections are
    // already HCI-connected when passed to the LECM.
    test_device()->SetDefaultCommandStatus(
        hci_spec::kReadRemoteVersionInfo, statuses_that_disable_autoconnect[i]);

    ConnectionResult result = fit::ok(nullptr);
    auto failure_cb = [&result](auto res) { result = std::move(res); };
    // Create an inbound HCI connection and try to register it with the LECM
    test_device()->ConnectLowEnergy(kAddressI);
    RunUntilIdle();
    auto link = MoveLastRemoteInitiated();
    ASSERT_TRUE(link);
    result = fit::ok(nullptr);
    conn_mgr()->RegisterRemoteInitiatedLink(
        std::move(link), BondableMode::Bondable, failure_cb);
    RunUntilIdle();
    // We always wait until the peer disconnects to relay connection failure
    // when dealing with the 0x3e kConnectionFailedToBeEstablished error.
    if (statuses_that_disable_autoconnect[i] ==
        pw::bluetooth::emboss::StatusCode::
            CONNECTION_FAILED_TO_BE_ESTABLISHED) {
      test_device()->Disconnect(kAddressI,
                                pw::bluetooth::emboss::StatusCode::
                                    CONNECTION_FAILED_TO_BE_ESTABLISHED);
      RunUntilIdle();
    }
    // Remote-initiated connection attempts that fail should not disable the
    // auto-connect flag.
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(Peer::ConnectionState::kNotConnected,
              peer->le()->connection_state());
    EXPECT_TRUE(peer->le()->should_auto_connect());
    // Allow successful interrogation later in the test
    test_device()->ClearDefaultCommandStatus(hci_spec::kReadRemoteVersionInfo);

    // Set this peer to reject all connections with
    // statuses_that_disable_autoconnect[i]
    FakePeer* peer_ref = test_device()->FindPeer(peer->address());
    ASSERT_TRUE(peer);
    peer_ref->set_connect_response(statuses_that_disable_autoconnect[i]);

    // User-initiated connection attempts that fail should not disable the
    // auto-connect flag.
    const LowEnergyConnectionOptions kNotAutoConnectOptions{.auto_connect =
                                                                false};
    conn_mgr()->Connect(peer->identifier(), failure_cb, kNotAutoConnectOptions);
    RunUntilIdle();

    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(Peer::ConnectionState::kNotConnected,
              peer->le()->connection_state());
    EXPECT_TRUE(peer->le()->should_auto_connect());

    // Emulate an auto-connection here, as we disable the auto-connect behavior
    // only for auto-connect-initiated attempts that fail, NOT for
    // user-initiated or remote-initiated connection attempts that fail.
    result = fit::ok(nullptr);
    const LowEnergyConnectionOptions kAutoConnectOptions{.auto_connect = true};
    conn_mgr()->Connect(peer->identifier(), failure_cb, kAutoConnectOptions);
    ASSERT_TRUE(peer->le());
    EXPECT_EQ(Peer::ConnectionState::kInitializing,
              peer->le()->connection_state());

    RunUntilIdle();

    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(Peer::ConnectionState::kNotConnected,
              peer->le()->connection_state());
    EXPECT_FALSE(peer->le()->should_auto_connect());
  }
}

#ifndef NINSPECT
TEST_F(LowEnergyConnectionManagerTest, Inspect) {
  inspect::Inspector inspector;
  conn_mgr()->AttachInspect(inspector.GetRoot(),
                            "low_energy_connection_manager");

  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  EXPECT_TRUE(peer->temporary());

  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto callback = [&conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
  };
  conn_mgr()->Connect(peer->identifier(), callback, kConnectionOptions);

  auto requests_matcher =
      AllOf(NodeMatches(NameMatches("pending_requests")),
            ChildrenMatch(ElementsAre(NodeMatches(
                AllOf(NameMatches("pending_request_0x0"),
                      PropertyList(UnorderedElementsAre(
                          StringIs("peer_id", peer->identifier().ToString()),
                          IntIs("callbacks", 1))))))));

  auto outbound_connector_matcher_attempt_0 = AllOf(
      NodeMatches(AllOf(NameMatches("outbound_connector"),
                        PropertyList(UnorderedElementsAre(
                            StringIs("peer_id", peer->identifier().ToString()),
                            IntIs("connection_attempt", 0),
                            BoolIs("is_outbound", true),
                            StringIs("state", "Connecting"))))));

  auto empty_connections_matcher =
      AllOf(NodeMatches(NameMatches("connections")),
            ChildrenMatch(::testing::IsEmpty()));

  auto conn_mgr_property_matcher = PropertyList(
      UnorderedElementsAre(UintIs("disconnect_explicit_disconnect_count", 0),
                           UintIs("disconnect_link_error_count", 0),
                           UintIs("disconnect_remote_disconnection_count", 0),
                           UintIs("disconnect_zero_ref_count", 0),
                           UintIs("incoming_connection_failure_count", 0),
                           UintIs("incoming_connection_success_count", 0),
                           UintIs("outgoing_connection_failure_count", 0),
                           UintIs("outgoing_connection_success_count", 0),
                           IntIs("recent_connection_failures", 0)));

  auto conn_mgr_during_connecting_matcher =
      AllOf(NodeMatches(AllOf(NameMatches("low_energy_connection_manager"),
                              conn_mgr_property_matcher)),
            ChildrenMatch(
                UnorderedElementsAre(requests_matcher,
                                     empty_connections_matcher,
                                     outbound_connector_matcher_attempt_0)));

  auto hierarchy = inspect::ReadFromVmo(inspector.DuplicateVmo());
  EXPECT_THAT(hierarchy.value(),
              ChildrenMatch(ElementsAre(conn_mgr_during_connecting_matcher)));

  // Finish connecting.
  RunUntilIdle();

  auto empty_requests_matcher =
      AllOf(NodeMatches(NameMatches("pending_requests")),
            ChildrenMatch(::testing::IsEmpty()));

  auto conn_matcher = NodeMatches(
      AllOf(NameMatches("connection_0x1"),
            PropertyList(UnorderedElementsAre(
                StringIs("peer_id", peer->identifier().ToString()),
                StringIs("peer_address", peer->address().ToString()),
                IntIs("ref_count", 1)))));

  auto connections_matcher = AllOf(NodeMatches(NameMatches("connections")),
                                   ChildrenMatch(ElementsAre(conn_matcher)));

  auto conn_mgr_property_matcher_after_connecting = PropertyList(
      UnorderedElementsAre(UintIs("disconnect_explicit_disconnect_count", 0),
                           UintIs("disconnect_link_error_count", 0),
                           UintIs("disconnect_remote_disconnection_count", 0),
                           UintIs("disconnect_zero_ref_count", 0),
                           UintIs("incoming_connection_failure_count", 0),
                           UintIs("incoming_connection_success_count", 0),
                           UintIs("outgoing_connection_failure_count", 0),
                           UintIs("outgoing_connection_success_count", 1),
                           IntIs("recent_connection_failures", 0)));

  auto conn_mgr_after_connecting_matcher =
      AllOf(NodeMatches(conn_mgr_property_matcher_after_connecting),
            ChildrenMatch(UnorderedElementsAre(empty_requests_matcher,
                                               connections_matcher)));

  hierarchy = inspect::ReadFromVmo(inspector.DuplicateVmo());
  EXPECT_THAT(hierarchy.value(),
              ChildrenMatch(ElementsAre(conn_mgr_after_connecting_matcher)));

  // LECM must be destroyed before the inspector to avoid a page fault on
  // destruction of inspect properties (they try to update the inspect VMO,
  // which is deleted on inspector destruction).
  DeleteConnMgr();
}
#endif  // NINSPECT

#ifndef NINSPECT
TEST_F(LowEnergyConnectionManagerTest, InspectFailedConnection) {
  inspect::Inspector inspector;
  conn_mgr()->AttachInspect(inspector.GetRoot(),
                            "low_energy_connection_manager");

  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  EXPECT_TRUE(peer->temporary());

  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  fake_peer->set_connect_status(
      pw::bluetooth::emboss::StatusCode::CONNECTION_LIMIT_EXCEEDED);
  test_device()->AddPeer(std::move(fake_peer));

  auto callback = [](auto result) { ASSERT_TRUE(result.is_error()); };
  conn_mgr()->Connect(peer->identifier(), callback, kConnectionOptions);
  RunUntilIdle();

  auto conn_mgr_property_matcher = PropertyList(
      UnorderedElementsAre(UintIs("disconnect_explicit_disconnect_count", 0),
                           UintIs("disconnect_link_error_count", 0),
                           UintIs("disconnect_remote_disconnection_count", 0),
                           UintIs("disconnect_zero_ref_count", 0),
                           UintIs("incoming_connection_failure_count", 0),
                           UintIs("incoming_connection_success_count", 0),
                           UintIs("outgoing_connection_failure_count", 1),
                           UintIs("outgoing_connection_success_count", 0),
                           IntIs("recent_connection_failures", 1)));

  auto hierarchy = inspect::ReadFromVmo(inspector.DuplicateVmo());
  EXPECT_THAT(
      hierarchy.value(),
      ChildrenMatch(ElementsAre(NodeMatches(conn_mgr_property_matcher))));

  RunFor(LowEnergyConnectionManager::
             kInspectRecentConnectionFailuresExpiryDuration -
         std::chrono::nanoseconds(1));
  hierarchy = inspect::ReadFromVmo(inspector.DuplicateVmo());
  EXPECT_THAT(
      hierarchy.value(),
      ChildrenMatch(ElementsAre(NodeMatches(conn_mgr_property_matcher))));

  // Failures should revert to 0 after expiry duration.
  RunFor(std::chrono::nanoseconds(1));
  conn_mgr_property_matcher = PropertyList(
      UnorderedElementsAre(UintIs("disconnect_explicit_disconnect_count", 0),
                           UintIs("disconnect_link_error_count", 0),
                           UintIs("disconnect_remote_disconnection_count", 0),
                           UintIs("disconnect_zero_ref_count", 0),
                           UintIs("incoming_connection_failure_count", 0),
                           UintIs("incoming_connection_success_count", 0),
                           UintIs("outgoing_connection_failure_count", 1),
                           UintIs("outgoing_connection_success_count", 0),
                           IntIs("recent_connection_failures", 0)));
  hierarchy = inspect::ReadFromVmo(inspector.DuplicateVmo());
  EXPECT_THAT(
      hierarchy.value(),
      ChildrenMatch(ElementsAre(NodeMatches(conn_mgr_property_matcher))));

  // LECM must be destroyed before the inspector to avoid a page fault on
  // destruction of inspect properties (they try to update the inspect VMO,
  // which is deleted on inspector destruction).
  DeleteConnMgr();
}
#endif  // NINSPECT

TEST_F(
    LowEnergyConnectionManagerTest,
    RegisterRemoteInitiatedLinkWithAddressDifferentFromIdentityAddressDoesNotCrash) {
  DeviceAddress kIdentityAddress(DeviceAddress::Type::kLEPublic,
                                 {1, 0, 0, 0, 0, 0});
  DeviceAddress kRandomAddress(DeviceAddress::Type::kLERandom,
                               {2, 0, 0, 0, 0, 0});
  Peer* peer = peer_cache()->NewPeer(kRandomAddress, /*connectable=*/true);
  sm::PairingData data;
  data.peer_ltk = kLTK;
  data.local_ltk = kLTK;
  data.irk = sm::Key(sm::SecurityProperties(), Random<UInt128>());
  data.identity_address = kIdentityAddress;
  EXPECT_TRUE(peer_cache()->StoreLowEnergyBond(peer->identifier(), data));
  EXPECT_EQ(peer->address(), kIdentityAddress);

  test_device()->AddPeer(
      std::make_unique<FakePeer>(kRandomAddress, dispatcher()));
  test_device()->ConnectLowEnergy(kRandomAddress);
  RunUntilIdle();

  auto link = MoveLastRemoteInitiated();
  ASSERT_TRUE(link);

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  conn_mgr()->RegisterRemoteInitiatedLink(
      std::move(link), BondableMode::Bondable, [&](auto result) {
        ASSERT_EQ(fit::ok(), result);
        conn_handle = std::move(result).value();
      });
  EXPECT_EQ(peer->le()->connection_state(),
            Peer::ConnectionState::kInitializing);

  RunUntilIdle();

  ASSERT_TRUE(conn_handle);
  EXPECT_TRUE(conn_handle->active());
  EXPECT_EQ(peer->identifier(), conn_handle->peer_identifier());
  EXPECT_TRUE(peer->connected());
}

TEST_F(LowEnergyConnectionManagerTest,
       ConnectSinglePeerWithInterrogationLongerThanCentralPauseTimeout) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  EXPECT_TRUE(peer->temporary());

  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  // Cause interrogation to stall so that we can expire the central pause
  // timeout.
  fit::closure send_read_remote_features_rsp;
  test_device()->pause_responses_for_opcode(
      hci_spec::kLEReadRemoteFeatures, [&](fit::closure unpause) {
        send_read_remote_features_rsp = std::move(unpause);
      });

  size_t hci_update_conn_param_count = 0;
  test_device()->set_le_connection_parameters_callback(
      [&](auto, auto) { hci_update_conn_param_count++; });

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto callback = [&conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
    EXPECT_TRUE(conn_handle->active());
  };

  EXPECT_TRUE(connected_peers().empty());
  conn_mgr()->Connect(peer->identifier(), callback, kConnectionOptions);
  ASSERT_TRUE(peer->le());
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer->le()->connection_state());

  RunUntilIdle();
  EXPECT_EQ(1u, connected_peers().size());
  EXPECT_EQ(1u, connected_peers().count(kAddress0));
  EXPECT_FALSE(conn_handle);
  EXPECT_EQ(hci_update_conn_param_count, 0u);

  RunFor(kLEConnectionPausePeripheral);
  EXPECT_FALSE(conn_handle);
  EXPECT_EQ(hci_update_conn_param_count, 0u);

  // Allow interrogation to complete.
  send_read_remote_features_rsp();
  RunUntilIdle();
  EXPECT_EQ(hci_update_conn_param_count, 1u);
  ASSERT_TRUE(conn_handle);
  EXPECT_TRUE(conn_handle->active());
  EXPECT_EQ(Peer::ConnectionState::kConnected, peer->le()->connection_state());
}

TEST_F(
    LowEnergyConnectionManagerTest,
    RegisterRemoteInitiatedLinkWithInterrogationLongerThanPeripheralPauseTimeout) {
  // A FakePeer does not support the HCI connection parameter update procedure
  // by default, so the L2CAP procedure will be used.
  test_device()->AddPeer(std::make_unique<FakePeer>(kAddress0, dispatcher()));

  // Cause interrogation to stall so that we can expire the peripheral pause
  // timeout.
  fit::closure send_read_remote_features_rsp;
  test_device()->pause_responses_for_opcode(
      hci_spec::kLEReadRemoteFeatures, [&](fit::closure unpause) {
        send_read_remote_features_rsp = std::move(unpause);
      });

  size_t l2cap_conn_param_update_count = 0;
  fake_l2cap()->set_connection_parameter_update_request_responder(
      [&](auto, auto) {
        l2cap_conn_param_update_count++;
        return true;
      });

  // First create a fake incoming connection.
  test_device()->ConnectLowEnergy(kAddress0);
  RunUntilIdle();
  auto link = MoveLastRemoteInitiated();
  ASSERT_TRUE(link);

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  conn_mgr()->RegisterRemoteInitiatedLink(
      std::move(link), BondableMode::Bondable, [&](auto result) {
        ASSERT_EQ(fit::ok(), result);
        conn_handle = std::move(result).value();
      });

  // A Peer should now exist in the cache.
  auto* peer = peer_cache()->FindByAddress(kAddress0);
  ASSERT_TRUE(peer);
  EXPECT_EQ(peer->le()->connection_state(),
            Peer::ConnectionState::kInitializing);

  RunUntilIdle();
  EXPECT_EQ(1u, connected_peers().size());
  EXPECT_EQ(1u, connected_peers().count(kAddress0));
  EXPECT_FALSE(conn_handle);
  EXPECT_EQ(l2cap_conn_param_update_count, 0u);

  RunFor(kLEConnectionPausePeripheral);
  EXPECT_FALSE(conn_handle);
  EXPECT_EQ(l2cap_conn_param_update_count, 0u);

  // Allow interrogation to complete.
  send_read_remote_features_rsp();
  RunUntilIdle();
  EXPECT_EQ(l2cap_conn_param_update_count, 1u);
  ASSERT_TRUE(conn_handle);
  EXPECT_TRUE(conn_handle->active());
  EXPECT_EQ(Peer::ConnectionState::kConnected, peer->le()->connection_state());
}

// Test fixture for tests that disconnect a connection in various ways and
// expect that controller packet counts are not cleared on disconnecting, but
// are cleared on disconnection complete. Tests should disconnect
// conn_handle0().
class PendingPacketsTest : public LowEnergyConnectionManagerTest {
 public:
  PendingPacketsTest() = default;
  ~PendingPacketsTest() override = default;

  void SetUp() override {
    LowEnergyConnectionManagerTest::SetUp();
    const DeviceAddress kPeerAddr0(DeviceAddress::Type::kLEPublic, {1});
    const DeviceAddress kPeerAddr1(DeviceAddress::Type::kLEPublic, {2});

    peer0_ = peer_cache()->NewPeer(kPeerAddr0, /*connectable=*/true);
    EXPECT_TRUE(peer0_->temporary());
    test_device()->AddPeer(
        std::make_unique<FakePeer>(kPeerAddr0, dispatcher()));

    peer1_ = peer_cache()->NewPeer(kPeerAddr1, /*connectable=*/true);
    EXPECT_TRUE(peer1_->temporary());
    test_device()->AddPeer(
        std::make_unique<FakePeer>(kPeerAddr1, dispatcher()));

    // Connect |peer0|
    conn_handle0_.reset();
    auto callback0 = [this](auto result) {
      ASSERT_EQ(fit::ok(), result);
      conn_handle0_ = std::move(result).value();
      EXPECT_TRUE(conn_handle0_->active());
    };
    conn_mgr()->Connect(peer0_->identifier(), callback0, kConnectionOptions);
    RunUntilIdle();

    // Connect |peer1|
    conn_handle1_.reset();
    auto callback1 = [this](auto result) {
      ASSERT_EQ(fit::ok(), result);
      conn_handle1_ = std::move(result).value();
      EXPECT_TRUE(conn_handle1_->active());
    };
    conn_mgr()->Connect(peer1_->identifier(), callback1, kConnectionOptions);
    RunUntilIdle();

    packet_count_ = 0;
    test_device()->SetDataCallback([&](const auto&) { packet_count_++; },
                                   dispatcher());
    test_device()->set_auto_completed_packets_event_enabled(false);
    test_device()->set_auto_disconnection_complete_event_enabled(false);
  }

  void TearDown() override {
    peer0_ = nullptr;
    peer1_ = nullptr;
    conn_handle0_.reset();
    conn_handle1_.reset();

    LowEnergyConnectionManagerTest::TearDown();
  }

  Peer* peer0() { return peer0_; }
  std::unique_ptr<LowEnergyConnectionHandle>& conn_handle0() {
    return conn_handle0_;
  }

 protected:
  hci_spec::ConnectionHandle handle0_;
  std::unique_ptr<LowEnergyConnectionHandle> conn_handle0_;
  std::unique_ptr<LowEnergyConnectionHandle> conn_handle1_;

 private:
  size_t packet_count_;
  Peer* peer0_;
  Peer* peer1_;
};

using LowEnergyConnectionManagerPendingPacketsTest = PendingPacketsTest;

TEST_F(LowEnergyConnectionManagerPendingPacketsTest, Disconnect) {
  hci::FakeAclConnection connection_0(
      acl_data_channel(), conn_handle0_->handle(), bt::LinkType::kLE);
  hci::FakeAclConnection connection_1(
      acl_data_channel(), conn_handle1_->handle(), bt::LinkType::kLE);

  acl_data_channel()->RegisterConnection(connection_0.GetWeakPtr());
  acl_data_channel()->RegisterConnection(connection_1.GetWeakPtr());

  // Fill controller buffer by sending |kLEMaxNumPackets| packets to |peer0|
  for (size_t i = 0; i < kLEMaxNumPackets; i++) {
    hci::ACLDataPacketPtr packet = hci::ACLDataPacket::New(
        conn_handle0_->handle(),
        hci_spec::ACLPacketBoundaryFlag::kFirstNonFlushable,
        hci_spec::ACLBroadcastFlag::kPointToPoint,
        /*payload_size=*/1);
    connection_0.QueuePacket(std::move(packet));
    RunUntilIdle();
  }

  // Queue packet for |peer1|
  hci::ACLDataPacketPtr packet = hci::ACLDataPacket::New(
      conn_handle1_->handle(),
      hci_spec::ACLPacketBoundaryFlag::kFirstNonFlushable,
      hci_spec::ACLBroadcastFlag::kPointToPoint,
      /*payload_size=*/1);
  connection_1.QueuePacket(std::move(packet));
  RunUntilIdle();

  // Packet for |peer1| should not have been sent because controller buffer is
  // full
  EXPECT_EQ(connection_0.queued_packets().size(), 0u);
  EXPECT_EQ(connection_1.queued_packets().size(), 1u);

  handle0_ = conn_handle0_->handle();

  // Send HCI Disconnect to controller
  EXPECT_TRUE(conn_mgr()->Disconnect(peer0()->identifier()));

  RunUntilIdle();

  // Packet for |peer1| should not have been sent before Disconnection Complete
  // event
  EXPECT_EQ(connection_0.queued_packets().size(), 0u);
  EXPECT_EQ(connection_1.queued_packets().size(), 1u);

  acl_data_channel()->UnregisterConnection(conn_handle0_->handle());

  // FakeController send us the HCI Disconnection Complete event
  test_device()->SendDisconnectionCompleteEvent(handle0_);
  RunUntilIdle();

  // |peer0|'s link should have been unregistered and packet for |peer1| should
  // have been sent
  EXPECT_EQ(connection_0.queued_packets().size(), 0u);
  EXPECT_EQ(connection_1.queued_packets().size(), 0u);
}

TEST_F(LowEnergyConnectionManagerPendingPacketsTest, ReleaseRef) {
  hci::FakeAclConnection connection_0(
      acl_data_channel(), conn_handle0_->handle(), bt::LinkType::kLE);
  hci::FakeAclConnection connection_1(
      acl_data_channel(), conn_handle1_->handle(), bt::LinkType::kLE);

  acl_data_channel()->RegisterConnection(connection_0.GetWeakPtr());
  acl_data_channel()->RegisterConnection(connection_1.GetWeakPtr());

  // Fill controller buffer by sending |kLEMaxNumPackets| packets to |peer0|
  for (size_t i = 0; i < kLEMaxNumPackets; i++) {
    hci::ACLDataPacketPtr packet = hci::ACLDataPacket::New(
        conn_handle0_->handle(),
        hci_spec::ACLPacketBoundaryFlag::kFirstNonFlushable,
        hci_spec::ACLBroadcastFlag::kPointToPoint,
        /*payload_size=*/1);
    connection_0.QueuePacket(std::move(packet));
    RunUntilIdle();
  }

  // Queue packet for |peer1|
  hci::ACLDataPacketPtr packet = hci::ACLDataPacket::New(
      conn_handle1_->handle(),
      hci_spec::ACLPacketBoundaryFlag::kFirstNonFlushable,
      hci_spec::ACLBroadcastFlag::kPointToPoint,
      /*payload_size=*/1);
  connection_1.QueuePacket(std::move(packet));
  RunUntilIdle();

  // Packet for |peer1| should not have been sent before Disconnection Complete
  // event
  EXPECT_EQ(connection_0.queued_packets().size(), 0u);
  EXPECT_EQ(connection_1.queued_packets().size(), 1u);

  handle0_ = conn_handle0_->handle();

  // Releasing ref should send HCI Disconnect to controller
  conn_handle0().reset();

  RunUntilIdle();

  // Packet for |peer1| should not have been sent before Disconnection Complete
  // event
  EXPECT_EQ(connection_0.queued_packets().size(), 0u);
  EXPECT_EQ(connection_1.queued_packets().size(), 1u);

  acl_data_channel()->UnregisterConnection(handle0_);

  // FakeController send us the HCI Disconnection Complete event
  test_device()->SendDisconnectionCompleteEvent(handle0_);
  RunUntilIdle();

  // |peer0|'s link should have been unregistered and packet for |peer1| should
  // have been sent
  EXPECT_EQ(connection_0.queued_packets().size(), 0u);
  EXPECT_EQ(connection_1.queued_packets().size(), 0u);
}

TEST_F(LowEnergyConnectionManagerTest, ConnectAndOpenL2cap) {
  constexpr l2cap::Psm kFakePsm = 15;
  constexpr l2cap::ChannelParameters kChannelParameters{
      .mode = l2cap::CreditBasedFlowControlMode::kLeCreditBasedFlowControl,
      .max_rx_sdu_size = std::nullopt,
      .flush_timeout = std::nullopt,
  };
  constexpr l2cap::ChannelId kFakeChannelId = l2cap::kFirstDynamicChannelId;
  constexpr sm::SecurityLevel kSecurityLevel = sm::SecurityLevel::kEncrypted;

  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto connection_callback = [&conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
  };

  EXPECT_TRUE(connected_peers().empty());
  conn_mgr()->Connect(
      peer->identifier(), connection_callback, kConnectionOptions);
  ASSERT_TRUE(peer->le());
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer->le()->connection_state());

  RunUntilIdle();
  EXPECT_EQ(conn_handle->bondable_mode(), sm::BondableMode::Bondable);

  EXPECT_EQ(1u, connected_peers().count(kAddress0));
  ASSERT_TRUE(conn_handle);
  EXPECT_TRUE(conn_handle->active());

  std::optional<WeakSelf<l2cap::Channel>::WeakPtr> channel;
  auto callback = [&channel](auto result) {
    ASSERT_TRUE(result.is_alive());
    channel = std::move(result);
  };

  fake_l2cap()->ExpectOutboundL2capChannel(conn_handle->handle(),
                                           kFakePsm,
                                           kFakeChannelId,
                                           kFakeChannelId,
                                           kChannelParameters);
  conn_mgr()->OpenL2capChannel(peer->identifier(),
                               kFakePsm,
                               kChannelParameters,
                               kSecurityLevel,
                               callback);

  RunUntilIdle();
  EXPECT_TRUE(channel);
  EXPECT_TRUE(channel->is_alive());
  EXPECT_TRUE(conn_handle->security().encrypted());
  EXPECT_EQ(conn_handle->bondable_mode(), sm::BondableMode::Bondable);
}

TEST_F(LowEnergyConnectionManagerTest, UnknownPeerFailOpenL2cap) {
  constexpr PeerId kUnknownId(1);
  constexpr l2cap::Psm kFakePsm = 15;
  constexpr l2cap::ChannelParameters kChannelParameters{
      .mode = l2cap::CreditBasedFlowControlMode::kLeCreditBasedFlowControl,
      .max_rx_sdu_size = std::nullopt,
      .flush_timeout = std::nullopt,
  };
  constexpr sm::SecurityLevel kSecurityLevel = sm::SecurityLevel::kEncrypted;

  std::optional<WeakSelf<l2cap::Channel>::WeakPtr> channel;
  auto callback = [&channel](auto result) { channel = std::move(result); };

  conn_mgr()->OpenL2capChannel(
      kUnknownId, kFakePsm, kChannelParameters, kSecurityLevel, callback);

  RunUntilIdle();
  EXPECT_TRUE(channel);
  EXPECT_FALSE(channel->is_alive());
}

TEST_F(LowEnergyConnectionManagerTest, ConnectAndOpenL2capAuthenticated) {
  constexpr l2cap::Psm kFakePsm = 15;
  constexpr l2cap::ChannelParameters kChannelParameters{
      .mode = l2cap::CreditBasedFlowControlMode::kLeCreditBasedFlowControl,
      .max_rx_sdu_size = std::nullopt,
      .flush_timeout = std::nullopt,
  };
  constexpr l2cap::ChannelId kFakeChannelId = l2cap::kFirstDynamicChannelId;
  constexpr sm::SecurityLevel kSecurityLevel =
      sm::SecurityLevel::kAuthenticated;

  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto connection_callback = [&conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
  };

  EXPECT_TRUE(connected_peers().empty());
  conn_mgr()->Connect(
      peer->identifier(), connection_callback, kConnectionOptions);
  ASSERT_TRUE(peer->le());
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer->le()->connection_state());

  RunUntilIdle();
  EXPECT_EQ(conn_handle->bondable_mode(), sm::BondableMode::Bondable);

  EXPECT_EQ(1u, connected_peers().count(kAddress0));
  ASSERT_TRUE(conn_handle);
  EXPECT_TRUE(conn_handle->active());

  std::optional<WeakSelf<l2cap::Channel>::WeakPtr> channel;
  auto callback = [&channel](auto result) {
    ASSERT_TRUE(result.is_alive());
    channel = std::move(result);
  };

  fake_l2cap()->ExpectOutboundL2capChannel(conn_handle->handle(),
                                           kFakePsm,
                                           kFakeChannelId,
                                           kFakeChannelId,
                                           kChannelParameters);
  conn_mgr()->OpenL2capChannel(peer->identifier(),
                               kFakePsm,
                               kChannelParameters,
                               kSecurityLevel,
                               callback);

  RunUntilIdle();
  EXPECT_TRUE(channel);
  EXPECT_TRUE(channel->is_alive());
  EXPECT_TRUE(conn_handle->security().encrypted());
  EXPECT_TRUE(conn_handle->security().authenticated());
  EXPECT_EQ(conn_handle->bondable_mode(), sm::BondableMode::Bondable);
}

TEST_F(LowEnergyConnectionManagerTest, ConnectAndOpenL2capSecureAuthenticated) {
  constexpr l2cap::Psm kFakePsm = 15;
  constexpr l2cap::ChannelParameters kChannelParameters{
      .mode = l2cap::CreditBasedFlowControlMode::kLeCreditBasedFlowControl,
      .max_rx_sdu_size = std::nullopt,
      .flush_timeout = std::nullopt,
  };
  constexpr l2cap::ChannelId kFakeChannelId = l2cap::kFirstDynamicChannelId;
  constexpr sm::SecurityLevel kSecurityLevel =
      sm::SecurityLevel::kSecureAuthenticated;

  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto connection_callback = [&conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
  };

  EXPECT_TRUE(connected_peers().empty());
  conn_mgr()->Connect(
      peer->identifier(), connection_callback, kConnectionOptions);
  ASSERT_TRUE(peer->le());
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer->le()->connection_state());

  RunUntilIdle();
  EXPECT_EQ(conn_handle->bondable_mode(), sm::BondableMode::Bondable);

  EXPECT_EQ(1u, connected_peers().count(kAddress0));
  ASSERT_TRUE(conn_handle);
  EXPECT_TRUE(conn_handle->active());

  std::optional<WeakSelf<l2cap::Channel>::WeakPtr> channel;
  auto callback = [&channel](auto result) {
    ASSERT_TRUE(result.is_alive());
    channel = std::move(result);
  };

  fake_l2cap()->ExpectOutboundL2capChannel(conn_handle->handle(),
                                           kFakePsm,
                                           kFakeChannelId,
                                           kFakeChannelId,
                                           kChannelParameters);
  conn_mgr()->OpenL2capChannel(peer->identifier(),
                               kFakePsm,
                               kChannelParameters,
                               kSecurityLevel,
                               callback);

  RunUntilIdle();
  EXPECT_TRUE(channel);
  EXPECT_TRUE(channel->is_alive());
  EXPECT_TRUE(conn_handle->security().encrypted());
  EXPECT_TRUE(conn_handle->security().authenticated());
  EXPECT_TRUE(conn_handle->security().secure_connections());
  EXPECT_EQ(conn_handle->bondable_mode(), sm::BondableMode::Bondable);
}

TEST_F(LowEnergyConnectionManagerTest, ConnectAndOpenL2capNonBondable) {
  constexpr l2cap::Psm kFakePsm = 15;
  constexpr l2cap::ChannelParameters kChannelParameters{
      .mode = l2cap::CreditBasedFlowControlMode::kLeCreditBasedFlowControl,
      .max_rx_sdu_size = std::nullopt,
      .flush_timeout = std::nullopt,
  };
  constexpr l2cap::ChannelId kFakeChannelId = l2cap::kFirstDynamicChannelId;
  constexpr sm::SecurityLevel kSecurityLevel =
      sm::SecurityLevel::kSecureAuthenticated;
  constexpr LowEnergyConnectionOptions kConnectionOptionsNonBondable{
      .bondable_mode = sm::BondableMode::NonBondable,
  };

  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto connection_callback = [&conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
  };

  EXPECT_TRUE(connected_peers().empty());
  conn_mgr()->Connect(
      peer->identifier(), connection_callback, kConnectionOptionsNonBondable);
  ASSERT_TRUE(peer->le());
  EXPECT_EQ(Peer::ConnectionState::kInitializing,
            peer->le()->connection_state());

  RunUntilIdle();
  EXPECT_EQ(conn_handle->bondable_mode(), sm::BondableMode::NonBondable);

  EXPECT_EQ(1u, connected_peers().count(kAddress0));
  ASSERT_TRUE(conn_handle);
  EXPECT_TRUE(conn_handle->active());

  std::optional<WeakSelf<l2cap::Channel>::WeakPtr> channel;
  auto callback = [&channel](auto result) {
    ASSERT_TRUE(result.is_alive());
    channel = std::move(result);
  };

  fake_l2cap()->ExpectOutboundL2capChannel(conn_handle->handle(),
                                           kFakePsm,
                                           kFakeChannelId,
                                           kFakeChannelId,
                                           kChannelParameters);
  conn_mgr()->OpenL2capChannel(peer->identifier(),
                               kFakePsm,
                               kChannelParameters,
                               kSecurityLevel,
                               callback);

  RunUntilIdle();
  EXPECT_TRUE(channel);
  EXPECT_TRUE(channel->is_alive());
  EXPECT_TRUE(conn_handle->security().encrypted());
  EXPECT_TRUE(conn_handle->security().authenticated());
  EXPECT_TRUE(conn_handle->security().secure_connections());
  EXPECT_EQ(conn_handle->bondable_mode(), sm::BondableMode::NonBondable);
}

TEST_F(LowEnergyConnectionManagerTest,
       IsoStreamManagerNotCreatedIfNotSupported) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto callback = [&conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
  };
  conn_mgr()->Connect(peer->identifier(), callback, kConnectionOptions);
  RunUntilIdle();
  EXPECT_EQ(1u, connected_peers().size());
  ASSERT_TRUE(conn_handle);

  int reject_count = 0;
  test_device()->set_le_cis_reject_callback(
      [&](hci_spec::ConnectionHandle) { ++reject_count; });

  // CIS request should not be rejected because there is no IsoStreamManager.
  DynamicByteBuffer request_packet =
      testing::LECisRequestEventPacket(conn_handle->handle(),
                                       /*cis_connection_handle=*/5,
                                       /*cig_id=*/6,
                                       /*cis_id=*/7);
  test_device()->SendCommandChannelPacket(request_packet);
  RunUntilIdle();
  EXPECT_EQ(reject_count, 0);
}

class LowEnergyConnectionManagerIsoSupportedTest
    : public LowEnergyConnectionManagerTest,
      public ::testing::WithParamInterface<hci_spec::LESupportedFeature> {
 public:
  void SetUp() override {
    adapter_state().low_energy_state.set_supported_features(
        static_cast<uint64_t>(GetParam()));
    LowEnergyConnectionManagerTest::SetUp();
  }
};

TEST_P(LowEnergyConnectionManagerIsoSupportedTest, IsoStreamManagerCreated) {
  auto* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  std::unique_ptr<LowEnergyConnectionHandle> conn_handle;
  auto callback = [&conn_handle](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_handle = std::move(result).value();
  };
  conn_mgr()->Connect(peer->identifier(), callback, kConnectionOptions);
  RunUntilIdle();
  EXPECT_EQ(1u, connected_peers().size());
  ASSERT_TRUE(conn_handle);

  hci_spec::ConnectionHandle cis_handle = 5;
  int reject_count = 0;
  test_device()->set_le_cis_reject_callback(
      [&](hci_spec::ConnectionHandle handle) {
        ++reject_count;
        EXPECT_EQ(handle, cis_handle);
      });

  // CIS request should be rejected.
  DynamicByteBuffer request_packet =
      testing::LECisRequestEventPacket(conn_handle->handle(),
                                       /*cis_connection_handle=*/cis_handle,
                                       /*cig_id=*/6,
                                       /*cis_id=*/7);
  test_device()->SendCommandChannelPacket(request_packet);
  RunUntilIdle();
  EXPECT_EQ(reject_count, 1);
}

INSTANTIATE_TEST_SUITE_P(
    CisFeatureBits,
    LowEnergyConnectionManagerIsoSupportedTest,
    ::testing::Values(
        hci_spec::LESupportedFeature::kConnectedIsochronousStreamPeripheral,
        hci_spec::LESupportedFeature::kConnectedIsochronousStreamCentral));

}  // namespace
}  // namespace bt::gap
